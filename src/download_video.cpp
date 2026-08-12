/**
 * @file download_video.cpp
 * @brief Video download orchestration implementation (see download_video.h).
 *
 * Logic extracted from main.cpp DownloadVideo (CLI) and shared by CLI/GUI:
 *   - onStage callbacks drive the GUI stage text (parsing / downloading the
 *     video track / downloading the audio track / merging);
 *   - each stream uses its own Ccurl instance (SetReferer anti-hotlink +
 *     optional cookie); progress is forwarded via onProgress;
 *   - cancellation checkpoints: before parsing, before each stream, before
 *     merging; during downloads the Ccurl write callback aborts;
 *   - on success the intermediate audio/video files are cleaned up (same as
 *     CLI); on merge failure both tracks are kept for manual merging.
 *
 * @author ErnestAgel
 * @date 2026-08-07
 * @license SPDX-License-Identifier: MIT
 */

#include "download_video.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Ccurl.h"
#include "avmerge.h"
#include "embed_python.h"
#include "video.h"

using namespace std;

void VideoDownloader::Cancel() {
    m_cancel.store(true);
}

bool VideoDownloader::IsCanceled() const {
    return m_cancel.load();
}

std::string VideoDownloader::LastError() const {
    return m_last_error;
}

std::string VideoDownloader::OutputPath() const {
    return m_output_path;
}

void VideoDownloader::Log(const std::string& strMsg) {
    if (onLog) {
        onLog(strMsg);
    } else {
        printf("%s\n", strMsg.c_str());
    }
}

VideoResult VideoDownloader::Run(const std::string& strVideoUrl,
                                 const std::string& strBasename, int nThreads,
                                 int nTimeout,
                                 const std::string& strCookiesFromBrowser,
                                 const std::string& strCookie) {
    m_last_error.clear();
    m_output_path.clear();

    /* Cancellation checkpoint: before parsing. */
    if (m_cancel.load()) {
        return VideoResult::Canceled;
    }

    /* ---- Stage 1: parse (synchronous, no progress, stage text only) ---- */
    if (onStage) {
        onStage(STAGE_PARSING);
    }
    Log("[INFO] start parsing video: " + strVideoUrl);
    vector<string> vecStreams;
    string strParseErr;
    if (!ParseVideoUrls(strVideoUrl, vecStreams, strCookiesFromBrowser,
                        strCookie, &strParseErr)) {
        m_last_error =
            "video parsing failed: verify the URL is valid/reachable and "
            "the Python runtime assets are intact";
        if (strParseErr.empty()) {
            strParseErr = "no downloadable media stream obtained "
                          "(no detailed error available)";
        }
        if (!strParseErr.empty()) {
            m_last_error += "\n[detail] " + strParseErr;
        }
        Log("[ERROR] " + m_last_error);
        return VideoResult::Error;
    }
    Log("[INFO] parsing succeeded: " + std::to_string(vecStreams.size()) +
        " media streams");

    /* ---- Stage 2: download each stream (DASH: video + audio, max 2) ---- */
    const size_t nStreamCount =
        vecStreams.size() < 2 ? vecStreams.size() : 2;
    for (size_t nIndex = 0; nIndex < nStreamCount; nIndex++) {
        /* Cancellation checkpoint: before each stream. */
        if (m_cancel.load()) {
            return VideoResult::Canceled;
        }
        const string strOut =
            (nIndex == 0) ? strBasename + ".mp4" : strBasename + ".m4a";
        if (onStage) {
            onStage(nIndex == 0 ? STAGE_VIDEO_DL : STAGE_AUDIO_DL);
        }
        Log("[INFO] downloading stream " + std::to_string(nIndex + 1) +
            " -> " + strOut);

        unique_ptr<Ccurl> cc = make_unique<Ccurl>();
        cc->SetReferer(strVideoUrl);  /* anti-hotlink: video page as Referer */
        if (!strCookie.empty()) {
            cc->SetCookie(strCookie);  /* streams may need login state */
        }
        /* Wrapped progress callback: check the orchestrator cancel flag
         * first, cancel the current stream on cancel (the Ccurl write
         * callback aborts within seconds), then forward to the caller. */
        {
            Ccurl* cc_ptr = cc.get();
            auto user_cb = onProgress;
            cc->onProgress =
                [this, cc_ptr, user_cb](const std::vector<ThreadProgress>& tp,
                                        double dTotalPercent,
                                        double dTotalSpeed) {
                    if (m_cancel.load()) {
                        cc_ptr->Cancel();
                    }
                    if (user_cb) {
                        user_cb(tp, dTotalPercent, dTotalSpeed);
                    }
                };
        }

        if (!cc->Init(vecStreams[nIndex], strOut, nThreads, nTimeout)) {
            m_last_error = cc->LastError().empty()
                               ? ("init failed: " + strOut)
                               : cc->LastError();
            Log("[ERROR] init failed: " + strOut + " - " + m_last_error);
            return VideoResult::Error;
        }
        /* Push an immediate 0-progress snapshot so the UI can draw the
         * battery-cell separators before the first callback. */
        if (onProgress) {
            auto parts = cc->SnapshotParts();
            onProgress(parts, 0.0, 0.0);
        }
        if (!cc->Download_Task()) {
            if (m_cancel.load() || cc->IsCanceled()) {
                Log("[INFO] canceled");
                return VideoResult::Canceled;
            }
            m_last_error = "stream " + std::to_string(nIndex + 1) +
                           " download failed: " + strOut;
            Log("[ERROR] " + m_last_error);
            return VideoResult::Error;
        }
    }

    /* ---- Stage 3: merge (DASH separated tracks; single stream skips) ---- */
    if (nStreamCount > 1) {
        /* Cancellation checkpoint: before merging. */
        if (m_cancel.load()) {
            return VideoResult::Canceled;
        }
        if (onStage) {
            onStage(STAGE_MERGING);
        }
        Log("[INFO] start merging audio/video tracks...");
        const string strVFile = strBasename + ".mp4";
        const string strAFile = strBasename + ".m4a";
        /* Container chosen by the video codec: VP9/AV1 -> .mkv, else .mp4. */
        const string strMerged =
            strBasename + "_full" + SuggestMergeExt(strVFile);
        string strMerr;
        string strMergedUsed = strMerged;
        if (!MergeMp4(strVFile, strAFile, strMerged, strMerr)) {
            /* Generic fallback: when the mp4 container rejects some codecs
             * (e.g. Opus audio / missing HEVC tags), retry with Matroska
             * (.mkv, compatible with almost all codecs). */
            if (strMerged.size() > 4 &&
                strMerged.compare(strMerged.size() - 4, 4, ".mp4") == 0) {
                const string strMergedMkv =
                    strMerged.substr(0, strMerged.size() - 4) + ".mkv";
                string strMerr2;
                if (MergeMp4(strVFile, strAFile, strMergedMkv, strMerr2)) {
                    Log("[INFO] mp4 container incompatible, merged to mkv "
                        "instead -> " + strMergedMkv);
                    strMergedUsed = strMergedMkv;
                    strMerr.clear();
                } else {
                    strMerr += " (mp4 failed); mkv fallback also failed: " +
                               strMerr2;
                }
            }
        }
        if (strMerr.empty()) {
            Log("[INFO] auto-merged audio/video tracks -> " + strMergedUsed);
            m_output_path = strMergedUsed;
            /* On success delete the intermediate track files and chunk
             * resume metadata, keeping only the merged output. */
            std::error_code ec;
            const bool bVOk = std::filesystem::remove(strVFile, ec);
            ec.clear();
            const bool bAOk = std::filesystem::remove(strAFile, ec);
            std::filesystem::remove(strVFile + ".curlbolt.part", ec);
            ec.clear();
            std::filesystem::remove(strAFile + ".curlbolt.part", ec);
            if (bVOk && bAOk) {
                Log("[INFO] cleaned up intermediate files: " + strVFile +
                    ", " + strAFile);
            } else {
                Log("[WARN] failed to clean up intermediate files, delete "
                    "manually: " + strVFile + " and " + strAFile);
            }
        } else {
            m_last_error = "auto merge failed: " + strMerr;
            Log("[ERROR] " + m_last_error);
            Log("[INFO] hint: keep both track files and merge manually with "
                "an external tool: ffmpeg -i " + strVFile + " -i " +
                strAFile + " -c copy " + strMergedUsed);
            return VideoResult::Error;
        }
    } else {
        /* Single stream (e.g. audio-only page): no merge, the output is the
         * video track file. */
        m_output_path = strBasename + ".mp4";
    }

    return VideoResult::Ok;
}
