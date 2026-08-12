/**
 * @file download_video.h
 * @brief Video download orchestration module (see gui-design.md).
 *
 * Extracted from main.cpp DownloadVideo and shared by CLI and GUI:
 *   parse (EmbedParseVideoUrls) -> per-stream Ccurl download (video track /
 *   audio track) -> merge (MergeMp4).
 *
 * GUI interface:
 *   - onStage: stage callback (STAGE_PARSING / STAGE_VIDEO_DL /
 *     STAGE_AUDIO_DL / STAGE_MERGING); the worker updates the snapshot stage
 *     from it (parse/merge are synchronous with stage text only);
 *   - onProgress: per-stream progress (same semantics as Ccurl::onProgress,
 *     ~200ms);
 *   - onLog: log callback (GUI appends to the log area; when unset the CLI
 *     keeps its original printf output);
 *   - Cancel()/IsCanceled(): cancellation support (checkpoints before
 *     parsing, before each stream, before merging).
 *
 * @author ErnestAgel
 * @date 2026-08-07
 * @license SPDX-License-Identifier: MIT
 */
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "progress.h"

class CThreadPool;

/** @brief Video download result. */
enum class VideoResult {
    Ok,       /**< All steps succeeded (merge included; a single stream has
               *  no merge) */
    Error,    /**< Parse/download/merge failed (LastError() has the reason) */
    Canceled, /**< Cancellation requested (partial files kept for resume) */
};

/**
 * @brief Video download orchestrator: parse -> download video/audio tracks
 *        -> auto merge.
 *
 * Thread model: Run() executes synchronously on the caller's worker thread;
 * stage/progress/log callbacks fire synchronously and must not call ImGui
 * APIs; the worker writes mutex-protected snapshots.
 * Single-task serial: one Run() per VideoDownloader instance (concurrent
 * tasks require engine memberization, issue R7).
 */
class VideoDownloader {
public:
    VideoDownloader() = default;
    ~VideoDownloader() = default;

    VideoDownloader(const VideoDownloader&) = delete;
    VideoDownloader& operator=(const VideoDownloader&) = delete;

    /** @brief Stage callback: STAGE_PARSING / STAGE_VIDEO_DL /
     *         STAGE_AUDIO_DL / STAGE_MERGING. */
    std::function<void(int)> onStage;

    /** @brief Per-stream progress callback: (parts, total percent, total
     *         speed); when unset Ccurl uses its default 1% gate printing. */
    std::function<void(const std::vector<ThreadProgress>&, double, double)>
        onProgress;

    /** @brief Log callback; when unset defaults to printf (CLI output). */
    std::function<void(const std::string&)> onLog;

    /**
     * @brief Request cancellation: sets the flag; checkpoints before
     *        parsing / each stream / merging abort.
     * @note Cancel during a download goes through Ccurl::Cancel semantics
     *       (write-callback checkpoint < 1s); partial files are kept for
     *       resume.
     */
    void Cancel();

    /** @brief Whether cancellation was requested. */
    bool IsCanceled() const;

    /**
     * @brief Attach the shared download pool (P8): each stream's Ccurl uses
     *        it for chunk jobs.
     * @param pPool Shared download pool; may be null.
     */
    void SetChunkPool(CThreadPool* pPool);

    /**
     * @brief Most recent failure reason (read after Result == Error).
     */
    std::string LastError() const;

    /**
     * @brief Run video download: parse -> per-stream download (up to video
     *        + audio tracks) -> auto merge.
     * @param strVideoUrl Video page URL (Bilibili/YouTube etc.).
     * @param strBasename Output base name (no extension; video track .mp4 /
     *        audio track .m4a / merged output <base>_full.<ext>).
     * @param nThreads Threads per stream (1~8, clamped by Ccurl).
     * @param nTimeout Low-speed timeout seconds (0 = unlimited).
     * @param strCookiesFromBrowser Browser cookie source (chrome/firefox/
     *        edge, may be empty).
     * @param strCookie Manual cookie string (may be empty).
     * @return Result (Canceled/Error/Ok); on Ok, OutputPath() is the final
     *         artifact.
     * @note EmbedPythonInit must run first (GUI: worker ensures it; CLI:
     *       main initializes).
     */
    VideoResult Run(const std::string& strVideoUrl,
                    const std::string& strBasename, int nThreads,
                    int nTimeout,
                    const std::string& strCookiesFromBrowser = "",
                    const std::string& strCookie = "",
                    const std::atomic<bool>* pbCancel = nullptr);

    /**
     * @brief Final output artifact path (read after Run).
     * @return Multi-stream: merged artifact (<base>_full.mkv/.mp4); single
     *         stream: video track file (<base>.mp4).
     */
    std::string OutputPath() const;

private:
    /** @brief Log: uses onLog when set, otherwise printf (CLI compatible). */
    void Log(const std::string& strMsg);

    std::atomic<bool> m_cancel{false};  /**< Cancellation flag */
    std::string m_last_error;           /**< Failure reason (LastError()) */
    std::string m_output_path;          /**< Final artifact path */
    CThreadPool* m_pChunkPool = nullptr; /**< Shared download pool (P8) */
};
