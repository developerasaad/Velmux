#include "velmux/ffmpeg/ffmpeg_backend.h"
#include "velmux/ffmpeg/codec_mapping.h"

#include "velmux/core/compression.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <sys/stat.h>
#include <unordered_map>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace velmux::ffmpeg {

using namespace velmux::core;
using velmux::backend::ExecuteParams;
using velmux::backend::ExecuteResult;
using velmux::backend::ProgressReport;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::optional<std::string> localPathFor(const MediaSource &source) {
  if (source.uri.rfind("file://", 0) == 0) {
    return source.uri.substr(7);
  }
  if (source.uri.rfind('/', 0) == 0) {
    return source.uri;
  }
  return std::nullopt;
}

namespace {

std::once_flag g_ffmpegInit;
std::string g_ffmpegVersion;

void initFfmpegLabel() {
  std::call_once(g_ffmpegInit, [] {
    g_ffmpegVersion =
        std::string("ffmpeg ") + av_version_info() + " (feature-selected)";
  });
}

std::string avErrorString(int errnum) {
  char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
  av_strerror(errnum, buf, sizeof(buf));
  return buf;
}

Error makeAvError(ErrorCode code, const std::string &context, int errnum) {
  return Error::make(code, context + ": " + avErrorString(errnum), "ffmpeg");
}

// Map a Velmux quality request (0..100) to the encoder's CRF-style quality
// parameter (higher quality -> lower CRF).
int qualityToCrf(VideoCodec codec, std::int32_t quality) {
  const int q = std::clamp(quality, 0, 100);
  switch (codec) {
    case VideoCodec::H264: // libx264 crf 0..51
      return 51 - (q * 51) / 100;
    case VideoCodec::AV1: // libsvtav1 crf 0..63
    case VideoCodec::VP9: // libvpx-vp9 crf 0..63
    case VideoCodec::VP8:
      return 63 - (q * 63) / 100;
    default:
      return 23;
  }
}

// Concrete libavcodec encoder names for the feature-selected build: prefer
// the external libraries Velmux pins (tools/ffmpeg-builder/versions.json).
const char *encoderNameFor(VideoCodec codec) {
  switch (codec) {
    case VideoCodec::H264:
      return "libx264";
    case VideoCodec::AV1:
      return "libsvtav1";
    case VideoCodec::VP8:
      return "libvpx-vp8";
    case VideoCodec::VP9:
      return "libvpx-vp9";
    case VideoCodec::Mpeg4Part2:
      return "mpeg4";
    case VideoCodec::Ffv1:
      return "ffv1";
    case VideoCodec::Mjpeg:
      return "mjpeg";
    default:
      return nullptr; // HEVC software encoder not pinned in V1 host build
  }
}

const char *encoderNameFor(AudioCodec codec) {
  switch (codec) {
    case AudioCodec::Aac:
    case AudioCodec::HeAac:
      return "aac"; // FFmpeg-native AAC encoder
    case AudioCodec::Mp3:
      return "libmp3lame";
    case AudioCodec::Opus:
      return "libopus";
    case AudioCodec::Vorbis:
      return "libvorbis";
    case AudioCodec::Flac:
      return "flac";
    case AudioCodec::Alac:
      return "alac";
    case AudioCodec::Pcm:
      return "pcm_s16le";
    case AudioCodec::Ac3:
      return "ac3";
    case AudioCodec::Eac3:
      return "eac3";
    case AudioCodec::WavPack:
      return "wavpack";
    default:
      return nullptr;
  }
}

struct CodecCleanup {
  ~CodecCleanup() {
    av_frame_free(&frame);
    av_packet_free(&packet);
    sws_freeContext(sws);
    swr_free(&swr);
    avcodec_free_context(&videoDec);
    avcodec_free_context(&audioDec);
    avcodec_free_context(&videoEnc);
    avcodec_free_context(&audioEnc);
    if (out != nullptr) {
      if (!(out->oformat->flags & AVFMT_NOFILE) && out->pb != nullptr) {
        avio_closep(&out->pb);
      }
      avformat_free_context(out);
    }
    if (in != nullptr) {
      avformat_close_input(&in);
    }
  }
  AVFormatContext *in = nullptr;
  AVFormatContext *out = nullptr;
  AVCodecContext *videoDec = nullptr;
  AVCodecContext *audioDec = nullptr;
  AVCodecContext *videoEnc = nullptr;
  AVCodecContext *audioEnc = nullptr;
  SwsContext *sws = nullptr;
  SwrContext *swr = nullptr;
  AVFrame *frame = nullptr;
  AVPacket *packet = nullptr;
};

} // namespace

// ---------------------------------------------------------------------------
// FfmpegInspector
// ---------------------------------------------------------------------------

Error FfmpegInspector::inspect(const MediaSource &source, MediaInfo &out) {
  initFfmpegLabel();
  const std::optional<std::string> path = localPathFor(source);
  if (!path.has_value()) {
    return Error::make(ErrorCode::SourceUnavailable,
                       "unsupported source URI (backend resolves local paths "
                       "and platform-mapped handles)",
                       "ffmpeg");
  }

  AVFormatContext *fmt = nullptr;
  int rc = avformat_open_input(&fmt, path->c_str(), nullptr, nullptr);
  if (rc < 0) {
    return makeAvError(ErrorCode::InspectionFailed, "cannot open input", rc);
  }
  rc = avformat_find_stream_info(fmt, nullptr);
  if (rc < 0) {
    avformat_close_input(&fmt);
    return makeAvError(ErrorCode::InspectionFailed, "cannot read stream info",
                       rc);
  }

  out = MediaInfo{};
  out.durationSec = fmt->duration != AV_NOPTS_VALUE
                        ? double(fmt->duration) / double(AV_TIME_BASE)
                        : 0.0;
  if (out.durationSec <= 0.0) {
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
      const AVStream *stream = fmt->streams[i];
      if (stream->duration != AV_NOPTS_VALUE && stream->time_base.den > 0) {
        const double d = double(stream->duration) *
                         double(stream->time_base.num) /
                         double(stream->time_base.den);
        out.durationSec = std::max(out.durationSec, d);
      }
    }
  }
  out.sizeBytes = 0;
  if (fmt->pb != nullptr) {
    const std::int64_t sz = avio_size(fmt->pb);
    if (sz > 0) {
      out.sizeBytes = sz;
    }
  }
  if (out.sizeBytes <= 0) {
    struct stat st{};
    if (::stat(path->c_str(), &st) == 0) {
      out.sizeBytes = st.st_size;
    }
  }
  if (auto container = containerFromAvFormatName(fmt->iformat->name)) {
    out.container = *container;
  }

  for (unsigned i = 0; i < fmt->nb_streams; ++i) {
    const AVStream *stream = fmt->streams[i];
    const AVCodecParameters *par = stream->codecpar;
    const std::string language = [&]() {
      if (const AVDictionaryEntry *entry =
              av_dict_get(stream->metadata, "language", nullptr, 0)) {
        return std::string(entry->value);
      }
      return std::string();
    }();
    switch (par->codec_type) {
      case AVMEDIA_TYPE_VIDEO: {
        if (auto codec = videoCodecFromAv(par->codec_id)) {
          VideoStream vs;
          vs.codec = *codec;
          vs.width = par->width;
          vs.height = par->height;
          vs.frameRate =
              stream->avg_frame_rate.den > 0
                  ? double(stream->avg_frame_rate.num) /
                        double(stream->avg_frame_rate.den)
                  : 0.0;
          vs.bitRate = par->bit_rate;
          if (par->format != -1) {
            const char *pf = av_get_pix_fmt_name(
                static_cast<AVPixelFormat>(par->format));
            if (pf != nullptr) {
              vs.pixelFormat = pf;
            }
          }
          vs.hdr = par->color_primaries == AVCOL_PRI_BT2020 ||
                   par->color_trc == AVCOL_TRC_SMPTE2084 ||
                   par->color_trc == AVCOL_TRC_ARIB_STD_B67;
          vs.language = language;
          vs.isDefault = (stream->disposition & AV_DISPOSITION_DEFAULT) != 0;
          out.video.push_back(std::move(vs));
        }
        break;
      }
      case AVMEDIA_TYPE_AUDIO: {
        if (auto codec = audioCodecFromAv(par->codec_id)) {
          AudioStream as;
          as.codec = *codec;
          as.channels = par->ch_layout.nb_channels;
          as.sampleRate = par->sample_rate;
          as.bitRate = par->bit_rate;
          as.language = language;
          as.isDefault = (stream->disposition & AV_DISPOSITION_DEFAULT) != 0;
          out.audio.push_back(std::move(as));
        }
        break;
      }
      case AVMEDIA_TYPE_SUBTITLE: {
        if (auto format = subtitleFormatFromAv(par->codec_id)) {
          SubtitleStream ss;
          ss.format = *format;
          ss.language = language;
          ss.isDefault = (stream->disposition & AV_DISPOSITION_DEFAULT) != 0;
          ss.isForced = (stream->disposition & AV_DISPOSITION_FORCED) != 0;
          out.subtitles.push_back(std::move(ss));
        }
        break;
      }
      default:
        break;
    }
  }

  avformat_close_input(&fmt);
  if (out.video.empty() && out.audio.empty()) {
    return Error::make(ErrorCode::InspectionFailed,
                       "no recognizable Velmux-supported streams", "ffmpeg");
  }
  return {};
}

// ---------------------------------------------------------------------------
// FfmpegExecutor
// ---------------------------------------------------------------------------

bool FfmpegExecutor::canExecute(const Pipeline &pipeline,
                                const ConversionRequest &request) const {
  for (const auto &assignment : pipeline.stages) {
    if (assignment.backend != BackendId::Ffmpeg) {
      return false;
    }
  }
  return !pipeline.stages.empty();
}

ExecuteResult FfmpegExecutor::execute(const ExecuteParams &params) {
  if (params.pipeline.videoStreamCopy && params.pipeline.audioStreamCopy) {
    return executeRemux(params);
  }
  return executeTranscode(params);
}

ExecuteResult FfmpegExecutor::executeRemux(const ExecuteParams &params) {
  ExecuteResult result;
  initFfmpegLabel();
  const std::optional<std::string> inPath =
      localPathFor(params.request.source);
  if (!inPath.has_value()) {
    result.error = Error::make(ErrorCode::SourceUnavailable,
                               "unsupported source URI", "ffmpeg");
    return result;
  }

  CodecCleanup cleanup;
  int rc = avformat_open_input(&cleanup.in, inPath->c_str(), nullptr, nullptr);
  if (rc < 0) {
    result.error = makeAvError(ErrorCode::SourceUnavailable, "open input", rc);
    return result;
  }
  if (avformat_find_stream_info(cleanup.in, nullptr) < 0) {
    result.error = Error::make(ErrorCode::ExecutionFailed,
                               "cannot read stream info", "ffmpeg");
    return result;
  }
  rc = avformat_alloc_output_context2(
      &cleanup.out, nullptr, avFormatNameFor(params.pipeline.container),
      params.outputPath.c_str());
  if (rc < 0 || cleanup.out == nullptr) {
    result.error = makeAvError(ErrorCode::ExecutionFailed,
                               "alloc output context", rc);
    return result;
  }

  // Decide which input streams survive (policy decided by the core:
  // video/audio preserve; subtitles per the user's explicit policy).
  std::vector<int> inIndexOfOut;
  std::vector<AVBSFContext *> bitstreamFilters;
  for (unsigned i = 0; i < cleanup.in->nb_streams; ++i) {
    const AVStream *inStream = cleanup.in->streams[i];
    const AVMediaType type = inStream->codecpar->codec_type;
    const bool keepVideo = type == AVMEDIA_TYPE_VIDEO &&
                           params.request.video.enabled;
    const bool keepAudio = type == AVMEDIA_TYPE_AUDIO &&
                           params.request.audio.enabled;
    const bool keepSub = type == AVMEDIA_TYPE_SUBTITLE &&
                         params.request.subtitles.policy ==
                             SubtitlePolicy::Preserve;
    if (!keepVideo && !keepAudio && !keepSub) {
      continue;
    }
    AVStream *outStream = avformat_new_stream(cleanup.out, nullptr);
    if (outStream == nullptr) {
      continue;
    }
    avcodec_parameters_copy(outStream->codecpar, inStream->codecpar);
    outStream->codecpar->codec_tag = 0;

    // Container-mandated bitstream transformation (spec §31).
    const AVCodecID cid = inStream->codecpar->codec_id;
    const bool isTs = strcmp(cleanup.out->oformat->name, "mpegts") == 0;
    const bool needsBsf =
        (cid == AV_CODEC_ID_H264 && isTs) || (cid == AV_CODEC_ID_HEVC && isTs);
    if (needsBsf) {
      const char *bsfName =
          cid == AV_CODEC_ID_H264 ? "h264_mp4toannexb" : "hevc_mp4toannexb";
      const AVBitStreamFilter *bsf = av_bsf_get_by_name(bsfName);
      AVBSFContext *bsfCtx = nullptr;
      if (bsf != nullptr && av_bsf_alloc(bsf, &bsfCtx) == 0) {
        avcodec_parameters_copy(bsfCtx->par_in, inStream->codecpar);
        bsfCtx->time_base_in = inStream->time_base;
        if (av_bsf_init(bsfCtx) == 0) {
          bitstreamFilters.push_back(bsfCtx);
        } else {
          av_bsf_free(&bsfCtx);
        }
      }
    }
    inIndexOfOut.push_back(int(i));
  }

  if (!(cleanup.out->oformat->flags & AVFMT_NOFILE)) {
    rc = avio_open(&cleanup.out->pb, params.outputPath.c_str(),
                   AVIO_FLAG_WRITE);
    if (rc < 0) {
      result.error =
          makeAvError(ErrorCode::ExecutionFailed, "open output", rc);
      return result;
    }
  }
  rc = avformat_write_header(cleanup.out, nullptr);
  if (rc < 0) {
    result.error =
        makeAvError(ErrorCode::ExecutionFailed, "write header", rc);
    return result;
  }

  cleanup.packet = av_packet_alloc();
  const std::int64_t totalBytes =
      cleanup.in->pb != nullptr ? avio_size(cleanup.in->pb) : 0;
  std::int64_t processedPackets = 0;
  bool cancelled = false;
  bool writeFailed = false;
  while (av_read_frame(cleanup.in, cleanup.packet) >= 0) {
    if (params.cancel && params.cancel->isCancelled()) {
      cancelled = true;
      av_packet_unref(cleanup.packet);
      break;
    }
    int outIndex = -1;
    for (std::size_t k = 0; k < inIndexOfOut.size(); ++k) {
      if (inIndexOfOut[k] == cleanup.packet->stream_index) {
        outIndex = int(k);
        break;
      }
    }
    if (outIndex < 0) {
      av_packet_unref(cleanup.packet);
      continue;
    }
    AVPacket *toWrite = cleanup.packet;
    for (auto *bsfCtx : bitstreamFilters) {
      if (bsfCtx->par_in->codec_id ==
          cleanup.in->streams[cleanup.packet->stream_index]->codecpar
              ->codec_id) {
        if (av_bsf_send_packet(bsfCtx, cleanup.packet) == 0) {
          AVPacket *filtered = av_packet_alloc();
          if (av_bsf_receive_packet(bsfCtx, filtered) == 0) {
            toWrite = filtered;
            av_packet_unref(cleanup.packet);
          } else {
            av_packet_free(&filtered);
          }
        }
        break;
      }
    }

    AVPacket outPacket = *toWrite;
    outPacket.stream_index = outIndex;
    const AVStream *srcStream = cleanup.in->streams[toWrite->stream_index];
    outPacket.pts = av_rescale_q_rnd(
        toWrite->pts, srcStream->time_base,
        cleanup.out->streams[outIndex]->time_base,
        AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
    outPacket.dts = av_rescale_q_rnd(
        toWrite->dts, srcStream->time_base,
        cleanup.out->streams[outIndex]->time_base,
        AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
    outPacket.duration =
        av_rescale_q(toWrite->duration, srcStream->time_base,
                     cleanup.out->streams[outIndex]->time_base);
    outPacket.pos = -1;

    if (av_interleaved_write_frame(cleanup.out, &outPacket) < 0) {
      writeFailed = true;
    }
    if (toWrite != cleanup.packet) {
      av_packet_free(&toWrite);
    }
    av_packet_unref(cleanup.packet);
    ++processedPackets;
    if (writeFailed) {
      break;
    }

    if (params.onProgress && totalBytes > 0 && (processedPackets % 64) == 0) {
      ProgressReport report;
      report.totalBytes = totalBytes;
      report.processedBytes =
          cleanup.in->pb != nullptr ? avio_tell(cleanup.in->pb) : 0;
      report.fraction = double(report.processedBytes) / double(totalBytes);
      params.onProgress(report);
    }
  }

  if (!writeFailed) {
    av_write_trailer(cleanup.out);
  }
  for (auto *bsfCtx : bitstreamFilters) {
    av_bsf_free(&bsfCtx);
  }

  if (cancelled) {
    result.error =
        Error::make(ErrorCode::Cancelled, "remux cancelled", "ffmpeg");
    return result;
  }
  if (writeFailed) {
    result.error = Error::make(ErrorCode::ExecutionFailed,
                               "write failed during remux", "ffmpeg");
    return result;
  }
  result.success = true;
  return result;
}

ExecuteResult FfmpegExecutor::executeTranscode(const ExecuteParams &params) {
  ExecuteResult result;
  initFfmpegLabel();
  const ConversionRequest &request = params.request;
  const Pipeline &pipeline = params.pipeline;

  const std::optional<std::string> inPath = localPathFor(request.source);
  if (!inPath.has_value()) {
    result.error = Error::make(ErrorCode::SourceUnavailable,
                               "unsupported source URI", "ffmpeg");
    return result;
  }

  CodecCleanup cleanup;
  int rc = avformat_open_input(&cleanup.in, inPath->c_str(), nullptr, nullptr);
  if (rc < 0) {
    result.error = makeAvError(ErrorCode::SourceUnavailable, "open input", rc);
    return result;
  }
  if (avformat_find_stream_info(cleanup.in, nullptr) < 0) {
    result.error = Error::make(ErrorCode::ExecutionFailed,
                               "cannot read stream info", "ffmpeg");
    return result;
  }

  int videoIn = -1, audioIn = -1;
  for (unsigned i = 0; i < cleanup.in->nb_streams; ++i) {
    const AVMediaType type = cleanup.in->streams[i]->codecpar->codec_type;
    if (type == AVMEDIA_TYPE_VIDEO && videoIn < 0) {
      videoIn = int(i);
    } else if (type == AVMEDIA_TYPE_AUDIO && audioIn < 0) {
      audioIn = int(i);
    }
  }

  rc = avformat_alloc_output_context2(
      &cleanup.out, nullptr, avFormatNameFor(pipeline.container),
      params.outputPath.c_str());
  if (rc < 0 || cleanup.out == nullptr) {
    result.error = makeAvError(ErrorCode::ExecutionFailed,
                               "alloc output context", rc);
    return result;
  }

  // ---- Video setup ---------------------------------------------------------
  int videoOut = -1;
  AVFrame *videoFrame = nullptr; // owned locally, freed at exit
  if (videoIn >= 0 && request.video.enabled) {
    const AVStream *inVideo = cleanup.in->streams[videoIn];
    const AVCodec *decCodec =
        avcodec_find_decoder(inVideo->codecpar->codec_id);
    if (decCodec == nullptr) {
      result.error = Error::make(
          ErrorCode::UnsupportedFeature,
          std::string("no FFmpeg decoder for input video codec: ") +
              avcodec_get_name(inVideo->codecpar->codec_id),
          "ffmpeg");
      return result;
    }
    cleanup.videoDec = avcodec_alloc_context3(decCodec);
    avcodec_parameters_to_context(cleanup.videoDec, inVideo->codecpar);
    if (avcodec_open2(cleanup.videoDec, decCodec, nullptr) < 0) {
      result.error = Error::make(ErrorCode::ExecutionFailed,
                                 "cannot open video decoder", "ffmpeg");
      return result;
    }

    const char *encName = encoderNameFor(pipeline.videoCodec);
    const AVCodec *encCodec = encName != nullptr
                                  ? avcodec_find_encoder_by_name(encName)
                                  : nullptr;
    if (encCodec == nullptr) {
      encCodec = avcodec_find_encoder(avCodecFor(pipeline.videoCodec));
    }
    if (encCodec == nullptr) {
      result.error = Error::make(
          ErrorCode::UnsupportedFeature,
          std::string("no FFmpeg encoder for output video codec: ") +
              codecName(pipeline.videoCodec),
          "ffmpeg");
      return result;
    }
    cleanup.videoEnc = avcodec_alloc_context3(encCodec);

    // Target geometry: requested resolution or source size; never upscale.
    std::int32_t outH =
        request.video.resolutionHeight.value_or(cleanup.videoDec->height);
    if (cleanup.videoDec->height > 0) {
      outH = std::min(outH, cleanup.videoDec->height);
    }
    outH = std::max(outH & ~1, 16);
    const double aspect =
        cleanup.videoDec->width > 0 && cleanup.videoDec->height > 0
            ? double(cleanup.videoDec->width) /
                  double(cleanup.videoDec->height)
            : 16.0 / 9.0;
    std::int32_t outW = std::int32_t(std::lround(outH * aspect)) & ~1;
    outW = std::max(outW, 16);
    cleanup.videoEnc->width = outW;
    cleanup.videoEnc->height = outH;
    if (inVideo->avg_frame_rate.den > 0 && inVideo->avg_frame_rate.num > 0) {
      cleanup.videoEnc->framerate = inVideo->avg_frame_rate;
      cleanup.videoEnc->time_base = AVRational{inVideo->avg_frame_rate.den,
                                               inVideo->avg_frame_rate.num};
    } else {
      cleanup.videoEnc->framerate = AVRational{30, 1};
      cleanup.videoEnc->time_base = AVRational{1, 30};
    }
    cleanup.videoEnc->pix_fmt = AV_PIX_FMT_YUV420P;
    if (cleanup.out->oformat->flags & AVFMT_GLOBALHEADER) {
      cleanup.videoEnc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    if (request.video.rateControl == RateControl::Bitrate &&
        request.video.bitRate.has_value()) {
      cleanup.videoEnc->bit_rate = *request.video.bitRate;
      cleanup.videoEnc->rc_max_rate = *request.video.bitRate;
      cleanup.videoEnc->rc_buffer_size = *request.video.bit_rate * 2;
    } else {
      // Quality-driven (CRF-style): explicit quality or goal-derived.
      const std::int32_t qualityPct =
          request.video.quality.has_value()
              ? *request.video.quality
              : std::int32_t(qualityFactorForGoal(request.goal.kind) * 100.0);
      const int crf = qualityToCrf(pipeline.videoCodec, qualityPct);
      av_opt_set(cleanup.videoEnc->priv_data, "crf",
                 std::to_string(crf).c_str(), 0);
      // SVT-AV1 uses a different option name; set both when accepted.
      if (pipeline.videoCodec == VideoCodec::AV1) {
        av_opt_set(cleanup.videoEnc->priv_data, "preset", "8", 0);
      }
    }
    if (avcodec_open2(cleanup.videoEnc, encCodec, nullptr) < 0) {
      result.error = Error::make(ErrorCode::ExecutionFailed,
                                 "cannot open video encoder", "ffmpeg");
      return result;
    }
    videoOut = int(cleanup.out->nb_streams);
    AVStream *outVideo = avformat_new_stream(cleanup.out, nullptr);
    avcodec_parameters_from_context(outVideo->codecpar, cleanup.videoEnc);
    outVideo->time_base = cleanup.videoEnc->time_base;

    cleanup.sws = sws_getContext(
        cleanup.videoDec->width, cleanup.videoDec->height,
        static_cast<AVPixelFormat>(cleanup.videoDec->pix_fmt), outW, outH,
        cleanup.videoEnc->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (cleanup.sws == nullptr) {
      result.error = Error::make(ErrorCode::ExecutionFailed,
                                 "cannot create pixel converter", "ffmpeg");
      return result;
    }
    videoFrame = av_frame_alloc();
    videoFrame->format = cleanup.videoEnc->pix_fmt;
    videoFrame->width = outW;
    videoFrame->height = outH;
    if (av_frame_get_buffer(videoFrame, 0) < 0) {
      av_frame_free(&videoFrame);
      result.error = Error::make(ErrorCode::ExecutionFailed,
                                 "cannot allocate video frame", "ffmpeg");
      return result;
    }
  }

  // ---- Audio setup -----------------------------------------------------------
  int audioOut = -1;
  if (audioIn >= 0 && request.audio.enabled) {
    const AVStream *inAudio = cleanup.in->streams[audioIn];
    const AVCodec *decCodec =
        avcodec_find_decoder(inAudio->codecpar->codec_id);
    if (decCodec == nullptr) {
      result.error = Error::make(
          ErrorCode::UnsupportedFeature,
          std::string("no FFmpeg decoder for input audio codec: ") +
              avcodec_get_name(inAudio->codecpar->codec_id),
          "ffmpeg");
      av_frame_free(&videoFrame);
      return result;
    }
    cleanup.audioDec = avcodec_alloc_context3(decCodec);
    avcodec_parameters_to_context(cleanup.audioDec, inAudio->codecpar);
    if (avcodec_open2(cleanup.audioDec, decCodec, nullptr) < 0) {
      result.error = Error::make(ErrorCode::ExecutionFailed,
                                 "cannot open audio decoder", "ffmpeg");
      av_frame_free(&videoFrame);
      return result;
    }

    const char *encName = encoderNameFor(pipeline.audioCodec);
    const AVCodec *encCodec = encName != nullptr
                                  ? avcodec_find_encoder_by_name(encName)
                                  : nullptr;
    if (encCodec == nullptr) {
      encCodec = avcodec_find_encoder(avCodecFor(pipeline.audioCodec));
    }
    if (encCodec == nullptr) {
      result.error = Error::make(
          ErrorCode::UnsupportedFeature,
          std::string("no FFmpeg encoder for output audio codec: ") +
              codecName(pipeline.audioCodec),
          "ffmpeg");
      av_frame_free(&videoFrame);
      return result;
    }
    cleanup.audioEnc = avcodec_alloc_context3(encCodec);
    cleanup.audioEnc->sample_rate =
        request.audio.sampleRate.value_or(cleanup.audioDec->sample_rate);
    if (cleanup.audioEnc->sample_rate <= 0) {
      cleanup.audioEnc->sample_rate = 48000;
    }
    const std::int32_t channels = request.audio.channels.value_or(
        cleanup.audioDec->ch_layout.nb_channels);
    av_channel_layout_default(&cleanup.audioEnc->ch_layout,
                              std::clamp(channels, 1, 8));
    if (encCodec->sample_fmts != nullptr) {
      // Prefer planar float when available (best supported by pinned encs).
      cleanup.audioEnc->sample_fmt = encCodec->sample_fmts[0];
      for (int i = 0; encCodec->sample_fmts[i] != -1; ++i) {
        if (encCodec->sample_fmts[i] == AV_SAMPLE_FMT_FLTP) {
          cleanup.audioEnc->sample_fmt = AV_SAMPLE_FMT_FLTP;
          break;
        }
      }
    } else {
      cleanup.audioEnc->sample_fmt = AV_SAMPLE_FMT_FLTP;
    }
    const std::int64_t defaultAudioBitrate =
        cleanup.audioDec->bit_rate > 0 ? cleanup.audioDec->bit_rate : 128000;
    cleanup.audioEnc->bit_rate =
        request.audio.bitRate.value_or(
            std::clamp<std::int64_t>(defaultAudioBitrate, 64000, 320000));
    if (cleanup.out->oformat->flags & AVFMT_GLOBALHEADER) {
      cleanup.audioEnc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    if (avcodec_open2(cleanup.audioEnc, encCodec, nullptr) < 0) {
      result.error = Error::make(ErrorCode::ExecutionFailed,
                                 "cannot open audio encoder", "ffmpeg");
      av_frame_free(&videoFrame);
      return result;
    }
    audioOut = int(cleanup.out->nb_streams);
    AVStream *outAudio = avformat_new_stream(cleanup.out, nullptr);
    avcodec_parameters_from_context(outAudio->codecpar, cleanup.audioEnc);
    outAudio->time_base = AVRational{1, cleanup.audioEnc->sample_rate};

    cleanup.swr = swr_alloc_set_opts(
        nullptr, cleanup.audioEnc->ch_layout.u.mask,
        cleanup.audioEnc->sample_fmt, cleanup.audioEnc->sample_rate,
        cleanup.audioDec->ch_layout.u.mask,
        static_cast<AVSampleFormat>(cleanup.audioDec->sample_fmt),
        cleanup.audioDec->sample_rate > 0 ? cleanup.audioDec->sample_rate
                                          : 48000,
        0, nullptr);
    if (cleanup.swr == nullptr || swr_init(cleanup.swr) < 0) {
      result.error = Error::make(ErrorCode::ExecutionFailed,
                                 "cannot create audio resampler", "ffmpeg");
      av_frame_free(&videoFrame);
      return result;
    }
  }

  if (videoOut < 0 && audioOut < 0) {
    result.error = Error::make(ErrorCode::InvalidArgument,
                               "nothing to convert", "ffmpeg");
    av_frame_free(&videoFrame);
    return result;
  }

  if (!(cleanup.out->oformat->flags & AVFMT_NOFILE)) {
    rc = avio_open(&cleanup.out->pb, params.outputPath.c_str(),
                   AVIO_FLAG_WRITE);
    if (rc < 0) {
      result.error =
          makeAvError(ErrorCode::ExecutionFailed, "open output", rc);
      av_frame_free(&videoFrame);
      return result;
    }
  }
  if (avformat_write_header(cleanup.out, nullptr) < 0) {
    result.error =
        Error::make(ErrorCode::ExecutionFailed, "write header", "ffmpeg");
    av_frame_free(&videoFrame);
    return result;
  }

  // ---- Central loop ------------------------------------------------------------
  cleanup.packet = av_packet_alloc();
  cleanup.frame = av_frame_alloc();
  const std::int64_t totalDurationUs =
      cleanup.in->duration != AV_NOPTS_VALUE ? cleanup.in->duration : 0;
  std::int64_t framesDone = 0;
  bool cancelled = false;
  bool writeFailed = false;

  // Timestamp bookkeeping for continuous output audio (resampler delay).
  std::int64_t audioSamplesIn = 0;

  const auto drainVideoEncoder = [&]() {
    if (cleanup.videoEnc == nullptr) {
      return;
    }
    avcodec_send_frame(cleanup.videoEnc, nullptr);
    AVPacket *encPacket = av_packet_alloc();
    while (avcodec_receive_packet(cleanup.videoEnc, encPacket) == 0) {
      encPacket->stream_index = videoOut;
      av_packet_rescale_ts(encPacket, cleanup.videoEnc->time_base,
                           cleanup.out->streams[videoOut]->time_base);
      if (av_interleaved_write_frame(cleanup.out, encPacket) < 0) {
        writeFailed = true;
      }
      av_packet_unref(encPacket);
    }
    av_packet_free(&encPacket);
  };
  const auto drainAudioEncoder = [&]() {
    if (cleanup.audioEnc == nullptr) {
      return;
    }
    avcodec_send_frame(cleanup.audioEnc, nullptr);
    AVPacket *encPacket = av_packet_alloc();
    while (avcodec_receive_packet(cleanup.audioEnc, encPacket) == 0) {
      encPacket->stream_index = audioOut;
      av_packet_rescale_ts(encPacket, cleanup.audioEnc->time_base,
                           cleanup.out->streams[audioOut]->time_base);
      if (av_interleaved_write_frame(cleanup.out, encPacket) < 0) {
        writeFailed = true;
      }
      av_packet_unref(encPacket);
    }
    av_packet_free(&encPacket);
  };

  while (!writeFailed && av_read_frame(cleanup.in, cleanup.packet) >= 0) {
    if (params.cancel && params.cancel->isCancelled()) {
      cancelled = true;
      av_packet_unref(cleanup.packet);
      break;
    }
    const bool isVideo = cleanup.packet->stream_index == videoIn;
    const bool isAudio = cleanup.packet->stream_index == audioIn;

    if (isVideo && cleanup.videoDec != nullptr && cleanup.videoEnc != nullptr) {
      if (avcodec_send_packet(cleanup.videoDec, cleanup.packet) == 0) {
        while (avcodec_receive_frame(cleanup.videoDec, cleanup.frame) == 0) {
          av_frame_make_writable(videoFrame);
          sws_scale(cleanup.sws, cleanup.frame->data, cleanup.frame->linesize,
                    0, cleanup.videoDec->height, videoFrame->data,
                    videoFrame->linesize);
          videoFrame->pts =
              av_rescale_q_rnd(cleanup.frame->pts, cleanup.videoDec->time_base,
                               cleanup.videoEnc->time_base,
                               AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
          if (avcodec_send_frame(cleanup.videoEnc, videoFrame) == 0) {
            AVPacket *encPacket = av_packet_alloc();
            while (avcodec_receive_packet(cleanup.videoEnc, encPacket) == 0) {
              encPacket->stream_index = videoOut;
              av_packet_rescale_ts(encPacket, cleanup.videoEnc->time_base,
                                   cleanup.out->streams[videoOut]->time_base);
              if (av_interleaved_write_frame(cleanup.out, encPacket) < 0) {
                writeFailed = true;
              }
              ++framesDone;
              av_packet_unref(encPacket);
            }
            av_packet_free(&encPacket);
          }
          if (params.onProgress && totalDurationUs > 0 &&
              (framesDone % 16) == 0) {
            const std::int64_t doneUs = av_rescale_q(
                cleanup.frame->pts, cleanup.videoDec->time_base,
                AVRational{1, 1000000});
            ProgressReport report;
            report.processedDurationUs = doneUs;
            report.totalDurationUs = totalDurationUs;
            report.fraction = std::clamp(double(doneUs) / double(totalDurationUs),
                                         0.0, 1.0);
            params.onProgress(report);
          }
          av_frame_unref(cleanup.frame);
        }
      }
    } else if (isAudio && cleanup.audioDec != nullptr &&
               cleanup.audioEnc != nullptr) {
      if (avcodec_send_packet(cleanup.audioDec, cleanup.packet) == 0) {
        while (avcodec_receive_frame(cleanup.audioDec, cleanup.frame) == 0) {
          const int maxOutSamples =
              cleanup.audioEnc->frame_size > 0
                  ? cleanup.audioEnc->frame_size
                  : cleanup.frame->nb_samples;
          AVFrame *audioFrame = av_frame_alloc();
          audioFrame->format = cleanup.audioEnc->sample_fmt;
          audioFrame->sample_rate = cleanup.audioEnc->sample_rate;
          av_channel_layout_copy(&audioFrame->ch_layout,
                                 &cleanup.audioEnc->ch_layout);
          audioFrame->nb_samples = maxOutSamples;
          if (av_frame_get_buffer(audioFrame, 0) < 0) {
            av_frame_free(&audioFrame);
            av_frame_unref(cleanup.frame);
            continue;
          }
          const int converted = swr_convert(
              cleanup.swr, audioFrame->data, maxOutSamples,
              const_cast<const uint8_t **>(cleanup.frame->data),
              cleanup.frame->nb_samples);
          if (converted > 0) {
            audioFrame->nb_samples = converted;
            audioFrame->pts = av_rescale_rnd(
                audioSamplesIn + swr_get_delay(cleanup.swr,
                                               cleanup.audioEnc->sample_rate),
                1, cleanup.audioEnc->sample_rate, AV_ROUND_NEAR_INF);
            if (avcodec_send_frame(cleanup.audioEnc, audioFrame) == 0) {
              AVPacket *encPacket = av_packet_alloc();
              while (avcodec_receive_packet(cleanup.audioEnc, encPacket) == 0) {
                encPacket->stream_index = audioOut;
                av_packet_rescale_ts(encPacket, cleanup.audioEnc->time_base,
                                     cleanup.out->streams[audioOut]->time_base);
                if (av_interleaved_write_frame(cleanup.out, encPacket) < 0) {
                  writeFailed = true;
                }
                av_packet_unref(encPacket);
              }
              av_packet_free(&encPacket);
            }
          }
          audioSamplesIn += cleanup.frame->nb_samples;
          av_frame_free(&audioFrame);
          av_frame_unref(cleanup.frame);
        }
      }
    }
    av_packet_unref(cleanup.packet);
  }

  drainVideoEncoder();
  drainAudioEncoder();
  if (!writeFailed) {
    av_write_trailer(cleanup.out);
  }
  result.framesProcessed = framesDone;
  av_frame_free(&videoFrame);

  if (cancelled) {
    result.error =
        Error::make(ErrorCode::Cancelled, "transcode cancelled", "ffmpeg");
    return result;
  }
  if (writeFailed) {
    result.error = Error::make(ErrorCode::ExecutionFailed,
                               "write failed during transcode", "ffmpeg");
    return result;
  }
  struct stat st{};
  if (::stat(params.outputPath.c_str(), &st) == 0) {
    result.bytesWritten = st.st_size;
  }
  result.success = true;
  return result;
}

// ---------------------------------------------------------------------------
// FfmpegBackend
// ---------------------------------------------------------------------------

Error FfmpegBackend::initialize() {
  initFfmpegLabel();
  implementationLabel_ = g_ffmpegVersion;
  return {};
}

BackendCapabilities FfmpegBackend::capabilities() const {
  BackendCapabilities caps;
  caps.backend = BackendId::Ffmpeg;
  caps.implementation = implementationLabel_.empty() ? "ffmpeg"
                                                     : implementationLabel_;
  caps.canInspect = true;

  const auto &features = FeatureRegistry::instance();

  // Enumerate the ACTUAL compiled libavcodec: capability reporting derives
  // from the real build, not from assumptions (spec §28).
  void *opaque = nullptr;
  const AVCodec *codec = nullptr;
  std::unordered_map<AVCodecID, bool> hasDecoder, hasEncoder;
  while ((codec = av_codec_iterate(&opaque)) != nullptr) {
    if (av_codec_is_decoder(codec)) {
      hasDecoder[codec->id] = true;
    } else if (av_codec_is_encoder(codec)) {
      hasEncoder[codec->id] = true;
    }
  }

  // Video: report only codecs the feature registry intentionally supports.
  for (const VideoCodec v :
       {VideoCodec::H264, VideoCodec::HEVC, VideoCodec::AV1, VideoCodec::VP8,
        VideoCodec::VP9, VideoCodec::Mpeg4Part2, VideoCodec::Mpeg2,
        VideoCodec::Mpeg1, VideoCodec::H263, VideoCodec::Vc1, VideoCodec::Wmv,
        VideoCodec::ProRes, VideoCodec::Dnxhd, VideoCodec::Ffv1,
        VideoCodec::Mjpeg}) {
    const CodecSupport featureSupport = features.videoSupport(v);
    const AVCodecID avId = avCodecFor(v);
    VideoCapability cap;
    cap.codec = v;
    cap.support.decode =
        featureSupport.decode && hasDecoder.count(avId) > 0;
    cap.support.encode =
        featureSupport.encode && hasEncoder.count(avId) > 0;
    cap.support.streamCopy = featureSupport.streamCopy;
    cap.hardware = false; // libav software implementation
    caps.video.push_back(cap);
  }

  for (const AudioCodec a :
       {AudioCodec::Aac, AudioCodec::HeAac, AudioCodec::Mp3,
        AudioCodec::Opus, AudioCodec::Vorbis, AudioCodec::Flac,
        AudioCodec::Alac, AudioCodec::Pcm, AudioCodec::Ac3, AudioCodec::Eac3,
        AudioCodec::Dts, AudioCodec::TrueHd, AudioCodec::Wma,
        AudioCodec::WavPack, AudioCodec::Ape, AudioCodec::AmrNb,
        AudioCodec::AmrWb}) {
    const CodecSupport featureSupport = features.audioSupport(a);
    const AVCodecID avId = avCodecFor(a);
    AudioCapability cap;
    cap.codec = a;
    cap.support.decode =
        featureSupport.decode && hasDecoder.count(avId) > 0;
    cap.support.encode =
        featureSupport.encode && hasEncoder.count(avId) > 0;
    cap.support.streamCopy = featureSupport.streamCopy;
    caps.audio.push_back(cap);
  }

  // Containers: probe the real muxers of this build.
  for (const Container c :
       {Container::Mp4, Container::M4v, Container::Mov, Container::Mkv,
        Container::WebM, Container::Avi, Container::MpegTs, Container::Flv,
        Container::ThreeGp, Container::M4a, Container::Ogg, Container::Mp3,
        Container::Wav, Container::Flac, Container::Asf}) {
    if (!features.knowsContainer(c)) {
      continue;
    }
    const AVOutputFormat *ofmt =
        av_guess_format(avFormatNameFor(c), nullptr, nullptr);
    if (ofmt != nullptr) {
      caps.writableContainers.push_back(c);
    }
    // Read-side: demuxers are matched by name; the mov/mkv family demuxers
    // cover several containers each.
    const AVInputFormat *ifmt = av_find_input_format(avFormatNameFor(c));
    if (ifmt != nullptr || c == Container::Mov || c == Container::Mkv ||
        c == Container::M4v || c == Container::M4a) {
      caps.readableContainers.push_back(c);
    }
  }
  return caps;
}

} // namespace velmux::ffmpeg
