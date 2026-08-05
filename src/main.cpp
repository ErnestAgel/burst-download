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
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include "Ccurl.h"
#include "video.h"

using namespace std;

/**
 * @brief 打印用法说明
 * @param prog 程序名
 */
static void PrintUsage(const char* prog) {
  printf("Usage: %s <url> [-o filename] [-t threads] [--timeout sec] [--no-timeout]\n", prog);
  printf("       %s --video <video-url> [-o basename] [-t threads] [--timeout sec]\n", prog);
  printf("  <url>          下载地址\n");
  printf("  --video <url>  视频下载模式：通过 yt-dlp 解析视频网页 URL（B站/YouTube 等 1000+ 网站），\n");
  printf("                 拿到媒体流直链后用多线程分片下载器下载（需已安装 yt-dlp）\n");
  printf("  -o filename    保存的文件名（默认 ./test；--video 模式为输出基础名，默认 ./video）\n");
  printf("  -t threads     下载线程数 1~%d（默认 %d）\n", MaxThread, MaxThread);
  printf("  --timeout N    下载无进展 N 秒后自动中断（默认 60，0 表示不限）\n");
  printf("  --no-timeout   强制下载不自动中断（等价 --timeout 0）\n");
  printf("  -h, --help     显示本帮助\n");
  printf("示例:\n");
  printf("  %s https://example.com/file.iso -o file.iso -t 8 --timeout 30\n", prog);
  printf("  %s --video https://www.bilibili.com/video/BVxxxx -o movie -t 8\n", prog);
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
                          int threads, int timeout) {
  vector<string> streams;
  if (!ParseVideoUrls(video_url, streams)) {
    printf("视频解析失败: 请确认已安装 yt-dlp（pip install yt-dlp 或官网单文件），且 URL 有效/可访问\n");
    return false;
  }
  printf("解析成功: 共 %zu 个媒体流\n", streams.size());

  bool all_ok = true;
  for (size_t i = 0; i < streams.size() && i < 2; i++) {
    string out = (i == 0) ? basename + ".mp4" : basename + ".m4a";
    printf("正在下载第 %zu 个流 -> %s\n", i + 1, out.c_str());
    unique_ptr<Ccurl> ptr = make_unique<Ccurl>();
    ptr->SetReferer(video_url);  /* 防盗链：以视频页 URL 作为 Referer（如 B站视频流） */
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
    printf("提示: 音视频分离流，可用 ffmpeg 合并: "
           "ffmpeg -i %s.mp4 -i %s.m4a -c copy %s_full.mp4\n",
           basename.c_str(), basename.c_str(), basename.c_str());
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

  string url;
  string filename = "./test";
  int threads = MaxThread;
  int timeout = 60;
  bool video_mode = false;
  string video_url;

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
    if (filename == "./test") {
      filename = "video";  /* 视频模式默认输出 ./video.mp4 */
    }
    if (!DownloadVideo(video_url, filename, threads, timeout)) {
      printf("视频下载失败（详见 download.log）\n");
      return 1;
    }
    return 0;
  }

  if (url.empty()) {
    PrintUsage(argv[0]);
    return 1;
  }

  unique_ptr<Ccurl> ptr = make_unique<Ccurl>();
  if (!ptr->Init(url, filename, threads, timeout)) {
    return 1;
  }
  if (!ptr->Download_Task()) {
    printf("下载失败: 存在未完成的分片（详见 download.log）\n");
    return 1;
  }

  return 0;
}
