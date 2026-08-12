/**
 * @file Ccurl.cpp
 * @brief Multi-threaded chunked downloader C++ implementation based on
 *        libcurl (cross-platform: Windows / Linux x86_64 / Linux aarch64).
 *
 * @author ErnestAgel
 * @date 2026-08-06
 * @copyright Copyright (c) 2026 ErnestAgel
 * @license SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <iostream>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <fcntl.h>
#include <errno.h>
#include <cstring>
#include <future>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "Ccurl.h"
#include "curl/mprintf.h"
#include "curlmulti.h"
#include "threadpool.h"
#include "sha256.h"

#ifdef _WIN32
/**
 * @brief Convert a UTF-8 string to UTF-16 (Windows wide-character file
 *        paths).
 * @param s UTF-8 input.
 * @return UTF-16 output (empty input returns an empty string).
 */
static std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
  return w;
}
#endif

/** @brief Convert a libcurl return code to a boolean. */
#define CHECK_CURL(value) ((value) == CURLE_OK)

extern "C" {
/** @brief Print an error log (file, line and errno description). */
#define LOG_ERR(...)                                              \
  printf("[%s %d] Erro:%s", __FILE__, __LINE__, strerror(errno)); \
  printf(__VA_ARGS__);

/** @brief Print an info log (file and line). */
#define LOG_INFO(...)                    \
  printf("[%s %d]", __FILE__, __LINE__); \
  printf(__VA_ARGS__);
}

/**
 * @brief Append a line to download.log (thread-safe): logs timeouts,
 *        failures and completions.
 */
static void AppendLog(const char* fmt, ...) {
  static std::mutex log_mutex;
  std::lock_guard<std::mutex> guard(log_mutex);
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  char ts[32] = {0};
  time_t now = time(NULL);
  struct tm* tm_now = localtime(&now);
  if (tm_now != nullptr) {
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm_now);
  }
  FILE* f = fopen("download.log", "a");
  if (f != nullptr) {
    fprintf(f, "[%s] %s\n", ts, buf);
    fclose(f);
  }
}

extern "C" {

/**
 * @brief libcurl write callback: write data into the mapped memory at the
 *        chunk offset (with chunk-boundary checks).
 * @return Bytes actually written.
 */
size_t File_Write(char* ptr, size_t size, size_t memb, void* userdata) {
  st_EasyList* info = (st_EasyList*)userdata;
  /* Cancellation checkpoint: return non-CURL_WRITEFUNC_OK to abort. */
  if (info->cancel_flag != nullptr && info->cancel_flag->load()) {
    return 0;
  }
  const size_t total = size * memb;
  /* Strict 206 (issue R2): when a Range request was answered with 200, the
   * server sent the full body; discard it so it is never written at the
   * chunk offset (the transfer degrades to a single stream later). */
  if (info->use_range && !info->bGot206) {
    return total;
  }
  size_t n = total;
  int64_t end_pos = info->end + 1;
  if (info->offset < end_pos) {
    if (info->offset + (int64_t)n > end_pos) {
      n = (size_t)(end_pos - info->offset);  /* clamp to the chunk end */
    }
    memcpy((info->file_ptr + info->offset), ptr, n);
    info->offset += n;
  }
  /* Bytes beyond the chunk are discarded; return the original count to
   * avoid triggering CURLE_WRITE_ERROR. */
  return total;
}

/**
 * @brief libcurl write callback: discard all received data (Range probe).
 * @return Bytes received.
 */
size_t DummyWrite(char* ptr, size_t size, size_t memb, void* userdata) {
  (void)ptr;
  (void)userdata;
  return size * memb;
}

/** @brief Case-insensitive header name comparison
 *         ("Content-Range" matches "content-range:"). */
static bool HeaderNameIs(const char* line, const char* name) {
  size_t nl = strlen(name);
  for (size_t i = 0; i < nl; i++) {
    char a = line[i], b = name[i];
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    if (a != b) return false;
  }
  return line[nl] == ':';
}

/** @brief Strip trailing CR/LF/space/tab from a header value. */
static std::string TrimHeaderValue(const char* pszValue) {
  std::string strOut(pszValue);
  const size_t nStart = strOut.find_first_not_of(" \t");
  if (nStart == std::string::npos) {
    return "";
  }
  strOut = strOut.substr(nStart);
  while (!strOut.empty() &&
         ((strOut.back() == '\r') || (strOut.back() == '\n') ||
          (strOut.back() == ' ') || (strOut.back() == '\t'))) {
    strOut.pop_back();
  }
  return strOut;
}

/** @brief Per-probe header context shared by the probe callbacks. */
typedef struct tagProbeCtx {
  curl_off_t nTotal;        /**< Content-Range total size (-1 = unknown) */
  BOOL32 bAcceptRanges;     /**< HEAD: Accept-Ranges: bytes */
  BOOL32 bStatus206;        /**< Range GET: final status is 206 */
  std::string strEtag;      /**< ETag header value */
  std::string strLastModified; /**< Last-Modified header value */
} TProbeCtx;

/**
 * @brief HEAD probe header callback: captures Accept-Ranges, ETag and
 *        Last-Modified.
 */
static size_t HeadProbeHeader(char* buffer, size_t size, size_t nitems,
                              void* userdata) {
  TProbeCtx* ctx = (TProbeCtx*)userdata;
  const size_t n = size * nitems;
  if (n == 0) {
    return 0;
  }
  if (HeaderNameIs(buffer, "accept-ranges") &&
      strstr(buffer, "bytes") != nullptr) {
    ctx->bAcceptRanges = TRUE;
  } else if (HeaderNameIs(buffer, "etag")) {
    ctx->strEtag = TrimHeaderValue(buffer + 5);
  } else if (HeaderNameIs(buffer, "last-modified")) {
    ctx->strLastModified = TrimHeaderValue(buffer + 14);
  }
  return n;
}

/**
 * @brief Range GET probe header callback: aborts on a 2xx non-206 response
 *        (server ignored Range), captures Content-Range/ETag/Last-Modified.
 */
static size_t RangeProbeHeader(char* buffer, size_t size, size_t nitems,
                               void* userdata) {
  TProbeCtx* ctx = (TProbeCtx*)userdata;
  const size_t n = size * nitems;
  if (n == 0) {
    return 0;
  }
  if ((n >= 5) && (strncmp(buffer, "HTTP/", 5) == 0)) {
    /* Status line: abort when a 2xx response is not 206 (full body case).
     * Redirects (3xx) pass through so FOLLOWLOCATION keeps working. */
    const char* p = strchr(buffer, ' ');
    if (p != nullptr) {
      while ((*p != '\0') && ((*p < '0') || (*p > '9'))) {
        ++p;
      }
      const long code = atol(p);
      if ((code >= 200) && (code <= 299) && (code != 206)) {
        return 0;  /* abort the transfer */
      }
      if (code == 206) {
        ctx->bStatus206 = TRUE;
      }
    }
    return n;
  }
  if (HeaderNameIs(buffer, "content-range")) {
    const char* slash = strrchr(buffer, '/');
    if (slash != nullptr && slash[1] != '\0' && slash[1] != '*') {
      ctx->nTotal = (curl_off_t)strtoll(slash + 1, nullptr, 10);
    }
  } else if (HeaderNameIs(buffer, "etag")) {
    ctx->strEtag = TrimHeaderValue(buffer + 5);
  } else if (HeaderNameIs(buffer, "last-modified")) {
    ctx->strLastModified = TrimHeaderValue(buffer + 14);
  }
  return n;
}

/**
 * @brief Chunk header callback: records whether the final response is 206
 *        and aborts a Range request answered with a 2xx non-206 body.
 */
static size_t ChunkStatusHeader(char* buffer, size_t size, size_t nitems,
                                void* userdata) {
  st_EasyList* info = (st_EasyList*)userdata;
  const size_t n = size * nitems;
  if ((n >= 5) && (strncmp(buffer, "HTTP/", 5) == 0)) {
    const char* p = strchr(buffer, ' ');
    if (p != nullptr) {
      while ((*p != '\0') && ((*p < '0') || (*p > '9'))) {
        ++p;
      }
      const long code = atol(p);
      info->bGot206 = (code == 206);
      if (info->use_range && (code >= 200) && (code <= 299) &&
          (code != 206)) {
        return 0;  /* abort: Range answered with the full body */
      }
    }
  }
  return n;
}

/**
 * @brief libcurl progress trampoline: delegates to the owning instance
 *        (issue R7 - no process-global progress state).
 * @return 0 to continue, 1 to abort (cancel).
 */
size_t progressFunc(void* userdata,
                    double totalDownload,
                    double nowDownload,
                    double totalUpload,
                    double nowUpload) {
  (void)totalUpload;
  (void)nowUpload;
  st_EasyList* info = (st_EasyList*)userdata;
  if (info->owner == nullptr) {
    return 0;
  }
  return (size_t)info->owner->ProgressCallback(info, totalDownload,
                                               nowDownload);
}
}  // extern "C"

size_t Ccurl::ProgressCallback(st_EasyList* pInfo, double dTotalDownload,
                               double dNowDownload) {
  m_progressMutex.lock();
  pInfo->download_len = dNowDownload;

  /* Cancellation checkpoint: return non-zero to abort the transfer. */
  if (pInfo->cancel_flag != nullptr && pInfo->cancel_flag->load()) {
    m_progressMutex.unlock();
    return 1;
  }

  /* Periodic dirty-page flush (crash consistency, issue R4). */
  const auto nowFlush = std::chrono::steady_clock::now();
  if (std::chrono::duration_cast<std::chrono::seconds>(
          nowFlush - m_lastFlushTime)
          .count() >= 10) {
    m_lastFlushTime = nowFlush;
    FlushMapping();
  }

  int percent = 0;
  double allDownload = m_dResumeLen;  /* resume base counts as downloaded */
  if (dTotalDownload > 0) {
    for (int i = 0; i < MaxThread + 1; i++) {
      if (m_pInfoTable[i] != nullptr) {
        allDownload += m_pInfoTable[i]->download_len;
      }
    }
    percent = (int)(allDownload / m_dFileLen * 100);
  }

  /* ---- GUI mode: ~200ms throttled callback ---- */
  if (pInfo->on_progress != nullptr && *pInfo->on_progress) {
    const auto nowCb = std::chrono::steady_clock::now();
    const long long nMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            nowCb - m_lastCbTime)
            .count();
    if (nMs >= 200) {
      m_lastCbTime = nowCb;
      time_t now = time(NULL);
      double speed = 0;
      if (m_tLast > 0 && now > m_tLast) {
        speed = (allDownload - m_dLastTotal) / (now - m_tLast);
      }
      m_dLastTotal = allDownload;
      m_tLast = now;

      std::vector<ThreadProgress> tp;
      tp.reserve(MaxThread);
      for (int i = 0; i < MaxThread + 1; i++) {
        st_EasyList* p = m_pInfoTable[i];
        if (p == nullptr) {
          continue;
        }
        ThreadProgress t;
        t.id = i;
        /* In-file absolute positions: downloaded = chunk start + chunk bytes
         * (resume base included), percent = position / file total. */
        t.file_start = (long long)p->part_start;
        t.downloaded = (long long)p->part_start + (long long)p->download_len;
        t.total = (long long)p->end + 1;
        t.file_total = (long long)m_dFileLen;
        t.percent = (m_dFileLen > 0)
                        ? (t.downloaded / (double)m_dFileLen * 100.0)
                        : 0.0;
        /* Per-thread speed: increment inside the throttle window. */
        time_t dt = (p->last_t > 0) ? (now - p->last_t) : 0;
        if (dt > 0 && p->download_len >= p->last_len) {
          t.speed = (p->download_len - p->last_len) / (double)dt;
        } else {
          t.speed = 0;
        }
        p->last_len = p->download_len;
        p->last_t = now;
        tp.push_back(t);
      }
      (*pInfo->on_progress)(tp, (double)percent, speed);
    }
    m_progressMutex.unlock();
    return 0;
  }

  /* ---- CLI mode: keep the original 1% gate printing ---- */
  if (percent >= m_nPrint) {
    time_t now = time(NULL);
    double speed = 0;
    if (m_tLast > 0 && now > m_tLast) {
      speed = (allDownload - m_dLastTotal) / (now - m_tLast);
    }
    m_dLastTotal = allDownload;
    m_tLast = now;
    if (speed > 0 && m_dFileLen > allDownload) {
      double remain_sec = (m_dFileLen - allDownload) / speed;
      int h = (int)(remain_sec / 3600);
      int m = (int)(remain_sec / 60) % 60;
      int s = (int)remain_sec % 60;
      LOG_INFO("percent: %d%% speed: %.2f MB/s ETA: %02d:%02d:%02d\n", percent,
               speed / (1024.0 * 1024.0), h, m, s);
    } else {
      LOG_INFO("percent: %d%%\n", percent);
    }
    m_nPrint = percent + 1;
  }
  m_progressMutex.unlock();

  return 0;
}

Ccurl::Ccurl() {
  for (int i = 0; i <= MaxThread; i++) {
    m_Easy_List[i] = nullptr;
  }
  m_lastFlushTime = std::chrono::steady_clock::now();
  curl_version_info_data* ver = curl_version_info(CURLVERSION_NOW);
  LOG_INFO("libcurl version %u.%u.%u\n", (ver->version_num >> 16) & 0xff,
           (ver->version_num >> 8) & 0xff, ver->version_num & 0xff);
}

Ccurl::~Ccurl() {
  this->Destory();
}

bool Ccurl::Init(const string url, string filename, int thread_num,
                 int timeout) {
  unique_lock<mutex> lock(m_lock);
  m_url = url;
  m_filename = filename;
  m_thread_num =
      thread_num < 1 ? 1 : (thread_num > MaxThread ? MaxThread : thread_num);
  m_timeout = timeout < 0 ? 0 : timeout;
  m_cancel_flag.store(false);   /* reset per task (instance reuse) */
  m_range_denied.store(false);
  m_remote_etag.clear();
  m_remote_last_modified.clear();
  m_nPrint = 1;                 /* reset the CLI 1% progress printing */
  m_dLastTotal = 0;
  m_tLast = 0;
  LOG_INFO(">>>>>\n");
  /* Order optimization (faster "Resume"): probe the size with HEAD first
   * (capturing Accept-Ranges inside), and only run the standalone Range
   * probe when HEAD did not confirm Range support. */
  if (!this->get_Download_FileSize()) {
    return false;
  }
  if (!m_range_known) {
    bool range_ok = this->Check_Range_Support();
    LOG_INFO("HTTP Range support: %s\n",
             range_ok ? "yes" : "no (fallback to single stream)");
  } else {
    LOG_INFO("HTTP Range support: %s (from HEAD Accept-Ranges)\n",
             m_range_supported ? "yes" : "no");
  }
  return this->File_Init(filename.c_str());
}

void Ccurl::Cancel() {
  m_cancel_flag.store(true);
}

bool Ccurl::IsCanceled() const {
  return m_cancel_flag.load();
}

std::string Ccurl::LastError() const {
  return m_last_error;
}

void Ccurl::SetReferer(const string& referer) {
  m_referer = referer;
}

void Ccurl::SetCookie(const string& cookie) {
  m_cookie = cookie;
}

void Ccurl::SetChunkPool(CThreadPool* pPool) {
  m_pChunkPool = pPool;
}

void Ccurl::SetChunkEngine(CCurlMultiEngine* pEngine) {
  m_pChunkEngine = pEngine;
}

void Ccurl::SetExternalCancel(std::atomic<bool>* pFlag) {
  m_pExternalCancel = pFlag;
}

void* Ccurl::Downloading(void* arg) {
  st_EasyList* info = (st_EasyList*)arg;
  const int max_retry = 3;
  const int64_t base_offset = info->offset;

  if (info->use_range) {
#ifdef _WIN32
    LOG_INFO("threadid: %p, download from: %lld to: %lld\n", info->thid,
             (long long)info->offset, (long long)info->end);
#else
    LOG_INFO("threadid: %ld, download from: %lld to: %lld\n", (long)info->thid,
             (long long)info->offset, (long long)info->end);
#endif
  } else {
#ifdef _WIN32
    LOG_INFO("threadid: %p, download whole range from: %lld\n", info->thid,
             (long long)info->offset);
#else
    LOG_INFO("threadid: %ld, download whole range from: %lld\n",
             (long)info->thid, (long long)info->offset);
#endif
  }

  for (int attempt = 0; attempt < max_retry; attempt++) {
    /* Cancellation checkpoint: no retry after cancel. */
    if (info->cancel_flag != nullptr && info->cancel_flag->load()) {
      info->success = false;
      return nullptr;
    }
    /* Issue O1: the chunk may already be complete from a partial attempt. */
    if (info->offset > info->end) {
      info->success = true;
      return nullptr;
    }
    /* Resume from the current written offset (issue O1), not the base. */
    char range[64] = {0};
    if (info->use_range) {
      snprintf(range, sizeof(range), "%lld-%lld",
               (long long)info->offset, (long long)info->end);
    }
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
      LOG_ERR("curl Easy init failed\n");
      break;
    }
    ConfigureEasyHandle(curl, info, info->use_range ? range : nullptr);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    /* Cancellation checkpoint: cancel-aborted transfers are not failures. */
    if (info->cancel_flag != nullptr && info->cancel_flag->load()) {
      info->success = false;
      return nullptr;
    }
    const bool b2xx = (http_code >= 200) && (http_code <= 299);
    if (info->use_range && b2xx && (http_code != 206)) {
      /* Server ignored Range (200): degrade to a single full-body stream. */
      if (info->range_denied != nullptr) {
        info->range_denied->store(true);
      }
      LOG_ERR("server ignored Range (HTTP %ld) on part %lld-%lld, "
              "restarting single-stream (url=%s)\n",
              http_code, (long long)info->offset, (long long)info->end,
              info->url);
      AppendLog("[ERROR] server ignored Range (HTTP %ld) on part %lld-%lld, "
                "restarting single-stream (url=%s)",
                http_code, (long long)info->offset, (long long)info->end,
                info->url);
      info->success = false;
      return nullptr;
    }
    if (b2xx && CHECK_CURL(res) && info->offset >= info->end + 1) {
      info->success = true;  /* chunk complete and full */
      return nullptr;
    }
    if (res == CURLE_OPERATION_TIMEDOUT) {
      LOG_ERR("part %lld-%lld timeout (no progress for %ld s)\n",
              (long long)info->offset, (long long)info->end, info->timeout);
      AppendLog("[WARN] timeout on part %lld-%lld (url=%s, no progress for "
                "%ld s)",
                (long long)info->offset, (long long)info->end, info->url,
                info->timeout);
      info->success = false;
      return nullptr;
    }
    if (!b2xx) {
      /* HTTP error (403/404 etc.): fail without retrying. */
      LOG_ERR("HTTP %ld on part %lld-%lld (url=%s)\n", http_code,
              (long long)info->offset, (long long)info->end, info->url);
      AppendLog("[ERROR] HTTP %ld on part %lld-%lld (url=%s)", http_code,
                (long long)info->offset, (long long)info->end, info->url);
      info->success = false;
      return nullptr;
    }
    LOG_ERR("res:%s, retry %d/%d from offset %lld\n",
            curl_easy_strerror(res), attempt + 1, max_retry,
            (long long)info->offset);
#ifdef _WIN32
    Sleep(1000);  /* wait 1s before the retry */
#else
    sleep(1);
#endif
  }
  info->success = false;
  AppendLog("[ERROR] part %lld-%lld failed after %d retries (url=%s)",
            (long long)base_offset, (long long)info->end, max_retry,
            info->url);

  return nullptr;
}

void Ccurl::ConfigureEasyHandle(CURL* curl, st_EasyList* pInfo,
                                const char* pszRange) {
  curl_easy_setopt(curl, CURLOPT_URL, pInfo->url);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT,
                   "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                   "AppleWebKit/537.36 (KHTML, like Gecko) "
                   "Chrome/115.0.0.0 Safari/537.36");
  if (pInfo->referer != nullptr && pInfo->referer[0] != '\0') {
    curl_easy_setopt(curl, CURLOPT_REFERER, pInfo->referer);
  }
  if (pInfo->cookie != nullptr && pInfo->cookie[0] != '\0') {
    curl_easy_setopt(curl, CURLOPT_COOKIE, pInfo->cookie);
  }
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, File_Write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, pInfo);
  /* Strict 206: track/abort a Range request answered with 200. */
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, ChunkStatusHeader);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, pInfo);
  /* P8-4: a chunk is a one-shot transfer.  Forbid connection reuse so the
   * connection is closed when the chunk completes instead of lingering in
   * the multi handle's keep-alive cache; otherwise a single-threaded
   * keep-alive server blocks on the idle connection and stalls every other
   * chunk (observed on CI).  This matches the legacy per-attempt easy
   * handle semantics, where curl_easy_cleanup closed the connection. */
  curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, progressFunc);
  curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, pInfo);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  if (pszRange != nullptr && pszRange[0] != '\0') {
    curl_easy_setopt(curl, CURLOPT_RANGE, pszRange);
  }
  if (pInfo->timeout > 0) {
    /* Low-speed timeout: abort after timeout seconds without progress. */
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, pInfo->timeout);
  }
}

/* ---- P8-4: curl_multi non-blocking chunk execution ---- */

bool Ccurl::RunChunksMulti() {
  for (int i = 0; i < m_thread_num; i++) {
    if (m_Easy_List[i]->success) {
      continue;  /* chunk-level resume: already full, skip */
    }
    st_EasyList* pInfo = m_Easy_List[i];
    pInfo->attempt = 0;
    pInfo->dwLane = kChunkLaneUnset;
    m_nChunksLeft.fetch_add(1);
    SubmitChunkAttempt(pInfo, FALSE);
  }

  std::unique_lock<std::mutex> lock(m_waitMutex);
  m_waitCv.wait(lock, [this]() { return m_nChunksLeft.load() == 0; });
  lock.unlock();

  bool all_ok = true;
  for (int i = 0; i < m_thread_num; i++) {
    if (!m_Easy_List[i]->success) {
      all_ok = false;
    }
  }
  return all_ok;
}

void Ccurl::SubmitChunkAttempt(st_EasyList* pInfo, BOOL32 bDelayed) {
  if (m_pChunkEngine == nullptr) {
    FinishChunk(pInfo, FALSE);
    return;
  }
  /* Cancellation checkpoint: no new attempt after cancel. */
  if (pInfo->cancel_flag != nullptr && pInfo->cancel_flag->load()) {
    FinishChunk(pInfo, FALSE);
    return;
  }
  /* Issue O1: the chunk may already be complete from a partial attempt. */
  if (pInfo->offset > pInfo->end) {
    FinishChunk(pInfo, TRUE);
    return;
  }

  /* Resume from the current written offset (issue O1), not the base. */
  char range[64] = {0};
  if (pInfo->use_range) {
    snprintf(range, sizeof(range), "%lld-%lld", (long long)pInfo->offset,
             (long long)pInfo->end);
  }

  TChunkJob tJob;
  tJob.pUserData = pInfo;
  tJob.dwLane = pInfo->dwLane;
  /* The engine polls the task-level cancel flag directly (P8-4) so a
   * canceled task is removed within one driver cycle even when a stalled
   * transfer never fires the progress callback that sets m_cancel_flag. */
  tJob.pCancelFlag = (m_pExternalCancel != nullptr) ? m_pExternalCancel
                                                    : &m_cancel_flag;
  const std::string strRange = pInfo->use_range ? range : "";
  tJob.fnCreateEasy = [this, pInfo, strRange]() -> CURL* {
    if (pInfo->cancel_flag != nullptr && pInfo->cancel_flag->load()) {
      return nullptr;
    }
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
      return nullptr;
    }
    pInfo->bGot206 = FALSE;  /* reset per attempt */
    ConfigureEasyHandle(curl, pInfo,
                        strRange.empty() ? nullptr : strRange.c_str());
    return curl;
  };
  tJob.fnDone = [this, pInfo](CURLcode res, long lHttpCode) {
    OnChunkDone(pInfo, res, lHttpCode);
  };

  const BOOL32 bOk = (bDelayed != FALSE)
                         ? m_pChunkEngine->SubmitChunkDelayed(tJob, 1000)
                         : m_pChunkEngine->SubmitChunk(tJob);
  pInfo->dwLane = tJob.dwLane;  /* remember the lane for retries */
  if (bOk == FALSE) {
    FinishChunk(pInfo, FALSE);
  }
}

void Ccurl::OnChunkDone(st_EasyList* pInfo, CURLcode res, long lHttpCode) {
  /* Cancellation checkpoint: cancel-aborted transfers are not failures. */
  if (pInfo->cancel_flag != nullptr && pInfo->cancel_flag->load()) {
    FinishChunk(pInfo, FALSE);
    return;
  }
  if (res == CURLE_ABORTED_BY_CALLBACK) {
    /* Canceled before start or easy-handle init failed. */
    FinishChunk(pInfo, FALSE);
    return;
  }

  const bool b2xx = (lHttpCode >= 200) && (lHttpCode <= 299);
  if (pInfo->use_range && b2xx && (lHttpCode != 206)) {
    /* Server ignored Range (200): degrade to a single full-body stream. */
    if (pInfo->range_denied != nullptr) {
      pInfo->range_denied->store(true);
    }
    LOG_ERR("server ignored Range (HTTP %ld) on part %lld-%lld, "
            "restarting single-stream (url=%s)\n",
            lHttpCode, (long long)pInfo->offset, (long long)pInfo->end,
            pInfo->url);
    AppendLog("[ERROR] server ignored Range (HTTP %ld) on part %lld-%lld, "
              "restarting single-stream (url=%s)",
              lHttpCode, (long long)pInfo->offset, (long long)pInfo->end,
              pInfo->url);
    FinishChunk(pInfo, FALSE);
    return;
  }
  if (b2xx && CHECK_CURL(res) && pInfo->offset >= pInfo->end + 1) {
    FinishChunk(pInfo, TRUE);  /* chunk complete and full */
    return;
  }
  if (res == CURLE_OPERATION_TIMEDOUT) {
    LOG_ERR("part %lld-%lld timeout (no progress for %ld s)\n",
            (long long)pInfo->offset, (long long)pInfo->end, pInfo->timeout);
    AppendLog("[WARN] timeout on part %lld-%lld (url=%s, no progress for "
              "%ld s)",
              (long long)pInfo->offset, (long long)pInfo->end, pInfo->url,
              pInfo->timeout);
    FinishChunk(pInfo, FALSE);
    return;
  }
  if (!b2xx) {
    /* HTTP error (403/404 etc.): fail without retrying. */
    LOG_ERR("HTTP %ld on part %lld-%lld (url=%s)\n", lHttpCode,
            (long long)pInfo->offset, (long long)pInfo->end, pInfo->url);
    AppendLog("[ERROR] HTTP %ld on part %lld-%lld (url=%s)", lHttpCode,
              (long long)pInfo->offset, (long long)pInfo->end, pInfo->url);
    FinishChunk(pInfo, FALSE);
    return;
  }

  /* Transient failure: retry with a 1s backoff (max 3 attempts total). */
  if (pInfo->attempt < 2) {
    LOG_ERR("res:%s, retry %d/3 from offset %lld\n",
            curl_easy_strerror(res), pInfo->attempt + 1,
            (long long)pInfo->offset);
    pInfo->attempt++;
    SubmitChunkAttempt(pInfo, TRUE);
    return;
  }
  AppendLog("[ERROR] part %lld-%lld failed after 3 retries (url=%s)",
            (long long)pInfo->part_start, (long long)pInfo->end, pInfo->url);
  FinishChunk(pInfo, FALSE);
}

void Ccurl::FinishChunk(st_EasyList* pInfo, BOOL32 bOk) {
  pInfo->success = (bOk != FALSE);
  std::lock_guard<std::mutex> lock(m_waitMutex);
  const int nLeft = m_nChunksLeft.fetch_sub(1) - 1;
  if (nLeft <= 0) {
    m_waitCv.notify_all();
  }
}

bool Ccurl::RunChunks() {
  bool all_ok = true;

  /* P8-4: with the shared curl_multi engine, chunk transfers run
   * non-blockingly on the engine's driver threads; each task keeps its
   * full chunk count in flight and tasks share the fixed lane count. */
  if (m_pChunkEngine != nullptr) {
    return RunChunksMulti();
  }

  /* P8: with a shared download pool, each chunk is one pool job; chunks
   * queue when the pool is saturated, and no per-chunk threads are created.
   * The caller must not submit-and-wait on the same pool thread. */
  if (m_pChunkPool != nullptr) {
    std::vector<std::future<void>> vecFutures;
    vecFutures.reserve(static_cast<size_t>(m_thread_num));
    for (int i = 0; i < m_thread_num; i++) {
      if (m_Easy_List[i]->success) {
        continue;  /* chunk-level resume: already full, skip */
      }
      std::future<void> fJob;
      if (!m_pChunkPool->Submit(
              [this, i]() { Downloading(m_Easy_List[i]); }, &fJob)) {
        LOG_ERR("chunk pool submit failed for chunk %d\n", i);
        m_Easy_List[i]->success = false;
        continue;
      }
      m_Easy_List[i]->thread_created = true;
      vecFutures.push_back(std::move(fJob));
    }
    for (std::future<void>& fJob : vecFutures) {
      fJob.wait();
    }
    for (int i = 0; i < m_thread_num; i++) {
      if (!m_Easy_List[i]->success) {
        all_ok = false;
      }
    }
    return all_ok;
  }

  for (int i = 0; i < m_thread_num; i++) {
    if (m_Easy_List[i]->success) {
      continue;  /* chunk-level resume: already full, skip */
    }
#ifdef _WIN32
    m_Easy_List[i]->thid =
        CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)&Downloading,
                     (LPVOID)m_Easy_List[i], 0, NULL);
    if (m_Easy_List[i]->thid != NULL) {
      m_Easy_List[i]->thread_created = true;
    } else {
      LOG_ERR("CreateThread failed for thread %d\n", i);
      m_Easy_List[i]->success = false;
    }
#else
    if (pthread_create(&(m_Easy_List[i]->thid), NULL, &Downloading,
                       (void*)m_Easy_List[i]) == 0) {
      m_Easy_List[i]->thread_created = true;
    } else {
      LOG_ERR("pthread_create failed for thread %d\n", i);
      m_Easy_List[i]->success = false;
    }
#endif
  }

  for (int i = 0; i < m_thread_num; i++) {
    if (m_Easy_List[i]->thread_created) {
#ifdef _WIN32
      WaitForSingleObject(m_Easy_List[i]->thid, INFINITE);
      CloseHandle(m_Easy_List[i]->thid);
#else
      pthread_join(m_Easy_List[i]->thid, NULL);
#endif
    }
    if (!m_Easy_List[i]->success) {
      all_ok = false;
    }
  }
  return all_ok;
}

bool Ccurl::VerifyAllPartsWritten() const {
  long long nTotal = 0;
  for (int i = 0; i < m_thread_num; i++) {
    if (m_Easy_List[i] == nullptr) {
      return false;
    }
    nTotal += (long long)(m_Easy_List[i]->offset -
                          m_Easy_List[i]->part_start);
  }
  return nTotal == (long long)m_fileLen;
}

bool Ccurl::Download_Task() {
  unique_lock<mutex> lock(m_lock);

  if (m_Easy_List[0] == nullptr) {
    return true;  /* no chunks (file already complete) */
  }
  /* Cancellation checkpoint: canceled before the transfer starts. */
  if (m_cancel_flag.load()) {
    return false;
  }
  bool all_ok = RunChunks();

  /* Strict 206 (issue R2): when the server ignored Range, restart once as a
   * single full-body stream (discard resume meta and all partial data). */
  if (!all_ok && m_range_denied.load()) {
    AppendLog("[WARN] server ignored Range, restarting as single stream: "
              "url=%s",
              m_url.c_str());
    m_range_supported = false;
    m_range_denied.store(false);
    m_part_written.clear();
    ClearPartMeta();
    DestoryUnlocked();
    m_thread_num = 1;
    if (!File_Init(m_filename.c_str())) {
      return false;
    }
    all_ok = RunChunks();
  }

  /* Cancellation checkpoint: distinguish cancel from failure. */
  if (m_cancel_flag.load()) {
    AppendLog("[INFO] download canceled: url=%s", m_url.c_str());
    SavePartMeta();  /* cancel (pause) also saves chunk progress */
    return false;
  }
  if (all_ok) {
    /* Issue O4: verify every part actually wrote its full range. */
    if (!VerifyAllPartsWritten()) {
      AppendLog("[ERROR] download size verification failed, parts incomplete "
                "(url=%s)",
                m_url.c_str());
      SavePartMeta();
      return false;
    }
    AppendLog("[INFO] download complete: url=%s, size verified", m_url.c_str());
    ClearPartMeta();
  } else {
    AppendLog("[ERROR] download task failed: some parts not completed "
              "(url=%s)",
              m_url.c_str());
    SavePartMeta();
  }
  return all_ok;
}

/* ---- Chunk-level resume metadata (<target>.curlbolt.part) ---- */

std::string Ccurl::MetaPath() const {
  return m_filename + ".curlbolt.part";
}

std::vector<ThreadProgress> Ccurl::SnapshotParts() const {
  std::vector<ThreadProgress> out;
  for (int i = 0; i < m_thread_num && m_Easy_List[i] != nullptr; i++) {
    ThreadProgress t;
    t.id = i;
    t.file_start = (long long)m_Easy_List[i]->part_start;
    t.downloaded =
        (long long)m_Easy_List[i]->part_start +
        (long long)m_Easy_List[i]->download_len; /* resume base included */
    t.total = (long long)m_Easy_List[i]->end + 1;
    t.file_total = (long long)m_fileLen;
    t.percent =
        (m_fileLen > 0) ? (t.downloaded / (double)m_fileLen * 100.0) : 0.0;
    t.speed = 0;
    out.push_back(t);
  }
  return out;
}

/**
 * @brief Read the resume meta (v2 with schema/etag/last_modified).
 *
 * Format:
 *   schema=2
 *   filelen=<total>
 *   etag=<...>
 *   last_modified=<...>
 *   part=<start>,<written>
 *   ...
 *
 * @return emMetaOk when usable and remote integrity matches, emMetaChanged
 *         when the remote content changed or the meta is legacy v1
 *         (conservative full restart), emMetaNone otherwise.
 */
Ccurl::TMetaResult Ccurl::LoadPartMeta() {
  m_part_written.clear();
  const std::string path = MetaPath();
#ifdef _WIN32
  FILE* f = _wfopen(Utf8ToWide(path).c_str(), L"rb");
#else
  FILE* f = fopen(path.c_str(), "rb");
#endif
  if (f == nullptr) {
    return emMetaNone;
  }
  long long filelen = -1;
  bool bSchemaV2 = false;
  std::string strEtag;
  std::string strLastModified;
  char line[256];
  while (fgets(line, sizeof(line), f) != nullptr) {
    if (strncmp(line, "schema=", 7) == 0) {
      bSchemaV2 = (atoll(line + 7) == 2);
    } else if (strncmp(line, "filelen=", 8) == 0) {
      filelen = atoll(line + 8);
    } else if (strncmp(line, "etag=", 5) == 0) {
      strEtag = TrimHeaderValue(line + 5);
    } else if (strncmp(line, "last_modified=", 14) == 0) {
      strLastModified = TrimHeaderValue(line + 14);
    } else {
      char* comma = strchr(line, ',');
      if (comma != nullptr) {
        long long written = atoll(comma + 1);
        m_part_written.push_back(written < 0 ? 0 : written);
      }
    }
  }
  fclose(f);
  /* The file size must match the current task (target changed -> discard). */
  if ((filelen != (long long)m_fileLen) || m_part_written.empty()) {
    m_part_written.clear();
    return emMetaNone;
  }
  /* Legacy v1 meta (no schema): conservative full restart once (upgrades to
   * v2 on the next save). */
  if (!bSchemaV2) {
    m_part_written.clear();
    return emMetaChanged;
  }
  /* Remote integrity (issue R3): compare ETag first, then Last-Modified. */
  if (!strEtag.empty() && !m_remote_etag.empty() &&
      (strEtag != m_remote_etag)) {
    m_part_written.clear();
    return emMetaChanged;
  }
  if (strEtag.empty() && !m_remote_etag.empty()) {
    /* The meta has no ETag but the server now provides one: cannot verify
     * the old data, restart conservatively. */
    m_part_written.clear();
    return emMetaChanged;
  }
  if (strEtag.empty() && m_remote_etag.empty() &&
      !strLastModified.empty() && !m_remote_last_modified.empty() &&
      (strLastModified != m_remote_last_modified)) {
    m_part_written.clear();
    return emMetaChanged;
  }
  return emMetaOk;
}

/**
 * @brief Flush dirty mapped pages to disk (issue R4).  No locking: called
 *        from transfer threads or under the lifecycle lock.
 */
void Ccurl::FlushMapping() {
#ifdef _WIN32
  if (m_pTrunck != nullptr) {
    FlushViewOfFile(m_pTrunck, 0);
  }
#else
  if (m_pTrunck != nullptr) {
    msync(m_pTrunck, (size_t)m_fileLen, MS_SYNC);
  }
#endif
}

void Ccurl::SavePartMeta() {
  FlushMapping();  /* issue R4: flush before recording progress */
  const std::string path = MetaPath();
#ifdef _WIN32
  FILE* f = _wfopen(Utf8ToWide(path).c_str(), L"wb");
#else
  FILE* f = fopen(path.c_str(), "wb");
#endif
  if (f == nullptr) {
    return;
  }
  fprintf(f, "schema=2\n");
  fprintf(f, "filelen=%lld\n", (long long)m_fileLen);
  fprintf(f, "etag=%s\n", m_remote_etag.c_str());
  fprintf(f, "last_modified=%s\n", m_remote_last_modified.c_str());
  for (int i = 0; i < m_thread_num; i++) {
    if (m_Easy_List[i] != nullptr) {
      /* Written bytes = write-callback position - chunk start (resume base
       * included). */
      long long written =
          (long long)(m_Easy_List[i]->offset - m_Easy_List[i]->part_start);
      if (written < 0) written = 0;
      fprintf(f, "%lld,%lld\n", (long long)m_Easy_List[i]->part_start,
              written);
    }
  }
  fclose(f);
}

void Ccurl::ClearPartMeta() {
  const std::string path = MetaPath();
#ifdef _WIN32
  _wremove(Utf8ToWide(path).c_str());
#else
  remove(path.c_str());
#endif
}

bool Ccurl::File_Init(const char* filename) {
  LOG_INFO(">>>>>\n");
  if (m_cancel_flag.load()) {
    return false;
  }

  /* The file size was probed by Init -> get_Download_FileSize(). */
  if (m_fileLen <= 0) {
    LOG_ERR("invalid file length: %lld\n", (long long)m_fileLen);
    m_last_error = "server returned no valid file size (the link may not be "
                   "a direct download, or the server does not support HEAD)";
    return false;
  }

  /* Chunk-level resume: the trusted source is the .curlbolt.part meta.
   * mmap preallocates the file to m_fileLen, so a stat check would wrongly
   * report "complete"; without meta we conservatively redownload. */
  m_resume_len = 0;
  m_part_written.clear();
  TMetaResult metaRes = emMetaNone;
  bool have_part_meta = false;
  bool bRemoteChanged = false;
#ifdef _WIN32
  std::wstring wfile = Utf8ToWide(filename);  /* UTF-8 -> UTF-16 */
  struct _stat64 st = {};
  if (_wstat64(wfile.c_str(), &st) == 0 && (st.st_mode & _S_IFREG) &&
      st.st_size > 0) {
    metaRes = LoadPartMeta();
  }
#else
  struct stat st = {};
  if (stat(filename, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
    metaRes = LoadPartMeta();
  }
#endif
  if (metaRes == emMetaOk) {
    have_part_meta = true;
    LOG_INFO("resume meta found, %d parts\n", (int)m_part_written.size());
  } else if (metaRes == emMetaChanged) {
    /* Issue R3: the remote content changed or the meta is legacy v1;
     * discard it and restart the full download. */
    bRemoteChanged = true;
    LOG_INFO("resume meta invalid/remote changed, restarting full download\n");
    m_part_written.clear();
    ClearPartMeta();
  } else if (st.st_size > 0) {
    LOG_INFO("existing file without resume meta (%lld bytes), redownload\n",
             (long long)st.st_size);
  }

  /* Server without Range support: single stream, no chunk resume. */
  int open_flags = O_RDWR | O_CREAT;
  if (!m_range_supported) {
    m_thread_num = 1;
    if (have_part_meta) {
      LOG_INFO("server does not support Range, discard resume meta and "
               "restart\n");
      m_part_written.clear();
      have_part_meta = false;
      ClearPartMeta();
      open_flags |= O_TRUNC;
    }
  }

  /* Thread count based on the whole file (min chunk 1MB; the chunk layout is
   * fixed and resume-independent). */
  {
    const int64_t kMinPartSize = 1 << 20; /* 1MB */
    int max_threads_by_size = (int)(m_fileLen / kMinPartSize);
    if (max_threads_by_size < 1) {
      max_threads_by_size = 1;
    }
    if (m_thread_num > max_threads_by_size) {
      m_thread_num = max_threads_by_size;
    }
  }
  /* The meta chunk count must match the current layout (threads/layout
   * changed -> discard). */
  if (have_part_meta && (int)m_part_written.size() != m_thread_num) {
    LOG_INFO("resume meta part count mismatch (%d vs %d), discard\n",
             (int)m_part_written.size(), m_thread_num);
    m_part_written.clear();
    have_part_meta = false;
    ClearPartMeta();
  }
  /* Resume base (progress accounting): sum of the written bytes. */
  if (have_part_meta) {
    for (int64_t w : m_part_written) {
      m_resume_len += w;
    }
    LOG_INFO("resume from part meta: %lld bytes done, %d parts\n",
             (long long)m_resume_len, m_thread_num);
  }

#ifdef _WIN32
  DWORD dwCreation = (open_flags & O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS;
  m_hFile = CreateFileW(wfile.c_str(), GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE |
                            FILE_SHARE_DELETE,
                        NULL,
                        dwCreation, FILE_ATTRIBUTE_NORMAL, NULL);
  if (m_hFile == INVALID_HANDLE_VALUE) {
    LOG_ERR("CreateFile:%s failed\n", filename);
    m_last_error = "failed to create the local file (save path missing or "
                   "not writable)";
    return false;
  }
  /* Extend the file to the target size and write the last byte so the
   * mapping covers the whole file. */
  LARGE_INTEGER li;
  li.QuadPart = m_fileLen - 1;
  if (!SetFilePointerEx(m_hFile, li, NULL, FILE_BEGIN)) {
    LOG_ERR("SetFilePointer failed\n");
    CloseHandle(m_hFile);
    m_hFile = INVALID_HANDLE_VALUE;
    return false;
  }
  DWORD written = 0;
  if (!WriteFile(m_hFile, "", 1, &written, NULL)) {
    LOG_ERR("WriteFile failed\n");
    CloseHandle(m_hFile);
    m_hFile = INVALID_HANDLE_VALUE;
    return false;
  }
  m_hMapping = CreateFileMappingW(m_hFile, NULL, PAGE_READWRITE, 0, 0, NULL);
  if (m_hMapping == NULL) {
    LOG_ERR("Mapping file failed\n");
    CloseHandle(m_hFile);
    m_hFile = INVALID_HANDLE_VALUE;
    return false;
  }
  m_pTrunck = (char*)MapViewOfFile(m_hMapping, FILE_MAP_WRITE, 0, 0, 0);
  if (m_pTrunck == NULL) {
    LOG_ERR("MapViewOfFile failed\n");
    CloseHandle(m_hMapping);
    m_hMapping = nullptr;
    CloseHandle(m_hFile);
    m_hFile = INVALID_HANDLE_VALUE;
    return false;
  }
#else
  m_fd = open(filename, open_flags, S_IRUSR | S_IWUSR);
  if (m_fd == -1) {
    LOG_ERR("Open file:%s failed\n", filename);
    return false;
  }
  if (-1 == lseek(m_fd, (off_t)m_fileLen - 1, SEEK_SET)) {
    LOG_ERR("seek file error\n");
    close(m_fd);
    m_fd = -1;
    return false;
  }
  if (1 != write(m_fd, "", 1)) {
    LOG_ERR("write error\n");
    close(m_fd);
    m_fd = -1;
    return false;
  }

  m_pTrunck =
      (char*)mmap(NULL, (size_t)m_fileLen, PROT_READ | PROT_WRITE, MAP_SHARED,
                  m_fd, 0);
  if (m_pTrunck == MAP_FAILED) {
    LOG_ERR("Mapping file failed\n");
    close(m_fd);
    m_fd = -1;
    m_pTrunck = nullptr;
    return false;
  }
#endif
  /**
   * Chunk layout: the whole file [0, m_fileLen-1] is split evenly by the
   * thread count (fixed layout, resume-independent).  On resume each chunk
   * continues from its recorded written position:
   *  --------------------------------------------------------
   * |              |              |               |         |
   * |  1st part    |  2nd part    |   3rd part    | ....... |
   * --------------------------------------------------------
   */
  int64_t part_Size = m_fileLen / m_thread_num;
  for (int i = 0; i < m_thread_num; i++) {
    m_Easy_List[i] = (st_EasyList*)malloc(sizeof(st_EasyList));
    m_Easy_List[i]->part_start = i * part_Size;
    if (i < m_thread_num - 1) {
      m_Easy_List[i]->end = (i + 1) * part_Size - 1;
    } else {
      m_Easy_List[i]->end = m_fileLen - 1;  /* last chunk takes the rest */
    }
    /* Chunk-level resume: full chunks are marked done and skipped; others
     * continue from part_start + written. */
    const int64_t part_len =
        m_Easy_List[i]->end - m_Easy_List[i]->part_start + 1;
    int64_t written =
        (have_part_meta && i < (int)m_part_written.size())
            ? m_part_written[i]
            : 0;
    if (written >= part_len) {
      m_Easy_List[i]->success = true;
      m_Easy_List[i]->offset = m_Easy_List[i]->end + 1;
    } else {
      m_Easy_List[i]->success = false;
      m_Easy_List[i]->offset = m_Easy_List[i]->part_start + written;
    }
    m_Easy_List[i]->file_ptr = m_pTrunck;
    m_Easy_List[i]->url = m_url.c_str();
    m_Easy_List[i]->download_len = written;  /* resume base for callbacks */
    m_Easy_List[i]->use_range = m_range_supported;
    m_Easy_List[i]->thread_created = false;
    m_Easy_List[i]->timeout = m_timeout;
    m_Easy_List[i]->referer = m_referer.c_str();
    m_Easy_List[i]->cookie = m_cookie.c_str();
    /* GUI progress/cancel extension. */
    m_Easy_List[i]->part_total =
        m_Easy_List[i]->end - m_Easy_List[i]->part_start + 1;
    m_Easy_List[i]->last_len = 0;
    m_Easy_List[i]->last_t = 0;
    m_Easy_List[i]->cancel_flag = &m_cancel_flag;
    m_Easy_List[i]->on_progress = onProgress ? &onProgress : nullptr;
    /* P2: strict 206 + single-stream degrade + periodic flush. */
    m_Easy_List[i]->bGot206 = FALSE;
    m_Easy_List[i]->range_denied = &m_range_denied;
    m_Easy_List[i]->owner = this;
    /* P8-4: per-chunk attempt state for the multi engine. */
    m_Easy_List[i]->attempt = 0;
    m_Easy_List[i]->dwLane = kChunkLaneUnset;
  }
  m_pInfoTable = m_Easy_List;
  m_dResumeLen = (double)m_resume_len;
  m_dFileLen = (double)m_fileLen;
  LOG_INFO("File Init success, threads: %d, resume: %lld bytes\n",
           m_thread_num, (long long)m_resume_len);

  return true;
}

bool Ccurl::Uploading_Task(const char* server_url) {
  bool flag = false;

  return flag;
}

bool Ccurl::get_Download_FileSize() {
  LOG_INFO(">>>>>\n");

  /* Cancellation checkpoint: stop during the probe phase returns promptly
   * instead of waiting out connect/low-speed timeouts. */
  if (m_cancel_flag.load()) {
    return false;
  }

  bool flag = false;
  m_easyHandle = curl_easy_init();
  if (m_easyHandle == nullptr) {
    LOG_ERR("curl Easy init failed\n");
    return false;
  }

  curl_easy_setopt(m_easyHandle, CURLOPT_URL, m_url.c_str());
  curl_easy_setopt(m_easyHandle, CURLOPT_FOLLOWLOCATION, 1L);
  if (!m_referer.empty()) {
    curl_easy_setopt(m_easyHandle, CURLOPT_REFERER, m_referer.c_str());
  }
  if (!m_cookie.empty()) {
    curl_easy_setopt(m_easyHandle, CURLOPT_COOKIE, m_cookie.c_str());
  }
  curl_easy_setopt(
      m_easyHandle, CURLOPT_USERAGENT,
      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, "
      "like Gecko) Chrome/115.0.0.0 Safari/537.36");
  curl_easy_setopt(m_easyHandle, CURLOPT_HEADER, 1);
  curl_easy_setopt(m_easyHandle, CURLOPT_NOBODY, 1);
  /* One HEAD confirms Range support (Accept-Ranges: bytes) and captures
   * ETag/Last-Modified for resume integrity. */
  TProbeCtx ctxHead = {};
  curl_easy_setopt(m_easyHandle, CURLOPT_HEADERFUNCTION, HeadProbeHeader);
  curl_easy_setopt(m_easyHandle, CURLOPT_HEADERDATA, &ctxHead);
  curl_easy_setopt(m_easyHandle, CURLOPT_CONNECTTIMEOUT, 10L);
  if (m_timeout > 0) {
    /* The probe is also protected by the low-speed timeout. */
    curl_easy_setopt(m_easyHandle, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(m_easyHandle, CURLOPT_LOW_SPEED_TIME, m_timeout);
  }

  /* Attempt 1: HEAD probe - candidate only (some CDNs/proxies return a
   * wrong Content-Length for HEAD, e.g. Bilibili streams returning 18 bytes
   * on other nodes; trusting it directly would corrupt the download).
   * Requires HTTP 200 to be a candidate; the final size comes from the
   * Range GET Content-Range. */
  curl_off_t head_len = -1;
  CURLcode res = curl_easy_perform(m_easyHandle);
  if (CHECK_CURL(res)) {
    long head_code = 0;
    curl_easy_getinfo(m_easyHandle, CURLINFO_RESPONSE_CODE, &head_code);
    curl_easy_getinfo(m_easyHandle, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T,
                      &head_len);
    if (head_code == 200 && head_len > 0) {
      m_remote_etag = ctxHead.strEtag;
      m_remote_last_modified = ctxHead.strLastModified;
      LOG_INFO("HEAD ok: content-length=%lld, accept-ranges=%s (candidate)\n",
               (long long)head_len,
               ctxHead.bAcceptRanges ? "yes" : "no");
    } else {
      LOG_INFO("HEAD ignored: code=%ld content-length=%lld, try Range GET\n",
               head_code, (long long)head_len);
    }
  } else {
    AppendLog("[WARN] HEAD probe failed, fallback to Range GET: url=%s, "
              "curl error: %s",
              m_url.c_str(), curl_easy_strerror(res));
  }
  curl_easy_cleanup(m_easyHandle);
  m_easyHandle = nullptr;
  if (m_cancel_flag.load()) {
    return false;
  }

  /* Attempt 2: Range GET bytes=0-0 - authoritative size probe (based on the
   * actual response, avoids HEAD false values). */
  CURL* probe = curl_easy_init();
  if (probe != nullptr) {
    curl_easy_setopt(probe, CURLOPT_URL, m_url.c_str());
    curl_easy_setopt(probe, CURLOPT_FOLLOWLOCATION, 1L);
    if (!m_referer.empty()) {
      curl_easy_setopt(probe, CURLOPT_REFERER, m_referer.c_str());
    }
    if (!m_cookie.empty()) {
      curl_easy_setopt(probe, CURLOPT_COOKIE, m_cookie.c_str());
    }
    curl_easy_setopt(
        probe, CURLOPT_USERAGENT,
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, "
        "like Gecko) Chrome/115.0.0.0 Safari/537.36");
    curl_easy_setopt(probe, CURLOPT_RANGE, "0-0"); /* request one byte */
    curl_easy_setopt(probe, CURLOPT_WRITEFUNCTION, DummyWrite);
    /* Issue R2: cap the probe so a server that ignores Range cannot make us
     * download the full body; the callback aborts on a 2xx non-206. */
    curl_easy_setopt(probe, CURLOPT_MAXFILESIZE, (curl_off_t)(1 << 20));
    TProbeCtx ctxRange = {};
    curl_easy_setopt(probe, CURLOPT_HEADERFUNCTION, RangeProbeHeader);
    curl_easy_setopt(probe, CURLOPT_HEADERDATA, &ctxRange);
    curl_easy_setopt(probe, CURLOPT_CONNECTTIMEOUT, 10L);
    if (m_timeout > 0) {
      curl_easy_setopt(probe, CURLOPT_LOW_SPEED_LIMIT, 1L);
      curl_easy_setopt(probe, CURLOPT_LOW_SPEED_TIME, m_timeout);
    }
    CURLcode res2 = curl_easy_perform(probe);
    long http_code = 0;
    curl_easy_getinfo(probe, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(probe);
    if (CHECK_CURL(res2) && ctxRange.bStatus206 && ctxRange.nTotal > 0) {
      m_fileLen = ctxRange.nTotal;
      LOG_INFO("download file length success (Range GET): %lld\n",
               (long long)m_fileLen);
      /* 206 confirms Range support; prefer the Range GET integrity fields. */
      m_range_supported = true;
      m_range_known = true;
      if (!ctxRange.strEtag.empty()) {
        m_remote_etag = ctxRange.strEtag;
      }
      if (!ctxRange.strLastModified.empty()) {
        m_remote_last_modified = ctxRange.strLastModified;
      }
      flag = true;
      m_dFileLen = (double)m_fileLen;
      return true;
    }
    LOG_INFO("Range GET not confirmed (code=%ld total=%lld), use HEAD "
             "candidate\n",
             http_code, (long long)ctxRange.nTotal);
  }
  if (m_cancel_flag.load()) {
    return false;
  }

  /* Fallback: when GET did not confirm, use the HEAD candidate. */
  if (head_len > 0) {
    m_fileLen = head_len;
    LOG_INFO("download file length success (HEAD fallback): %lld\n",
             (long long)m_fileLen);
    if (ctxHead.bAcceptRanges) {
      m_range_supported = true;
      m_range_known = true;
    }
    flag = true;
    m_dFileLen = (double)m_fileLen;
    return true;
  }

  LOG_ERR("file_size failed (HEAD + Range GET)\n");
  AppendLog("[ERROR] probe file size failed (HEAD + Range GET): url=%s",
            m_url.c_str());
  m_fileLen = -1;
  m_dFileLen = -1;
  flag = false;
  return flag;
}

bool Ccurl::Check_Range_Support() {
  if (m_cancel_flag.load()) {
    m_range_supported = false;
    return false;
  }
  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    m_range_supported = false;
    return false;
  }
  curl_easy_setopt(curl, CURLOPT_URL, m_url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  if (!m_referer.empty()) {
    curl_easy_setopt(curl, CURLOPT_REFERER, m_referer.c_str());
  }
  if (!m_cookie.empty()) {
    curl_easy_setopt(curl, CURLOPT_COOKIE, m_cookie.c_str());
  }
  curl_easy_setopt(curl, CURLOPT_RANGE, "0-0");      /* request one byte */
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, DummyWrite);
  /* Issue R2: cap the probe and abort on a 2xx non-206 response. */
  curl_easy_setopt(curl, CURLOPT_MAXFILESIZE, (curl_off_t)(1 << 20));
  TProbeCtx ctx = {};
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, RangeProbeHeader);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ctx);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  if (m_timeout > 0) {
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, m_timeout);
  }
  curl_easy_setopt(curl, CURLOPT_USERAGENT,
      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, "
      "like Gecko) Chrome/115.0.0.0 Safari/537.36");
  CURLcode res = curl_easy_perform(curl);
  curl_easy_cleanup(curl);
  if (m_cancel_flag.load()) {
    m_range_supported = false;
    return false;
  }

  m_range_supported = (res == CURLE_OK && ctx.bStatus206);
  if (!ctx.strEtag.empty()) {
    m_remote_etag = ctx.strEtag;
  }
  if (!ctx.strLastModified.empty()) {
    m_remote_last_modified = ctx.strLastModified;
  }
  return m_range_supported;
}

/**
 * @brief Compute the SHA-256 digest of the downloaded file (issue O4).
 * @param strDigest Output lowercase hex digest.
 * @return TRUE on success (files over 4 GiB are rejected).
 */
bool Ccurl::VerifySha256(std::string& strDigest) {
  if ((m_pTrunck == nullptr) || (m_fileLen <= 0)) {
    return false;
  }
  TSha256Ctx tCtx;
  u8 byDigest[32];
  Sha256Init(tCtx);
  Sha256Update(tCtx, (const u8*)m_pTrunck, (size_t)m_fileLen);
  Sha256Final(tCtx, byDigest);
  strDigest = Sha256Hex(byDigest);
  return true;
}

void Ccurl::DestoryUnlocked() {
  for (int i = 0; i < m_thread_num; i++) {
    if (m_Easy_List[i] != nullptr) {
      free(m_Easy_List[i]);
      m_Easy_List[i] = nullptr;
    }
  }
#ifdef _WIN32
  if (m_pTrunck != nullptr) {
    UnmapViewOfFile(m_pTrunck);
    m_pTrunck = nullptr;
  }
  if (m_hMapping != nullptr) {
    CloseHandle(m_hMapping);
    m_hMapping = nullptr;
  }
  if (m_hFile != INVALID_HANDLE_VALUE) {
    CloseHandle(m_hFile);
    m_hFile = INVALID_HANDLE_VALUE;
  }
#else
  if (m_pTrunck != nullptr) {
    munmap(m_pTrunck, (size_t)m_fileLen);
    m_pTrunck = nullptr;
  }
  if (m_fd >= 0) {
    close(m_fd);
    m_fd = -1;
  }
#endif
}

void Ccurl::Destory() {
  unique_lock<mutex> lock(m_lock);
  DestoryUnlocked();
}
