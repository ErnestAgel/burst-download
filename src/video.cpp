/**
 * @file video.cpp
 * @brief 视频直链解析模块实现：调用外部视频解析器解析视频网页 URL
 *
 * 设计：不自行实现各网站解析器（避免"一个网站一个库"），统一交由系统安装的
 * 视频解析组件（支持 B站/YouTube 等主流网站）解析出媒体流直链，再由本项目
 * 的多线程分片下载器下载。高清流解析可携带浏览器登录 Cookie
 * （--cookies-from-browser 或 --cookies 文件），如 B站 720p+ 需要登录态。
 *
 * @author ErnestAgel
 * @date 2026-08-06
 * @copyright Copyright (c) 2026 ErnestAgel
 * @license SPDX-License-Identifier: MIT
 */

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "video.h"

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

using namespace std;

bool ParseVideoUrls(const string& url, vector<string>& urls,
                    const string& cookies_from_browser,
                    const string& cookies_file) {
  urls.clear();

  /* 解析器参数说明：
   *   -f "bestvideo+bestaudio/best"  优先音视频分离的最佳组合（或单文件 best）
   *   -g                             只打印媒体流直链，不下载
   *   --no-playlist                  视频 URL 属于合集时只取当前视频
   *   --cookies-from-browser <name>  从浏览器读取登录 Cookie（高清流需登录态）
   *   --cookies <file>               Netscape 格式 Cookie 文件 */
  string cmd = "yt-dlp -f \"bestvideo+bestaudio/best\" -g --no-playlist";
  if (!cookies_from_browser.empty()) {
    cmd += " --cookies-from-browser " + cookies_from_browser;
  }
  if (!cookies_file.empty()) {
    cmd += " --cookies '" + cookies_file + "'";
  }
  cmd += " '" + url + "' 2>/dev/null";

  FILE* fp = popen(cmd.c_str(), "r");
  if (fp == nullptr) {
    return false;
  }
  char line[4096];
  while (fgets(line, sizeof(line), fp) != nullptr) {
    string s = line;
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
      s.pop_back();
    }
    if (!s.empty()) {
      urls.push_back(s);
    }
  }
  int rc = pclose(fp);

  return rc == 0 && !urls.empty();
}
