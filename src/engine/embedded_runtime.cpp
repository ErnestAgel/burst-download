/**
 * @file embedded_runtime.cpp
 * @brief Extract the embedded Python runtime blob from the executable tail
 *        into a temp cache (with FNV-1a64 validation and a cross-process
 *        cache lock, issue R8).
 */

#include "embedded_runtime.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#include <fcntl.h>
#endif

namespace {

const char kMagicHead[] = "BURSTARC";
const char kMagicTail[] = "BURSTEND";
const size_t kFooterSize = 32;

/* Stale cache lock threshold: a lock older than 10 minutes is broken. */
const long kCacheLockStaleSec = 600;

std::string g_exe_path;
std::string g_runtime_last_error;

void SetRuntimeError(const char* pszFmt, const std::string& strArg) {
  char buf[512];
  snprintf(buf, sizeof(buf), pszFmt, strArg.c_str());
  g_runtime_last_error = buf;
}

uint64_t Fnv1a64(const uint8_t* data, size_t len) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (size_t i = 0; i < len; ++i) {
    h ^= data[i];
    h *= 0x100000001b3ULL;
  }
  return h;
}

/* Cache root directory (under the temp dir; rebuilt automatically after the
 * system cleans it up). */
std::string TempCacheDir() {
#ifdef _WIN32
  char buf[MAX_PATH];
  DWORD n = GetTempPathA(MAX_PATH, buf);
  if (n == 0 || n >= MAX_PATH) return "";
  return std::string(buf) + "burst-runtime";
#else
  const char* td = getenv("TMPDIR");
  std::string base = (td && *td) ? td : "/tmp";
  return base + "/burst-runtime-" + std::to_string(getuid());
#endif
}

bool ReadFooter(const std::string& exe_path, size_t& blob_size,
                uint64_t& hash) {
  std::ifstream in(exe_path, std::ios::binary);
  if (!in) return false;
  in.seekg(0, std::ios::end);
  const std::streamoff len = in.tellg();
  if (len < (std::streamoff)kFooterSize) return false;
  in.seekg(len - (std::streamoff)kFooterSize);
  char footer[kFooterSize];
  in.read(footer, kFooterSize);
  if (!in) return false;
  if (std::memcmp(footer, kMagicHead, 8) != 0) return false;
  if (std::memcmp(footer + 24, kMagicTail, 8) != 0) return false;
  uint64_t sz = 0;
  std::memcpy(&sz, footer + 8, 8);
  std::memcpy(&hash, footer + 16, 8);
  if (sz == 0 || sz > (uint64_t)len - kFooterSize) return false;
  blob_size = static_cast<size_t>(sz);
  return true;
}

bool ReadMarker(const std::string& cache, uint64_t& hash, size_t& size) {
  std::ifstream in(cache + "/manifest", std::ios::binary);
  if (!in) return false;
  uint64_t h = 0, sz = 0;
  in.read(reinterpret_cast<char*>(&h), 8);
  in.read(reinterpret_cast<char*>(&sz), 8);
  if (!in) return false;
  hash = h;
  size = static_cast<size_t>(sz);
  return true;
}

void WriteMarker(const std::string& cache, uint64_t hash, size_t size) {
  std::ofstream out(cache + "/manifest", std::ios::binary);
  uint64_t sz = size;
  out.write(reinterpret_cast<const char*>(&hash), 8);
  out.write(reinterpret_cast<const char*>(&sz), 8);
}

/**
 * @brief Acquire the cache lock (exclusive directory create; stale locks
 *        older than 10 minutes are broken).  Waits up to ~10s for a fresh
 *        lock.
 *
 * std::filesystem is used (instead of CreateFileA / open()) so the lock
 * path is handled exactly like every other cache path on Windows and never
 * depends on the ANSI code page (v2.4.2 regression on non-UTF-8 systems).
 * @param strLock Lock directory path.
 * @return TRUE when the lock is held.
 */
bool AcquireCacheLock(const std::string& strLock) {
  std::error_code ec;
  for (int i = 0; i < 50; ++i) {
    if (std::filesystem::create_directory(strLock, ec)) {
      return true;
    }
    ec.clear();
    /* The lock exists: break it when it is stale. */
    const std::filesystem::file_time_type ft =
        std::filesystem::last_write_time(strLock, ec);
    if (!ec) {
      const auto age = std::chrono::duration_cast<std::chrono::seconds>(
                           std::filesystem::file_time_type::clock::now() -
                           ft)
                           .count();
      if (age > kCacheLockStaleSec) {
        std::filesystem::remove_all(strLock, ec);
        ec.clear();
        continue;
      }
    }
    ec.clear();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  return false;
}

/** @brief Release the cache lock directory. */
void ReleaseCacheLock(const std::string& strLock) {
  std::error_code ec;
  std::filesystem::remove_all(strLock, ec);
}

}  // namespace

void EmbedSetExePath(const std::string& exe_path) { g_exe_path = exe_path; }

std::string EmbedGetExePath() { return g_exe_path; }

std::string EmbedRuntimeCacheRoot() { return TempCacheDir(); }

std::string EmbedRuntimeLastError() { return g_runtime_last_error; }

/** @brief A cache is reusable only when its critical entries are present;
 *         partial caches (e.g. after antivirus cleanup or an interrupted
 *         run) must be rebuilt instead of reused. */
bool CacheContentsUsable(const std::string& strDir)
{
    const bool bStdlib =
        (access((strDir + "/python311.zip").c_str(), 0) == 0) ||
        ((access((strDir + "/stdlib").c_str(), 0) == 0) &&
         (access((strDir + "/stdlib/encodings/__init__.pyc").c_str(), 0) ==
          0));
    const bool bYtDlp =
        (access((strDir + "/yt_dlp/__init__.pyc").c_str(), 0) == 0);
    return bStdlib && bYtDlp;
}

/** @brief Cache dir whose version marker matches the current blob and whose
 *         stdlib is present; empty when none matches. */
std::string MatchingCache(const std::string& strPrimary,
                          const std::string& strFallback, uint64_t hash,
                          size_t size) {
  for (const std::string& strDir : {strPrimary, strFallback}) {
    uint64_t mh = 0;
    size_t ms = 0;
    if (ReadMarker(strDir, mh, ms) && mh == hash && ms == size &&
        CacheContentsUsable(strDir)) {
      return strDir;
    }
  }
  return "";
}

bool ExtractEmbeddedRuntime(std::string& home) {
  home.clear();
  if (g_exe_path.empty()) {
    SetRuntimeError("executable path not set", "");
    return false;
  }

  size_t blob_size = 0;
  uint64_t hash = 0;
  if (!ReadFooter(g_exe_path, blob_size, hash)) {
    SetRuntimeError("no embedded runtime footer in the executable", "");
    return false;
  }

  const std::string cache = TempCacheDir();
  if (cache.empty()) {
    SetRuntimeError("temp cache directory unavailable", "");
    return false;
  }

  /* Hash-suffixed fallback cache: used when the primary cache is locked by
   * a still-running older process (Windows cannot delete files mapped as
   * DLLs), so a stale cache can never block the runtime. */
  char szHash[9];
  snprintf(szHash, sizeof(szHash), "%08llx", (unsigned long long)hash);
  const std::string strFallbackCache = cache + "-" + szHash;

  /* Cache hit: reuse when the version marker matches and stdlib exists. */
  const std::string strMatched =
      MatchingCache(cache, strFallbackCache, hash, blob_size);
  if (!strMatched.empty()) {
    home = strMatched;
    return true;
  }

  /* Cross-process cache lock (issue R8): serialize extraction and prevent
   * half-built caches.  Lock failure is not fatal: the temp-dir + atomic
   * rename below keeps the cache consistent, so a broken lock must never
   * block the runtime (v2.4.2 regression on non-UTF-8 systems). */
  const std::string strLock = cache + ".lock";
  const bool bLocked = AcquireCacheLock(strLock);
  if (!bLocked) {
    fprintf(stderr,
            "[runtime] cache lock unavailable (%s), extracting unlocked\n",
            strLock.c_str());
  }

  /* Re-check the marker under the lock: another process may have completed
   * the extraction while we waited. */
  const std::string strMatchedNow =
      MatchingCache(cache, strFallbackCache, hash, blob_size);
  if (!strMatchedNow.empty()) {
    if (bLocked) {
      ReleaseCacheLock(strLock);
    }
    home = strMatchedNow;
    return true;
  }

  /* Read the blob. */
  std::ifstream in(g_exe_path, std::ios::binary);
  if (!in) {
    SetRuntimeError("failed to open the executable", "");
    if (bLocked) {
      ReleaseCacheLock(strLock);
    }
    return false;
  }
  in.seekg(0, std::ios::end);
  const std::streamoff len = in.tellg();
  in.seekg(len - (std::streamoff)kFooterSize - (std::streamoff)blob_size);
  std::vector<uint8_t> blob(blob_size);
  in.read(reinterpret_cast<char*>(blob.data()), blob_size);
  if (!in) {
    SetRuntimeError("failed to read the embedded runtime blob", "");
    if (bLocked) {
      ReleaseCacheLock(strLock);
    }
    return false;
  }
  if (Fnv1a64(blob.data(), blob.size()) != hash) {
    SetRuntimeError("embedded runtime blob hash mismatch (corrupt)", "");
    if (bLocked) {
      ReleaseCacheLock(strLock);
    }
    return false;
  }

  /* Extract into a per-process temp dir, then atomically swap it into the
   * cache (issue R8: no half-built cache is ever visible). */
  const std::string tmp_cache =
      cache + ".tmp-" + std::to_string(
#ifdef _WIN32
          GetCurrentProcessId()
#else
          getpid()
#endif
      );
  std::error_code ec;
  std::filesystem::remove_all(tmp_cache, ec);
  std::filesystem::create_directories(tmp_cache, ec);

  size_t pos = 0;
  const auto rd = [&](void* dst, size_t n) -> bool {
    if (pos + n > blob.size()) return false;
    std::memcpy(dst, blob.data() + pos, n);
    pos += n;
    return true;
  };

  uint32_t count = 0;
  if (!rd(&count, 4)) {
    std::filesystem::remove_all(tmp_cache, ec);
    SetRuntimeError("embedded runtime blob format invalid", "");
    if (bLocked) {
      ReleaseCacheLock(strLock);
    }
    return false;
  }

  bool ok = true;
  for (uint32_t i = 0; i < count && ok; ++i) {
    uint16_t plen = 0;
    if (!rd(&plen, 2)) { ok = false; break; }
    std::string rel(plen, '\0');
    if (!rd(&rel[0], plen)) { ok = false; break; }
    uint64_t dlen = 0;
    if (!rd(&dlen, 8)) { ok = false; break; }
    /* Path safety: relative paths only; reject absolute paths and "..". */
    if (rel.empty() || rel[0] == '/' || rel.find("..") != std::string::npos) {
      ok = false;
      break;
    }
    const std::string full = tmp_cache + "/" + rel;
    std::filesystem::create_directories(
        std::filesystem::path(full).parent_path(), ec);
    std::ofstream out(full, std::ios::binary);
    if (!out) { ok = false; break; }
    size_t left = static_cast<size_t>(dlen);
    while (left > 0 && ok) {
      const size_t chunk = left < (1u << 20) ? left : (1u << 20);
      if (pos + chunk > blob.size()) { ok = false; break; }
      out.write(reinterpret_cast<const char*>(blob.data()) + pos,
                static_cast<std::streamsize>(chunk));
      pos += chunk;
      left -= chunk;
    }
    if (!out) ok = false;
  }

  if (!ok || pos != blob.size()) {
    std::filesystem::remove_all(tmp_cache, ec);
    SetRuntimeError("failed to extract embedded runtime files", "");
    if (bLocked) {
      ReleaseCacheLock(strLock);
    }
    return false;
  }
  WriteMarker(tmp_cache, hash, blob_size);

  /* Swap into the primary cache; when its files are locked (an older burst
   * is still running), use the hash-suffixed cache instead so the runtime
   * stays usable. */
  std::string strHome = tmp_cache;
  std::filesystem::remove_all(cache, ec);
  ec.clear();
  std::filesystem::rename(tmp_cache, cache, ec);
  if (!ec) {
    strHome = cache;
  } else {
    ec.clear();
    std::filesystem::remove_all(strFallbackCache, ec);
    ec.clear();
    std::filesystem::rename(tmp_cache, strFallbackCache, ec);
    if (!ec) {
      strHome = strFallbackCache;
    }
  }
  if (bLocked) {
    ReleaseCacheLock(strLock);
  }
  home = strHome;
  return true;
}
