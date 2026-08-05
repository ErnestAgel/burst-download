/**
 * @file Ccurl.cpp
 * @brief 基于 libcurl 的多线程分片下载器 C++ 实现（跨平台：Windows / Linux x86_64 / Linux aarch64）
 *
 * @author ErnestAgel
 * @date 2026-08-06
 */

#include <stdio.h>
#include <cstdio>
#include <cstdarg>
#include <iostream>
#include <algorithm>
#include <ctime>
#include <fcntl.h>
#include <errno.h>
#include <cstring>

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

/** @brief 将 libcurl 返回码转换为布尔值 */
#define CHECK_CURL(value) value == CURLE_OK ? true : false

extern "C" {
/** @brief 打印错误日志（含文件、行号与 errno 描述） */
#define LOG_ERR(...)                                              \
  printf("[%s %d] Erro:%s", __FILE__, __LINE__, strerror(errno)); \
  printf(__VA_ARGS__);

/** @brief 打印信息日志（含文件、行号） */
#define LOG_INFO(...)                    \
  printf("[%s %d]", __FILE__, __LINE__); \
  printf(__VA_ARGS__);
}

st_EasyList** g_pInfoTable;   /**< 全局分片任务表指针，供进度回调汇总各线程下载量 */
double g_filelen;             /**< 待下载文件总大小（字节） */
double g_resume_len;          /**< 断点续传基数：本地已存在的字节数（计入进度统计） */

/**
 * @brief 追加写日志文件（download.log，线程安全）：记录超时中断/失败/成功等事件
 * @param fmt 格式化字符串（同 printf）
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

/* 进度显示状态：多线程进度回调共享，用互斥锁保护（跨平台 std::mutex） */
static int g_print = 1;                 /**< 下一次要打印的百分比 */
static double g_last_total = 0;         /**< 上次打印时的累计下载量 */
static time_t g_last_t = 0;             /**< 上次打印时间 */
static std::mutex g_progress_mutex;     /**< 进度回调互斥锁 */

extern "C" {

/**
 * @brief libcurl 写回调：将下载数据写入映射内存的对应偏移（带分片边界检查）
 * @param ptr 收到的数据指针
 * @param size 单个数据块大小
 * @param memb 数据块数量
 * @param userdata 指向 st_EasyList 的用户数据
 * @return 实际写入的字节数
 */
size_t File_Write(char* ptr, size_t size, size_t memb, void* userdata) {
  st_EasyList* info = (st_EasyList*)userdata;
  size_t total = size * memb;
  size_t n = total;
  int64_t end_pos = info->end + 1;
  if (info->offset < end_pos) {
    if (info->offset + (int64_t)n > end_pos) {
      n = (size_t)(end_pos - info->offset);  /* 截断到分片末尾，防止越界写 */
    }
    memcpy((info->file_ptr + info->offset), ptr, n);
    info->offset += n;
  }
  /* 超出分片范围的数据直接丢弃；返回原始字节数，避免触发 CURLE_WRITE_ERROR */
  return total;
}

/**
 * @brief libcurl 写回调：丢弃收到的数据（用于 Range 支持检测）
 * @param ptr 收到的数据指针
 * @param size 单个数据块大小
 * @param memb 数据块数量
 * @param userdata 未使用
 * @return 实际接收的字节数
 */
size_t DummyWrite(char* ptr, size_t size, size_t memb, void* userdata) {
  (void)ptr;
  (void)userdata;
  return size * memb;
}

/**
 * @brief libcurl 进度回调：汇总各线程下载量并打印整体百分比、速率与剩余时间
 * @param userdata 指向 st_EasyList 的用户数据
 * @param totalDownload 总下载字节数
 * @param nowDownload 当前已下载字节数
 * @param totalUpload 总上传字节数
 * @param nowUpload 当前已上传字节数
 * @return 0 表示继续下载
 */
size_t progressFunc(void* userdata,
                 double totalDownload,
                 double nowDownload,
                 double totalUpload,
                 double nowUpload) {
  g_progress_mutex.lock();
  st_EasyList* info = (st_EasyList*)userdata;
  info->download_len = nowDownload;

  int percent = 0;
  double allDownload = g_resume_len;  /* 续传时以本地已存在字节数为基数 */
  if (totalDownload > 0) {
    for (int i = 0; i < MaxThread + 1; i++) {
      if (g_pInfoTable[i] != nullptr) {
        allDownload += g_pInfoTable[i]->download_len;
      }
    }
    percent = (int)(allDownload / g_filelen * 100);
  }

  if (percent >= g_print) {
    time_t now = time(NULL);
    double speed = 0;
    if (g_last_t > 0 && now > g_last_t) {
      speed = (allDownload - g_last_total) / (now - g_last_t);
    }
    g_last_total = allDownload;
    g_last_t = now;
    if (speed > 0 && g_filelen > allDownload) {
      double remain_sec = (g_filelen - allDownload) / speed;
      int h = (int)(remain_sec / 3600);
      int m = (int)(remain_sec / 60) % 60;
      int s = (int)remain_sec % 60;
      LOG_INFO("percent: %d%% speed: %.2f MB/s ETA: %02d:%02d:%02d\n", percent,
               speed / (1024.0 * 1024.0), h, m, s);
    } else {
      LOG_INFO("percent: %d%%\n", percent);
    }
    g_print = percent + 1;
  }
  g_progress_mutex.unlock();

  return 0;
}
}

Ccurl::Ccurl() {
  for (int i = 0; i <= MaxThread; i++) {
    m_Easy_List[i] = nullptr;
  }
  curl_version_info_data* ver = curl_version_info(CURLVERSION_NOW);
  LOG_INFO("libcurl version %u.%u.%u\n", (ver->version_num >> 16) & 0xff,
           (ver->version_num >> 8) & 0xff, ver->version_num & 0xff);
}

Ccurl::~Ccurl() {
  this->Destory();
}

bool Ccurl::Init(const string url, string filename, int thread_num, int timeout) {
  unique_lock<mutex> lock(m_lock);
  m_url = url;
  m_filename = filename;
  m_thread_num = thread_num < 1 ? 1 : (thread_num > MaxThread ? MaxThread : thread_num);
  m_timeout = timeout < 0 ? 0 : timeout;
  LOG_INFO(">>>>>\n");
  bool flag = this->Check_Range_Support();
  LOG_INFO("HTTP Range support: %s\n", flag ? "yes" : "no (fallback to single stream)");
  flag = this->File_Init(filename.c_str());

  return flag;
}

void *Ccurl::Downloading(void* arg) {
  st_EasyList* info = (st_EasyList*)arg;
  char range[64] = {0};
  const int max_retry = 3;
  const int64_t base_offset = info->offset;

  if (info->use_range) {
    snprintf(range, sizeof(range), "%lld-%lld",
             (long long)info->offset, (long long)info->end);
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
    LOG_INFO("threadid: %ld, download whole range from: %lld\n", (long)info->thid,
             (long long)info->offset);
#endif
  }

  for (int attempt = 0; attempt < max_retry; attempt++) {
    info->offset = base_offset;  /* 重试前恢复本分片起点 */
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
      LOG_ERR("curl Easy init failed\n");
      break;
    }
    curl_easy_setopt(curl, CURLOPT_URL, info->url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, File_Write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, info);

    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, progressFunc);
    curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, info);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (info->use_range) {
      curl_easy_setopt(curl, CURLOPT_RANGE, range);
    }
    if (info->timeout > 0) {
      /* 低速超时：下载无进展（低于 1 字节/秒）持续 timeout 秒则中断 */
      curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
      curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, info->timeout);
    }

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (CHECK_CURL(res) && info->offset >= info->end + 1) {
      info->success = true;  /* 本分片下载成功且已写满 */
      return nullptr;
    }
    if (res == CURLE_OPERATION_TIMEDOUT) {
      /* 超时即中断（不重试），输出详细日志 */
      LOG_ERR("part %lld-%lld timeout (no progress for %ld s)\n",
              (long long)info->offset, (long long)info->end, info->timeout);
      AppendLog("[WARN] timeout on part %lld-%lld (url=%s, no progress for %ld s)",
                (long long)info->offset, (long long)info->end, info->url, info->timeout);
      info->success = false;
      return nullptr;
    }
    LOG_ERR("res:%s, retry %d/%d\n", curl_easy_strerror(res), attempt + 1, max_retry);
#ifdef _WIN32
    Sleep(1000);  /* 重试前等待 1 秒 */
#else
    sleep(1);
#endif
  }
  info->success = false;
  AppendLog("[ERROR] part %lld-%lld failed after %d retries (url=%s)",
            (long long)base_offset, (long long)info->end, max_retry, info->url);

  return nullptr;
}

bool Ccurl::Download_Task() {
  unique_lock<mutex> lock(m_lock);
  bool all_ok = true;

  if (m_Easy_List[0] == nullptr) {
    return true;  /* 无分片任务（文件已完整），直接视为成功 */
  }
  for (int i = 0; i < m_thread_num; i++) {
#ifdef _WIN32
    m_Easy_List[i]->thid = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)&Downloading,
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
  if (all_ok) {
    AppendLog("[INFO] download complete: url=%s", m_url.c_str());
  } else {
    AppendLog("[ERROR] download task failed: some parts not completed (url=%s)",
              m_url.c_str());
  }
  return all_ok;
}



bool Ccurl::File_Init(const char* filename) {
  LOG_INFO(">>>>>\n");

  this->get_Download_FileSize();
  if (m_fileLen <= 0) {
    LOG_ERR("invalid file length: %lld\n", (long long)m_fileLen);
    return false;
  }

  /* 断点续传：检测本地已存在文件大小 */
  m_resume_len = 0;
#ifdef _WIN32
  struct _stat64 st;
  if (_stat64(filename, &st) == 0 && (st.st_mode & _S_IFREG)) {
    m_resume_len = (int64_t)st.st_size;
  }
#else
  struct stat st;
  if (stat(filename, &st) == 0 && S_ISREG(st.st_mode)) {
    m_resume_len = st.st_size;
  }
#endif
  if (m_resume_len >= m_fileLen) {
    LOG_INFO("file already complete (%lld bytes), skip download\n",
             (long long)m_resume_len);
    return true;  /* 文件已就绪，无需下载 */
  }
  if (m_resume_len > 0) {
    LOG_INFO("resume from existing %lld bytes\n", (long long)m_resume_len);
  }

  /* 服务器不支持 Range 时退化为单线程整段下载；断点数据无法续接，丢弃重来 */
  int open_flags = O_RDWR | O_CREAT;
  if (!m_range_supported) {
    m_thread_num = 1;
    if (m_resume_len > 0) {
      LOG_INFO("server does not support Range, discard existing %lld bytes and restart\n",
               (long long)m_resume_len);
      m_resume_len = 0;
      open_flags |= O_TRUNC;
    }
  }
  /* 剩余字节数小于线程数时，线程数不超过剩余字节数 */
  int64_t remain = m_fileLen - m_resume_len;
  if (remain < m_thread_num) {
    m_thread_num = (int)remain;
  }

#ifdef _WIN32
  DWORD dwCreation = (open_flags & O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS;
  m_hFile = CreateFileA(filename, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        dwCreation, FILE_ATTRIBUTE_NORMAL, NULL);
  if (m_hFile == INVALID_HANDLE_VALUE) {
    LOG_ERR("CreateFile:%s failed\n", filename);
    return false;
  }
  /* 将文件扩展到目标大小并写满末尾 1 字节，保证映射覆盖整个文件 */
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
  m_hMapping = CreateFileMappingA(m_hFile, NULL, PAGE_READWRITE, 0, 0, NULL);
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
      (char*)mmap(NULL, (size_t)m_fileLen, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0);
  if (m_pTrunck == MAP_FAILED) {
    LOG_ERR("Mapping file failed\n");
    close(m_fd);
    m_fd = -1;
    m_pTrunck = nullptr;
    return false;
  }
#endif
  /**
   * 分片示意：剩余区间 [m_resume_len, m_fileLen-1] 按线程数均分，最后一个线程负责余数：
   *  --------------------------------------------------------
   * |              |              |               |         |
   * |  1st part    |  2st part    |   3rd part    | ....... |
   * ---------------------------------------------------------
   */
  int64_t part_Size = remain / m_thread_num;
  for (int i = 0; i < m_thread_num; i++) {
    m_Easy_List[i] = (st_EasyList*)malloc(sizeof(st_EasyList));
    m_Easy_List[i]->offset = m_resume_len + i * part_Size;
    if (i < m_thread_num - 1) {
      m_Easy_List[i]->end = m_resume_len + (i + 1) * part_Size - 1;
    } else {
      m_Easy_List[i]->end = m_fileLen - 1;  /* 最后一个线程负责余数部分 */
    }
    m_Easy_List[i]->file_ptr = m_pTrunck;
    m_Easy_List[i]->url = m_url.c_str();
    m_Easy_List[i]->download_len = 0;
    m_Easy_List[i]->use_range = m_range_supported;
    m_Easy_List[i]->success = false;
    m_Easy_List[i]->thread_created = false;
    m_Easy_List[i]->timeout = m_timeout;
  }
  g_pInfoTable = m_Easy_List;
  g_resume_len = (double)m_resume_len;
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

  bool flag = false;
  m_easyHandle = curl_easy_init();
  if (m_easyHandle == nullptr) {
    LOG_ERR("curl Easy init failed\n");
    return false;
  }

  curl_easy_setopt(m_easyHandle, CURLOPT_URL, m_url.c_str());
  curl_easy_setopt(m_easyHandle, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(
      m_easyHandle, CURLOPT_USERAGENT,
      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, "
      "like Gecko) Chrome/115.0.0.0 Safari/537.36");
  curl_easy_setopt(m_easyHandle, CURLOPT_HEADER, 1);
  curl_easy_setopt(m_easyHandle, CURLOPT_NOBODY, 1);
  curl_easy_setopt(m_easyHandle, CURLOPT_CONNECTTIMEOUT, 10L);
  if (m_timeout > 0) {
    /* 探测请求同样受低速超时保护，避免服务器挂起时卡住 */
    curl_easy_setopt(m_easyHandle, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(m_easyHandle, CURLOPT_LOW_SPEED_TIME, m_timeout);
  }

  CURLcode res = curl_easy_perform(m_easyHandle);
  if (CHECK_CURL(res)) {
    res = curl_easy_getinfo(m_easyHandle, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T,
                            &m_fileLen);
    LOG_INFO("dowload File length success: %lld\n", (long long)m_fileLen);
    flag = true;
    g_filelen = (double)m_fileLen;
  } else {
    LOG_ERR("file_size failed\n");
    AppendLog("[ERROR] probe file size failed: url=%s, curl error: %s",
              m_url.c_str(), curl_easy_strerror(res));
    m_fileLen = -1;
    g_filelen = -1;
    flag = false;
  }

  curl_easy_cleanup(m_easyHandle);
  return flag;
}

bool Ccurl::Check_Range_Support() {
  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    m_range_supported = false;
    return false;
  }
  curl_easy_setopt(curl, CURLOPT_URL, m_url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_RANGE, "0-0");      /* 只请求第一个字节 */
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, DummyWrite);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  if (m_timeout > 0) {
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, m_timeout);
  }
  curl_easy_setopt(curl, CURLOPT_USERAGENT,
      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, "
      "like Gecko) Chrome/115.0.0.0 Safari/537.36");
  CURLcode res = curl_easy_perform(curl);
  long code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_cleanup(curl);

  m_range_supported = (res == CURLE_OK && code == 206);
  return m_range_supported;
}

void Ccurl::Destory() {
  unique_lock<mutex> lock(m_lock);
  for (int i = 0; i < m_thread_num; i++) 
  {
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
