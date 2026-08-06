/**
 * @file avmerge.h
 * @brief 内置音视频轨合并器：将分离的音视频轨（MP4 容器）合并为单文件 MP4
 *
 * 基于 FFmpeg 静态库（libavformat/libavcodec/libavutil）实现，仅做容器层
 * 重新封装（remux，-c copy 语义），不重新编码：
 *   - 视频轨：H.264 (avc1) MP4
 *   - 音频轨：AAC (mp4a) M4A
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
 * @brief 将分离的音视频轨 MP4 合并为单文件 MP4（仅重新封装，不重新编码）
 * @param video_path 视频轨文件路径（MP4 容器，H.264/avc1）
 * @param audio_path 音频轨文件路径（MP4/M4A 容器，AAC/mp4a）
 * @param output_path 输出合并文件路径
 * @param err 失败时输出错误描述（成功时不变）
 * @return 合并是否成功
 * @note 仅支持 FFmpeg 可识别的媒体容器；失败时调用方应提示用户改用外部工具手动合并
 */
bool MergeMp4(const std::string& video_path, const std::string& audio_path,
              const std::string& output_path, std::string& err);

#endif  // AVMERGE_H
