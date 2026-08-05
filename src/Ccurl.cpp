/**
 * @file Ccurl.cpp
 * @author Ernest
 * @brief 
 * @date 2026-08-06
 */
#include <stdio.h>
#include <cstdio>
#include <iostream>
#include <algorithm>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <thread>
#include <errno.h>
#include <cstring>
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

extern "C" {

/**
 * @brief libcurl 写回调：将下载数据写入 mmap 映射内存的对应偏移
 * @param ptr 收到的数据指针
 * @param size 单个数据块大小
 * @param memb 数据块数量
 * @param userdata 指向 st_EasyList 的用户数据
 * @return 实际写入的字节数
 */
size_t File_Write(char* ptr, size_t size, size_t memb, void* userdata) {
  st_EasyList* info = (st_EasyList*)userdata;
    memcpy((info->file_ptr +info->offset), ptr, size * memb);
  info->offset += size * memb;

  return size * memb;
}

/**
 * @brief libcurl 进度回调：汇总各线程下载量并打印整体百分比
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
  int percent = 0;
  static int print = 1;
  st_EasyList* info = (st_EasyList*)userdata;
  info->download_len = nowDownload;

  if (totalDownload > 0) {
    int i = 0;
    double allDownload = 0;
    for (i = 0; i <= MaxThread; i++) {
      allDownload += g_pInfoTable[i]->download_len;
    }
    percent = (int)(allDownload / g_filelen * 100);
  }

  if (percent == print) {
    LOG_INFO("percent: %d%%\n", percent);
    print += 1;
  }

  return 0;
}
}

Ccurl::Ccurl() {
  curl_version_info_data* ver = curl_version_info(CURLVERSION_NOW);
  LOG_INFO("libcurl version %u.%u.%u\n", (ver->version_num >> 16) & 0xff,
           (ver->version_num >> 8) & 0xff, ver->version_num & 0xff);
}

Ccurl::~Ccurl() {
  this->Destory();
}

bool Ccurl::Init(const string url, string filename) {
  unique_lock<mutex> lock(m_lock);
  bool flag = true;
  m_url = url;
  m_filename = filename;
  LOG_INFO(">>>>>\n");
  flag = this->File_Init(filename.c_str());

  return flag;
}

void *Ccurl::Downloading(void* arg) {
  st_EasyList* info = (st_EasyList*)arg;
  CURLcode res;
  char range[64] = {0};

  snprintf(range, 64, "%d-%d", info->offset, info->end);
  LOG_INFO("threadid: %ld, download from: %d to: %d\n", info->thid,
                                                 info->offset, info->end);

  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    LOG_ERR("curl Easy init failed\n");
  } 
  else 
  {
    curl_easy_setopt(curl, CURLOPT_URL, info->url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, File_Write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, info);

    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, progressFunc);
    curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, info);
    curl_easy_setopt(curl, CURLOPT_RANGE, range);

    res = curl_easy_perform(curl);
    if (!CHECK_CURL(res)) {
      LOG_ERR("res:%s\n", curl_easy_strerror(res));
    }
    curl_easy_cleanup(curl);
  }

  return nullptr;
}

bool Ccurl::Download_Task() {
  unique_lock<mutex> lock(m_lock);
  bool flag = false;

  for (int i = 0; i <= MaxThread; i++) {
    pthread_create(&(m_Easy_List[i]->thid), NULL, &Downloading, (void*)m_Easy_List[i]);
  }
  
  for (int i = 0; i <= MaxThread; i++) {
    pthread_join(m_Easy_List[i]->thid, NULL);
  }
  return flag;
}



bool Ccurl::File_Init(const char* filename) {
  st_EasyList* info;
  LOG_INFO(">>>>>\n");

  this->get_Download_FileSize();
  m_fd = open(filename, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
  if (m_fd == -1) {
    LOG_ERR("Open file:%s failed\n", filename);
    return false;
  }
  if (-1 == lseek(m_fd, m_fileLen - 1, SEEK_SET)) {
    LOG_ERR("seek file error\n");
    return false;
  }
  if (1 != write(m_fd, "", 1)) {
    LOG_ERR("write error\n");
    return false;
  }

  m_pTrunck =
      (char*)mmap(NULL, m_fileLen, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0);
  if (m_pTrunck == MAP_FAILED) {
    LOG_ERR("Mapping file failed\n");
    close(m_fd);
  }
  /**
   * 分片示意：文件被均分为 MaxThread 段，最后一个线程负责余数部分：
   *  --------------------------------------------------------
   * |              |              |               |         |
   * |  1st part    |  2st part    |   3rd part    | ....... |
   * ---------------------------------------------------------
   */
  long part_Size = m_fileLen / MaxThread;
  for (int i = 0; i <= MaxThread; i++) {
    m_Easy_List[i] = (st_EasyList*)malloc(sizeof(st_EasyList));
    m_Easy_List[i]->offset = i * part_Size;
    if (i < MaxThread) {
      m_Easy_List[i]->end = (i + 1) * part_Size - 1;
    } else {
      m_Easy_List[i]->end = m_fileLen - 1;
    }
    m_Easy_List[i]->file_ptr = m_pTrunck;
    m_Easy_List[i]->url = m_url.c_str();
    m_Easy_List[i]->download_len = 0;
  }
  g_pInfoTable = m_Easy_List;
  LOG_INFO("File Init success\n");

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

  curl_easy_setopt(m_easyHandle, CURLOPT_URL, m_url.c_str());
  curl_easy_setopt(
      m_easyHandle, CURLOPT_USERAGENT,
      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, "
      "like Gecko) Chrome/115.0.0.0 Safari/537.36");
  curl_easy_setopt(m_easyHandle, CURLOPT_HEADER, 1);
  curl_easy_setopt(m_easyHandle, CURLOPT_NOBODY, 1);

  CURLcode res = curl_easy_perform(m_easyHandle);
  if (CHECK_CURL(res)) {
    res = curl_easy_getinfo(m_easyHandle, CURLINFO_CONTENT_LENGTH_DOWNLOAD,
                            &m_fileLen);
    LOG_INFO("dowload File length success: %f\n", m_fileLen);
    flag = true;
    g_filelen = m_fileLen;
  } else {
    LOG_ERR("file_size:%f\n", m_fileLen);
    m_fileLen = -1;
    g_filelen = m_fileLen;
    flag = false;
  }

  curl_easy_cleanup(m_easyHandle);
  return flag;
}

void Ccurl::Destory() {
  unique_lock<mutex> lock(m_lock);
  for (int i = 0; i < MaxThread; i++) 
  {
    free(m_Easy_List[i]);
  }
  munmap(m_pTrunck, m_fileLen);
  close(m_fd);
}
