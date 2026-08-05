/**
 * @file video.h
 * @brief 视频直链解析模块：通过外部视频解析器解析视频网页 URL，得到可直接下载的媒体流地址
 *
 * @author ErnestAgel
 * @date 2026-08-06
 * @copyright Copyright (c) 2026 ErnestAgel
 * @license SPDX-License-Identifier: MIT
 */

#include <string>
#include <vector>

/**
 * @brief 通过外部视频解析器解析视频网页 URL，得到可直接下载的媒体流地址列表
 * @param url 视频网页 URL（如 B站/YouTube 等视频页）
 * @param urls 输出：媒体流 URL 列表（DASH 分离时依次为视频轨、音频轨）
 * @param cookies_from_browser 从浏览器读取登录 Cookie（chrome/firefox/edge 等，可为空）：
 *        用于解析需要登录态的高清视频流（如 B站 720p+）
 * @param cookies_file Netscape 格式 Cookie 文件路径（可为空），与 cookies_from_browser 二选一
 * @return 解析是否成功（失败时 urls 为空）
 * @note 依赖系统安装的视频解析组件（跨平台命令行工具），支持网站范围与该组件一致
 */
bool ParseVideoUrls(const std::string& url, std::vector<std::string>& urls,
                    const std::string& cookies_from_browser = "",
                    const std::string& cookies_file = "");
