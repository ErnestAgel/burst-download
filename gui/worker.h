/**
 * @file worker.h
 * @brief Background download task runner: snapshot, cancel and reclamation
 *        for the GUI (see multi-task-design.md; R12 thread model).
 *
 * Responsibilities:
 * - Runs file/video download tasks on a reusable worker pool (CThreadPool)
 *   instead of creating one std::thread per task;
 * - Ccurl::onProgress callbacks write a snapshot (mutex protected) that the
 *   UI thread reads every frame;
 * - Cancel() is called from the UI thread and aborts via Ccurl write/progress
 *   checkpoints;
 * - Reclamation: Join(timeout) is bounded for the UI; the destructor waits
 *   unbounded for the current job, so no thread is ever left joinable.
 *
 * Thread rules: the worker must never call ImGui; the UI thread must never
 * touch Ccurl directly.
 */

#pragma once

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../src/progress.h"
#include "threadpool.h"

/**
 * @brief Background download task runner (single active task).
 */
class CDownloadWorker
{
public:
    CDownloadWorker();
    ~CDownloadWorker();

    /** @brief Non-copyable (owns a pool and a job handle). */
    CDownloadWorker(const CDownloadWorker&) = delete;
    CDownloadWorker& operator=(const CDownloadWorker&) = delete;

    /**
     * @brief Start a file download task on the worker pool.
     * @param url Direct file URL (http/https).
     * @param path Save path (UTF-8; Windows wide-char handled by Ccurl).
     * @param threads Chunk threads (1-8; clamped inside Ccurl).
     * @param timeout Low-speed timeout seconds (0 = unlimited).
     * @param preserve_snapshot TRUE keeps the previous snapshot for resume.
     * @return TRUE when the task was queued; FALSE when one is already running.
     */
    bool StartFileDownload(const std::string& url, const std::string& path,
                           int threads, int timeout = 60,
                           bool preserve_snapshot = false);

    /**
     * @brief Start a video download task (parse, tracks, merge).
     * @param url Video page URL (Bilibili/YouTube etc.).
     * @param basename Output base name (no extension).
     * @param threads Per-stream chunk threads.
     * @param timeout Low-speed timeout seconds.
     * @param preserve_snapshot TRUE keeps the previous snapshot for resume.
     * @return TRUE when the task was queued; FALSE when one is running.
     */
    bool StartVideoDownload(const std::string& url, const std::string& basename,
                            int threads, int timeout = 60,
                            bool preserve_snapshot = false);

    /** @brief Request cancellation (thread-safe); partial files are kept. */
    void Cancel();

    /** @brief TRUE while a task job is still running. */
    bool IsRunning() const;

    /**
     * @brief Read the current snapshot (scalars + parts + ring log).
     * @param out Receives the snapshot copy.
     * @return Current DownloadStage.
     */
    int GetSnapshot(DownloadSnapshot& out);

    /**
     * @brief Wait for the current job to finish.
     * @param timeout_sec Bounded wait seconds (0 = unlimited).
     * @return TRUE when the job finished; FALSE on timeout (warn only).
     */
    bool Join(int timeout_sec);

    /** @brief Append a ring-buffer log line (thread-safe). */
    void AddLog(const std::string& msg);

    /**
     * @brief Clear snapshot and log back to idle (call after the job ended).
     */
    void Reset();

private:
    /** @brief Job body for file downloads. */
    void WorkerFunc(const std::string& url, const std::string& path,
                    int threads, int timeout);

    /** @brief Job body for video downloads. */
    void VideoWorkerFunc(const std::string& url, const std::string& basename,
                         int threads, int timeout);

    /** @brief Update stage under lock and append a log line. */
    void SetStage(int stage, const std::string& status,
                  const std::string& logmsg);

    /** @brief Format the ETA text ("--" when speed is zero). */
    static std::string FormatEta(double remain_bytes, double speed);

    std::future<void>       m_futJob;      /**< Current job completion handle */
    std::atomic<bool>       m_running{false}; /**< Task job is running */
    std::atomic<bool>       m_cancel{false};  /**< Cancellation request flag */

    mutable std::mutex      m_mutex;       /**< Guards snapshot and log */
    DownloadSnapshot        m_snapshot;    /**< Progress snapshot */
    std::vector<std::string> m_log;        /**< Ring log (kMaxLog entries) */
    static const size_t     kMaxLog = 300;

    CThreadPool             m_cPool{2};    /**< Reusable worker pool (R12) */
};
