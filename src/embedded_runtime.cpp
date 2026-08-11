#include "embedded_runtime.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

const char kMagicHead[] = "BURSTARC";
const char kMagicTail[] = "BURSTEND";
const size_t kFooterSize = 32;

std::string g_exe_path;

uint64_t Fnv1a64(const uint8_t* data, size_t len) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (size_t i = 0; i < len; ++i) {
    h ^= data[i];
    h *= 0x100000001b3ULL;
  }
  return h;
}

/* 缓存根目录（临时目录下，被系统清理后下次自动重建） */
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

bool ReadFooter(const std::string& exe_path, size_t& blob_size, uint64_t& hash) {
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

  /* 缓存命中：版本标记一致且 stdlib 存在则直接复用 */
  {
    uint64_t mh = 0;
    size_t ms = 0;
    if (ReadMarker(cache, mh, ms) && mh == hash && ms == blob_size &&
        access((cache + "/stdlib").c_str(), 0) == 0) {
      home = cache;
      return true;
    }
  }

  /* 读取 blob */
  std::ifstream in(g_exe_path, std::ios::binary);
  if (!in) return false;
  in.seekg(0, std::ios::end);
  const std::streamoff len = in.tellg();
  in.seekg(len - (std::streamoff)kFooterSize - (std::streamoff)blob_size);
  std::vector<uint8_t> blob(blob_size);
  in.read(reinterpret_cast<char*>(blob.data()), blob_size);
  if (!in) return false;
  if (Fnv1a64(blob.data(), blob.size()) != hash) return false;  /* 损坏 */

  /* 重建缓存 */
  std::error_code ec;
  std::filesystem::remove_all(cache, ec);
  std::filesystem::create_directories(cache, ec);

  size_t pos = 0;
  const auto rd = [&](void* dst, size_t n) -> bool {
    if (pos + n > blob.size()) return false;
    std::memcpy(dst, blob.data() + pos, n);
    pos += n;
    return true;
  };

  uint32_t count = 0;
  if (!rd(&count, 4)) {
    std::filesystem::remove_all(cache, ec);
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
    /* 路径安全检查：仅允许相对路径，拒绝绝对路径与 .. 穿越 */
    if (rel.empty() || rel[0] == '/' || rel.find("..") != std::string::npos) {
      ok = false;
      break;
    }
    const std::string full = cache + "/" + rel;
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
    std::filesystem::remove_all(cache, ec);
    return false;
  }
  WriteMarker(cache, hash, blob_size);
  home = cache;
  return true;
}
