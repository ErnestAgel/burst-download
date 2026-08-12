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
#include "taskexec.h"

CTaskModel::CTaskModel()
    : m_cQueue(m_cExecPool.ThreadCount(), m_cExecPool), m_dwNextModelId(1)
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
    EnsureChunkEngine(nThreads);
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
    pTask->bPendingRemove = FALSE;
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
    EnsureChunkEngine(nThreads);
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
    pTask->bPendingRemove = FALSE;
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
    }
    if (dwQueueId == 0)
    {
        return;
    }

    /* Still queued (not running): cancel marks it terminal synchronously,
     * so remove and delete artifacts right away. */
    TTaskState em = emTaskRunning;
    for (const TDownloadTask& tQueueTask : m_cQueue.Snapshot())
    {
        if (tQueueTask.dwId == dwQueueId)
        {
            em = tQueueTask.emState;
            break;
        }
    }
    if (em == emTaskPending)
    {
        m_cQueue.CancelTask(dwQueueId);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_mapQueueToModel.erase(dwQueueId);
            m_mapTasks.erase(dwModelId);
        }
        DeleteArtifacts(*pTask);
        return;
    }

    /* Running: mark for removal and cancel; the executor deletes artifacts
     * and drops the row when the task ends (no UI blocking). */
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        pTask->bPendingRemove = TRUE;
    }
    m_cQueue.CancelTask(dwQueueId);
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

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (pTask->bPreserveSnapshot == FALSE)
        {
            pTask->tSnap = DownloadSnapshot();
            pTask->vecLog.clear();
        }
        LogLocked(*pTask, pTask->bVideo != FALSE
                              ? "[INFO] video task started: " + pTask->strUrl
                              : "[INFO] task started: " + pTask->strUrl);
    }
    /* Immediate stage feedback so the UI does not look frozen during slow
     * parsing (2026-08-12). */
    SetStage(*pTask, pTask->bVideo != FALSE ? STAGE_PARSING : STAGE_DOWNLOADING,
             pTask->bVideo != FALSE ? "parsing" : "downloading");

    TTaskExecOptions tOpts;
    tOpts.strUrl = pTask->strUrl;
    tOpts.strOutput = pTask->strOutput;
    tOpts.nThreads = pTask->nThreads;
    tOpts.nTimeout = pTask->nTimeout;
    tOpts.bVideo = pTask->bVideo;
    tOpts.bVerifySha256 = FALSE;
    tOpts.bDeletePartial = FALSE;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        tOpts.pChunkEngine = m_pChunkEngine.get();
    }
    /* P8-4: no fair-split budget.  Every task keeps its full chunk count
     * in flight; the shared engine's lanes advance all tasks concurrently. */

    TTaskExecCallbacks tCb;
    tCb.fnOnStage = [this, pTask](int nStage) {
        std::lock_guard<std::mutex> lock(m_mutex);
        pTask->tSnap.stage = nStage;
        if (nStage == STAGE_AUDIO_DL)
        {
            pTask->tSnap.totalPercent = 0.0;
            pTask->tSnap.totalSpeed = 0.0;
            pTask->tSnap.eta = "--";
            pTask->tSnap.threads.clear();
        }
    };
    tCb.fnOnProgress =
        [this, pTask](const std::vector<ThreadProgress>& tp, double dPercent,
                      double dSpeed) {
            std::lock_guard<std::mutex> lock(m_mutex);
            pTask->tSnap.totalPercent = dPercent;
            pTask->tSnap.totalSpeed = dSpeed;
            pTask->tSnap.threads.assign(tp.begin(), tp.end());
            double dFileTotal = tp.empty() ? 0.0 : (double)tp[0].file_total;
            double dRemain =
                (dFileTotal > 0.0 && dPercent < 100.0)
                    ? dFileTotal * (100.0 - dPercent) / 100.0
                    : 0.0;
            pTask->tSnap.eta = FormatEta(dRemain, dSpeed);
        };
    tCb.fnOnLog = [this, pTask](const std::string& strMsg) {
        std::lock_guard<std::mutex> lock(m_mutex);
        LogLocked(*pTask, strMsg);
    };
    tCb.fnIsCanceled = [&cCtx]() { return cCtx.IsCanceled(); };

    std::string strOutput;
    std::string strError;
    const BOOL32 bOk = TaskExecRun(tOpts, tCb, strOutput, strError);

    int nFinalStage = STAGE_ERROR;
    std::string strStatus = "error";
    if (bOk != FALSE)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            pTask->tSnap.totalPercent = 100.0;
            pTask->tSnap.totalSpeed = 0.0;
            pTask->tSnap.eta = "--";
            for (ThreadProgress& t : pTask->tSnap.threads)
            {
                t.downloaded = t.total;
                t.percent = (t.file_total > 0)
                                ? (t.total / (double)t.file_total * 100.0)
                                : 100.0;
                t.speed = 0.0;
            }
            pTask->strOutput = strOutput;
        }
        nFinalStage = STAGE_DONE;
        strStatus = strOutput;
    }
    else if (cCtx.IsCanceled() == TRUE)
    {
        nFinalStage = STAGE_CANCELED;
        strStatus = "canceled";
    }
    else
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        pTask->tSnap.error = strError.empty() ? "task failed" : strError;
    }
    SetStage(*pTask, nFinalStage, strStatus);

    /* Stop requested while running: delete artifacts and drop the row now
     * that the executor has finished (non-blocking from the UI). */
    if (pTask->bPendingRemove != FALSE)
    {
        DeleteArtifacts(*pTask);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_mapQueueToModel.erase(pTask->dwQueueTaskId);
        m_mapTasks.erase(pTask->dwModelId);
    }
    return (bOk != FALSE) ? TRUE : FALSE;
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

void CTaskModel::EnsureChunkEngine(int nThreads)
{
    if (m_pChunkEngine != nullptr)
    {
        return;
    }
    int n = nThreads < 1 ? 1 : (nThreads > 8 ? 8 : nThreads);
    m_pChunkEngine = std::make_unique<CCurlMultiEngine>(static_cast<u32>(n));
}

void CTaskModel::OnUiTick()
{
    if (m_cQueue.ActiveCount() == 0u)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pChunkEngine.reset();
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
