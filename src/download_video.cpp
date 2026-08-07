/**
 * @file download_video.cpp
 * @brief 视频下载编排实现（见 download_video.h）
 *
 * 逻辑源自 main.cpp 的 DownloadVideo（CLI），抽取为 CLI/GUI 共用：
 *   - 阶段回调 onStage 供 GUI 显示"解析中/下载视频轨/下载音频轨/合并中"（F8）；
 *   - 每流使用独立 Ccurl 实例（SetReferer 防盗链 + 可选 Cookie），onProgress 透传；
 *   - 取消检查点：解析前、每流下载前、合并前（§5.2）；下载中经 Ccurl 写回调中止；
 *   - 合并成功后清理音视频中间文件（与 CLI 一致）；合并失败保留两轨供手动合并。
 *
 * @author ErnestAgel
 * @date 2026-08-07
 * @license SPDX-License-Identifier: MIT
 */

#include "download_video.h"

#include <cstdio>
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

void VideoDownloader::Log(const std::string& msg) {
    if (onLog) {
        onLog(msg);
    } else {
        printf("%s\n", msg.c_str());
    }
}

VideoResult VideoDownloader::Run(const std::string& video_url,
                                 const std::string& basename, int threads,
                                 int timeout,
                                 const std::string& cookies_from_browser,
                                 const std::string& cookie) {
    m_last_error.clear();
    m_output_path.clear();

    /* 取消检查点：解析前（§5.2） */
    if (m_cancel.load()) {
        return VideoResult::Canceled;
    }

    /* ---- 阶段 1：解析（同步阻塞，无进度，仅阶段状态） ---- */
    if (onStage) {
        onStage(STAGE_PARSING);
    }
    Log("[INFO] 开始解析视频: " + video_url);
    vector<string> streams;
    if (!ParseVideoUrls(video_url, streams, cookies_from_browser, cookie)) {
        m_last_error =
            "视频解析失败：请确认 URL 有效/可访问，且 Python 运行时资源完整";
        Log("[ERROR] " + m_last_error);
        return VideoResult::Error;
    }
    Log("[INFO] 解析成功: 共 " + std::to_string(streams.size()) +
        " 个媒体流");

    /* ---- 阶段 2：逐流下载（DASH 分离时最多视频轨 + 音频轨 2 个流） ---- */
    const size_t stream_count = streams.size() < 2 ? streams.size() : 2;
    for (size_t i = 0; i < stream_count; i++) {
        /* 取消检查点：每流下载前 */
        if (m_cancel.load()) {
            return VideoResult::Canceled;
        }
        const string out =
            (i == 0) ? basename + ".mp4" : basename + ".m4a";
        if (onStage) {
            onStage(i == 0 ? STAGE_VIDEO_DL : STAGE_AUDIO_DL);
        }
        Log("[INFO] 正在下载第 " + std::to_string(i + 1) + " 个流 -> " + out);

        unique_ptr<Ccurl> cc = make_unique<Ccurl>();
        cc->SetReferer(video_url); /* 防盗链：以视频页 URL 作为 Referer（如 B站视频流） */
        if (!cookie.empty()) {
            cc->SetCookie(cookie); /* 下载流时携带 Cookie（高清流需登录态） */
        }
        cc->onProgress = onProgress; /* GUI 注入进度回调；CLI 为空走默认 1% 门控打印 */

        if (!cc->Init(streams[i], out, threads, timeout)) {
            m_last_error = cc->LastError().empty()
                               ? ("初始化失败: " + out)
                               : cc->LastError();
            Log("[ERROR] 初始化失败: " + out + " - " + m_last_error);
            return VideoResult::Error;
        }
        if (!cc->Download_Task()) {
            if (m_cancel.load() || cc->IsCanceled()) {
                Log("[INFO] 已取消");
                return VideoResult::Canceled;
            }
            m_last_error = "第 " + std::to_string(i + 1) + " 个流下载失败: " +
                           out;
            Log("[ERROR] " + m_last_error);
            return VideoResult::Error;
        }
    }

    /* ---- 阶段 3：合并（音视频分离流 DASH；单流跳过） ---- */
    if (stream_count > 1) {
        /* 取消检查点：合并前 */
        if (m_cancel.load()) {
            return VideoResult::Canceled;
        }
        if (onStage) {
            onStage(STAGE_MERGING);
        }
        Log("[INFO] 开始合并音视频轨...");
        const string vfile = basename + ".mp4";
        const string afile = basename + ".m4a";
        /* 输出容器按视频轨编码自动选择：VP9/AV1 -> .mkv，其余 -> .mp4 */
        const string merged = basename + "_full" + SuggestMergeExt(vfile);
        string merr;
        if (MergeMp4(vfile, afile, merged, merr)) {
            Log("[INFO] 已自动合并音视频轨 -> " + merged);
            m_output_path = merged;
            /* 合并成功：删除音视频中间文件，仅保留合并产物 */
            if (remove(vfile.c_str()) == 0 && remove(afile.c_str()) == 0) {
                Log("[INFO] 已清理中间文件: " + vfile + ", " + afile);
            } else {
                Log("[WARN] 中间文件清理失败，可手动删除 " + vfile + " 和 " +
                    afile);
            }
        } else {
            m_last_error = "自动合并失败: " + merr;
            Log("[ERROR] " + m_last_error);
            Log("[INFO] 提示: 可保留两轨文件，用外部工具手动合并: "
                "ffmpeg -i " + vfile + " -i " + afile + " -c copy " + merged);
            return VideoResult::Error; /* 两轨已下载，但用户期望单文件 → 报错指引 */
        }
    } else {
        /* 单流（如纯音频视频页）：无合并，产物即视频轨文件 */
        m_output_path = basename + ".mp4";
    }

    return VideoResult::Ok;
}
