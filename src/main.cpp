/**
 * @file main.cpp
 * @brief 程序入口：多线程分片下载命令行工具
 *
 * 用法: curl_download <url> [-o 文件名] [-t 线程数]
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
#include "Ccurl.h"

using namespace std;

/**
 * @brief 打印用法说明
 * @param prog 程序名
 */
static void PrintUsage(const char* prog) {
  printf("Usage: %s <url> [-o filename] [-t threads] [--timeout sec] [--no-timeout]\n", prog);
  printf("  <url>         下载地址\n");
  printf("  -o filename   保存的文件名（默认 ./test）\n");
  printf("  -t threads    下载线程数 1~%d（默认 %d）\n", MaxThread, MaxThread);
  printf("  --timeout N   下载无进展 N 秒后自动中断（默认 60，0 表示不限）\n");
  printf("  --no-timeout  强制下载不自动中断（等价 --timeout 0）\n");
  printf("  -h, --help    显示本帮助\n");
  printf("示例: %s https://example.com/file.iso -o file.iso -t 8 --timeout 30\n", prog);
  printf("日志: 超时中断/失败/完成详情写入 download.log\n");
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

  /* 帮助参数可作为第一个参数 */
  if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
    PrintUsage(argv[0]);
    return 0;
  }

  string url = argv[1];
  string filename = "./test";
  int threads = MaxThread;
  int timeout = 60;  /* 默认：60 秒无进展自动中断 */

  for (int i = 2; i < argc; i++) {
    if (strcmp(argv[i], "-o") == 0 && i + 1 < argc && argv[i + 1][0] != '-') {
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
    } else {
      printf("未知参数: %s\n", argv[i]);
      PrintUsage(argv[0]);
      return 1;
    }
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
