#ifndef BURST_TASK_H
#define BURST_TASK_H

/**
 * @file task.h
 * @brief Download task data model and state machine (pure logic, P5).
 */

#include <atomic>
#include <string>

#include "burst_types.h"

/** @brief Task lifecycle states. */
typedef enum tagTaskState
{
    emTaskPending = 0,
    emTaskRunning,
    emTaskDone,
    emTaskError,
    emTaskCanceled,
    emTaskRemoved
} TTaskState;

/** @brief One download task (plain data; UI/CLI snapshot friendly). */
typedef struct tagDownloadTask
{
    u64         dwId;
    std::string strUrl;
    std::string strOutput;
    s32         nThreads;
    s32         nTimeout;
    BOOL32      bVideoMode;
    TTaskState  emState;
    std::string strError;
    s64         nCreatedAt;
    s64         nFinishedAt;
} TDownloadTask;

/**
 * @brief Per-task cancellation flag shared with the executor.
 *
 * The executor checks IsCanceled() at its own checkpoints; the queue only
 * sets the flag and never forcibly kills a running job.
 */
class CTaskContext
{
public:
    CTaskContext() = default;

    /** @brief Request cancellation (thread-safe). */
    void Cancel() { m_bCancel.store(TRUE); }

    /** @brief TRUE after Cancel() was called. */
    BOOL32 IsCanceled() const { return m_bCancel.load(); }

    /** @brief Clear the flag (retry/reuse). */
    void Reset() { m_bCancel.store(FALSE); }

private:
    std::atomic<BOOL32> m_bCancel{FALSE};
};

/** @brief TRUE when the task may move from Pending to Running. */
BOOL32 TaskCanStart(const TDownloadTask& tTask);

/** @brief TRUE when a terminal transition from Running is legal. */
BOOL32 TaskCanFinish(const TDownloadTask& tTask, TTaskState emNext);

/** @brief Move Pending -> Running (no-op when illegal). */
void TaskStart(TDownloadTask& tTask);

/** @brief Move Running -> Done/Error/Canceled (no-op when illegal). */
void TaskFinish(TDownloadTask& tTask, TTaskState emNext);

/** @brief Reset a terminal task back to Pending for retry. */
void TaskResetForRetry(TDownloadTask& tTask);

#endif  // BURST_TASK_H
