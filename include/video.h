/**
 * @file video.h
 * @brief Video URL parsing module: in-process embedded CPython + yt_dlp
 *        parses video page URLs.
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
#include <atomic>

/**
 * @brief Parse a video page URL into downloadable media stream URLs.
 * @param strUrl Video page URL (e.g. Bilibili/YouTube video page).
 * @param vecUrls Output: media stream URL list (DASH order: video, audio).
 * @param strCookiesFromBrowser Browser cookie source (chrome/firefox/edge
 *        etc., may be empty).
 * @param strCookie Manual cookie string (may be empty); some streams need
 *        login state.
 * @param pstrErr Output: underlying reason on failure (may be null).
 * @return TRUE on success.
 * @note Depends on the embedded Python runtime
 *       (third_party/python/runtime); EmbedPythonInit must run first.
 */
bool ParseVideoUrls(const std::string& strUrl,
                    std::vector<std::string>& vecUrls,
                    const std::string& strCookiesFromBrowser = "",
                    const std::string& strCookie = "",
                    std::string* pstrErr = nullptr,
                    const std::atomic<bool>* pbCancel = nullptr);

#endif  // VIDEO_H
