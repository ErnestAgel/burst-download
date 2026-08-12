/**
 * @file avmerge.cpp
 * @brief Built-in audio/video track merger implementation: FFmpeg static
 *        library two-input remux (-c copy semantics).
 *
 * Implementation notes:
 *   - avformat_open_input opens the video/audio MP4 tracks, output is a new
 *     MP4;
 *   - the output enables +faststart (moov at the front for network playback /
 *     quick seeking);
 *   - the two streams are interleaved by DTS (av_interleaved_write_frame) so
 *     samples stay in time order;
 *   - only codec parameters are copied (avcodec_parameters_copy), media data
 *     is untouched;
 *   - cancellation checkpoints (issue R6) abort the write loop.
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

/* Capture FFmpeg error logs so the real failure reason reaches the error
 * dialog (the GUI has no visible stderr). */
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

/* Suggest the container extension by the video codec: VP9/AV1/VP8 (typical
 * WebM) -> .mkv, otherwise .mp4. */
std::string ExtForVideoCodec(int codec_id) {
  if (codec_id == AV_CODEC_ID_VP9 || codec_id == AV_CODEC_ID_VP8 ||
      codec_id == AV_CODEC_ID_AV1) {
    return ".mkv";
  }
  return ".mp4";
}

/* Rescale a packet's timestamps from the input stream timebase to the
 * output stream timebase. */
void RescalePkt(AVPacket* pkt, const AVStream* is, const AVStream* os) {
  if (pkt->pts != AV_NOPTS_VALUE) {
    pkt->pts = av_rescale_q_rnd(pkt->pts, is->time_base, os->time_base,
                                (AVRounding)(AV_ROUND_NEAR_INF |
                                             AV_ROUND_PASS_MINMAX));
  }
  if (pkt->dts != AV_NOPTS_VALUE) {
    pkt->dts = av_rescale_q_rnd(pkt->dts, is->time_base, os->time_base,
                                (AVRounding)(AV_ROUND_NEAR_INF |
                                             AV_ROUND_PASS_MINMAX));
  }
  if (pkt->duration > 0) {
    pkt->duration = av_rescale_q(pkt->duration, is->time_base, os->time_base);
  }
  pkt->pos = -1;
}

/* Open one input file and copy all its streams into the output context;
 * returns the input stream count or -1. */
int AddInput(AVFormatContext** in_ctx, const char* path,
             AVFormatContext* out_ctx, std::vector<int>& stream_map,
             std::string& err) {
  if (avformat_open_input(in_ctx, path, nullptr, nullptr) != 0) {
    err = "failed to open input file: ";
    err += path;
    return -1;
  }
  for (unsigned i = 0; i < (*in_ctx)->nb_streams; i++) {
    AVStream* ist = (*in_ctx)->streams[i];
    AVStream* ost = avformat_new_stream(out_ctx, nullptr);
    if (!ost) {
      err = "failed to create the output stream";
      return -1;
    }
    if (avcodec_parameters_copy(ost->codecpar, ist->codecpar) < 0) {
      err = "failed to copy codec parameters";
      return -1;
    }
    ost->codecpar->codec_tag = 0; /* let the output muxer decide */
    stream_map.push_back(static_cast<int>(ost->index));
  }
  return static_cast<int>((*in_ctx)->nb_streams);
}

}  // namespace

std::string SuggestMergeExt(const std::string& strVideoPath) {
  AVFormatContext* c = nullptr;
  if (avformat_open_input(&c, strVideoPath.c_str(), nullptr, nullptr) != 0 ||
      !c) {
    return ".mp4";  /* default mp4 when the track cannot be opened */
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

bool MergeMp4(const std::string& strVideoPath,
              const std::string& strAudioPath,
              const std::string& strOutputPath, std::string& strErr,
              const std::atomic<bool>* pbCancel) {
  g_avlog.clear();
  av_log_set_callback(AvLogCapture);
  av_log_set_level(AV_LOG_ERROR);

  AVFormatContext* vctx = nullptr;
  AVFormatContext* actx = nullptr;
  AVFormatContext* octx = nullptr;
  bool ok = false;

  do {
    /* Output container (inferred by the file suffix, e.g. mp4). */
    if (avformat_alloc_output_context2(&octx, nullptr, nullptr,
                                       strOutputPath.c_str()) < 0 ||
        !octx) {
      strErr = "failed to create the output container";
      break;
    }

    /* Open both inputs and map their streams. */
    std::vector<int> v_map, a_map;
    if (AddInput(&vctx, strVideoPath.c_str(), octx, v_map, strErr) < 0) break;
    if (AddInput(&actx, strAudioPath.c_str(), octx, a_map, strErr) < 0) break;
    if (v_map.empty() && a_map.empty()) {
      strErr = "input files contain no media streams";
      break;
    }

    /* Open the output file; +faststart puts moov at the front. */
    if (!(octx->oformat->flags & AVFMT_NOFILE)) {
      if (avio_open(&octx->pb, strOutputPath.c_str(), AVIO_FLAG_WRITE) < 0) {
        strErr = "failed to create the output file: " + strOutputPath;
        break;
      }
    }
    AVDictionary* opts = nullptr;
    /* Only MP4 outputs enable moov-at-front (faststart); Matroska outputs
     * have no such option and would error. */
    if (strOutputPath.size() > 4 &&
        strOutputPath.compare(strOutputPath.size() - 4, 4, ".mp4") == 0) {
      av_dict_set(&opts, "movflags", "+faststart", 0);
    }
    if (avformat_write_header(octx, &opts) < 0) {
      strErr = "failed to write the output header (codec may be "
               "unsupported)";
      av_dict_free(&opts);
      break;
    }
    av_dict_free(&opts);

    /* Two inputs interleaved by DTS (-c copy). */
    AVPacket* pv = av_packet_alloc();
    AVPacket* pa = av_packet_alloc();
    if (!pv || !pa) {
      av_packet_free(&pv);
      av_packet_free(&pa);
      strErr = "out of memory";
      break;
    }
    bool veof = false, aeof = false;
    bool v_pending = false, a_pending = false;
    long long nPackets = 0;
    while (!(veof && aeof)) {
      /* Cancellation checkpoint (issue R6): check every 256 packets. */
      if (((nPackets++ & 255) == 0) && (pbCancel != nullptr) &&
          pbCancel->load()) {
        strErr = "merge canceled";
        break;
      }
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
      /* Interleave by DTS; when either timestamp is invalid, write the valid
       * side first to avoid write failures. */
      if (v_pending && (aeof || !a_pending || pa->dts == AV_NOPTS_VALUE ||
                        (pv->dts != AV_NOPTS_VALUE && pv->dts <= pa->dts))) {
        if (av_interleaved_write_frame(octx, pv) < 0) {
          av_packet_free(&pv);
          av_packet_free(&pa);
          strErr = "failed to write media data";
          break;
        }
        av_packet_unref(pv);
        v_pending = false;
      } else if (a_pending) {
        if (av_interleaved_write_frame(octx, pa) < 0) {
          av_packet_free(&pv);
          av_packet_free(&pa);
          strErr = "failed to write media data";
          break;
        }
        av_packet_unref(pa);
        a_pending = false;
      }
    }
    av_packet_free(&pv);
    av_packet_free(&pa);
    if (!strErr.empty()) break;

    if (av_write_trailer(octx) < 0) {
      strErr = "failed to write the output trailer";
      break;
    }
    ok = true;
  } while (false);

  /* Cleanup. */
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
    remove(strOutputPath.c_str()); /* clean up the partial output */
    /* Append the real FFmpeg error log (trailing whitespace stripped). */
    std::string l = TakeAvLog();
    while (!l.empty() && (l.back() == '\n' || l.back() == '\r' ||
                          l.back() == ' ' || l.back() == '\t')) {
      l.pop_back();
    }
    if (!l.empty()) {
      strErr += " | ffmpeg: ";
      strErr += l;
    }
  }
  return ok;
}
