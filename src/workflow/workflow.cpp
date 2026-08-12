/**
 * @file workflow.cpp
 * @brief Unified CLI execution flows (P9): single file / batch / video.
 *
 * Behavior-preserving extraction from the former src/main.cpp: the entry
 * point parses arguments, this module runs the download.
 */

#include "workflow.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "Ccurl.h"
#include "curlmulti.h"
#include "naming.h"
#include "pathutil.h"
#include "taskexec.h"
#include "taskqueue.h"
#include "threadpool.h"

namespace
{

/** @brief Clamp the chunk thread setting to the supported 1..8 range. */
s32 ClampThreads(s32 nThreads)
{
    if (nThreads < 1)
    {
        return 1;
    }
    if (nThreads > 8)
    {
        return 8;
    }
    return nThreads;
}

/**
 * @brief Video download mode: parse media URLs and download each stream.
 * @param strVideoUrl Video page URL.
 * @param strBasename Output base name (video track .mp4 / audio track .m4a).
 * @param nThreads Download threads per stream.
 * @param nTimeout Low-speed timeout in seconds.
 * @param strCookiesFromBrowser Browser cookie source.
 * @param strCookie Manual cookie string.
 * @param pChunkEngine Shared curl_multi engine (P8-4).
 * @return TRUE when all streams downloaded and merged.
 */
bool DownloadVideo(const std::string& strVideoUrl,
                   const std::string& strBasename, s32 nThreads,
                   s32 nTimeout,
                   const std::string& strCookiesFromBrowser,
                   const std::string& strCookie,
                   CCurlMultiEngine* pChunkEngine)
{
    TTaskExecOptions tOpts;
    tOpts.strUrl = strVideoUrl;
    tOpts.strOutput = strBasename;
    tOpts.nThreads = nThreads;
    tOpts.nTimeout = nTimeout;
    tOpts.bVideo = TRUE;
    tOpts.strCookiesFromBrowser = strCookiesFromBrowser;
    tOpts.strCookie = strCookie;
    tOpts.pChunkEngine = pChunkEngine;

    TTaskExecCallbacks tCb;
    tCb.fnOnLog = [](const std::string& strMsg) {
        printf("%s\n", strMsg.c_str());
    };
    std::string strOutPath;
    std::string strError;
    return TaskExecRun(tOpts, tCb, strOutPath, strError) == TRUE;
}

/**
 * @brief Execute one plain file download (shared by single and batch mode).
 * @param tOpts Parsed CLI options (naming, verify, cookie, delete-partial).
 * @param strUrl URL to download.
 * @param strOutPath Receives the resolved output path.
 * @param pChunkEngine Shared curl_multi engine (P8-4).
 * @param pCancelFlag Task cancel flag polled by the engine (may be null).
 * @param strError Failure reason on FALSE.
 * @return TRUE on success.
 */
BOOL32 ExecuteFileTask(const TCliOptions& tOpts, const std::string& strUrl,
                       std::string& strOutPath,
                       CCurlMultiEngine* pChunkEngine,
                       std::atomic<bool>* pCancelFlag,
                       std::string& strError)
{
    std::string strFilename = tOpts.strFilename;
    if (tOpts.bOutputSet == FALSE)
    {
        std::string strBase = SanitizeFileName(UrlBaseName(strUrl));
        if (strBase.empty())
        {
            strBase = "download.dat";
        }
        strFilename = "./" + StampName(strBase);
    }
    else if (FileExists(strFilename) && (tOpts.bContinue == FALSE))
    {
        strFilename = StampName(strFilename);
        printf("target file already exists, using: %s\n",
               strFilename.c_str());
    }
    strOutPath = strFilename;

    TTaskExecOptions tOptsExec;
    tOptsExec.strUrl = strUrl;
    tOptsExec.strOutput = strFilename;
    tOptsExec.strCookie = tOpts.strCookie;
    tOptsExec.nThreads = tOpts.nThreads;
    tOptsExec.nTimeout = tOpts.nTimeout;
    tOptsExec.bVideo = FALSE;
    tOptsExec.bVerifySha256 = tOpts.bVerify;
    tOptsExec.bDeletePartial = tOpts.bDeletePartial;
    tOptsExec.pChunkEngine = pChunkEngine;
    tOptsExec.pCancelFlag = pCancelFlag;

    TTaskExecCallbacks tCb;
    tCb.fnOnLog = [](const std::string& strMsg) {
        printf("%s\n", strMsg.c_str());
    };
    return TaskExecRun(tOptsExec, tCb, strOutPath, strError);
}

}  // namespace

int RunWorkflow(const TCliOptions& tOpts)
{
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
        std::string strFilename = tOpts.strFilename;
        if (tOpts.bOutputSet == FALSE)
        {
            std::string strBase = SanitizeFileName(UrlBaseName(tOpts.strVideoUrl));
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
            strFilename = strBase + "_" + CurrentTimeStamp();
        }
        else if (VideoOutputExists(strFilename))
        {
            strFilename += "_" + CurrentTimeStamp();
            printf("output already exists, using: %s\n",
                   strFilename.c_str());
        }

        CCurlMultiEngine cChunkEngine(
            static_cast<u32>(ClampThreads(tOpts.nThreads)));
        if (!DownloadVideo(tOpts.strVideoUrl, strFilename, tOpts.nThreads,
                           tOpts.nTimeout, tOpts.strCookiesFromBrowser,
                           tOpts.strCookie, &cChunkEngine))
        {
            printf("video download failed (see download.log)\n");
            return 1;
        }
        return 0;
    }

    if (tOpts.vecUrls.empty())
    {
        printf("no download URL\n");
        return 1;
    }

    /* Single URL keeps the historical behavior exactly. */
    if (tOpts.vecUrls.size() == 1u)
    {
        CCurlMultiEngine cChunkEngine(
            static_cast<u32>(ClampThreads(tOpts.nThreads)));
        std::string strOutPath;
        std::string strError;
        return ExecuteFileTask(tOpts, tOpts.vecUrls[0], strOutPath,
                               &cChunkEngine, nullptr, strError)
                   ? 0
                   : 1;
    }

    /* Multiple URLs: run as a batch through the task queue (P5). */
    if (tOpts.bOutputSet == TRUE)
    {
        printf("-o cannot be used with multiple URLs; each URL gets a "
               "timestamped default name\n");
        return 1;
    }
    printf("batch download: %u URLs, %d concurrent\n",
           static_cast<unsigned>(tOpts.vecUrls.size()), tOpts.nJobs);

    CThreadPool cExecPool(static_cast<u32>(tOpts.nJobs));
    CCurlMultiEngine cChunkEngine(
        static_cast<u32>(ClampThreads(tOpts.nThreads)));
    CTaskQueue cQueue(static_cast<u32>(tOpts.nJobs), cExecPool);
    /* P8-4: every task keeps its full -t chunk count in flight; the shared
     * multi engine's lanes advance all concurrent tasks. */
    for (const std::string& strUrl : tOpts.vecUrls)
    {
        cQueue.AddTask(strUrl, "", tOpts.nThreads, tOpts.nTimeout, FALSE);
    }
    cQueue.Start(
        [&tOpts, &cChunkEngine](TDownloadTask& tTask,
                                CTaskContext& cCtx) -> BOOL32 {
            std::string strError;
            const BOOL32 bOk =
                ExecuteFileTask(tOpts, tTask.strUrl, tTask.strOutput,
                                &cChunkEngine, cCtx.CancelFlagPtr(),
                                strError);
            tTask.strError = strError;
            return bOk;
        });
    cQueue.WaitAll();

    u32 dwOk = 0;
    u32 dwFail = 0;
    u32 dwCanceled = 0;
    const std::vector<TDownloadTask> vecTasks = cQueue.Snapshot();
    for (const TDownloadTask& tTask : vecTasks)
    {
        if (tTask.emState == emTaskDone)
        {
            ++dwOk;
        }
        else if (tTask.emState == emTaskError)
        {
            ++dwFail;
            printf("[%llu] failed: %s (%s)\n",
                   static_cast<unsigned long long>(tTask.dwId),
                   tTask.strUrl.c_str(),
                   tTask.strError.empty() ? "download failed"
                                          : tTask.strError.c_str());
        }
        else if (tTask.emState == emTaskCanceled)
        {
            ++dwCanceled;
        }
    }
    printf("batch finished: %u ok, %u failed, %u canceled\n", dwOk, dwFail,
           dwCanceled);
    if ((dwFail == 0u) && (dwCanceled == 0u))
    {
        return 0;
    }
    if (dwOk > 0u)
    {
        return 3;  /* partial success */
    }
    return 1;
}
