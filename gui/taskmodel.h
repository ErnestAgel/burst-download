/**
 * @file taskmodel.h
 * @brief Multi-task queue model for the GUI (P5-4).
 *
 * CTaskModel owns the shared CThreadPool + CTaskQueue and one UI-facing
 * task entry per download.  Each entry keeps its own DownloadSnapshot and
 * ring log; the UI reads Rows()/TaskDetail() under a mutex and issues
 * Add/Cancel/Resume/Stop/Remove through the model.  Executor bodies run on
 * pool threads and never touch ImGui.
 */

#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../src/progress.h"
#include "task.h"
#include "taskqueue.h"
#include "threadpool.h"

/** @brief Multi-task queue model for the GUI. */
class CTaskModel
{
public:
    CTaskModel();
    ~CTaskModel();

    CTaskModel(const CTaskModel&) = delete;
    CTaskModel& operator=(const CTaskModel&) = delete;

    /** @brief One row of the UI task table. */
    typedef struct tagTaskRow
    {
        u64         dwModelId;
        std::string strUrl;
        std::string strOutput;
        BOOL32      bVideo;
        TTaskState  emState;
        std::string strError;
        int         nStage;
        double      dPercent;
        double      dSpeed;
        std::string strEta;
    } TTaskRow;

    /**
     * @brief Add a file download task.
     * @return Model id (> 0) or 0 when input is invalid.
     */
    u64 AddFileTask(const std::string& strUrl, const std::string& strPath,
                    int nThreads, int nTimeout, BOOL32 bPreserveSnapshot);

    /**
     * @brief Add a video download task.
     * @return Model id (> 0) or 0 when input is invalid.
     */
    u64 AddVideoTask(const std::string& strUrl, const std::string& strBasename,
                     int nThreads, int nTimeout, BOOL32 bPreserveSnapshot);

    /** @brief Cancel a running task (partial files kept for resume). */
    void CancelTask(u64 dwModelId);

    /** @brief Re-queue a canceled task with preserved progress. */
    void ResumeTask(u64 dwModelId);

    /** @brief Cancel and remove a task, deleting its artifacts. */
    void StopTask(u64 dwModelId);

    /** @brief Remove a terminal task from the list. */
    void RemoveTask(u64 dwModelId);

    /** @brief Remove every terminal task. */
    void ClearFinished();

    /** @brief UI housekeeping: destroy the idle download pool (P8). */
    void OnUiTick();

    /** @brief Cancel all running tasks (exit path). */
    void CancelAll();

    /** @brief Wait until every task reaches a terminal state (exit path). */
    void WaitAll();

    /** @brief Snapshot of the visible task rows. */
    std::vector<TTaskRow> Rows() const;

    /** @brief Detail snapshot + log for one task. */
    BOOL32 TaskDetail(u64 dwModelId, DownloadSnapshot& tOut,
                      std::vector<std::string>& vecLogOut) const;

    /** @brief Number of tasks still pending or running. */
    u32 ActiveCount() const;

private:
    typedef struct tagModelTask
    {
        u64                dwModelId;
        std::string        strUrl;
        std::string        strOutput;
        int                nThreads;
        int                nTimeout;
        BOOL32             bVideo;
        u64                dwQueueTaskId;
        BOOL32             bPreserveSnapshot;
        BOOL32             bPendingRemove;
        DownloadSnapshot   tSnap;
        std::vector<std::string> vecLog;
    } TModelTask;

    /** @brief Queue executor: run the unified task executor for the task. */
    BOOL32 RunTaskBody(u64 dwQueueTaskId, TDownloadTask& tQueueTask,
                       CTaskContext& cCtx);

    /** @brief Lazily create the shared download pool (size = thread setting). */
    void EnsureChunkPool(int nThreads);

    /** @brief Append a timestamped log line (caller holds m_mutex). */
    void LogLocked(TModelTask& tTask, const std::string& strMsg);

    /** @brief Set stage + status under lock. */
    void SetStage(TModelTask& tTask, int nStage,
                  const std::string& strStatus);

    /** @brief Delete output artifacts of a stopped task. */
    void DeleteArtifacts(const TModelTask& tTask) const;

    /** @brief Format ETA text ("--" when speed is zero). */
    static std::string FormatEta(double dRemain, double dSpeed);

    CThreadPool m_cExecPool{2};               /**< task orchestration pool */
    CTaskQueue  m_cQueue;
    std::unique_ptr<CThreadPool> m_pChunkPool; /**< shared download pool (P8) */
    mutable std::mutex m_mutex;
    std::map<u64, std::shared_ptr<TModelTask> > m_mapTasks;
    std::map<u64, u64> m_mapQueueToModel;
    u64 m_dwNextModelId;
};
