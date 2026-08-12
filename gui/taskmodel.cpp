/**
 * @file taskmodel.cpp
 * @brief Multi-task queue model implementation (P5-4).
 */

#include "taskmodel.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <memory>
#include <thread>

#include "Ccurl.h"
#include "download_video.h"
#include "embed_python.h"

CTaskModel::CTaskModel()
    : m_cQueue(m_cPool.ThreadCount(), m_cPool), m_dwNextModelId(1)
{
    m_cQueue.Start(
        [this](TDownloadTask& tTask, CTaskContext& cCtx) -> BOOL32 {
            return RunTaskBody(tTask.dwId, tTask, cCtx);
        });
}

CTaskModel::~CTaskModel()
{
    CancelAll();
    WaitAll();
}

u64 CTaskModel::AddFileTask(const std::string& strUrl,
                            const std::string& strPath, int nThreads,
                            int nTimeout, BOOL32 bPreserveSnapshot)
{
    if (strUrl.empty() || strPath.empty())
    {
        return 0;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const u64 dwQueueId =
        m_cQueue.AddTask(strUrl, strPath, nThreads, nTimeout, FALSE);
    if (dwQueueId == 0)
    {
        return 0;
    }
    const u64 dwModelId = m_dwNextModelId++;
    std::shared_ptr<TModelTask> pTask = std::make_shared<TModelTask>();
    pTask->dwModelId = dwModelId;
    pTask->strUrl = strUrl;
    pTask->strOutput = strPath;
    pTask->nThreads = nThreads;
    pTask->nTimeout = nTimeout;
    pTask->bVideo = FALSE;
    pTask->dwQueueTaskId = dwQueueId;
    pTask->bPreserveSnapshot = bPreserveSnapshot;
    m_mapTasks.emplace(dwModelId, pTask);
    m_mapQueueToModel.emplace(dwQueueId, dwModelId);
    return dwModelId;
}

u64 CTaskModel::AddVideoTask(const std::string& strUrl,
                             const std::string& strBasename, int nThreads,
                             int nTimeout, BOOL32 bPreserveSnapshot)
{
    if (strUrl.empty() || strBasename.empty())
    {
        return 0;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const u64 dwQueueId =
        m_cQueue.AddTask(strUrl, strBasename, nThreads, nTimeout, TRUE);
    if (dwQueueId == 0)
    {
        return 0;
    }
    const u64 dwModelId = m_dwNextModelId++;
    std::shared_ptr<TModelTask> pTask = std::make_shared<TModelTask>();
    pTask->dwModelId = dwModelId;
    pTask->strUrl = strUrl;
    pTask->strOutput = strBasename;
    pTask->nThreads = nThreads;
    pTask->nTimeout = nTimeout;
    pTask->bVideo = TRUE;
    pTask->dwQueueTaskId = dwQueueId;
    pTask->bPreserveSnapshot = bPreserveSnapshot;
    m_mapTasks.emplace(dwModelId, pTask);
    m_mapQueueToModel.emplace(dwQueueId, dwModelId);
    return dwModelId;
}

void CTaskModel::CancelTask(u64 dwModelId)
{
    u64 dwQueueId = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::map<u64, std::shared_ptr<TModelTask> >::const_iterator it =
            m_mapTasks.find(dwModelId);
        if (it != m_mapTasks.end())
        {
            dwQueueId = it->second->dwQueueTaskId;
        }
    }
    if (dwQueueId != 0)
    {
        m_cQueue.CancelTask(dwQueueId);
    }
}

void CTaskModel::ResumeTask(u64 dwModelId)
{
    std::shared_ptr<TModelTask> pTask;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::map<u64, std::shared_ptr<TModelTask> >::iterator it =
            m_mapTasks.find(dwModelId);
        if (it != m_mapTasks.end())
        {
            pTask = it->second;
        }
    }
    if (pTask == nullptr)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    const u64 dwQueueId =
        m_cQueue.AddTask(pTask->strUrl, pTask->strOutput, pTask->nThreads,
                         pTask->nTimeout, pTask->bVideo);
    if (dwQueueId == 0)
    {
        return;
    }
    pTask->dwQueueTaskId = dwQueueId;
    pTask->bPreserveSnapshot = TRUE;
    m_mapQueueToModel.emplace(dwQueueId, dwModelId);
}

void CTaskModel::StopTask(u64 dwModelId)
{
    std::shared_ptr<TModelTask> pTask;
    u64 dwQueueId = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::map<u64, std::shared_ptr<TModelTask> >::iterator it =
            m_mapTasks.find(dwModelId);
        if (it == m_mapTasks.end())
        {
            return;
        }
        pTask = it->second;
        dwQueueId = pTask->dwQueueTaskId;
        m_mapTasks.erase(it);
        m_mapQueueToModel.erase(dwQueueId);
    }
    if (dwQueueId != 0)
    {
        m_cQueue.CancelTask(dwQueueId);
        /* Wait (bounded) until the executor returns so no worker still
         * touches the artifacts. */
        for (int nTry = 0; nTry < 200; ++nTry)
        {
            bool bTerminal = true;
            const std::vector<TDownloadTask> vecTasks = m_cQueue.Snapshot();
            for (const TDownloadTask& tQueueTask : vecTasks)
            {
                if (tQueueTask.dwId == dwQueueId)
                {
                    const TTaskState em = tQueueTask.emState;
                    if ((em == emTaskPending) || (em == emTaskRunning))
                    {
                        bTerminal = false;
                    }
                    break;
                }
            }
            if (bTerminal)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    DeleteArtifacts(*pTask);
}

void CTaskModel::RemoveTask(u64 dwModelId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::map<u64, std::shared_ptr<TModelTask> >::iterator it =
        m_mapTasks.find(dwModelId);
    if (it == m_mapTasks.end())
    {
        return;
    }
    m_mapQueueToModel.erase(it->second->dwQueueTaskId);
    m_mapTasks.erase(it);
}

void CTaskModel::ClearFinished()
{
    std::vector<u64> vecFinished;
    const std::vector<TDownloadTask> vecQueueTasks = m_cQueue.Snapshot();
    std::map<u64, TTaskState> mapQueueState;
    for (const TDownloadTask& tQueueTask : vecQueueTasks)
    {
        mapQueueState[tQueueTask.dwId] = tQueueTask.emState;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (std::map<u64, std::shared_ptr<TModelTask> >::iterator it =
                 m_mapTasks.begin();
             it != m_mapTasks.end(); ++it)
        {
            std::map<u64, TTaskState>::const_iterator itState =
                mapQueueState.find(it->second->dwQueueTaskId);
            const TTaskState em = (itState != mapQueueState.end())
                                      ? itState->second
                                      : it->second->dwQueueTaskId == 0
                                            ? emTaskRemoved
                                            : emTaskPending;
            if ((em == emTaskDone) || (em == emTaskError) ||
                (em == emTaskCanceled))
            {
                vecFinished.push_back(it->first);
            }
        }
        for (u64 dwId : vecFinished)
        {
            std::map<u64, std::shared_ptr<TModelTask> >::iterator it =
                m_mapTasks.find(dwId);
            if (it != m_mapTasks.end())
            {
                m_mapQueueToModel.erase(it->second->dwQueueTaskId);
                m_mapTasks.erase(it);
            }
        }
    }
}

void CTaskModel::CancelAll()
{
    m_cQueue.CancelAll();
}

void CTaskModel::WaitAll()
{
    m_cQueue.WaitAll();
}

std::vector<CTaskModel::TTaskRow> CTaskModel::Rows() const
{
    const std::vector<TDownloadTask> vecQueueTasks = m_cQueue.Snapshot();
    std::map<u64, TTaskState> mapQueueState;
    for (const TDownloadTask& tQueueTask : vecQueueTasks)
    {
        mapQueueState[tQueueTask.dwId] = tQueueTask.emState;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<TTaskRow> vecRows;
    vecRows.reserve(m_mapTasks.size());
    for (std::map<u64, std::shared_ptr<TModelTask> >::const_iterator it =
             m_mapTasks.begin();
         it != m_mapTasks.end(); ++it)
    {
        const TModelTask& tTask = *it->second;
        TTaskRow tRow;
        tRow.dwModelId = tTask.dwModelId;
        tRow.strUrl = tTask.strUrl;
        tRow.strOutput = tTask.strOutput;
        tRow.bVideo = tTask.bVideo;
        tRow.nStage = tTask.tSnap.stage;
        tRow.dPercent = tTask.tSnap.totalPercent;
        tRow.dSpeed = tTask.tSnap.totalSpeed;
        tRow.strEta = tTask.tSnap.eta;
        std::map<u64, TTaskState>::const_iterator itState =
            mapQueueState.find(tTask.dwQueueTaskId);
        tRow.emState = (itState != mapQueueState.end())
                           ? itState->second
                           : emTaskPending;
        tRow.strError = tTask.tSnap.error;
        vecRows.push_back(tRow);
    }
    return vecRows;
}

BOOL32 CTaskModel::TaskDetail(u64 dwModelId, DownloadSnapshot& tOut,
                              std::vector<std::string>& vecLogOut) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::map<u64, std::shared_ptr<TModelTask> >::const_iterator it =
        m_mapTasks.find(dwModelId);
    if (it == m_mapTasks.end())
    {
        return FALSE;
    }
    tOut = it->second->tSnap;
    vecLogOut = it->second->vecLog;
    return TRUE;
}

u32 CTaskModel::ActiveCount() const
{
    const std::vector<TDownloadTask> vecQueueTasks = m_cQueue.Snapshot();
    u32 dwActive = 0;
    for (const TDownloadTask& tQueueTask : vecQueueTasks)
    {
        if ((tQueueTask.emState == emTaskPending) ||
            (tQueueTask.emState == emTaskRunning))
        {
            ++dwActive;
        }
    }
    return dwActive;
}

BOOL32 CTaskModel::RunTaskBody(u64 dwQueueTaskId, TDownloadTask& tQueueTask,
                               CTaskContext& cCtx)
{
    (void)tQueueTask;
    std::shared_ptr<TModelTask> pTask;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::map<u64, u64>::const_iterator itMap =
            m_mapQueueToModel.find(dwQueueTaskId);
        if (itMap == m_mapQueueToModel.end())
        {
            return FALSE;
        }
        std::map<u64, std::shared_ptr<TModelTask> >::const_iterator itTask =
            m_mapTasks.find(itMap->second);
        if (itTask == m_mapTasks.end())
        {
            return FALSE;
        }
        pTask = itTask->second;
    }

    if (pTask->bVideo != FALSE)
    {
        RunVideoTask(*pTask, cCtx);
    }
    else
    {
        RunFileTask(*pTask, cCtx);
    }
    return (cCtx.IsCanceled() == TRUE) ? FALSE : TRUE;
}

void CTaskModel::LogLocked(TModelTask& tTask, const std::string& strMsg)
{
    char szTs[32] = {0};
    const time_t tNow = time(nullptr);
    struct tm* ptmNow = localtime(&tNow);
    if (ptmNow != nullptr)
    {
        strftime(szTs, sizeof(szTs), "%H:%M:%S", ptmNow);
    }
    std::string strLine = "[";
    strLine += szTs;
    strLine += "] ";
    strLine += strMsg;
    tTask.vecLog.push_back(strLine);
    if (tTask.vecLog.size() > 300)
    {
        tTask.vecLog.erase(tTask.vecLog.begin(),
                           tTask.vecLog.begin() +
                               (tTask.vecLog.size() - 300));
    }
}

void CTaskModel::SetStage(TModelTask& tTask, int nStage,
                          const std::string& strStatus)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    tTask.tSnap.stage = nStage;
    tTask.tSnap.status = strStatus;
}

std::string CTaskModel::FormatEta(double dRemain, double dSpeed)
{
    if ((dSpeed <= 0.0) || (dRemain <= 0.0))
    {
        return "--";
    }
    const double dSec = dRemain / dSpeed;
    char szBuf[32];
    if (dSec >= 3600.0)
    {
        snprintf(szBuf, sizeof(szBuf), "%02d:%02d:%02d",
                 static_cast<int>(dSec / 3600.0),
                 static_cast<int>(dSec / 60.0) % 60,
                 static_cast<int>(dSec) % 60);
    }
    else
    {
        snprintf(szBuf, sizeof(szBuf), "%02d:%02d",
                 static_cast<int>(dSec / 60.0),
                 static_cast<int>(dSec) % 60);
    }
    return std::string(szBuf);
}

void CTaskModel::RunFileTask(TModelTask& tTask, CTaskContext& cCtx)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (tTask.bPreserveSnapshot == FALSE)
        {
            tTask.tSnap = DownloadSnapshot();
            tTask.vecLog.clear();
        }
        LogLocked(tTask, "[INFO] task started: " + tTask.strUrl);
    }
    SetStage(tTask, STAGE_DOWNLOADING, "downloading");

    std::error_code ec;
    std::filesystem::path fsPath(tTask.strOutput);
    if (fsPath.has_parent_path() && !fsPath.parent_path().empty())
    {
        std::filesystem::create_directories(fsPath.parent_path(), ec);
    }

    std::unique_ptr<Ccurl> cc = std::make_unique<Ccurl>();
    cc->onProgress =
        [this, &tTask, &cCtx, cc_ptr = cc.get()](
            const std::vector<ThreadProgress>& tp, double dPercent,
            double dSpeed) {
            if (cCtx.IsCanceled() == TRUE)
            {
                cc_ptr->Cancel();
            }
            std::lock_guard<std::mutex> lock(m_mutex);
            tTask.tSnap.stage = STAGE_DOWNLOADING;
            tTask.tSnap.totalPercent = dPercent;
            tTask.tSnap.totalSpeed = dSpeed;
            tTask.tSnap.threads.assign(tp.begin(), tp.end());
            double dFileTotal = tp.empty() ? 0.0 : (double)tp[0].file_total;
            double dRemain =
                (dFileTotal > 0.0 && dPercent < 100.0)
                    ? dFileTotal * (100.0 - dPercent) / 100.0
                    : 0.0;
            tTask.tSnap.eta = FormatEta(dRemain, dSpeed);
        };

    if (cCtx.IsCanceled() == TRUE)
    {
        SetStage(tTask, STAGE_CANCELED, "canceled");
        return;
    }

    if (!cc->Init(tTask.strUrl, tTask.strOutput, tTask.nThreads,
                  tTask.nTimeout))
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            tTask.tSnap.error = cc->LastError().empty()
                                    ? "init failed"
                                    : cc->LastError();
            LogLocked(tTask, "[ERROR] init failed: " + tTask.strUrl);
        }
        SetStage(tTask, STAGE_ERROR, "error");
        return;
    }
    {
        const std::vector<ThreadProgress> vecParts = cc->SnapshotParts();
        cc->onProgress(vecParts, 0.0, 0.0);
    }

    const bool bOk = cc->Download_Task();
    if (bOk)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            tTask.tSnap.totalPercent = 100.0;
            tTask.tSnap.totalSpeed = 0.0;
            tTask.tSnap.eta = "--";
            for (ThreadProgress& t : tTask.tSnap.threads)
            {
                t.downloaded = t.total;
                t.percent = (t.file_total > 0)
                                ? (t.total / (double)t.file_total * 100.0)
                                : 100.0;
                t.speed = 0.0;
            }
            LogLocked(tTask, "[INFO] download complete: " + tTask.strOutput);
        }
        SetStage(tTask, STAGE_DONE, tTask.strOutput);
    }
    else if ((cCtx.IsCanceled() == TRUE) || cc->IsCanceled())
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            LogLocked(tTask, "[INFO] canceled, partial files kept for resume");
        }
        SetStage(tTask, STAGE_CANCELED, "canceled");
    }
    else
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            tTask.tSnap.error = "download failed, see download.log";
            LogLocked(tTask, "[ERROR] download failed: " + tTask.strUrl);
        }
        SetStage(tTask, STAGE_ERROR, "error");
    }
}

void CTaskModel::RunVideoTask(TModelTask& tTask, CTaskContext& cCtx)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (tTask.bPreserveSnapshot == FALSE)
        {
            tTask.tSnap = DownloadSnapshot();
            tTask.vecLog.clear();
        }
        LogLocked(tTask, "[INFO] video task started: " + tTask.strUrl);
    }

    std::error_code ec;
    std::filesystem::path fsPath(tTask.strOutput);
    if (fsPath.has_parent_path() && !fsPath.parent_path().empty())
    {
        std::filesystem::create_directories(fsPath.parent_path(), ec);
    }

    if (cCtx.IsCanceled() == TRUE)
    {
        SetStage(tTask, STAGE_CANCELED, "canceled");
        return;
    }

    if (!EmbedPythonInit())
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            tTask.tSnap.error =
                "Python runtime init failed: assets/ (stdlib/yt_dlp) is "
                "missing";
            LogLocked(tTask,
                      "[ERROR] video parsing failed: Python runtime");
        }
        SetStage(tTask, STAGE_ERROR, "error");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        LogLocked(tTask, "[INFO] checking parser update (10s budget)...");
    }
    std::string strUpMsg;
    if (EmbedAutoUpdateParser(strUpMsg) && !strUpMsg.empty())
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        LogLocked(tTask, "[INFO] " + strUpMsg);
    }
    if (cCtx.IsCanceled() == TRUE)
    {
        SetStage(tTask, STAGE_CANCELED, "canceled");
        return;
    }

    VideoDownloader vd;
    vd.onStage = [this, &tTask, &cCtx, vd_ptr = &vd](int nStage) {
        if (cCtx.IsCanceled() == TRUE)
        {
            vd_ptr->Cancel();
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        tTask.tSnap.stage = nStage;
        if (nStage == STAGE_AUDIO_DL)
        {
            tTask.tSnap.totalPercent = 0.0;
            tTask.tSnap.totalSpeed = 0.0;
            tTask.tSnap.eta = "--";
            tTask.tSnap.threads.clear();
        }
    };
    vd.onProgress =
        [this, &tTask, &cCtx, vd_ptr = &vd](
            const std::vector<ThreadProgress>& tp, double dPercent,
            double dSpeed) {
            if (cCtx.IsCanceled() == TRUE)
            {
                vd_ptr->Cancel();
            }
            std::lock_guard<std::mutex> lock(m_mutex);
            tTask.tSnap.totalPercent = dPercent;
            tTask.tSnap.totalSpeed = dSpeed;
            tTask.tSnap.threads.assign(tp.begin(), tp.end());
            double dFileTotal = tp.empty() ? 0.0 : (double)tp[0].file_total;
            double dRemain =
                (dFileTotal > 0.0 && dPercent < 100.0)
                    ? dFileTotal * (100.0 - dPercent) / 100.0
                    : 0.0;
            tTask.tSnap.eta = FormatEta(dRemain, dSpeed);
        };
    vd.onLog = [this, &tTask](const std::string& strMsg) {
        std::lock_guard<std::mutex> lock(m_mutex);
        LogLocked(tTask, strMsg);
    };

    const VideoResult r = vd.Run(tTask.strUrl, tTask.strOutput,
                                 tTask.nThreads, tTask.nTimeout);
    if (r == VideoResult::Ok)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            tTask.tSnap.totalPercent = 100.0;
            tTask.tSnap.totalSpeed = 0.0;
            tTask.tSnap.eta = "--";
            LogLocked(tTask, "[INFO] video download complete: " +
                                 vd.OutputPath());
        }
        SetStage(tTask, STAGE_DONE, vd.OutputPath());
    }
    else if ((r == VideoResult::Canceled) || (cCtx.IsCanceled() == TRUE))
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            LogLocked(tTask, "[INFO] canceled, partial files kept for resume");
        }
        SetStage(tTask, STAGE_CANCELED, "canceled");
    }
    else
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            tTask.tSnap.error = vd.LastError().empty()
                                    ? "video download failed"
                                    : vd.LastError();
            LogLocked(tTask, "[ERROR] video download failed: " +
                                 tTask.strUrl);
        }
        SetStage(tTask, STAGE_ERROR, "error");
    }
}

void CTaskModel::DeleteArtifacts(const TModelTask& tTask) const
{
    std::error_code ec;
    if (tTask.bVideo != FALSE)
    {
        const char* kExts[] = {".mp4", ".m4a", ".mkv", ".webm"};
        for (const char* pszExt : kExts)
        {
            std::filesystem::remove(tTask.strOutput + pszExt, ec);
            std::filesystem::remove(tTask.strOutput + "_full" + pszExt, ec);
            std::filesystem::remove(tTask.strOutput + pszExt +
                                        ".curlbolt.part",
                                    ec);
        }
    }
    else
    {
        std::filesystem::remove(tTask.strOutput, ec);
        std::filesystem::remove(tTask.strOutput + ".curlbolt.part", ec);
    }
}
