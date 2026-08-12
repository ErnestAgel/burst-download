/**
 * @file progress.h
 * @brief 下载进度快照数据结构（GUI 与 Ccurl 回调共用，Phase 1）
 *
 * 设计要点（见 gui-design.md §4.2）：
 * - 工作线程写快照（加锁），UI 主线程每帧读（加锁）；
 * - 锁内不做分配/IO：threads 向量由 UI 侧按需读取，回调侧每 200ms 构建一次；
 * - 本文件不依赖 Ccurl，仅纯数据结构，供 src/ 与 gui/ 双向引用。
 *
 * @author ErnestAgel
 * @date 2026-08-07
 * @license SPDX-License-Identifier: MIT
 */
#pragma once

#include <string>
#include <vector>

/**
 * @brief 单个分片线程的进度（Phase 3 用户需求：按文件内位置显示）
 *
 * downloaded/total/percent 均为"文件内绝对位置"语义：
 *   - downloaded = 分片文件内起点(file_start) + 本片已下载量（续传含基数）
 *   - total       = 分片终点 + 1（文件内绝对位置）
 *   - percent     = downloaded / file_total（与总进度对齐，不再从 0 起算）
 */
struct ThreadProgress {
    int         id;          /**< 分片线程号（0 起） */
    long long   file_start;  /**< 分片在文件内的起始偏移（含断点续传基数） */
    long long   downloaded;  /**< 文件内已下载到的位置（file_start + 本片已下载量） */
    long long   total;       /**< 分片终点 + 1（文件内绝对位置） */
    long long   file_total;  /**< 整个文件大小（含续传基数，与总进度分母一致） */
    double      speed;       /**< 本线程速率（B/s，节流窗口内均值） */
    double      percent;     /**< 文件内位置百分比（downloaded/file_total，0~100） */
};

/**
 * @brief 下载阶段枚举（快照 stage 字段取值）
 */
enum DownloadStage {
    STAGE_IDLE        = 0,  /**< 空闲（无任务） */
    STAGE_PARSING     = 1,  /**< 解析中（视频模式） */
    STAGE_DOWNLOADING = 2,  /**< 下载中（文件模式） */
    STAGE_MERGING     = 3,  /**< 合并中（视频模式） */
    STAGE_DONE        = 4,  /**< 完成 */
    STAGE_ERROR       = 5,  /**< 错误（error 字段填充，供弹窗） */
    STAGE_CANCELED    = 6,  /**< 已取消 */
    /* ---- Phase 2 视频模式细分（F8 四阶段：解析中/下载视频轨/下载音频轨/合并中） ---- */
    STAGE_VIDEO_DL    = 7,  /**< 下载视频轨 */
    STAGE_AUDIO_DL    = 8,  /**< 下载音频轨 */
};

/**
 * @brief 下载快照：由工作线程写入、UI 主线程每帧读取
 */
struct DownloadSnapshot {
    int         stage;        /**< DownloadStage */
    std::string status;       /**< 阶段文本（视频：解析中/下载视频轨/下载音频轨/合并中） */
    std::string error;        /**< 错误信息（stage==ERROR 时填充，供弹窗） */
    double      totalPercent; /**< 总百分比（0~100） */
    double      totalSpeed;   /**< 总速率（B/s） */
    std::string eta;          /**< 剩余时间文本（"00:12"；速率为 0 时 "--"） */
    std::vector<ThreadProgress> threads; /**< 各分片线程进度 */
    std::vector<std::string> log;        /**< 环形日志（事件/错误，UI 日志区渲染） */
};
