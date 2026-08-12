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
#include <sys/stat.h>

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
 * @brief Acquire the cache lock file (exclusive create; stale locks older
 *        than 10 minutes are broken).  Waits up to ~10s for a fresh lock.
 * @param strLock Lock file path.
 * @return TRUE when the lock is held.
 */
bool AcquireCacheLock(const std::string& strLock) {
  for (int i = 0; i < 50; ++i) {
#ifdef _WIN32
    HANDLE h = CreateFileA(strLock.c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
      const std::string strInfo = std::to_string(GetCurrentProcessId()) + "\n";
      DWORD dwWritten = 0;
      WriteFile(h, strInfo.c_str(), (DWORD)strInfo.size(), &dwWritten, NULL);
      CloseHandle(h);
      return true;
    }
#else
    int fd = open(strLock.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) {
      const std::string strInfo = std::to_string(getpid()) + "\n";
      (void)!write(fd, strInfo.c_str(), strInfo.size());
      close(fd);
      return true;
    }
#endif
    /* The lock exists: break it when it is stale. */
    struct stat st = {};
    if (stat(strLock.c_str(), &st) == 0) {
      const time_t now = time(nullptr);
      if (now >= st.st_mtime &&
          (now - st.st_mtime) > kCacheLockStaleSec) {
        remove(strLock.c_str());
        continue;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  return false;
}

/** @brief Release the cache lock file. */
void ReleaseCacheLock(const std::string& strLock) {
  remove(strLock.c_str());
}

}  // namespace

void EmbedSetExePath(const std::string& exe_path) { g_exe_path = exe_path; }

std::string EmbedGetExePath() { return g_exe_path; }

bool ExtractEmbeddedRuntime(std::string& home) {
  home.clear();
  if (g_exe_path.empty()) return false;

  size_t blob_size = 0;
  uint64_t hash = 0;
  if (!ReadFooter(g_exe_path, blob_size, hash)) return false;

  const std::string cache = TempCacheDir();
  if (cache.empty()) return false;

  /* Cache hit: reuse when the version marker matches and stdlib exists. */
  {
    uint64_t mh = 0;
    size_t ms = 0;
    if (ReadMarker(cache, mh, ms) && mh == hash && ms == blob_size &&
        (access((cache + "/python311.zip").c_str(), 0) == 0 ||
         access((cache + "/stdlib").c_str(), 0) == 0)) {
      home = cache;
      return true;
    }
  }

  /* Cross-process cache lock (issue R8): serialize extraction and prevent
   * half-built caches. */
  const std::string strLock = cache + ".lock";
  if (!AcquireCacheLock(strLock)) {
    return false;
  }

  /* Re-check the marker under the lock: another process may have completed
   * the extraction while we waited. */
  {
    uint64_t mh = 0;
    size_t ms = 0;
    if (ReadMarker(cache, mh, ms) && mh == hash && ms == blob_size &&
        (access((cache + "/python311.zip").c_str(), 0) == 0 ||
         access((cache + "/stdlib").c_str(), 0) == 0)) {
      ReleaseCacheLock(strLock);
      home = cache;
      return true;
    }
  }

  /* Read the blob. */
  std::ifstream in(g_exe_path, std::ios::binary);
  if (!in) {
    ReleaseCacheLock(strLock);
    return false;
  }
  in.seekg(0, std::ios::end);
  const std::streamoff len = in.tellg();
  in.seekg(len - (std::streamoff)kFooterSize - (std::streamoff)blob_size);
  std::vector<uint8_t> blob(blob_size);
  in.read(reinterpret_cast<char*>(blob.data()), blob_size);
  if (!in) {
    ReleaseCacheLock(strLock);
    return false;
  }
  if (Fnv1a64(blob.data(), blob.size()) != hash) {
    ReleaseCacheLock(strLock);
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
    ReleaseCacheLock(strLock);
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
    ReleaseCacheLock(strLock);
    return false;
  }
  WriteMarker(tmp_cache, hash, blob_size);

  /* Swap the temp cache into place. */
  std::filesystem::remove_all(cache, ec);
  std::filesystem::rename(tmp_cache, cache, ec);
  if (ec) {
    std::filesystem::remove_all(tmp_cache, ec);
    ReleaseCacheLock(strLock);
    return false;
  }
  ReleaseCacheLock(strLock);
  home = cache;
  return true;
}
