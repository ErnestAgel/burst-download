/**
 * @file main.cpp
 * @brief CLI entry point (RunCli): multi-threaded chunked downloader with
 *        video download mode (--video).
 *
 * Usage:
 *   burst <url> [-o filename] [-t threads] [--timeout sec] [--no-timeout]
 *   burst --video <video-url> [-o basename] [-t threads] [--timeout sec]
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
#include "avmerge.h"
#include "cli_parse.h"
#include "download_video.h"
#include "embed_python.h"
#include "pathutil.h"
#include "version.h"
#include "video.h"

using namespace std;

/**
 * @brief Check whether a file already exists.
 * @param strPath File path to check.
 * @return TRUE when the file exists.
 */
static bool FileExists(const std::string& strPath)
{
    return access(strPath.c_str(), F_OK) == 0;
}

/**
 * @brief Check whether any video-mode output already exists.
 * @param strBasename Output base name.
 * @return TRUE when a video/audio/merged output already exists.
 */
static bool VideoOutputExists(const std::string& strBasename)
{
    const char* kExts[] = {".mp4", ".m4a", ".mkv", ".webm"};
    for (const char* pszExt : kExts)
    {
        if (FileExists(strBasename + pszExt) ||
            FileExists(strBasename + "_full" + pszExt))
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Current local timestamp string (YYYYMMDD_HHMMSS, default naming).
 * @return Timestamp string.
 */
static std::string CurrentTimeStamp()
{
    char szBuf[32];
    const time_t tNow = time(nullptr);
    struct tm* ptmNow = localtime(&tNow);
    if (ptmNow != nullptr)
    {
        strftime(szBuf, sizeof(szBuf), "%Y%m%d_%H%M%S", ptmNow);
    }
    else
    {
        snprintf(szBuf, sizeof(szBuf), "%lld", (long long)tNow);
    }
    return std::string(szBuf);
}

/**
 * @brief Append a timestamp before the extension: file.iso -> file_ts.iso.
 * @param strBase Base name.
 * @return Stamped name.
 */
static std::string StampName(const std::string& strBase)
{
    const std::string strTs = CurrentTimeStamp();
    const size_t nDot = strBase.find_last_of('.');
    const size_t nSlash = strBase.find_last_of("/\\");
    if ((nDot != std::string::npos) &&
        ((nSlash == std::string::npos) || (nDot > nSlash)))
    {
        return strBase.substr(0, nDot) + "_" + strTs + strBase.substr(nDot);
    }
    return strBase + "_" + strTs;
}

/**
 * @brief Print command-line usage.
 * @param pszProg Program name.
 */
static void PrintUsage(const char* pszProg)
{
    printf("burst %s (Burst Download)\n", BURST_VERSION_STRING);
    printf("Usage: %s <url> [-o filename] [-t threads] [--timeout sec] "
           "[--no-timeout]\n", pszProg);
    printf("       %s --video <video-url> [-o basename] [-t threads] "
           "[--timeout sec]\n", pszProg);
    printf("  <url>          download URL\n");
    printf("  --video <url>  video mode: parse a video page URL (Bilibili / "
           "YouTube etc.),\n");
    printf("                 resolve media stream URLs and download them with "
           "the\n");
    printf("                 multi-threaded chunked downloader (built-in "
           "parser)\n");
    printf("  --cookies-from-browser <name>  video mode: read login cookies "
           "from a browser\n");
    printf("                 (chrome/firefox/edge etc.) for logged-in HD "
           "streams\n");
    printf("  --cookie <str> request Cookie (e.g. \"SESSDATA=xxx; "
           "bili_jct=xxx\"),\n");
    printf("                 applies to video streams and plain downloads\n");
    printf("  -o filename    output file name (default: URL-derived name + "
           "timestamp to\n");
    printf("                 avoid overwrite; --video uses it as the output "
           "base name)\n");
    printf("  -t threads     download threads 1~%d (default adapts to CPU "
           "cores: %d)\n",
           BurstMaxThreads(), BurstDefaultThreads());
    printf("  --timeout N    abort after N seconds without progress (default "
           "60, 0 = unlimited)\n");
    printf("  --no-timeout   disable automatic timeout (same as --timeout "
           "0)\n");
    printf("  --update-parser  update the built-in video parser to the latest "
           "version (needs network)\n");
    printf("  --no-auto-update  video mode: disable automatic parser update "
           "checks (default on, throttled to once per 24h)\n");
    printf("  -h, --help     show this help\n");
    printf("  -v, --version  show version\n");
    printf("Examples:\n");
    printf("  %s https://example.com/file.iso -o file.iso -t 8 --timeout "
           "30\n", pszProg);
    printf("  %s --video https://www.bilibili.com/video/BVxxxx -o movie -t "
           "8\n", pszProg);
    printf("  %s --video https://www.bilibili.com/video/BVxxxx -o movie "
           "--cookies-from-browser chrome\n", pszProg);
    printf("  %s https://example.com/private.zip -o p.zip --cookie "
           "\"SESSDATA=xxx\"\n", pszProg);
    printf("Logs: timeout interruptions, failures and completions are "
           "written to download.log\n");
}

/**
 * @brief Video download mode: parse media URLs and download each stream.
 * @param strVideoUrl Video page URL.
 * @param strBasename Output base name (video track .mp4 / audio track .m4a).
 * @param nThreads Download threads per stream.
 * @param nTimeout Low-speed timeout in seconds.
 * @param strCookiesFromBrowser Browser cookie source.
 * @param strCookie Manual cookie string.
 * @return TRUE when all streams downloaded and merged.
 */
static bool DownloadVideo(const std::string& strVideoUrl,
                          const std::string& strBasename, s32 nThreads,
                          s32 nTimeout,
                          const std::string& strCookiesFromBrowser,
                          const std::string& strCookie)
{
    /* Orchestration lives in src/download_video.* (shared with the GUI);
     * the CLI sets no callbacks, so the default printf output is used. */
    VideoDownloader vd;
    const VideoResult r = vd.Run(strVideoUrl, strBasename, nThreads,
                                 nTimeout, strCookiesFromBrowser, strCookie);
    return r == VideoResult::Ok;
}

/**
 * @brief Program entry for the CLI mode.
 * @param nArgc Argument count.
 * @param ppszArgv Argument vector.
 * @return Exit code (0 success, 1 failure or usage error).
 */
int RunCli(int argc, char** argv)
{
    TCliOptions tOpts = {};
    std::string strError;
    if (!CliParseArgs((s32)argc, argv, tOpts, BurstDefaultThreads(),
                      BurstMaxThreads(), strError))
    {
        if (!strError.empty())
        {
            printf("%s\n", strError.c_str());
        }
        PrintUsage(argv[0]);
        return 1;
    }

    if (tOpts.emAction == emCliActionVersion)
    {
        printf("burst %s (Burst Download)\n", BURST_VERSION_STRING);
        printf("platform: ");
#ifdef _WIN32
        printf("Windows x86_64\n");
#elif defined(__aarch64__)
        printf("Linux aarch64\n");
#else
        printf("Linux x86_64\n");
#endif
        return 0;
    }
    if (tOpts.emAction == emCliActionHelp)
    {
        PrintUsage(argv[0]);
        return 0;
    }
    if (tOpts.emAction == emCliActionUpdateParser)
    {
        std::string strMsg;
        if (!EmbedUpdateParser(argv[0], strMsg))
        {
            printf("update failed: %s\n", strMsg.c_str());
            return 1;
        }
        printf("%s\n", strMsg.c_str());
        return 0;
    }

    /* Initialize the embedded Python runtime (locate order: assets/ next to
     * the exe, temp cache, CURLBOLT_PYHOME, compile-time source tree). */
    EmbedPythonInit();

    /* Video download mode. */
    if (tOpts.bVideoMode == TRUE)
    {
        if (tOpts.strVideoUrl.empty())
        {
            printf("missing --video value\n");
            return 1;
        }
        /* When -o is omitted, derive a base name from the video page URL and
         * append a timestamp; when -o is given but the output exists, append
         * a timestamp to avoid overwriting. */
        if (tOpts.bOutputSet == FALSE)
        {
            std::string strBase =
                SanitizeFileName(UrlBaseName(tOpts.strVideoUrl));
            if (strBase.empty())
            {
                strBase = "video";
            }
            const size_t nDot = strBase.find_last_of('.');
            if (nDot != std::string::npos)
            {
                strBase = strBase.substr(0, nDot);  /* base name w/o ext */
            }
            if (strBase.empty())
            {
                strBase = "video";
            }
            tOpts.strFilename = strBase + "_" + CurrentTimeStamp();
        }
        else if (VideoOutputExists(tOpts.strFilename))
        {
            tOpts.strFilename += "_" + CurrentTimeStamp();
            printf("output already exists, using: %s\n",
                   tOpts.strFilename.c_str());
        }
        /* Auto-update the parser (24h throttle; failures are silent and do
         * not block parsing). */
        if (tOpts.bAutoUpdateParser == TRUE)
        {
            std::string strUpMsg;
            if (EmbedAutoUpdateParser(strUpMsg) && !strUpMsg.empty())
            {
                printf("%s\n", strUpMsg.c_str());
            }
        }
        if (!DownloadVideo(tOpts.strVideoUrl, tOpts.strFilename,
                           tOpts.nThreads, tOpts.nTimeout,
                           tOpts.strCookiesFromBrowser, tOpts.strCookie))
        {
            printf("video download failed (see download.log)\n");
            return 1;
        }
        return 0;
    }

    if (tOpts.strUrl.empty())
    {
        PrintUsage(argv[0]);
        return 1;
    }

    /* When -o is omitted, derive the name from the URL plus a timestamp;
     * when -o is given but the file exists, append a timestamp. */
    if (tOpts.bOutputSet == FALSE)
    {
        std::string strBase = SanitizeFileName(UrlBaseName(tOpts.strUrl));
        if (strBase.empty())
        {
            strBase = "download.dat";
        }
        tOpts.strFilename = "./" + StampName(strBase);
    }
    else if (FileExists(tOpts.strFilename))
    {
        tOpts.strFilename = StampName(tOpts.strFilename);
        printf("target file already exists, using: %s\n",
               tOpts.strFilename.c_str());
    }

    unique_ptr<Ccurl> ptr = make_unique<Ccurl>();
    if (!tOpts.strCookie.empty())
    {
        /* Plain downloads may carry cookies. */
        ptr->SetCookie(tOpts.strCookie);
    }
    if (!ptr->Init(tOpts.strUrl, tOpts.strFilename, tOpts.nThreads,
                   tOpts.nTimeout))
    {
        return 1;
    }
    if (!ptr->Download_Task())
    {
        printf("download failed: some chunks are incomplete "
               "(see download.log)\n");
        return 1;
    }

    return 0;
}
