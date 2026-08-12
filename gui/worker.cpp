/**
 * @file worker.cpp
 * @brief Background download worker thread implementation (see worker.h).
 *
 * @author ErnestAgel
 * @date 2026-08-07
 * @license SPDX-License-Identifier: MIT
 */

#include "worker.h"

#include <chrono>
#include <cstdio>
#include <filesystem>

#include "Ccurl.h"
#include "download_video.h"
#include "embed_python.h"
#include "i18n.h"

DownloadWorker::DownloadWorker() {
    m_snapshot.stage = STAGE_IDLE;
    m_snapshot.status = "";
    m_snapshot.totalPercent = 0;
    m_snapshot.totalSpeed = 0;
    m_snapshot.eta = "--";
}

DownloadWorker::~DownloadWorker() {
    /* Exit rule: cancel first, then join; detach is forbidden. */
    if (m_thread.joinable()) {
        if (m_running.load()) {
            Cancel();
        }
        Join(5);
    }
}

bool DownloadWorker::StartFileDownload(const std::string& url,
                                       const std::string& path, int threads,
                                       int timeout, bool preserve_snapshot) {
    if (m_running.load()) {
        return false;  /* single-task serial: a task is already running */
    }
    if (url.empty() || path.empty()) {
        return false;
    }
    /* A previous task may have finished but was never joined (assigning a
     * joinable thread would terminate). */
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_cancel.store(false);
    m_joined.store(false);
    /* A new task clears the previous snapshot/log; "Resume" keeps them so
     * the UI continues from the paused progress (the first Ccurl progress
     * callback recalibrates with the resume base). */
    if (!preserve_snapshot) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_snapshot = DownloadSnapshot();
        m_snapshot.stage = STAGE_IDLE;
        m_snapshot.eta = "--";
        m_log.clear();
    }
    AddLog("[INFO] task started: " + url);

    m_thread = std::thread(&DownloadWorker::WorkerFunc, this, url, path,
                           threads, timeout);
    m_running.store(true);
    return true;
}

bool DownloadWorker::StartVideoDownload(const std::string& url,
                                        const std::string& basename,
                                        int threads, int timeout,
                                        bool preserve_snapshot) {
    if (m_running.load()) {
        return false;  /* single-task serial: a task is already running */
    }
    if (url.empty() || basename.empty()) {
        return false;
    }
    /* A previous task may have finished but was never joined. */
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_cancel.store(false);
    m_joined.store(false);
    /* Same as StartFileDownload: Resume keeps the snapshot. */
    if (!preserve_snapshot) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_snapshot = DownloadSnapshot();
        m_snapshot.stage = STAGE_IDLE;
        m_snapshot.eta = "--";
        m_log.clear();
    }
    AddLog("[INFO] task started: " + url);

    m_thread = std::thread(&DownloadWorker::VideoWorkerFunc, this, url,
                           basename, threads, timeout);
    m_running.store(true);
    return true;
}

void DownloadWorker::Cancel() {
    m_cancel.store(true);
    AddLog("[INFO] cancel requested");
}

bool DownloadWorker::IsRunning() const {
    return m_running.load();
}

int DownloadWorker::GetSnapshot(DownloadSnapshot& out) {
    std::lock_guard<std::mutex> lock(m_mutex);
    out = m_snapshot;
    out.log = m_log;  /* the ring log travels with the snapshot */
    return out.stage;
}

bool DownloadWorker::Join(int timeout_sec) {
    if (!m_thread.joinable()) {
        m_joined.store(true);
        return true;
    }
    if (m_joined.exchange(true)) {
        return !m_thread.joinable();
    }
    if (timeout_sec <= 0) {
        m_thread.join();
    } else {
        /* Bounded wait: poll the thread-end flag (std::thread has no
         * timed_join). */
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(timeout_sec);
        while (m_running.load() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (m_running.load()) {
            return false;  /* timeout: warn only, do not force-kill */
        }
        m_thread.join();
    }
    m_joined.store(true);
    return true;
}

void DownloadWorker::AddLog(const std::string& msg) {
    std::lock_guard<std::mutex> lock(m_mutex);
    char ts[32] = {0};
    time_t now = time(nullptr);
    struct tm* tm_now = localtime(&now);
    if (tm_now != nullptr) {
        strftime(ts, sizeof(ts), "%H:%M:%S", tm_now);
    }
    std::string line = "[";
    line += ts;
    line += "] ";
    line += msg;
    m_log.push_back(line);
    if (m_log.size() > kMaxLog) {
        m_log.erase(m_log.begin(), m_log.begin() + (m_log.size() - kMaxLog));
    }
}

void DownloadWorker::Reset() {
    /* Called after Stop: clear the snapshot and log back to idle (the
     * caller must ensure the worker thread has ended). */
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_cancel.store(false);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_snapshot = DownloadSnapshot();
    m_snapshot.stage = STAGE_IDLE;
    m_snapshot.eta = "--";
    m_log.clear();
}

void DownloadWorker::SetStage(int stage, const std::string& status,
                              const std::string& logmsg) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_snapshot.stage = stage;
    m_snapshot.status = status;
    if (!logmsg.empty()) {
        m_log.push_back(logmsg);
        if (m_log.size() > kMaxLog) {
            m_log.erase(m_log.begin(),
                        m_log.begin() + (m_log.size() - kMaxLog));
        }
    }
}

std::string DownloadWorker::FormatEta(double remain_bytes, double speed) {
    if (speed <= 0 || remain_bytes <= 0) {
        return "--";
    }
    double sec = remain_bytes / speed;
    if (sec >= 3600) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", (int)(sec / 3600),
                 (int)(sec / 60) % 60, (int)sec % 60);
        return buf;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d", (int)(sec / 60), (int)sec % 60);
    return buf;
}

void DownloadWorker::WorkerFunc(const std::string& url, const std::string& path,
                                int threads, int timeout) {
    SetStage(STAGE_DOWNLOADING, "downloading", "[INFO] start downloading: " +
                                                    url);

    /* Create the parent directory before starting (Ccurl only writes the
     * file; failures are left to Init to report). */
    std::error_code ec;
    std::filesystem::path fs_path(path);
    if (fs_path.has_parent_path() && !fs_path.parent_path().empty()) {
        std::filesystem::create_directories(fs_path.parent_path(), ec);
    }

    std::unique_ptr<Ccurl> cc = std::make_unique<Ccurl>();
    cc->onProgress = [this, cc_ptr = cc.get()](
                         const std::vector<ThreadProgress>& tp,
                         double totalPercent, double totalSpeed) {
        /* Cancel propagation: worker.Cancel() only sets the flag, so forward
         * it to Ccurl::Cancel() here to set m_cancel_flag; the write-callback
         * checkpoint then aborts the transfer. */
        if (m_cancel.load()) {
            cc_ptr->Cancel();
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_snapshot.stage = STAGE_DOWNLOADING;
        m_snapshot.totalPercent = totalPercent;
        m_snapshot.totalSpeed = totalSpeed;
        m_snapshot.threads.assign(tp.begin(), tp.end());
        /* Remaining bytes = file total x (100 - total percent) (threads use
         * in-file positions and cannot be summed). */
        double file_total = tp.empty() ? 0.0 : (double)tp[0].file_total;
        double remain = (file_total > 0 && totalPercent < 100)
                            ? file_total * (100.0 - totalPercent) / 100.0
                            : 0.0;
        m_snapshot.eta = FormatEta(remain, totalSpeed);
    };

    /* Cancellation checkpoint (parse/probe stage: do not start the transfer
     * when canceled). */
    if (m_cancel.load()) {
        SetStage(STAGE_CANCELED, "canceled",
                 "[INFO] canceled (transfer not started)");
        m_running.store(false);
        return;
    }

    bool init_ok = cc->Init(url, path, threads, timeout);
    if (!init_ok) {
        if (m_cancel.load()) {
            SetStage(STAGE_CANCELED, "canceled", "[INFO] canceled");
        } else {
            std::string detail = cc->LastError();
            if (detail.empty()) {
                detail = i18n::T("err.guide.init");
            }
            std::string err = "[ERROR] init failed: " + url;
            SetStage(STAGE_ERROR, "error", err);
            m_snapshot.error = detail;  /* specific reason (F12 popup) */
        }
        m_running.store(false);
        return;
    }
    /* Push an immediate 0-progress snapshot so the UI draws the battery-cell
     * separators before the first XFERINFO callback. */
    {
        auto parts = cc->SnapshotParts();
        cc->onProgress(parts, 0.0, 0.0);
    }

    bool ok = cc->Download_Task();
    if (ok) {
        /* Done: fix the snapshot so the total and per-thread progress are
         * 100% (the last progress callback may have stopped below 100% and
         * no further callback arrives).  Done wins over cancel races: a
         * cancel arriving after Run returned must not misreport. */
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_snapshot.totalPercent = 100.0;
            m_snapshot.totalSpeed = 0;
            m_snapshot.eta = "--";
            /* Done: each thread's downloaded = chunk end (in-file position),
             * percent = its in-file end percentage. */
            for (auto& t : m_snapshot.threads) {
                t.downloaded = t.total;
                t.percent =
                    (t.file_total > 0)
                        ? (t.total / (double)t.file_total * 100.0)
                        : 100.0;
                t.speed = 0;
            }
        }
        SetStage(STAGE_DONE, path, "[INFO] download complete: " + path);
    } else if (m_cancel.load() || cc->IsCanceled()) {
        SetStage(STAGE_CANCELED, "canceled",
                 "[INFO] canceled, partial files kept for resume");
    } else {
        m_snapshot.error =
            "download failed (some threads incomplete), see download.log";
        SetStage(STAGE_ERROR, "error", "[ERROR] download failed: " + url);
    }
    m_running.store(false);
}

void DownloadWorker::VideoWorkerFunc(const std::string& url,
                                     const std::string& basename,
                                     int threads, int timeout) {
    /* Create the parent directory before starting (the basename's parent,
     * e.g. the user's save directory; Ccurl only writes the file). */
    std::error_code ec;
    std::filesystem::path fs_path(basename);
    if (fs_path.has_parent_path() && !fs_path.parent_path().empty()) {
        std::filesystem::create_directories(fs_path.parent_path(), ec);
    }

    /* Cancellation checkpoint (before parsing): do not start when canceled. */
    if (m_cancel.load()) {
        SetStage(STAGE_CANCELED, "", "[INFO] canceled (transfer not started)");
        m_running.store(false);
        return;
    }

    /* Ensure the embedded Python runtime is initialized (idempotent;
     * main_gui already tried the exe-adjacent path at startup, this is a
     * fallback - the dev build can fall back to the compile-time macro
     * third_party/python/runtime).  On failure report a clear error and do
     * not parse. */
    if (!EmbedPythonInit()) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_snapshot.error =
                "Python runtime init failed: the assets/ runtime directory "
                "(stdlib/yt_dlp) is missing.\n"
                "Dev builds use the repository third_party/python/runtime; "
                "releases must ship assets/ next to the executable.";
        }
        SetStage(STAGE_ERROR, "",
                 "[ERROR] video parsing failed: Python runtime is not "
                 "initialized");
        m_running.store(false);
        return;
    }

    /* Auto-update the video parser (24h throttle; failures are silent and do
     * not block parsing).  The call is cancel-aware (issue R6). */
    AddLog("[INFO] checking parser update (24h throttle, GitHub budget "
           "10s)...");
    {
        std::string up_msg;
        if (EmbedAutoUpdateParser(up_msg, &m_cancel)) {
            if (!up_msg.empty()) {
                AddLog("[INFO] " + up_msg);
            }
        } else {
            AddLog("[WARN] parser update check failed or timed out, "
                   "continuing with the bundled parser");
        }
    }
    /* Cancellation checkpoint: do not start parsing after a cancel during
     * the auto-update. */
    if (m_cancel.load()) {
        SetStage(STAGE_CANCELED, "",
                 "[INFO] canceled (transfer not started)");
        m_running.store(false);
        return;
    }

    VideoDownloader vd;
    /* Stage callback (F8): parsing -> downloading video track -> downloading
     * audio track -> merging; progress callbacks do not overwrite the stage
     * so the fine-grained stage text stays visible. */
    vd.onStage = [this, vd_ptr = &vd](int stage) {
        /* Cancel propagation: worker.Cancel() -> VideoDownloader::Cancel()
         * (the before-parse / before-stream / before-merge checkpoints rely
         * on vd.m_cancel). */
        if (m_cancel.load()) {
            vd_ptr->Cancel();
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_snapshot.stage = stage;
        /* Stream switch (video -> audio): reset the previous stream's 100%
         * progress so the total bar does not jump backwards. */
        if (stage == STAGE_AUDIO_DL) {
            m_snapshot.totalPercent = 0;
            m_snapshot.totalSpeed = 0;
            m_snapshot.eta = "--";
            m_snapshot.threads.clear();
        }
    };
    /* Per-stream progress callback: same snapshot updates as file mode
     * (~200ms, scalars copied under lock). */
    vd.onProgress = [this, vd_ptr = &vd](
                        const std::vector<ThreadProgress>& tp,
                        double totalPercent, double totalSpeed) {
        /* Cancel propagation during a download: the Ccurl write-callback
         * checkpoint aborts the current stream (<1s). */
        if (m_cancel.load()) {
            vd_ptr->Cancel();
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_snapshot.totalPercent = totalPercent;
        m_snapshot.totalSpeed = totalSpeed;
        m_snapshot.threads.assign(tp.begin(), tp.end());
        /* Remaining bytes = file total x (100 - total percent). */
        double file_total = tp.empty() ? 0.0 : (double)tp[0].file_total;
        double remain = (file_total > 0 && totalPercent < 100)
                            ? file_total * (100.0 - totalPercent) / 100.0
                            : 0.0;
        m_snapshot.eta = FormatEta(remain, totalSpeed);
    };
    vd.onLog = [this](const std::string& msg) { AddLog(msg); };

    VideoResult result = vd.Run(url, basename, threads, timeout, "", "",
                                &m_cancel);
    if (result == VideoResult::Ok) {
        /* Done wins over cancel races (a cancel arriving after Run returned
         * must not misreport). */
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_snapshot.totalPercent = 100.0;
            m_snapshot.totalSpeed = 0;
            m_snapshot.eta = "--";
            for (auto& t : m_snapshot.threads) {
                t.downloaded = t.total;
                t.percent =
                    (t.file_total > 0)
                        ? (t.total / (double)t.file_total * 100.0)
                        : 100.0;
                t.speed = 0;
            }
        }
        SetStage(STAGE_DONE, vd.OutputPath(),
                 "[INFO] video download complete: " + vd.OutputPath());
    } else if (result == VideoResult::Canceled || m_cancel.load()) {
        SetStage(STAGE_CANCELED, "", "[INFO] canceled, partial files kept "
                                     "for resume");
    } else {
        /* Parse/download/merge failure: the specific reason goes to the F12
         * popup (lock-protected, read by the UI every frame). */
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_snapshot.error = vd.LastError().empty()
                                   ? "video download failed"
                                   : vd.LastError();
        }
        SetStage(STAGE_ERROR, "", "[ERROR] video download failed: " + url);
    }
    m_running.store(false);
}
