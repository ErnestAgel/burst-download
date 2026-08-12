/**
 * @file taskexec.cpp
 * @brief Unified single-task executor implementation (P8 / de-duplication).
 */

#include "taskexec.h"

#include <cstdio>
#include <filesystem>
#include <memory>

#include "Ccurl.h"
#include "download_video.h"
#include "embed_python.h"
#include "threadpool.h"

namespace
{

/** @brief File download body shared by CLI and GUI. */
BOOL32 RunFileExec(const TTaskExecOptions& tOpts, TTaskExecCallbacks& tCb,
                   std::string& strOutputPath, std::string& strError)
{
    strOutputPath = tOpts.strOutput;
    if (tCb.fnOnStage)
    {
        tCb.fnOnStage(STAGE_DOWNLOADING);
    }
    if (tCb.fnOnLog)
    {
        tCb.fnOnLog("[INFO] start downloading: " + tOpts.strUrl);
    }

    std::error_code ec;
    std::filesystem::path fsPath(tOpts.strOutput);
    if (fsPath.has_parent_path() && !fsPath.parent_path().empty())
    {
        std::filesystem::create_directories(fsPath.parent_path(), ec);
    }

    std::unique_ptr<Ccurl> cc = std::make_unique<Ccurl>();
    if (tOpts.pChunkPool != nullptr)
    {
        cc->SetChunkPool(tOpts.pChunkPool);
    }
    if (!tOpts.strCookie.empty())
    {
        cc->SetCookie(tOpts.strCookie);
    }
    if (tCb.fnOnProgress)
    {
        cc->onProgress =
            [&tCb, cc_ptr = cc.get()](const std::vector<ThreadProgress>& tp,
                                      double dPercent, double dSpeed) {
                if (tCb.fnIsCanceled && (tCb.fnIsCanceled() == TRUE))
                {
                    cc_ptr->Cancel();
                }
                tCb.fnOnProgress(tp, dPercent, dSpeed);
            };
    }

    if (tCb.fnIsCanceled && (tCb.fnIsCanceled() == TRUE))
    {
        strError = "canceled";
        if (tCb.fnOnStage)
        {
            tCb.fnOnStage(STAGE_CANCELED);
        }
        return FALSE;
    }

    if (!cc->Init(tOpts.strUrl, tOpts.strOutput, tOpts.nThreads,
                  tOpts.nTimeout))
    {
        strError = cc->LastError().empty() ? "init failed"
                                           : cc->LastError();
        if (tCb.fnOnLog)
        {
            tCb.fnOnLog("[ERROR] init failed: " + tOpts.strUrl);
        }
        if (tCb.fnOnStage)
        {
            tCb.fnOnStage(STAGE_ERROR);
        }
        return FALSE;
    }
    if (tCb.fnOnProgress)
    {
        const std::vector<ThreadProgress> vecParts = cc->SnapshotParts();
        tCb.fnOnProgress(vecParts, 0.0, 0.0);
    }

    const bool bOk = cc->Download_Task();
    if (bOk)
    {
        if (tOpts.bVerifySha256 == TRUE)
        {
            std::string strDigest;
            if (!cc->VerifySha256(strDigest))
            {
                strError = "sha256 verification failed";
                if (tCb.fnOnLog)
                {
                    tCb.fnOnLog("[ERROR] " + strError);
                }
                if (tCb.fnOnStage)
                {
                    tCb.fnOnStage(STAGE_ERROR);
                }
                return FALSE;
            }
            if (tCb.fnOnLog)
            {
                tCb.fnOnLog("sha256: " + strDigest);
            }
        }
        if (tCb.fnOnLog)
        {
            tCb.fnOnLog("[INFO] download complete: " + tOpts.strOutput);
        }
        if (tCb.fnOnStage)
        {
            tCb.fnOnStage(STAGE_DONE);
        }
        return TRUE;
    }

    if (tCb.fnIsCanceled && (tCb.fnIsCanceled() == TRUE))
    {
        strError = "canceled";
        if (tCb.fnOnLog)
        {
            tCb.fnOnLog("[INFO] canceled, partial files kept for resume");
        }
        if (tCb.fnOnStage)
        {
            tCb.fnOnStage(STAGE_CANCELED);
        }
        return FALSE;
    }

    if (tOpts.bDeletePartial == TRUE)
    {
        std::remove(tOpts.strOutput.c_str());
        std::remove((tOpts.strOutput + ".curlbolt.part").c_str());
        if (tCb.fnOnLog)
        {
            tCb.fnOnLog("[INFO] partial file deleted (--delete-partial)");
        }
    }
    strError = "download failed: some chunks are incomplete (see download.log)";
    if (tCb.fnOnLog)
    {
        tCb.fnOnLog("[ERROR] " + strError);
    }
    if (tCb.fnOnStage)
    {
        tCb.fnOnStage(STAGE_ERROR);
    }
    return FALSE;
}

/** @brief Video download body shared by CLI and GUI. */
BOOL32 RunVideoExec(const TTaskExecOptions& tOpts, TTaskExecCallbacks& tCb,
                    std::string& strOutputPath, std::string& strError)
{
    strOutputPath = tOpts.strOutput;
    std::error_code ec;
    std::filesystem::path fsPath(tOpts.strOutput);
    if (fsPath.has_parent_path() && !fsPath.parent_path().empty())
    {
        std::filesystem::create_directories(fsPath.parent_path(), ec);
    }

    if (tCb.fnIsCanceled && (tCb.fnIsCanceled() == TRUE))
    {
        strError = "canceled";
        if (tCb.fnOnStage)
        {
            tCb.fnOnStage(STAGE_CANCELED);
        }
        return FALSE;
    }

    if (!EmbedPythonInit())
    {
        strError = "Python runtime init failed: assets/ (stdlib/yt_dlp) is "
                   "missing";
        if (tCb.fnOnLog)
        {
            tCb.fnOnLog("[ERROR] video parsing failed: Python runtime");
        }
        if (tCb.fnOnStage)
        {
            tCb.fnOnStage(STAGE_ERROR);
        }
        return FALSE;
    }

    if (tCb.fnOnLog)
    {
        tCb.fnOnLog("[INFO] checking parser update (10s budget)...");
    }
    {
        std::string strUpMsg;
        if (EmbedAutoUpdateParser(strUpMsg) && !strUpMsg.empty() &&
            tCb.fnOnLog)
        {
            tCb.fnOnLog("[INFO] " + strUpMsg);
        }
    }
    if (tCb.fnIsCanceled && (tCb.fnIsCanceled() == TRUE))
    {
        strError = "canceled";
        if (tCb.fnOnStage)
        {
            tCb.fnOnStage(STAGE_CANCELED);
        }
        return FALSE;
    }

    VideoDownloader vd;
    vd.SetChunkPool(tOpts.pChunkPool);
    if (tCb.fnOnStage)
    {
        vd.onStage = [&tCb, vd_ptr = &vd](int nStage) {
            if (tCb.fnIsCanceled && (tCb.fnIsCanceled() == TRUE))
            {
                vd_ptr->Cancel();
            }
            tCb.fnOnStage(nStage);
        };
    }
    if (tCb.fnOnProgress)
    {
        vd.onProgress =
            [&tCb, vd_ptr = &vd](const std::vector<ThreadProgress>& tp,
                                 double dPercent, double dSpeed) {
                if (tCb.fnIsCanceled && (tCb.fnIsCanceled() == TRUE))
                {
                    vd_ptr->Cancel();
                }
                tCb.fnOnProgress(tp, dPercent, dSpeed);
            };
    }
    if (tCb.fnOnLog)
    {
        vd.onLog = [&tCb](const std::string& strMsg) {
            tCb.fnOnLog(strMsg);
        };
    }

    const VideoResult r =
        vd.Run(tOpts.strUrl, tOpts.strOutput, tOpts.nThreads, tOpts.nTimeout,
               tOpts.strCookiesFromBrowser, tOpts.strCookie);
    if (r == VideoResult::Ok)
    {
        strOutputPath = vd.OutputPath();
        if (tCb.fnOnLog)
        {
            tCb.fnOnLog("[INFO] video download complete: " + strOutputPath);
        }
        if (tCb.fnOnStage)
        {
            tCb.fnOnStage(STAGE_DONE);
        }
        return TRUE;
    }
    if ((r == VideoResult::Canceled) ||
        (tCb.fnIsCanceled && (tCb.fnIsCanceled() == TRUE)))
    {
        strError = "canceled";
        if (tCb.fnOnLog)
        {
            tCb.fnOnLog("[INFO] canceled, partial files kept for resume");
        }
        if (tCb.fnOnStage)
        {
            tCb.fnOnStage(STAGE_CANCELED);
        }
        return FALSE;
    }
    strError = vd.LastError().empty() ? "video download failed"
                                      : vd.LastError();
    if (tCb.fnOnLog)
    {
        tCb.fnOnLog("[ERROR] video download failed: " + tOpts.strUrl);
    }
    if (tCb.fnOnStage)
    {
        tCb.fnOnStage(STAGE_ERROR);
    }
    return FALSE;
}

}  // namespace

BOOL32 TaskExecRun(const TTaskExecOptions& tOpts, TTaskExecCallbacks& tCb,
                   std::string& strOutputPath, std::string& strError)
{
    if (tOpts.strUrl.empty())
    {
        strError = "empty URL";
        return FALSE;
    }
    if (tOpts.bVideo != FALSE)
    {
        return RunVideoExec(tOpts, tCb, strOutputPath, strError);
    }
    return RunFileExec(tOpts, tCb, strOutputPath, strError);
}
