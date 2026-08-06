/**
 * @file avmerge.h
 * @brief 内置音视频轨合并器：将分离的音视频轨合并为单文件（MP4/MKV 容器）
 *
 * 基于 FFmpeg 静态库（libavformat/libavcodec/libavutil）实现，仅做容器层
 * 重新封装（remux，-c copy 语义），不重新编码：
 *   - 视频轨：H.264 (avc1) MP4 或 VP9/AV1 (WebM)
 *   - 音频轨：AAC (mp4a) M4A 或 Opus (WebM)
 * 输出容器按视频轨编码自动选择：VP9/AV1/VP8 -> .mkv（Matroska），其余 -> .mp4。
 * 合并结果含视频 + 音频两个轨道，可用普通播放器直接播放。全程进程内，
 * 无需外部 ffmpeg 程序。
 *
 * @author ErnestAgel
 * @date 2026-08-06
 * @copyright Copyright (c) 2026 ErnestAgel
 * @license SPDX-License-Identifier: MIT
 */

#ifndef AVMERGE_H
#define AVMERGE_H

#include <string>

/**
 * @brief 根据视频轨编码建议合并输出扩展名：VP9/AV1/VP8 -> .mkv，其余 -> .mp4
 * @param video_path 视频轨文件路径（MP4/WebM 容器）
 * @return 建议的扩展名（含点），如 ".mkv" 或 ".mp4"
 * @note 视频轨打不开或无法识别时返回 ".mp4"（保持默认行为）
 */
std::string SuggestMergeExt(const std::string& video_path);

/**
 * @brief 将分离的音视频轨合并为单文件（仅重新封装，不重新编码）
 * @param video_path 视频轨文件路径（MP4 容器 H.264，或 WebM 容器 VP9/AV1）
 * @param audio_path 音频轨文件路径（MP4/M4A 容器 AAC，或 WebM 容器 Opus）
 * @param output_path 输出合并文件路径（扩展名决定容器：.mp4 / .mkv）
 * @param err 失败时输出错误描述（成功时不变）
 * @return 合并是否成功
 * @note 仅支持 FFmpeg 可识别的媒体容器；失败时调用方应提示用户改用外部工具手动合并
 */
bool MergeMp4(const std::string& video_path, const std::string& audio_path,
              const std::string& output_path, std::string& err);

#endif  // AVMERGE_H
