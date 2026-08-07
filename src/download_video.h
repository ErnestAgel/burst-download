/**
 * @file download_video.h
 * @brief 视频下载编排模块（Phase 2，见 gui-design.md §5.3 / PROGRESS_GUI Phase 2）
 *
 * 从 main.cpp 的 DownloadVideo 抽取而来，供 CLI 与 GUI 共用：
 *   解析（EmbedParseVideoUrls）→ 逐流 Ccurl 下载（视频轨/音频轨）→ 合并（MergeMp4）
 *
 * 与 GUI 的接口：
 *   - onStage：阶段回调（STAGE_PARSING / STAGE_VIDEO_DL / STAGE_AUDIO_DL / STAGE_MERGING），
 *     工作线程据此更新快照 stage（解析/合并为同步阻塞，无进度，仅阶段文本）；
 *   - onProgress：单流下载进度回调（与文件模式 Ccurl::onProgress 同语义，每 ~200ms）；
 *   - onLog：日志回调（GUI 追加到日志区；CLI 不设置时默认 printf，保持原命令行输出）；
 *   - Cancel()/IsCanceled()：取消支持（§5.2 检查点：解析前/每流下载前/合并前，延迟 < 1s）。
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

/** @brief 视频下载结果 */
enum class VideoResult {
    Ok,       /**< 全部成功（含合并；单流无合并） */
    Error,    /**< 解析/下载/合并失败（LastError() 取原因） */
    Canceled, /**< 已请求取消（部分文件保留可续传） */
};

/**
 * @brief 视频下载编排器：解析 → 下载视频轨/音频轨 → 自动合并
 *
 * 线程模型：Run() 在调用方工作线程中同步执行；阶段/进度/日志回调同步回调，
 * 回调内不得调用 ImGui API（R2），由工作线程写 mutex 保护快照。
 * 单任务串行：一个 VideoDownloader 实例一次 Run（多任务并发需全局状态成员化，R1）。
 */
class VideoDownloader {
public:
    VideoDownloader() = default;
    ~VideoDownloader() = default;

    VideoDownloader(const VideoDownloader&) = delete;
    VideoDownloader& operator=(const VideoDownloader&) = delete;

    /** @brief 阶段回调：STAGE_PARSING / STAGE_VIDEO_DL / STAGE_AUDIO_DL / STAGE_MERGING */
    std::function<void(int)> onStage;

    /** @brief 单流下载进度回调：(各分片进度, 总百分比, 总速率 B/s)；不设置则走 Ccurl 默认 1% 门控打印 */
    std::function<void(const std::vector<ThreadProgress>&, double, double)>
        onProgress;

    /** @brief 日志回调；不设置时默认 printf（CLI 保持原输出） */
    std::function<void(const std::string&)> onLog;

    /**
     * @brief 请求取消：置标志，在解析前/每流下载前/合并前检查点中止
     * @note 下载中取消经 Ccurl::Cancel 语义（写回调检查点 <1s）；取消后残留分片保留可续传
     */
    void Cancel();

    /** @brief 是否已请求取消 */
    bool IsCanceled() const;

    /**
     * @brief 最近一次失败的具体原因（Result==Error 后读取，供 GUI 弹窗）
     */
    std::string LastError() const;

    /**
     * @brief 执行视频下载：解析 → 逐流下载（最多视频轨+音频轨 2 个）→ 自动合并
     * @param video_url 视频网页 URL（B站/YouTube 等）
     * @param basename 输出基础名（不含扩展名；视频轨 .mp4 / 音频轨 .m4a / 合并产物 <base>_full.<ext>）
     * @param threads 每流下载线程数（1~10，Ccurl 内部再钳位）
     * @param timeout 低速超时秒数（0=不限）
     * @param cookies_from_browser 浏览器 Cookie 来源（chrome/firefox/edge，可为空）
     * @param cookie 手动 Cookie 字符串（可为空）
     * @return 下载结果（Canceled/Error/Ok）；Ok 时 OutputPath() 为最终产物
     * @note 需先 EmbedPythonInit（GUI 由 worker 在任务前确保；CLI 在 main 中初始化）
     */
    VideoResult Run(const std::string& video_url, const std::string& basename,
                    int threads, int timeout,
                    const std::string& cookies_from_browser = "",
                    const std::string& cookie = "");

    /**
     * @brief 最终输出产物路径（Run 结束后读取）
     * @return 多流=合并产物（<base>_full.mkv/.mp4）；单流=视频轨文件（<base>.mp4）
     */
    std::string OutputPath() const;

private:
    /** @brief 日志：设置 onLog 走回调，否则 printf（CLI 兼容） */
    void Log(const std::string& msg);

    std::atomic<bool> m_cancel{false};  /**< 取消标志 */
    std::string m_last_error;           /**< 失败原因（LastError() 读取） */
    std::string m_output_path;          /**< 最终产物路径 */
};
