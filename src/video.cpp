/**
 * @file video.cpp
 * @brief Video URL parsing module implementation: in-process embedded
 *        CPython + yt_dlp parses video page URLs.
 *
 * Design: runtime assets (stdlib/, yt_dlp/) are loaded by the embed_python
 * module from the assets/ directory; pure in-process calls, no external
 * programs.  Parsed media stream URLs are downloaded by the project's
 * multi-threaded chunked downloader.  HD streams may carry browser login
 * cookies (--cookies-from-browser or --cookie), e.g. Bilibili 720p+.
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

bool ParseVideoUrls(const string& strUrl, vector<string>& vecUrls,
                    const string& strCookiesFromBrowser,
                    const string& strCookie, string* pstrErr,
                    const std::atomic<bool>* pbCancel) {
    string strDetail;
    if (!EmbedParseVideoUrls(strUrl, vecUrls, strCookiesFromBrowser,
                             strCookie, strDetail, pbCancel)) {
        fprintf(stderr, "[video] parsing failed: %s\n", strDetail.c_str());
        if (pstrErr) {
            *pstrErr = strDetail;
        }
        return false;
    }
    return true;
}
