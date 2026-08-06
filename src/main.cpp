/**
 * @file main.cpp
 * @brief 程序入口：多线程分片下载命令行工具（支持 --video 视频直链下载模式）
 *
 * 用法:
 *   curl_download <url> [-o filename] [-t threads] [--timeout sec] [--no-timeout]
 *   curl_download --video <video-url> [-o basename] [-t threads] [--timeout sec]
 *
 * @author ErnestAgel
 * @date 2026-08-06
 * @copyright Copyright (c) 2026 ErnestAgel
 * @license SPDX-License-Identifier: MIT
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include "Ccurl.h"
#include "video.h"
#include "embed_python.h"
#include "avmerge.h"

using namespace std;

/**
 * @brief 当前时间戳字符串（YYYYMMDD_HHMMSS，用于默认命名防覆盖）
 */
static string CurrentTimeStamp() {
  char buf[32];
  time_t t = time(nullptr);
  struct tm* tm_now = localtime(&t);
  if (tm_now != nullptr) {
    strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", tm_now);
  } else {
    snprintf(buf, sizeof(buf), "%ld", (long)t);
  }
  return string(buf);
}

/**
 * @brief 从 URL 推断基础名称：取最后一个路径段，去掉查询串与片段
 * @param url 下载地址（普通文件或视频页）
 * @return 推断名（可能带扩展名；无法推断时返回空串）
 */
static string UrlBaseName(const string& url) {
  string u = url;
  size_t q = u.find_first_of("?#");
  if (q != string::npos) u = u.substr(0, q);
  while (!u.empty() && u.back() == '/') u.pop_back();
  size_t slash = u.find_last_of("/\\");
  return (slash != string::npos) ? u.substr(slash + 1) : u;
}

/**
 * @brief 在基础名上追加时间戳（插到扩展名之前）：file.iso -> file_20260807_043000.iso
 */
static string StampName(const string& base) {
  string ts = CurrentTimeStamp();
  size_t dot = base.find_last_of('.');
  size_t slash = base.find_last_of("/\\");
  if (dot != string::npos && (slash == string::npos || dot > slash)) {
    return base.substr(0, dot) + "_" + ts + base.substr(dot);
  }
  return base + "_" + ts;
}

/**
 * @brief 打印用法说明
 * @param prog 程序名
 */
static void PrintUsage(const char* prog) {
  printf("Usage: %s <url> [-o filename] [-t threads] [--timeout sec] [--no-timeout]\n", prog);
  printf("       %s --video <video-url> [-o basename] [-t threads] [--timeout sec]\n", prog);
  printf("  <url>          下载地址\n");
  printf("  --video <url>  视频下载模式：解析视频网页 URL（B站/YouTube 等主流网站），\n");
  printf("                 拿到媒体流直链后用多线程分片下载器下载（内置解析引擎）\n");
  printf("  --cookies-from-browser <name>  视频模式：从浏览器读取登录 Cookie（chrome/firefox/edge 等），\n");
  printf("                 用于解析需要登录态的高清视频流（如 B站 720p+）\n");
  printf("  --cookie <str> 请求 Cookie（如 \"SESSDATA=xxx; bili_jct=xxx\"），视频流与普通下载均适用\n");
  printf("  -o filename    保存的文件名（未指定时按 URL 推断 + 时间戳自动命名防覆盖；"
         "--video 模式为输出基础名，默认 <URL名>_<时间戳>）\n");
  printf("  -t threads     下载线程数 1~%d（默认 %d）\n", MaxThread, MaxThread);
  printf("  --timeout N    下载无进展 N 秒后自动中断（默认 60，0 表示不限）\n");
  printf("  --no-timeout   强制下载不自动中断（等价 --timeout 0）\n");
  printf("  --update-parser  在线更新内置视频解析组件到最新版（需网络，无需重新编译）\n");
  printf("  -h, --help     显示本帮助\n");
  printf("示例:\n");
  printf("  %s https://example.com/file.iso -o file.iso -t 8 --timeout 30\n", prog);
  printf("  %s --video https://www.bilibili.com/video/BVxxxx -o movie -t 8\n", prog);
  printf("  %s --video https://www.bilibili.com/video/BVxxxx -o movie --cookies-from-browser chrome\n", prog);
  printf("  %s https://example.com/private.zip -o p.zip --cookie \"SESSDATA=xxx\"\n", prog);
  printf("日志: 超时中断/失败/完成详情写入 download.log\n");
}

/**
 * @brief 视频下载模式：解析视频直链并用多线程分片下载器逐个下载
 * @param video_url 视频网页 URL
 * @param basename 输出基础名（视频轨 .mp4 / 音频轨 .m4a）
 * @param threads 下载线程数
 * @param timeout 低速超时秒数
 * @return 是否全部成功
 */
static bool DownloadVideo(const string& video_url, const string& basename,
                          int threads, int timeout,
                          const string& cookies_from_browser,
                          const string& cookie_str) {
  vector<string> streams;
  if (!ParseVideoUrls(video_url, streams, cookies_from_browser, cookie_str)) {
    printf("视频解析失败: 请确认 URL 有效/可访问，且 Python 运行时资源完整\n");
    return false;
  }
  printf("解析成功: 共 %zu 个媒体流\n", streams.size());

  bool all_ok = true;
  for (size_t i = 0; i < streams.size() && i < 2; i++) {
    string out = (i == 0) ? basename + ".mp4" : basename + ".m4a";
    printf("正在下载第 %zu 个流 -> %s\n", i + 1, out.c_str());
    unique_ptr<Ccurl> ptr = make_unique<Ccurl>();
    ptr->SetReferer(video_url);  /* 防盗链：以视频页 URL 作为 Referer（如 B站视频流） */
    if (!cookie_str.empty()) {
      ptr->SetCookie(cookie_str);  /* 下载流时携带 Cookie（高清流需登录态） */
    }
    if (!ptr->Init(streams[i], out, threads, timeout)) {
      all_ok = false;
      break;
    }
    if (!ptr->Download_Task()) {
      all_ok = false;
      break;
    }
  }
  if (all_ok && streams.size() > 1) {
    /* 音视频分离流（DASH）：内置合并器自动合并为单文件（进程内，无需外部工具） */
    string vfile = basename + ".mp4";
    string afile = basename + ".m4a";
    string merged = basename + "_full.mp4";
    string merr;
    if (MergeMp4(vfile, afile, merged, merr)) {
      printf("已自动合并音视频轨 -> %s\n", merged.c_str());
      /* 合并成功：删除音视频中间文件，仅保留合并产物 */
      if (remove(vfile.c_str()) == 0 && remove(afile.c_str()) == 0) {
        printf("已清理中间文件: %s, %s\n", vfile.c_str(), afile.c_str());
      } else {
        printf("提示: 中间文件清理失败，可手动删除 %s 和 %s\n",
               vfile.c_str(), afile.c_str());
      }
    } else {
      printf("自动合并失败: %s\n", merr.c_str());
      printf("提示: 可保留两轨文件，用外部工具手动合并: "
             "ffmpeg -i %s -i %s -c copy %s\n",
             vfile.c_str(), afile.c_str(), merged.c_str());
    }
  }
  return all_ok;
}

/**
 * @brief 程序入口
 * @param argc 参数个数
 * @param argv 参数数组
 * @return 程序退出码（0 成功，1 失败或用法错误）
 */
int main(int argc, char** argv) {
  if (argc < 2) {
    PrintUsage(argv[0]);
    return 1;
  }
  if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
    PrintUsage(argv[0]);
    return 0;
  }

  /* 初始化嵌入的 Python 运行时（优先可执行文件同目录 python_runtime/，
   * 否则回退到编译期宏 PYTHON_RUNTIME_FALLBACK 的源码树资源） */
  {
    string exe_dir(argv[0]);
    size_t slash = exe_dir.find_last_of("/\\");
    string py_home = (slash != string::npos)
                         ? exe_dir.substr(0, slash) + "/python_runtime"
                         : "";
    EmbedPythonInit(py_home);
  }

  string url;
  string filename = "./test";
  int threads = MaxThread;
  int timeout = 60;
  bool video_mode = false;
  string video_url;
  string cookies_from_browser;
  string cookie_str;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--video") == 0 && i + 1 < argc && argv[i + 1][0] != '-') {
      video_mode = true;
      video_url = argv[++i];
    } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc && argv[i + 1][0] != '-') {
      filename = argv[++i];
    } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc && argv[i + 1][0] != '-') {
      threads = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc && argv[i + 1][0] != '-') {
      timeout = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--no-timeout") == 0) {
      timeout = 0;  /* 强制下载，不自动中断 */
    } else if (strcmp(argv[i], "--update-parser") == 0) {
      string msg;
      if (!EmbedUpdateParser(argv[0], msg)) {
        printf("更新失败: %s\n", msg.c_str());
        return 1;
      }
      printf("%s\n", msg.c_str());
      return 0;
    } else if (strcmp(argv[i], "--cookies-from-browser") == 0 && i + 1 < argc && argv[i + 1][0] != '-') {
      cookies_from_browser = argv[++i];
    } else if (strcmp(argv[i], "--cookie") == 0 && i + 1 < argc && argv[i + 1][0] != '-') {
      cookie_str = argv[++i];
    } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      PrintUsage(argv[0]);
      return 0;
    } else if (argv[i][0] != '-' && url.empty()) {
      url = argv[i];  /* 第一个位置参数作为下载地址 */
    } else {
      printf("未知参数: %s\n", argv[i]);
      PrintUsage(argv[0]);
      return 1;
    }
  }

  /* 视频下载模式 */
  if (video_mode) {
    if (video_url.empty()) {
      printf("缺少 --video 参数值\n");
      return 1;
    }
    /* 未指定 -o 时：视频模式按视频页 URL 推断基础名 + 时间戳自动命名（防覆盖） */
    if (filename == "./test") {
      string base = UrlBaseName(video_url);
      size_t dot = base.find_last_of('.');
      if (dot != string::npos) base = base.substr(0, dot);  /* basename 不带扩展名 */
      if (base.empty()) base = "video";
      filename = base + "_" + CurrentTimeStamp();
    }
    if (!DownloadVideo(video_url, filename, threads, timeout,
                       cookies_from_browser, cookie_str)) {
      printf("视频下载失败（详见 download.log）\n");
      return 1;
    }
    return 0;
  }

  if (url.empty()) {
    PrintUsage(argv[0]);
    return 1;
  }

  /* 未指定 -o 时：普通下载按 URL 推断 + 时间戳自动命名（防覆盖，便于多次下载不同文件） */
  if (filename == "./test") {
    string base = UrlBaseName(url);
    if (base.empty()) base = "download.dat";
    filename = "./" + StampName(base);
  }

  unique_ptr<Ccurl> ptr = make_unique<Ccurl>();
  if (!cookie_str.empty()) {
    ptr->SetCookie(cookie_str);  /* 普通下载也可携带 Cookie（如需登录的文件） */
  }
  if (!ptr->Init(url, filename, threads, timeout)) {
    return 1;
  }
  if (!ptr->Download_Task()) {
    printf("下载失败: 存在未完成的分片（详见 download.log）\n");
    return 1;
  }

  return 0;
}
