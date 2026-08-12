#ifndef BURST_TASK_QUEUE_H
#define BURST_TASK_QUEUE_H

/**
 * @file taskqueue.h
 * @brief Multi-task scheduler: concurrency slots, pool execution and
 *        persistence (P5, R12).
 *
 * The queue owns each task in a stable heap entry so pool jobs never hold
 * references into a resizing container.  Executors run on the shared
 * CThreadPool and honor CTaskContext cancellation at their own checkpoints.
 */

#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "task.h"
#include "threadpool.h"

/** @brief Multi-task scheduler. */
class CTaskQueue
{
public:
    /**
     * @brief Executor contract; sets tTask.strError on failure.
     * @param tTask Task being executed (state already Running).
     * @param cCtx Per-task cancellation flag.
     * @return TRUE on success, FALSE on failure.
     */
    typedef std::function<BOOL32(TDownloadTask& tTask, CTaskContext& cCtx)>
        TTaskExecutor;

    /**
     * @brief Create a queue with fixed concurrency slots.
     * @param dwSlots Maximum simultaneously running tasks (>= 1).
     * @param cPool Shared worker pool used to execute tasks.
     */
    CTaskQueue(u32 dwSlots, CThreadPool& cPool);

    /** @brief Wait for running tasks, then release entries. */
    ~CTaskQueue();

    CTaskQueue(const CTaskQueue&) = delete;
    CTaskQueue& operator=(const CTaskQueue&) = delete;

    /**
     * @brief Append a task to the queue.
     * @return The new task id, or 0 when the URL is empty.
     */
    u64 AddTask(const std::string& strUrl, const std::string& strOutput,
                s32 nThreads, s32 nTimeout, BOOL32 bVideoMode);

    /**
     * @brief Begin scheduling; pending tasks are dispatched into the pool.
     * @param fnExecutor Callable that performs the actual download.
     */
    void Start(TTaskExecutor fnExecutor);

    /** @brief Cancel one task (running executors honor the context). */
    void CancelTask(u64 dwId);

    /** @brief Cancel every task. */
    void CancelAll();

    /** @brief Block until every task is in a terminal state. */
    void WaitAll();

    /** @brief Copy of the current task list (thread-safe). */
    std::vector<TDownloadTask> Snapshot() const;

    /** @brief Persist tasks to a file (atomic temp + rename). */
    BOOL32 Save(const std::string& strPath) const;

    /** @brief Load tasks; corrupt data resets the queue to empty. */
    BOOL32 Load(const std::string& strPath);

    /** @brief Concurrency slot count. */
    u32 Slots() const;

    /** @brief Pending plus running task count. */
    u32 ActiveCount() const;

private:
    /** @brief Stable per-task entry shared with pool jobs. */
    typedef struct tagQueueEntry
    {
        TDownloadTask tTask;
        CTaskContext  cCtx;
    } TQueueEntry;

    /** @brief Fill free slots with pending tasks (caller holds m_mutex). */
    void DispatchLocked();

    /** @brief Pool job body: run executor, finalize state, free a slot. */
    void RunTask(std::shared_ptr<TQueueEntry> pEntry);

    CThreadPool&                    m_cPool;
    u32                             m_dwSlots;
    u64                             m_dwNextId;
    BOOL32                          m_bStarted;
    TTaskExecutor                   m_fnExecutor;
    std::map<u64, std::shared_ptr<TQueueEntry> > m_mapEntries;
    u32                             m_dwRunning;
    mutable std::mutex              m_mutex;
    std::condition_variable         m_cvIdle;
};

#endif  // BURST_TASK_QUEUE_H
