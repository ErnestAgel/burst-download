/**
 * @file video.h
 * @brief 视频直链解析模块：进程内嵌入 CPython + yt_dlp 解析视频网页 URL
 *
 * @author ErnestAgel
 * @date 2026-08-06
 * @copyright Copyright (c) 2026 ErnestAgel
 * @license SPDX-License-Identifier: MIT
 */

#ifndef VIDEO_H
#define VIDEO_H

#include <string>
#include <vector>

/**
 * @brief 解析视频网页 URL，得到可直接下载的媒体流地址列表
 * @param url 视频网页 URL（如 B站/YouTube 等视频页）
 * @param urls 输出：媒体流 URL 列表（DASH 分离时依次为视频轨、音频轨）
 * @param cookies_from_browser 浏览器 Cookie 来源（chrome/firefox/edge 等，可为空）
 * @param cookie 手动 Cookie 字符串（可为空），部分流需登录态
 * @param err 输出：失败时的底层原因（可为空指针）
 * @return 是否成功
 * @note 依赖嵌入的 Python 运行时（third_party/python/runtime），须先 EmbedPythonInit
 */
bool ParseVideoUrls(const std::string& url, std::vector<std::string>& urls,
                    const std::string& cookies_from_browser = "",
                    const std::string& cookie = "",
                    std::string* err = nullptr);

#endif  // VIDEO_H
