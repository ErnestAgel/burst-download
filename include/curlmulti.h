/**
 * @file curlmulti.h
 * @brief Shared curl_multi transfer engine (P8-4).
 *
 * The pre-P8-4 chunk model ran one blocking curl_easy_perform per pool
 * worker, so a task could only keep as many connections active as the
 * threads it was granted.  This engine replaces that model: each lane
 * (driver thread) owns one CURLM handle and advances any number of active
 * transfers non-blockingly.  A task therefore keeps its full chunk count
 * in flight (8-thread pool -> 8 slice connections per task), while several
 * tasks share the same fixed lane count and all advance asynchronously.
 *
 * Design notes:
 * - Lane count equals the user's thread setting (1..8).  Each lane is one
 *   driver thread plus one CURLM handle; libcurl state is never shared
 *   across lanes, so no cross-thread CURLM access occurs.
 * - A job is {opaque user data, easy-handle factory, completion callback,
 *   optional cancel flag}.  The completion callback runs on the lane
 *   thread and may re-submit the job through SubmitChunkDelayed (retry
 *   with backoff); retries stay on the lane that started the chunk.
 * - Canceled jobs are removed from the multi handle promptly by the driver
 *   loop; canceled or failed-to-start jobs complete with
 *   CURLE_ABORTED_BY_CALLBACK.
 * - Shutdown cancels every active/pending/retry job and joins all driver
 *   threads, so waiters are always released (no thread is left joinable).
 */

#ifndef BURST_CURLMULTI_H
#define BURST_CURLMULTI_H

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <curl/curl.h>

#include "burst_types.h"

/** @brief Lane that has not been assigned to a job yet. */
static const u32 kChunkLaneUnset = 0xFFFFFFFFu;

/** @brief One chunk transfer handled by the engine. */
typedef struct tagChunkJob
{
    void* pUserData;  /**< Task context (opaque to the engine) */
    /** @brief Build and configure one transfer attempt; NULL means the
     *         attempt cannot start (canceled/init failure). */
    std::function<CURL*()> fnCreateEasy;
    /** @brief Completion decision; runs on the lane thread and may
     *         re-submit the job (retry) or finalize it. */
    std::function<void(CURLcode, long)> fnDone;
    std::atomic<bool>* pCancelFlag;  /**< Optional cancellation flag */
    u32 dwLane;                      /**< Assigned lane (kChunkLaneUnset) */
} TChunkJob;

/**
 * @brief Shared non-blocking curl_multi engine (P8-4).
 */
class CCurlMultiEngine
{
public:
    /**
     * @brief Create an engine with the given lane count.
     * @param dwLaneCount Driver threads / CURLM handles (clamped to >= 1;
     *                    callers use the user's 1..8 thread setting).
     */
    explicit CCurlMultiEngine(u32 dwLaneCount);

    /**
     * @brief Cancel every job, join all drivers, then release resources.
     */
    ~CCurlMultiEngine();

    CCurlMultiEngine(const CCurlMultiEngine&) = delete;
    CCurlMultiEngine& operator=(const CCurlMultiEngine&) = delete;

    /**
     * @brief Queue one chunk attempt for immediate execution.
     * @param tJob Job to run; the lane is assigned here (round-robin) when
     *             dwLane is kChunkLaneUnset, and the assigned lane is
     *             written back for retry re-submission.
     * @return TRUE when accepted; FALSE after Shutdown().
     */
    BOOL32 SubmitChunk(TChunkJob& tJob);

    /**
     * @brief Re-queue a job after a backoff delay (retry path).
     * @param tJob Job whose dwLane was assigned by a previous submit.
     * @param dwDelayMs Delay before the next attempt starts.
     * @return TRUE when accepted; FALSE after Shutdown().
     */
    BOOL32 SubmitChunkDelayed(TChunkJob& tJob, u64 dwDelayMs);

    /** @brief Driver thread / CURLM handle count. */
    u32 LaneCount() const;

    /** @brief Active plus queued job count (informational/tests). */
    u32 ActiveCount() const;

    /** @brief Stop accepting jobs and cancel all transfers (idempotent). */
    void Shutdown();

private:
    /** @brief Per-lane driver state. */
    typedef struct tagLaneState
    {
        CURLM* pMulti;  /**< Multi handle owned by this lane */
        std::thread thDriver;  /**< Driver thread */
        std::deque<TChunkJob> deqPending;  /**< Waiting for a driver cycle */
        /** @brief Delayed retries: {ready time, job}. */
        std::deque<std::pair<std::chrono::steady_clock::time_point,
                             TChunkJob> > deqRetry;
        /** @brief Active transfers (touched only by the driver thread). */
        std::map<CURL*, std::shared_ptr<TChunkJob> > mapActive;
        std::mutex mutex;  /**< Protects deqPending/deqRetry */
    } TLaneState;

    /** @brief Driver loop: start jobs, drive the multi handle, finalize. */
    void DriverLoop(u32 dwLane);

    std::vector<std::unique_ptr<TLaneState> > m_vecLanes;
    std::atomic<BOOL32> m_bShutdown{false};  /**< Shutdown flag */
    std::atomic<u32> m_dwNextLane{0u};       /**< Round-robin counter */
};

#endif  // BURST_CURLMULTI_H
