/**
 * @file main.cpp
 * @brief CLI 入口（RunCli）：多线程分片下载命令行工具（支持 --video 视频直链下载模式）
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

#ifdef _WIN32
#include <io.h>
#define access _access
#define F_OK 0
#else
#include <unistd.h>
#endif
#include <memory>
#include <string>
#include <vector>
#include "Ccurl.h"
#include "app.h"
#include "video.h"
#include "embed_python.h"
#include "avmerge.h"
#include "download_video.h"

using namespace std;

/**
 * @brief 文件是否已存在
 */
static bool FileExists(const std::string& path) {
  return access(path.c_str(), F_OK) == 0;
}

/**
 * @brief 视频模式输出是否已存在（视频轨/音频轨/合并产物任一命中即视为冲突）
 */
static bool VideoOutputExists(const std::string& basename) {
  const char* exts[] = {".mp4", ".m4a", ".mkv", ".webm"};
  for (const char* e : exts) {
    if (FileExists(basename + e) || FileExists(basename + "_full" + e)) {
      return true;
    }
  }
  return false;
}

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
  printf("  --no-auto-update  视频模式：关闭启动时自动检查/更新解析组件（默认开启，24 小时节流一次）\n");
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
 * @return 是否全部成功（合并失败视为失败，两轨文件保留可手动合并）
 */
static bool DownloadVideo(const string& video_url, const string& basename,
                          int threads, int timeout,
                          const string& cookies_from_browser,
                          const string& cookie_str) {
  /* 编排逻辑抽至 src/download_video.*（CLI/GUI 共用）；CLI 不设回调，
   * 内部默认 printf 输出与原先一致；onProgress 为空时 Ccurl 保留 1% 门控打印 */
  VideoDownloader vd;
  VideoResult r = vd.Run(video_url, basename, threads, timeout,
                         cookies_from_browser, cookie_str);
  return r == VideoResult::Ok;
}

/**
 * @brief 程序入口
 * @param argc 参数个数
 * @param argv 参数数组
 * @return 程序退出码（0 成功，1 失败或用法错误）
 */
int RunCli(int argc, char** argv) {
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
  bool auto_update_parser = true;

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
    } else if (strcmp(argv[i], "--no-auto-update") == 0) {
      auto_update_parser = false;
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
    /* 未指定 -o 时：视频模式按视频页 URL 推断基础名 + 时间戳自动命名（防覆盖）
     * 指定 -o 但同名输出已存在时：基础名追加时间戳避让 */
    if (filename == "./test") {
      string base = UrlBaseName(video_url);
      size_t dot = base.find_last_of('.');
      if (dot != string::npos) base = base.substr(0, dot);  /* basename 不带扩展名 */
      if (base.empty()) base = "video";
      filename = base + "_" + CurrentTimeStamp();
    } else if (VideoOutputExists(filename)) {
      filename += "_" + CurrentTimeStamp();
      printf("同名输出已存在，为避免覆盖改用: %s\n", filename.c_str());
    }
    /* 自动更新解析组件（24h 节流；失败静默，不阻塞解析） */
    if (auto_update_parser) {
      string up_msg;
      if (EmbedAutoUpdateParser(up_msg) && !up_msg.empty()) {
        printf("%s\n", up_msg.c_str());
      }
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

  /* 未指定 -o 时：普通下载按 URL 推断 + 时间戳自动命名（防覆盖，便于多次下载不同文件）
   * 指定 -o 但目标文件已存在时：追加时间戳避让，避免覆盖已有文件 */
  if (filename == "./test") {
    string base = UrlBaseName(url);
    if (base.empty()) base = "download.dat";
    filename = "./" + StampName(base);
  } else if (FileExists(filename)) {
    filename = StampName(filename);
    printf("目标文件已存在，为避免覆盖改用: %s\n", filename.c_str());
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
