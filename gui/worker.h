/**
 * @file worker.h
 * @brief 后台下载工作线程 + 进度快照 + 取消（GUI Phase 1，见 gui-design.md §4/§5.2）
 *
 * 职责：
 * - 在独立 std::thread 中执行文件下载任务（Ccurl Init + Download_Task）；
 * - Ccurl::onProgress 回调每 ~200ms 写入快照（mutex 保护），UI 主线程每帧读取；
 * - Cancel() 从 UI 线程调用，置位后 Ccurl 在写回调/进度回调检查点中止；
 * - 退出流程：Join(超时) 等待工作线程结束，禁止 detach（§8.4 铁律）。
 *
 * 线程模型（R2）：工作线程绝不调用 ImGui API；UI 线程绝不直接操作 Ccurl。
 *
 * @author ErnestAgel
 * @date 2026-08-07
 * @license SPDX-License-Identifier: MIT
 */
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../src/progress.h"

/**
 * @brief 后台下载工作线程（单任务串行，MVP 一次一个任务）
 */
class DownloadWorker {
public:
    DownloadWorker();
    ~DownloadWorker();

    /** 禁止拷贝（含线程成员） */
    DownloadWorker(const DownloadWorker&) = delete;
    DownloadWorker& operator=(const DownloadWorker&) = delete;

    /**
     * @brief 启动一个文件下载任务（内部创建工作线程，立即返回）
     * @param url 文件直链（http/https）
     * @param path 保存路径（UTF-8；Windows 中文路径由 Ccurl 宽字符入口处理）
     * @param threads 线程数（1~10，Ccurl 内部再钳位）
     * @param timeout 低速超时秒数（0=不限，默认 60）
     * @return 是否成功启动（已在运行则返回 false）
     */
    bool StartFileDownload(const std::string& url, const std::string& path,
                           int threads, int timeout = 60);

    /**
     * @brief 请求取消（线程安全，UI 线程可调）；取消后部分文件保留可续传
     */
    void Cancel();

    /**
     * @brief 是否正在运行（工作线程存活）
     */
    bool IsRunning() const;

    /**
     * @brief 读取当前快照（锁内拷贝标量 + 分片表 + 环形日志）
     * @param out 输出快照
     * @return 当前 stage（DownloadStage）
     */
    int GetSnapshot(DownloadSnapshot& out);

    /**
     * @brief 等待工作线程结束（退出流程用，§8.4）
     * @param timeout_sec 超时秒数（0=不限；超时仅告警不强杀）
     * @return 线程是否已结束（false 表示超时）
     */
    bool Join(int timeout_sec);

    /**
     * @brief 追加一条日志（环形缓冲，线程安全）
     * @param msg 日志文本
     */
    void AddLog(const std::string& msg);

private:
    /** @brief 工作线程入口：执行下载任务编排 */
    void WorkerFunc(const std::string& url, const std::string& path,
                    int threads, int timeout);

    /** @brief 锁内更新阶段状态并写日志 */
    void SetStage(int stage, const std::string& status, const std::string& logmsg);

    /** @brief 计算 ETA 文本（速率为 0 时 "--"） */
    static std::string FormatEta(double remain_bytes, double speed);

    std::thread m_thread;               /**< 工作线程 */
    std::atomic<bool> m_running{false}; /**< 工作线程是否存活 */
    std::atomic<bool> m_cancel{false};  /**< 取消请求标志 */
    std::atomic<bool> m_joined{false};  /**< Join 已调用过（防重复 join） */

    mutable std::mutex m_mutex;         /**< 保护快照与日志 */
    DownloadSnapshot m_snapshot;        /**< 进度快照（stage/percent/threads 等） */
    std::vector<std::string> m_log;     /**< 环形日志（上限 kMaxLog 条） */
    static const size_t kMaxLog = 300;
};
