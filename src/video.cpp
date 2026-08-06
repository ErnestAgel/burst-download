/**
 * @file video.cpp
 * @brief 视频直链解析模块实现：进程内嵌入 CPython + yt_dlp 解析视频网页 URL
 *
 * 设计：运行时资源（stdlib/、yt_dlp/）由 embed_python 模块从 python_runtime/
 * 目录加载，纯进程内代码调用，不依赖任何外部程序。解析得到媒体流直链后
 * 交由本项目多线程分片下载器下载。高清流解析可携带浏览器登录 Cookie
 * （--cookies-from-browser 或 --cookie），如 B站 720p+ 需要登录态。
 *
 * @author ErnestAgel
 * @date 2026-08-06
 * @copyright Copyright (c) 2026 ErnestAgel
 * @license SPDX-License-Identifier: MIT
 */

#include <cstdio>
#include <string>
#include <vector>
#include "embed_python.h"
#include "video.h"

using namespace std;

bool ParseVideoUrls(const string& url, vector<string>& urls,
                    const string& cookies_from_browser,
                    const string& cookie) {
  string err;
  if (!EmbedParseVideoUrls(url, urls, cookies_from_browser, cookie, err)) {
    fprintf(stderr, "[video] 解析失败: %s\n", err.c_str());
    return false;
  }
  return true;
}
