/**
 * @file avmerge.h
 * @brief Built-in audio/video track merger: remuxes separate tracks into a
 *        single file (MP4/MKV container).
 *
 * Built on the FFmpeg static libraries (libavformat/libavcodec/libavutil);
 * container-level remux only (-c copy semantics), no re-encoding:
 *   - video track: H.264 (avc1) MP4 or VP9/AV1 (WebM)
 *   - audio track: AAC (mp4a) M4A or Opus (WebM)
 * The output container follows the video codec: VP9/AV1/VP8 -> .mkv
 * (Matroska), otherwise .mp4.  All in-process, no external ffmpeg binary.
 *
 * @author ErnestAgel
 * @date 2026-08-06
 * @copyright Copyright (c) 2026 ErnestAgel
 * @license SPDX-License-Identifier: MIT
 */

#ifndef AVMERGE_H
#define AVMERGE_H

#include <atomic>
#include <string>

/**
 * @brief Suggest a merge output extension by the video codec:
 *        VP9/AV1/VP8 -> .mkv, otherwise .mp4.
 * @param strVideoPath Video track file path (MP4/WebM container).
 * @return Suggested extension including the dot (".mkv" or ".mp4").
 * @note Returns ".mp4" when the track cannot be opened or recognized.
 */
std::string SuggestMergeExt(const std::string& strVideoPath);

/**
 * @brief Merge separate audio/video tracks into one file (remux only).
 * @param strVideoPath Video track file path (H.264 MP4 or VP9/AV1 WebM).
 * @param strAudioPath Audio track file path (AAC M4A or Opus WebM).
 * @param strOutputPath Output path (extension selects the container).
 * @param strErr Failure description on error (unchanged on success).
 * @param pbCancel Optional cancel flag checked inside the write loop
 *        (issue R6).
 * @return TRUE when the merge succeeded.
 * @note On failure the caller should guide the user to merge manually with
 *       an external tool.
 */
bool MergeMp4(const std::string& strVideoPath,
              const std::string& strAudioPath,
              const std::string& strOutputPath, std::string& strErr,
              const std::atomic<bool>* pbCancel = nullptr);

#endif  // AVMERGE_H
