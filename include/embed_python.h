/**
 * @file embed_python.h
 * @brief 进程内嵌入 CPython + yt_dlp 的视频直链解析（纯代码调用，无外部进程）
 *
 * 运行时资源（stdlib/、yt_dlp/）从 python_home 加载：
 *   - 默认 <可执行文件同目录>/python_runtime/
 *   - 编译期宏 PYTHON_RUNTIME_FALLBACK 指定的源码树路径（开发调试用）
 *
 * @author ErnestAgel
 * @date 2026-08-06
 * @copyright Copyright (c) 2026 ErnestAgel
 * @license SPDX-License-Identifier: MIT
 */

#ifndef EMBED_PYTHON_H
#define EMBED_PYTHON_H

#include <string>
#include <vector>

/**
 * @brief 初始化嵌入的 CPython（进程内调用一次，线程安全计数）
 * @param python_home 运行时资源根目录；为空时依次尝试：
 *                    exe 同目录/python_runtime → 环境变量 CURLBOLT_PYHOME →
 *                    编译期宏 PYTHON_RUNTIME_FALLBACK
 * @return 是否初始化成功
 */
bool EmbedPythonInit(const std::string& python_home = "");

/**
 * @brief 用嵌入的 yt_dlp 解析视频网页，得到媒体流直链列表
 * @param url 视频网页 URL（如 B站/YouTube 视频页）
 * @param urls 输出：媒体流直链列表（DASH 分离时依次为视频轨、音频轨）
 * @param cookies_from_browser 浏览器 Cookie 来源（chrome/firefox/edge，可为空）
 * @param cookie 手动 Cookie 字符串（可为空）
 * @param err 失败原因描述
 * @return 是否成功
 * @note 必须先在 EmbedPythonInit 成功后调用
 */
bool EmbedParseVideoUrls(const std::string& url,
                         std::vector<std::string>& urls,
                         const std::string& cookies_from_browser,
                         const std::string& cookie,
                         std::string& err);

/**
 * @brief 释放嵌入的 CPython 与资源（可选调用，进程退出前）
 */
void EmbedPythonShutdown();

#endif  // EMBED_PYTHON_H
