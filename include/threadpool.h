#ifndef BURST_THREADPOOL_H
#define BURST_THREADPOOL_H

/**
 * @file threadpool.h
 * @brief Fixed-size worker thread pool with guaranteed reclamation (R12).
 *
 * Design notes:
 * - Workers are created once and reused across jobs, so tasks no longer
 *   create/destroy a std::thread per invocation.
 * - The pool never cancels or kills a running job; jobs must honor their
 *   own cancellation checkpoints so shutdown stays bounded.
 * - Shutdown() drains queued jobs, then joins every worker; the destructor
 *   performs the same, so no thread is ever left joinable (no std::terminate
 *   path, see issue-analysis R1/R12).
 * - Jobs must not submit-and-wait on the same pool (would exhaust workers).
 */

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "burst_types.h"

/** @brief Fixed-size worker thread pool for reusable task execution. */
class CThreadPool
{
public:
    /**
     * @brief Create a pool with the given number of worker threads.
     * @param dwThreadCount Worker count (clamped to at least 1).
     */
    explicit CThreadPool(u32 dwThreadCount);

    /**
     * @brief Drain queued jobs, join all workers, then release resources.
     */
    ~CThreadPool();

    CThreadPool(const CThreadPool&) = delete;
    CThreadPool& operator=(const CThreadPool&) = delete;

    /**
     * @brief Queue a job for execution by an idle worker.
     * @param fnJob Callable body; exceptions thrown inside are contained so
     *             the pool keeps running.
     * @param fJobOut Optional future that becomes ready when the job ends.
     * @return TRUE when accepted; FALSE after Shutdown() has begun.
     */
    BOOL32 Submit(std::function<void()> fnJob,
                  std::future<void>* fJobOut = nullptr);

    /**
     * @brief Number of jobs waiting for a worker plus jobs being executed.
     */
    u32 PendingCount() const;

    /** @brief Number of worker threads. */
    u32 ThreadCount() const;

    /**
     * @brief Stop accepting jobs; queued jobs still run to completion,
     *        then every worker is joined.
     */
    void Shutdown();

private:
    /** @brief Queued job entry. */
    typedef struct tagQueuedJob
    {
        u64                   dwId;
        std::function<void()> fnBody;
    } TQueuedJob;

    /** @brief Worker loop: pop a job, run it, repeat until drained. */
    void WorkerLoop();

    std::vector<std::thread> m_vecWorkers;
    std::deque<TQueuedJob>   m_deqPending;
    mutable std::mutex       m_mutex;
    std::condition_variable  m_cvWork;
    u64                      m_dwNextId;
    u32                      m_dwOutstanding;
    BOOL32                   m_bShutdown;
};

#endif  // BURST_THREADPOOL_H
