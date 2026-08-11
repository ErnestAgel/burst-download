/**
 * @file avmerge.cpp
 * @brief 内置音视频轨合并器实现：FFmpeg 静态库双输入 remux（-c copy 语义）
 *
 * 实现要点：
 *   - avformat_open_input 打开视频轨/音频轨两个 MP4，输出到新 MP4
 *   - 输出开启 +faststart（moov 前置，便于网络播放/快速拖动）
 *   - 双流按 DTS 交错写帧（av_interleaved_write_frame），保证样本时间顺序
 *   - 仅拷贝编解码参数（avcodec_parameters_copy），不触碰媒体数据
 *
 * @author ErnestAgel
 * @date 2026-08-06
 * @copyright Copyright (c) 2026 ErnestAgel
 * @license SPDX-License-Identifier: MIT
 */

#include "avmerge.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/codec_id.h>
#include <libavutil/opt.h>
}

namespace {

/* 捕获 FFmpeg 错误日志，便于把真实失败原因带进报错（GUI 无 stderr 可见） */
thread_local std::string g_avlog;

void AvLogCapture(void*, int level, const char* fmt, va_list vl) {
  if (level > AV_LOG_ERROR) return;
  char buf[1024];
  vsnprintf(buf, sizeof(buf), fmt, vl);
  g_avlog += buf;
  if (g_avlog.size() > 4096) {
    g_avlog.erase(0, g_avlog.size() - 4096);
  }
}

std::string TakeAvLog() {
  std::string s = g_avlog;
  g_avlog.clear();
  return s;
}

/* 根据视频流编码建议容器扩展名：VP9/AV1/VP8（WebM 典型）-> .mkv，其余 -> .mp4 */
std::string ExtForVideoCodec(int codec_id) {
  if (codec_id == AV_CODEC_ID_VP9 || codec_id == AV_CODEC_ID_VP8 ||
      codec_id == AV_CODEC_ID_AV1) {
    return ".mkv";
  }
  return ".mp4";
}

/* 将包时间戳从输入流时基换算到输出流时基 */
void RescalePkt(AVPacket* pkt, const AVStream* is, const AVStream* os) {
  if (pkt->pts != AV_NOPTS_VALUE) {
    pkt->pts = av_rescale_q_rnd(pkt->pts, is->time_base, os->time_base,
                                (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
  }
  if (pkt->dts != AV_NOPTS_VALUE) {
    pkt->dts = av_rescale_q_rnd(pkt->dts, is->time_base, os->time_base,
                                (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
  }
  if (pkt->duration > 0) {
    pkt->duration = av_rescale_q(pkt->duration, is->time_base, os->time_base);
  }
  pkt->pos = -1;
}

/* 打开一个输入文件并复制其所有流到输出容器，返回输入流数或 -1 */
int AddInput(AVFormatContext** in_ctx, const char* path,
             AVFormatContext* out_ctx, std::vector<int>& stream_map,
             std::string& err) {
  if (avformat_open_input(in_ctx, path, nullptr, nullptr) != 0) {
    err = "打开输入文件失败: ";
    err += path;
    return -1;
  }
  for (unsigned i = 0; i < (*in_ctx)->nb_streams; i++) {
    AVStream* ist = (*in_ctx)->streams[i];
    AVStream* ost = avformat_new_stream(out_ctx, nullptr);
    if (!ost) {
      err = "创建输出流失败";
      return -1;
    }
    if (avcodec_parameters_copy(ost->codecpar, ist->codecpar) < 0) {
      err = "复制编解码参数失败";
      return -1;
    }
    ost->codecpar->codec_tag = 0; /* 交由输出 muxer 决定 */
    stream_map.push_back(static_cast<int>(ost->index));
  }
  return static_cast<int>((*in_ctx)->nb_streams);
}

}  // namespace

std::string SuggestMergeExt(const std::string& video_path) {
  AVFormatContext* c = nullptr;
  if (avformat_open_input(&c, video_path.c_str(), nullptr, nullptr) != 0 || !c) {
    return ".mp4";  /* 打不开时按默认 mp4 处理 */
  }
  std::string ext = ".mp4";
  for (unsigned i = 0; i < c->nb_streams; i++) {
    if (c->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      ext = ExtForVideoCodec(c->streams[i]->codecpar->codec_id);
      break;
    }
  }
  avformat_close_input(&c);
  return ext;
}

bool MergeMp4(const std::string& video_path, const std::string& audio_path,
              const std::string& output_path, std::string& err) {
  g_avlog.clear();
  av_log_set_callback(AvLogCapture);
  av_log_set_level(AV_LOG_ERROR);

  AVFormatContext* vctx = nullptr;
  AVFormatContext* actx = nullptr;
  AVFormatContext* octx = nullptr;
  bool ok = false;

  do {
    /* 输出容器（按文件名后缀推断 mp4） */
    if (avformat_alloc_output_context2(&octx, nullptr, nullptr,
                                       output_path.c_str()) < 0 ||
        !octx) {
      err = "创建输出容器失败";
      break;
    }

    /* 打开两个输入并映射流 */
    std::vector<int> v_map, a_map;
    if (AddInput(&vctx, video_path.c_str(), octx, v_map, err) < 0) break;
    if (AddInput(&actx, audio_path.c_str(), octx, a_map, err) < 0) break;
    if (v_map.empty() && a_map.empty()) {
      err = "输入文件不含媒体流";
      break;
    }

    /* 打开输出文件，+faststart 让 moov 前置 */
    if (!(octx->oformat->flags & AVFMT_NOFILE)) {
      if (avio_open(&octx->pb, output_path.c_str(), AVIO_FLAG_WRITE) < 0) {
        err = "创建输出文件失败: " + output_path;
        break;
      }
    }
    AVDictionary* opts = nullptr;
    /* 仅 MP4 输出启用 moov 前置（faststart）；Matroska 输出无此选项，传入会报错 */
    if (output_path.size() > 4 &&
        output_path.compare(output_path.size() - 4, 4, ".mp4") == 0) {
      av_dict_set(&opts, "movflags", "+faststart", 0);
    }
    if (avformat_write_header(octx, &opts) < 0) {
      err = "写入输出头失败（编码格式可能不受支持）";
      av_dict_free(&opts);
      break;
    }
    av_dict_free(&opts);

    /* 双输入按 DTS 交错写帧（-c copy） */
    AVPacket* pv = av_packet_alloc();
    AVPacket* pa = av_packet_alloc();
    if (!pv || !pa) {
      av_packet_free(&pv);
      av_packet_free(&pa);
      err = "分配内存失败";
      break;
    }
    bool veof = false, aeof = false;
    bool v_pending = false, a_pending = false;
    while (!(veof && aeof)) {
      if (!v_pending && !veof) {
        if (av_read_frame(vctx, pv) < 0) {
          veof = true;
        } else {
          RescalePkt(pv, vctx->streams[pv->stream_index],
                     octx->streams[v_map[pv->stream_index]]);
          pv->stream_index = v_map[pv->stream_index];
          v_pending = true;
        }
      }
      if (!a_pending && !aeof) {
        if (av_read_frame(actx, pa) < 0) {
          aeof = true;
        } else {
          RescalePkt(pa, actx->streams[pa->stream_index],
                     octx->streams[a_map[pa->stream_index]]);
          pa->stream_index = a_map[pa->stream_index];
          a_pending = true;
        }
      }
      if (veof && aeof) break;
      /* 交错写帧：按 DTS 排序；任一时间戳无效时按“先写有效侧”处理，避免写入失败 */
      if (v_pending && (aeof || !a_pending || pa->dts == AV_NOPTS_VALUE ||
                        (pv->dts != AV_NOPTS_VALUE && pv->dts <= pa->dts))) {
        if (av_interleaved_write_frame(octx, pv) < 0) {
          av_packet_free(&pv);
          av_packet_free(&pa);
          err = "写入媒体数据失败";
          break;
        }
        av_packet_unref(pv);
        v_pending = false;
      } else if (a_pending) {
        if (av_interleaved_write_frame(octx, pa) < 0) {
          av_packet_free(&pv);
          av_packet_free(&pa);
          err = "写入媒体数据失败";
          break;
        }
        av_packet_unref(pa);
        a_pending = false;
      }
    }
    av_packet_free(&pv);
    av_packet_free(&pa);
    if (!err.empty()) break;

    if (av_write_trailer(octx) < 0) {
      err = "写入输出尾部失败";
      break;
    }
    ok = true;
  } while (false);

  /* 清理 */
  if (vctx) avformat_close_input(&vctx);
  if (actx) avformat_close_input(&actx);
  if (octx) {
    if (!(octx->oformat->flags & AVFMT_NOFILE) && octx->pb)
      avio_closep(&octx->pb);
    avformat_free_context(octx);
  }
  av_log_set_callback(av_log_default_callback);
  av_log_set_level(AV_LOG_INFO);
  if (!ok) {
    remove(output_path.c_str()); /* 失败清理半成品 */
    /* 附上 FFmpeg 真实错误日志（去行尾空白） */
    std::string l = TakeAvLog();
    while (!l.empty() && (l.back() == '\n' || l.back() == '\r' ||
                          l.back() == ' ' || l.back() == '\t')) {
      l.pop_back();
    }
    if (!l.empty()) {
      err += " | ffmpeg: ";
      err += l;
    }
  }
  return ok;
}
