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
#include "embedded_runtime.h"
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
    if (tOpts.pChunkEngine != nullptr)
    {
        cc->SetChunkEngine(tOpts.pChunkEngine);
    }
    cc->SetExternalCancel(tOpts.pCancelFlag);
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

    auto fnRunOnce = [&](VideoResult& rOut, std::string& strOutPathOut,
                         std::string& strLastErr, BOOL32& bParseOkOut) {
        VideoDownloader vd;
        vd.SetChunkEngine(tOpts.pChunkEngine);
        vd.SetExternalCancel(tOpts.pCancelFlag);
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
        rOut = vd.Run(tOpts.strUrl, tOpts.strOutput, tOpts.nThreads,
                      tOpts.nTimeout, tOpts.strCookiesFromBrowser,
                      tOpts.strCookie);
        strOutPathOut = vd.OutputPath();
        strLastErr = vd.LastError();
        bParseOkOut = vd.ParseOk();
    };

    VideoResult r = VideoResult::Error;
    BOOL32 bParseOk = FALSE;
    fnRunOnce(r, strOutputPath, strError, bParseOk);
    if ((r == VideoResult::Error) && (bParseOk == FALSE))
    {
        /* Parse failed: force an update, then retry parsing once. */
        if (tCb.fnOnLog)
        {
            tCb.fnOnLog("[INFO] parsing failed; updating parser and "
                        "retrying...");
        }
        std::string strUpMsg;
        EmbedUpdateParser(EmbedGetExePath(), strUpMsg);
        if (!strUpMsg.empty() && tCb.fnOnLog)
        {
            tCb.fnOnLog("[INFO] " + strUpMsg);
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
        VideoResult r2 = VideoResult::Error;
        BOOL32 bParseOk2 = FALSE;
        fnRunOnce(r2, strOutputPath, strError, bParseOk2);
        r = r2;
    }

    if (r == VideoResult::Ok)
    {
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
    if (strError.empty())
    {
        strError = "video download failed";
    }
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
