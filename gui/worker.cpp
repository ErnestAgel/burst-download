/**
 * @file worker.cpp
 * @brief 后台下载工作线程实现（见 worker.h）
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
    /* 退出铁律（§8.4）：先置取消，再 join，禁止 detach 后退出 */
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
        return false;  /* 单任务串行：已有一个任务在跑 */
    }
    if (url.empty() || path.empty()) {
        return false;
    }
    /* 前一个任务可能已自然结束但线程未 join（joinable 的 thread 重新赋值会 terminate） */
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_cancel.store(false);
    m_joined.store(false);
    /* 新任务清空上一任务快照与日志；"继续"（断点续传）保留快照，
     * 让 UI 从暂停时进度续走（Ccurl 首个进度回调含 resume 基数自动校准） */
    if (!preserve_snapshot) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_snapshot = DownloadSnapshot();
        m_snapshot.stage = STAGE_IDLE;
        m_snapshot.eta = "--";
        m_log.clear();
    }
    AddLog("[INFO] 开始任务: " + url);

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
        return false;  /* 单任务串行：已有一个任务在跑 */
    }
    if (url.empty() || basename.empty()) {
        return false;
    }
    /* 前一个任务可能已自然结束但线程未 join（joinable 的 thread 重新赋值会 terminate） */
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_cancel.store(false);
    m_joined.store(false);
    /* 同 StartFileDownload：继续（续传）保留快照 */
    if (!preserve_snapshot) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_snapshot = DownloadSnapshot();
        m_snapshot.stage = STAGE_IDLE;
        m_snapshot.eta = "--";
        m_log.clear();
    }
    AddLog("[INFO] 开始任务: " + url);

    m_thread = std::thread(&DownloadWorker::VideoWorkerFunc, this, url,
                           basename, threads, timeout);
    m_running.store(true);
    return true;
}

void DownloadWorker::Cancel() {
    m_cancel.store(true);
    AddLog("[INFO] 已请求取消");
}

bool DownloadWorker::IsRunning() const {
    return m_running.load();
}

int DownloadWorker::GetSnapshot(DownloadSnapshot& out) {
    std::lock_guard<std::mutex> lock(m_mutex);
    out = m_snapshot;
    out.log = m_log;  /* 环形日志随快照带出（UI 日志区渲染） */
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
        /* 有限等待：轮询线程结束标志（std::thread 无 timed_join，轮询实现） */
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(timeout_sec);
        while (m_running.load() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (m_running.load()) {
            return false;  /* 超时：仅告警不强杀（§8.4） */
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
    /* 停止任务后调用：清空快照与日志回初始空闲态（调用方须确保工作线程已结束） */
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
    SetStage(STAGE_DOWNLOADING, "downloading", "[INFO] 开始下载: " + url);

    /* 启动前创建父目录（Ccurl 只写文件不建目录；失败留给 Init 报错，见注意事项 5） */
    std::error_code ec;
    std::filesystem::path fs_path(path);
    if (fs_path.has_parent_path() && !fs_path.parent_path().empty()) {
        std::filesystem::create_directories(fs_path.parent_path(), ec);
    }

    std::unique_ptr<Ccurl> cc = std::make_unique<Ccurl>();
    cc->onProgress = [this, cc_ptr = cc.get()](
                         const std::vector<ThreadProgress>& tp,
                         double totalPercent, double totalSpeed) {
        /* 取消传导（Phase 1 遗漏接线，Phase 2 修复）：worker.Cancel() 只置标志，
         * 必须在此转调 Ccurl::Cancel() 置 m_cancel_flag，写回调检查点才能中止传输 */
        if (m_cancel.load()) {
            cc_ptr->Cancel();
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_snapshot.stage = STAGE_DOWNLOADING;
        m_snapshot.totalPercent = totalPercent;
        m_snapshot.totalSpeed = totalSpeed;
        /* 累计已下载字节（估算总字节 → ETA） */
        long long done = 0;
        m_snapshot.threads.assign(tp.begin(), tp.end()); /* 复用已分配容量 */
        for (const auto& t : tp) {
            done += t.downloaded;
        }
        double remain = 0;
        if (totalPercent > 0 && totalPercent < 100) {
            double total = done / (totalPercent / 100.0);
            remain = total - done;
        }
        m_snapshot.eta = FormatEta(remain, totalSpeed);
    };

    /* 取消检查点（解析/探测阶段取消：置位后不再启动传输） */
    if (m_cancel.load()) {
        SetStage(STAGE_CANCELED, "canceled", "[INFO] 已取消（未开始传输）");
        m_running.store(false);
        return;
    }

    bool init_ok = cc->Init(url, path, threads, timeout);
    if (!init_ok) {
        if (m_cancel.load()) {
            SetStage(STAGE_CANCELED, "canceled", "[INFO] 已取消");
        } else {
            std::string detail = cc->LastError();
            if (detail.empty()) {
                detail = i18n::T("err.guide.init");
            }
            std::string err = "[ERROR] 初始化失败: " + url;
            SetStage(STAGE_ERROR, "error", err);
            m_snapshot.error = detail;  /* 具体原因（供 F12 弹窗） */
        }
        m_running.store(false);
        return;
    }

    bool ok = cc->Download_Task();
    if (ok) {
        /* 下载完成：修正快照，总进度与各线程进度均置 100%
         * （最后一次进度回调可能停在 <100%，且完成后不再有回调更新 UI）
         * 完成优先判定：取消/完成竞态（Run 已返回后才点取消）不误报已取消 */
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_snapshot.totalPercent = 100.0;
            m_snapshot.totalSpeed = 0;
            m_snapshot.eta = "--";
            for (auto& t : m_snapshot.threads) {
                t.downloaded = t.total;
                t.percent = 100.0;
                t.speed = 0;
            }
        }
        SetStage(STAGE_DONE, path, "[INFO] 下载完成: " + path);
    } else if (m_cancel.load() || cc->IsCanceled()) {
        SetStage(STAGE_CANCELED, "canceled",
                 "[INFO] 已取消，部分文件保留可续传");
    } else {
        m_snapshot.error = "下载失败（部分分片未完成），详见 download.log";
        SetStage(STAGE_ERROR, "error",
                 "[ERROR] 下载失败: " + url);
    }
    m_running.store(false);
}

void DownloadWorker::VideoWorkerFunc(const std::string& url,
                                     const std::string& basename,
                                     int threads, int timeout) {
    /* 启动前创建父目录（basename 的父目录，如用户填的保存目录；Ccurl 只写文件不建目录） */
    std::error_code ec;
    std::filesystem::path fs_path(basename);
    if (fs_path.has_parent_path() && !fs_path.parent_path().empty()) {
        std::filesystem::create_directories(fs_path.parent_path(), ec);
    }

    /* 取消检查点（解析前）：置位则不再启动传输 */
    if (m_cancel.load()) {
        SetStage(STAGE_CANCELED, "", "[INFO] 已取消（未开始传输）");
        m_running.store(false);
        return;
    }

    /* 确保嵌入的 Python 运行时已初始化（幂等；main_gui 启动时已尝试 exe 同目录路径，
     * 此处兜底再试一次——开发构建可回退编译期宏 third_party/python/runtime）。
     * 初始化失败：明确报错指引，不继续解析 */
    if (!EmbedPythonInit()) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_snapshot.error =
                "Python 运行时初始化失败：缺少 python_runtime 资源（stdlib/yt_dlp）。\n"
                "开发构建使用仓库 third_party/python/runtime；发布物需将 python_runtime "
                "随 exe 同目录分发。";
        }
        SetStage(STAGE_ERROR, "",
                 "[ERROR] 视频解析失败: Python 运行时未初始化");
        m_running.store(false);
        return;
    }

    VideoDownloader vd;
    /* 阶段回调（F8）：解析中→下载视频轨→下载音频轨→合并中；
     * 进度回调不覆盖 stage，保持细分阶段显示 */
    vd.onStage = [this, vd_ptr = &vd](int stage) {
        /* 取消传导：worker.Cancel() → VideoDownloader::Cancel()
         * （解析前/每流下载前/合并前检查点依赖 vd.m_cancel 生效） */
        if (m_cancel.load()) {
            vd_ptr->Cancel();
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_snapshot.stage = stage;
        /* 流切换（视频轨→音频轨）：上一流进度 100% 需重置，避免总进度条倒跳 */
        if (stage == STAGE_AUDIO_DL) {
            m_snapshot.totalPercent = 0;
            m_snapshot.totalSpeed = 0;
            m_snapshot.eta = "--";
            m_snapshot.threads.clear();
        }
    };
    /* 单流下载进度回调：与文件模式相同的快照更新（每 ~200ms，锁内拷贝标量） */
    vd.onProgress = [this, vd_ptr = &vd](
                        const std::vector<ThreadProgress>& tp,
                        double totalPercent, double totalSpeed) {
        /* 下载中取消传导：置位后 Ccurl 写回调检查点中止当前流（<1s） */
        if (m_cancel.load()) {
            vd_ptr->Cancel();
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_snapshot.totalPercent = totalPercent;
        m_snapshot.totalSpeed = totalSpeed;
        long long done = 0;
        m_snapshot.threads.assign(tp.begin(), tp.end());
        for (const auto& t : tp) {
            done += t.downloaded;
        }
        double remain = 0;
        if (totalPercent > 0 && totalPercent < 100) {
            double total = done / (totalPercent / 100.0);
            remain = total - done;
        }
        m_snapshot.eta = FormatEta(remain, totalSpeed);
    };
    vd.onLog = [this](const std::string& msg) { AddLog(msg); };

    VideoResult result = vd.Run(url, basename, threads, timeout);
    if (result == VideoResult::Ok) {
        /* 完成优先判定：取消/完成竞态（Run 已返回后才点取消）不误报已取消 */
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_snapshot.totalPercent = 100.0;
            m_snapshot.totalSpeed = 0;
            m_snapshot.eta = "--";
            for (auto& t : m_snapshot.threads) {
                t.downloaded = t.total;
                t.percent = 100.0;
                t.speed = 0;
            }
        }
        SetStage(STAGE_DONE, vd.OutputPath(),
                 "[INFO] 视频下载完成: " + vd.OutputPath());
    } else if (result == VideoResult::Canceled || m_cancel.load()) {
        SetStage(STAGE_CANCELED, "", "[INFO] 已取消，部分文件保留可续传");
    } else {
        /* 解析/下载/合并失败：具体原因传至 F12 弹窗（锁保护，UI 每帧读取） */
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_snapshot.error = vd.LastError().empty()
                                   ? "视频下载失败"
                                   : vd.LastError();
        }
        SetStage(STAGE_ERROR, "", "[ERROR] 视频下载失败: " + url);
    }
    m_running.store(false);
}
