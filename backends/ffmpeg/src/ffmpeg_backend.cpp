#include "velmux/ffmpeg/ffmpeg_backend.h"
#include "velmux/ffmpeg/codec_mapping.h"
#include "subtitle_tools.h"

#include "velmux/core/compression.h"
#include "velmux/core/feature_registry.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sys/stat.h>
#include <unordered_map>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
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

std::optional<std::string> localPathForUri(const std::string &uri) {
  if (uri.rfind("file://", 0) == 0) {
    return uri.substr(7);
  }
  if (uri.rfind('/', 0) == 0) {
    return uri;
  }
  return std::nullopt;
}

namespace {

// Target duration of one independently finalized transcode segment
// (spec §25: video transcode -> independently finalized segments). Actual
// boundaries land on the closest frame; the checkpoint records the real
// position reached.
constexpr std::int64_t kSegmentTargetUs = 10'000'000;

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
    avfilter_graph_free(&filterGraph);
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
    for (const auto &temp : tempFiles) {
      std::error_code ec;
      std::filesystem::remove(temp, ec);
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
  AVFilterGraph *filterGraph = nullptr;
  // Scratch files materialized for burn-in (shifted subtitle copies).
  std::vector<std::string> tempFiles;
};

// Escapes a filesystem path for use as a single-quoted option value inside
// a libavfilter graph description (ffmpeg-utils "Quoting and escaping"):
// within '...' only \\ and ' need escaping at the option level, and the
// graph-level parser additionally treats : as an option separator, so both
// are escaped. The caller wraps the result in quotes.
std::string escapeFilterPath(const std::string &path) {
  std::string out;
  out.reserve(path.size() + 8);
  for (const char c : path) {
    if (c == '\\' || c == ':' || c == '\'') {
      out += '\\';
    }
    out += c;
  }
  return out;
}

// True when the directory contains at least one regular font file directly
// (libass loads the FILES inside fontsdir; nested trees are not scanned).
bool dirContainsFontFile(const std::string &dir) {
  std::error_code ec;
  for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    std::string name = entry.path().filename().string();
    for (auto &c : name) {
      c = char(std::tolower(static_cast<unsigned char>(c)));
    }
    if (name.size() > 4 &&
        (name.rfind(".ttf") == name.size() - 4 ||
         name.rfind(".otf") == name.size() - 4 ||
         name.rfind(".ttc") == name.size() - 4)) {
      return true;
    }
  }
  return false;
}

// Font source for libass (burn-in rendering). libass loads the font FILES
// in the given directory (no recursion) and — without a system font
// provider, which hermetic static builds never have — can only match fonts
// by family name. The SRT-to-ASS conversion requests "Arial" by default,
// so the discovered family is applied via force_style for SRT-like sources;
// ASS/SSA files keep their authored styling (embedded fonts still work via
// the embedded provider).
struct FontSource {
  std::string dir;
  std::string family; // empty = unknown, do not force
};

FontSource findFontSource() {
  if (const char *env = ::getenv("VELMUX_FONTS_DIR");
      env != nullptr && *env != '\0' && dirContainsFontFile(env)) {
    const char *family = ::getenv("VELMUX_FONT_FAMILY");
    return FontSource{env, family != nullptr ? family : ""};
  }
  struct Candidate {
    const char *dir;
    const char *family;
  };
  static const Candidate candidates[] = {
      {"/usr/share/fonts/truetype/dejavu", "DejaVu Sans"}, // Debian/Ubuntu
      {"/usr/share/fonts/truetype", "DejaVu Sans"},        // Linux, flat tree
      {"/System/Library/Fonts", "Helvetica Neue"},         // iOS / macOS
      {"/system/fonts", "Roboto"},                         // Android
      {"/usr/share/fonts", ""},                            // unknown layout
      {"/usr/local/share/fonts", ""},                      // manual installs
  };
  for (const auto &c : candidates) {
    if (dirContainsFontFile(c.dir)) {
      return FontSource{c.dir, c.family};
    }
  }
  // One level deeper (e.g. /usr/share/fonts/<family>/Font.ttf): family
  // unknown.
  std::error_code ec;
  for (const char *root : {"/usr/share/fonts", "/usr/local/share/fonts"}) {
    for (const auto &entry : std::filesystem::directory_iterator(root, ec)) {
      if (entry.is_directory(ec) && dirContainsFontFile(entry.path().string())) {
        return FontSource{entry.path().string(), ""};
      }
    }
  }
  return {};
}

// Text subtitle codec ids the libass-backed `subtitles` filter can render.
bool isBurnableSubtitleCodec(AVCodecID id) {
  switch (id) {
    case AV_CODEC_ID_SUBRIP:
    case AV_CODEC_ID_ASS:
    case AV_CODEC_ID_SSA:
    case AV_CODEC_ID_WEBVTT:
    case AV_CODEC_ID_MOV_TEXT:
    case AV_CODEC_ID_TEXT:
      return true;
    default:
      return false; // PGS/DVB/VobSub are bitmap formats (libass cannot)
  }
}

// File extension for a stream-copyable extraction of a text subtitle codec.
const char *subtitleFileExtensionFor(AVCodecID id) {
  switch (id) {
    case AV_CODEC_ID_SUBRIP:
    case AV_CODEC_ID_TEXT:
      return ".srt";
    case AV_CODEC_ID_ASS:
    case AV_CODEC_ID_SSA:
      return ".ass";
    case AV_CODEC_ID_WEBVTT:
      return ".vtt";
    default:
      return nullptr;
  }
}

// Extracts one subtitle stream into a standalone text subtitle file via
// demux + packet-copy remux (valid for text formats whose packets are the
// file's native cue representation). Used to materialize a burn-in source
// when a timing offset must be applied first.
bool extractSubtitleStreamToFile(const std::string &inPath, int streamIndex,
                                 const std::string &outPath) {
  AVFormatContext *in = nullptr;
  AVFormatContext *out = nullptr;
  bool ok = false;
  do {
    if (avformat_open_input(&in, inPath.c_str(), nullptr, nullptr) < 0 ||
        avformat_find_stream_info(in, nullptr) < 0) {
      break;
    }
    if (streamIndex < 0 ||
        static_cast<unsigned>(streamIndex) >= in->nb_streams) {
      break;
    }
    const AVCodecID cid = in->streams[streamIndex]->codecpar->codec_id;
    const char *ext = subtitleFileExtensionFor(cid);
    if (ext == nullptr) {
      break; // cannot stream-copy this codec into a sidecar file
    }
    if (avformat_alloc_output_context2(&out, nullptr, nullptr,
                                       outPath.c_str()) < 0 ||
        out == nullptr) {
      break;
    }
    AVStream *outStream = avformat_new_stream(out, nullptr);
    if (outStream == nullptr) {
      break;
    }
    avcodec_parameters_copy(outStream->codecpar, in->streams[streamIndex]->codecpar);
    outStream->codecpar->codec_tag = 0;
    av_dict_copy(&outStream->metadata, in->streams[streamIndex]->metadata, 0);
    if (!(out->oformat->flags & AVFMT_NOFILE) &&
        avio_open(&out->pb, outPath.c_str(), AVIO_FLAG_WRITE) < 0) {
      break;
    }
    if (avformat_write_header(out, nullptr) < 0) {
      break;
    }
    AVPacket *pkt = av_packet_alloc();
    bool writeFailed = false;
    while (!writeFailed && av_read_frame(in, pkt) >= 0) {
      if (pkt->stream_index != streamIndex) {
        av_packet_unref(pkt);
        continue;
      }
      const AVStream *src = in->streams[streamIndex];
      const AVRounding rounding = static_cast<AVRounding>(
          AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
      pkt->pts = av_rescale_q_rnd(pkt->pts, src->time_base,
                                  outStream->time_base, rounding);
      pkt->dts = av_rescale_q_rnd(pkt->dts, src->time_base,
                                  outStream->time_base, rounding);
      pkt->duration =
          av_rescale_q(pkt->duration, src->time_base, outStream->time_base);
      pkt->pos = -1;
      pkt->stream_index = 0;
      if (av_interleaved_write_frame(out, pkt) < 0) {
        writeFailed = true;
      }
      av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    if (writeFailed) {
      break;
    }
    av_write_trailer(out);
    ok = true;
  } while (false);
  if (out != nullptr) {
    if (!(out->oformat->flags & AVFMT_NOFILE) && out->pb != nullptr) {
      avio_closep(&out->pb);
    }
    avformat_free_context(out);
  }
  if (in != nullptr) {
    avformat_close_input(&in);
  }
  return ok;
}

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
    // Burn-in rewrites video pixels; a copy pipeline can never serve it
    // (the planner enforces this; the executor refuses defensively).
    if (params.request.subtitles.policy == SubtitlePolicy::BurnIn) {
      ExecuteResult result;
      result.error = Error::make(
          ErrorCode::InvalidArgument,
          "internal: burn-in cannot run on a stream-copy pipeline", "ffmpeg");
      return result;
    }
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
  std::vector<int> outIndexOfSub; // output indices of subtitle streams
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
    if (type == AVMEDIA_TYPE_SUBTITLE) {
      outIndexOfSub.push_back(outStream->index);
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

    // av_interleaved_write_frame takes ownership of the packet content it
    // is given. A shallow struct copy would share the data buffer with the
    // source packet without raising its refcount — a use-after-free once
    // the source is unreffed. Move the reference into a dedicated packet
    // instead.
    AVPacket *outPacket = av_packet_alloc();
    const int srcStreamIndex = toWrite->stream_index;
    av_packet_move_ref(outPacket, toWrite);
    if (toWrite != cleanup.packet) {
      av_packet_free(&toWrite); // filtered shell, now empty
    }
    outPacket->stream_index = outIndex;
    const AVStream *srcStream = cleanup.in->streams[srcStreamIndex];
    const AVRounding rounding = static_cast<AVRounding>(
        AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
    outPacket->pts = av_rescale_q_rnd(
        outPacket->pts, srcStream->time_base,
        cleanup.out->streams[outIndex]->time_base, rounding);
    outPacket->dts = av_rescale_q_rnd(
        outPacket->dts, srcStream->time_base,
        cleanup.out->streams[outIndex]->time_base, rounding);
    outPacket->duration =
        av_rescale_q(outPacket->duration, srcStream->time_base,
                     cleanup.out->streams[outIndex]->time_base);
    outPacket->pos = -1;
    // Timing offset on preserved subtitle tracks (spec §14: adjust
    // subtitle timing — applied, never silently ignored).
    if (outPacket->pts != AV_NOPTS_VALUE &&
        params.request.subtitles.timingOffsetMs != 0 &&
        std::find(outIndexOfSub.begin(), outIndexOfSub.end(), outIndex) !=
            outIndexOfSub.end()) {
      outPacket->pts += av_rescale_q(
          params.request.subtitles.timingOffsetMs, AVRational{1, 1000},
          cleanup.out->streams[outIndex]->time_base);
      if (outPacket->pts < 0) {
        outPacket->pts = 0;
      }
    }

    if (av_interleaved_write_frame(cleanup.out, outPacket) < 0) {
      writeFailed = true;
    }
    av_packet_free(&outPacket);
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
  // Decide between the single-pass path (short jobs, exotic containers,
  // unknown duration) and the segmented path (long jobs: independently
  // finalized, validated, committed segments + assembly; resumable).
  const ConversionRequest &request = params.request;

  bool segmentable = request.video.enabled &&
                     params.pipeline.videoStreamCopy == false;
  std::int64_t durationUs = 0;
  std::string inPathStr;
  if (segmentable) {
    // Cheap metadata-only open: the source duration decides.
    if (const std::optional<std::string> inPath = localPathFor(request.source);
        inPath.has_value()) {
      inPathStr = *inPath;
      AVFormatContext *probe = nullptr;
      if (avformat_open_input(&probe, inPath->c_str(), nullptr, nullptr) == 0 &&
          avformat_find_stream_info(probe, nullptr) >= 0 &&
          probe->duration != AV_NOPTS_VALUE && probe->duration > 0) {
        durationUs = av_rescale_q(probe->duration, AVRational{1, AV_TIME_BASE},
                                  AVRational{1, 1000000});
      }
      if (probe != nullptr) {
        avformat_close_input(&probe);
      }
    }
    switch (params.pipeline.container) {
      case Container::Mp4:
      case Container::Mkv:
      case Container::WebM:
      case Container::MpegTs:
      case Container::M4a:
      case Container::Mp3:
      case Container::Ogg:
        break; // concat-friendly
      default:
        segmentable = false;
        break;
    }
    if (durationUs < 4 * kSegmentTargetUs) {
      // Short jobs: checkpointing overhead is not warranted (spec §26:
      // recovery protects LONG work; a fresh pass costs little here).
      segmentable = false;
    }
  }
  if (segmentable && !params.jobId.empty()) {
    return executeTranscodeSegmented(params);
  }
  return executeTranscodeRange(params, 0, 0, false);
}

ExecuteResult FfmpegExecutor::executeTranscodeRange(
    const ExecuteParams &params, std::int64_t rangeStartUs,
    std::int64_t rangeEndUs, bool rangeActive) {
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

  // Range playback: seek to the keyframe at/before the range start; decoded
  // content before the boundary is skipped below (segment starts are
  // IDR-clean because every range run opens fresh encoders and decoders).
  if (rangeActive && rangeStartUs > 0) {
    const int seekStream = videoIn >= 0 ? videoIn : 0;
    const std::int64_t target = av_rescale_q(
        rangeStartUs, AVRational{1, 1000000},
        cleanup.in->streams[seekStream]->time_base);
    if (avformat_seek_file(cleanup.in, seekStream, INT64_MIN, target,
                           INT64_MAX, AVSEEK_FLAG_BACKWARD) < 0) {
      result.error = Error::make(ErrorCode::ExecutionFailed,
                                 "cannot seek to the segment boundary",
                                 "ffmpeg");
      return result;
    }
  }
  const bool rangeBounded = rangeActive;
  // Frame/packet timestamps are compared against the range in microseconds.
  auto frameUs = [&](const AVFrame *f, const AVCodecContext *dec) -> std::int64_t {
    if (f->pts == AV_NOPTS_VALUE || dec->time_base.num <= 0) {
      return -1;
    }
    return av_rescale_q(f->pts, dec->time_base, AVRational{1, 1000000});
  };

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
  // Burn-in routing: when subtitles are burned, decoded frames pass through
  // a libavfilter graph (scale -> subtitles[libass] -> buffersink) instead of
  // the direct sws path. Owned by cleanup.filterGraph.
  AVFilterContext *burnSrc = nullptr;
  AVFilterContext *burnSink = nullptr;
  // Time base the buffersrc was declared with; pushed frames' pts are
  // normalized into it (decoders may adjust their own time base mid-stream).
  AVRational burnGraphTb{0, 1};
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
    // Demuxed packet timestamps arrive in the stream's time base: declare
    // that to the decoder (the documented contract) and pin the decoder's
    // output time base so filter-graph creation cannot see the 0/1
    // placeholder some decoders leave until the first frame.
    cleanup.videoDec->pkt_timebase = inVideo->time_base;
    if (cleanup.videoDec->time_base.num <= 0 ||
        cleanup.videoDec->time_base.den <= 0) {
      cleanup.videoDec->time_base = inVideo->time_base;
    }
    if (avcodec_open2(cleanup.videoDec, decCodec, nullptr) < 0) {
      result.error = Error::make(ErrorCode::ExecutionFailed,
                                 "cannot open video decoder", "ffmpeg");
      return result;
    }
    if (cleanup.videoDec->time_base.num <= 0 ||
        cleanup.videoDec->time_base.den <= 0) {
      // A few decoders still reset it during open.
      cleanup.videoDec->time_base = inVideo->time_base;
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
      cleanup.videoEnc->rc_buffer_size = *request.video.bitRate * 2;
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

    if (request.subtitles.policy == SubtitlePolicy::BurnIn) {
      // Resolve the burn-in source: explicit external file, else the first
      // embedded subtitle stream.
      std::string subPath;
      int subGlobalIndex = -1; // stream index in the input container
      int subOrdinal = -1;     // subtitle-stream ordinal (the filter's `si`)
      if (request.subtitles.externalUri.has_value()) {
        const std::optional<std::string> ext =
            localPathForUri(*request.subtitles.externalUri);
        if (!ext.has_value()) {
          result.error = Error::make(ErrorCode::SourceUnavailable,
                                     "external subtitle URI is not a local path",
                                     "ffmpeg");
          return result;
        }
        subPath = *ext;
      } else {
        for (unsigned i = 0; i < cleanup.in->nb_streams; ++i) {
          if (cleanup.in->streams[i]->codecpar->codec_type ==
              AVMEDIA_TYPE_SUBTITLE) {
            subGlobalIndex = int(i);
            break;
          }
        }
        if (subGlobalIndex < 0) {
          result.error = Error::make(
              ErrorCode::InvalidArgument,
              "burn-in requested but the source has no subtitle track and no "
              "external subtitle file was provided",
              "ffmpeg");
          return result;
        }
        const AVCodecID subCodec =
            cleanup.in->streams[subGlobalIndex]->codecpar->codec_id;
        if (!isBurnableSubtitleCodec(subCodec)) {
          result.error = Error::make(
              ErrorCode::UnsupportedFeature,
              std::string("burn-in of bitmap subtitle streams (") +
                  avcodec_get_name(subCodec) +
                  ") requires rendering support that is not available yet",
              "ffmpeg");
          return result;
        }
        subPath = *inPath;
        // The subtitles filter's `si` option counts SUBTITLE streams only:
        // convert the global stream index into its subtitle ordinal.
        for (int s = 0; s < subGlobalIndex; ++s) {
          if (cleanup.in->streams[s]->codecpar->codec_type ==
              AVMEDIA_TYPE_SUBTITLE) {
            ++subOrdinal;
          }
        }
        ++subOrdinal; // this stream is the (ordinal+1)-th subtitle stream
      }

      // Timing offset: materialize a shifted copy before rendering
      // (spec §14: adjust subtitle timing — never silently ignored).
      if (request.subtitles.timingOffsetMs != 0) {
        const std::filesystem::path outFs(params.outputPath);
        const std::string stem = outFs.stem().string();
        if (subGlobalIndex >= 0) {
          const AVCodecID subCodec =
              cleanup.in->streams[subGlobalIndex]->codecpar->codec_id;
          const char *ext = subtitleFileExtensionFor(subCodec);
          if (ext == nullptr) {
            result.error = Error::make(
                ErrorCode::UnsupportedFeature,
                "a timing offset cannot be applied to this embedded subtitle "
                "codec yet",
                "ffmpeg");
            return result;
          }
          const std::string extracted =
              (outFs.parent_path() / ("." + stem + "-subs" + ext)).string();
          if (!extractSubtitleStreamToFile(*inPath, subGlobalIndex,
                                           extracted)) {
            result.error =
                Error::make(ErrorCode::ExecutionFailed,
                            "cannot extract the embedded subtitle stream for "
                            "timing adjustment",
                            "ffmpeg");
            return result;
          }
          cleanup.tempFiles.push_back(extracted);
          subPath = extracted;
          subGlobalIndex = -1; // now a standalone file
          subOrdinal = -1;
        }
        auto subExtOf = [](const std::string &p) {
          const auto dot = p.rfind('.');
          return dot == std::string::npos ? std::string(".srt")
                                          : p.substr(dot);
        };
        const std::string shifted =
            (outFs.parent_path() / ("." + stem + "-shifted" + subExtOf(subPath)))
                .string();
        std::string shiftReason;
        if (!subtitles::shiftSubtitleFile(
                subPath, shifted, request.subtitles.timingOffsetMs,
                shiftReason)) {
          result.error = Error::make(
              ErrorCode::UnsupportedFeature,
              "cannot apply the subtitle timing offset: " + shiftReason,
              "ffmpeg");
          return result;
        }
        cleanup.tempFiles.push_back(shifted);
        subPath = shifted;
      }

      const FontSource fontSource = findFontSource();
      if (fontSource.dir.empty()) {
        result.error = Error::make(
            ErrorCode::UnsupportedFeature,
            "no fonts directory found for subtitle rendering "
            "(set VELMUX_FONTS_DIR)",
            "ffmpeg");
        return result;
      }

      cleanup.filterGraph = avfilter_graph_alloc();
      const AVFilter *srcFilter = avfilter_get_by_name("buffer");
      const AVFilter *sinkFilter = avfilter_get_by_name("buffersink");
      if (cleanup.filterGraph == nullptr || srcFilter == nullptr ||
          sinkFilter == nullptr) {
        result.error = Error::make(ErrorCode::ExecutionFailed,
                                   "cannot allocate subtitle filter graph",
                                   "ffmpeg");
        return result;
      }
      char srcArgs[512];
      AVRational sar = cleanup.videoDec->sample_aspect_ratio;
      if (sar.num <= 0 || sar.den <= 0) {
        sar = AVRational{0, 1};
      }
      std::snprintf(srcArgs, sizeof(srcArgs),
                    "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d"
                    ":pixel_aspect=%d/%d",
                    cleanup.videoDec->width, cleanup.videoDec->height,
                    cleanup.videoDec->pix_fmt,
                    // Decoded frames carry pts in the DECODER's time base —
                    // declaring the stream's here would rescale every frame
                    // into the wrong timeline and subtitles would never fire.
                    cleanup.videoDec->time_base.num,
                    cleanup.videoDec->time_base.den, sar.num, sar.den);
      burnGraphTb = cleanup.videoDec->time_base;
      const int srcRc = avfilter_graph_create_filter(
          &burnSrc, srcFilter, "in", srcArgs, nullptr, cleanup.filterGraph);
      const int sinkRc =
          srcRc >= 0
              ? avfilter_graph_create_filter(&burnSink, sinkFilter, "out",
                                             nullptr, nullptr,
                                             cleanup.filterGraph)
              : srcRc;
      if (srcRc < 0 || sinkRc < 0) {
        const int rc = srcRc < 0 ? srcRc : sinkRc;
        result.error = Error::make(
            ErrorCode::ExecutionFailed,
            "cannot create subtitle filter endpoints (" +
                std::string(srcArgs) + "): " + avErrorString(rc),
            "ffmpeg");
        return result;
      }
      const enum AVPixelFormat sinkFmts[] = {cleanup.videoEnc->pix_fmt,
                                             AV_PIX_FMT_NONE};
      // The binary blob size must EXCLUDE the AV_PIX_FMT_NONE terminator:
      // buffersink turns every entry into a format-list item, and a -1 entry
      // makes later format negotiation dereference a NULL pixel-format
      // descriptor (hard crash in avfilter_graph_config).
      if (av_opt_set_bin(burnSink, "pix_fmts",
                         reinterpret_cast<const uint8_t *>(sinkFmts),
                         sizeof(sinkFmts) - sizeof(sinkFmts[0]),
                         AV_OPT_SEARCH_CHILDREN) < 0) {
        result.error = Error::make(
            ErrorCode::ExecutionFailed,
            "cannot constrain the subtitle filter output format", "ffmpeg");
        return result;
      }
      std::string graphDesc =
          "scale=w=" + std::to_string(outW) + ":h=" + std::to_string(outH) +
          ",subtitles=filename='" + escapeFilterPath(subPath) +
          "':fontsdir='" + escapeFilterPath(fontSource.dir) + "'";
      if (subOrdinal >= 0) {
        graphDesc += ":si=" + std::to_string(subOrdinal);
      }
      // SRT-like sources render with libass's default style (family
      // "Arial"), which never matches the fontsdir contents on a provider-
      // less static build. Redirect the family to the discovered font so
      // text actually renders. ASS/SSA keep their authored styling.
      const bool subIsAss =
          subGlobalIndex >= 0
              ? (cleanup.in->streams[subGlobalIndex]->codecpar->codec_id ==
                     AV_CODEC_ID_ASS ||
                 cleanup.in->streams[subGlobalIndex]->codecpar->codec_id ==
                     AV_CODEC_ID_SSA)
              : (subPath.size() > 4 &&
                 (subPath.compare(subPath.size() - 4, 4, ".ass") == 0 ||
                  subPath.compare(subPath.size() - 4, 4, ".ssa") == 0 ||
                  subPath.compare(subPath.size() - 4, 4, ".ASS") == 0 ||
                  subPath.compare(subPath.size() - 4, 4, ".SSA") == 0));
      if (!subIsAss && !fontSource.family.empty()) {
        graphDesc += ":force_style='FontName=" +
                     escapeFilterPath(fontSource.family) + "'";
      }
      AVFilterInOut *outputs = avfilter_inout_alloc();
      AVFilterInOut *inputs = avfilter_inout_alloc();
      bool graphOk = outputs != nullptr && inputs != nullptr;
      if (graphOk) {
        outputs->name = av_strdup("in");
        outputs->filter_ctx = burnSrc;
        outputs->pad_idx = 0;
        outputs->next = nullptr;
        inputs->name = av_strdup("out");
        inputs->filter_ctx = burnSink;
        inputs->pad_idx = 0;
        inputs->next = nullptr;
        graphOk = avfilter_graph_parse_ptr(cleanup.filterGraph,
                                           graphDesc.c_str(), &inputs,
                                           &outputs, nullptr) >= 0;
      }
      avfilter_inout_free(&inputs);
      avfilter_inout_free(&outputs);
      if (graphOk) {
        graphOk = avfilter_graph_config(cleanup.filterGraph, nullptr) >= 0;
      }
      if (!graphOk) {
        result.error = Error::make(
            ErrorCode::ExecutionFailed,
            "cannot initialize the subtitle rendering graph (" + graphDesc +
                ")",
            "ffmpeg");
        return result;
      }
    } else {
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
    // Encoders with restricted rate lists (e.g. Opus: 8/12/16/24/48 kHz)
    // cannot open at arbitrary rates: pick the closest supported one and
    // resample into it.
    if (encCodec->supported_samplerates != nullptr) {
      bool supported = false;
      int best = 0;
      int bestDistance = 0;
      for (int i = 0; encCodec->supported_samplerates[i] != 0; ++i) {
        const int candidate = encCodec->supported_samplerates[i];
        if (candidate == cleanup.audioEnc->sample_rate) {
          supported = true;
          break;
        }
        const int distance =
            std::abs(candidate - cleanup.audioEnc->sample_rate);
        if (best == 0 || distance < bestDistance) {
          best = candidate;
          bestDistance = distance;
        }
      }
      if (!supported && best > 0) {
        cleanup.audioEnc->sample_rate = best;
      }
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

    cleanup.swr = swr_alloc();
    if (cleanup.swr != nullptr &&
        swr_alloc_set_opts2(&cleanup.swr,
                            &cleanup.audioEnc->ch_layout,
                            cleanup.audioEnc->sample_fmt,
                            cleanup.audioEnc->sample_rate,
                            &cleanup.audioDec->ch_layout,
                            static_cast<AVSampleFormat>(
                                cleanup.audioDec->sample_fmt),
                            cleanup.audioDec->sample_rate > 0
                                ? cleanup.audioDec->sample_rate
                                : 48000,
                            0, nullptr) < 0) {
      swr_free(&cleanup.swr);
    }
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

  // ---- Subtitle preservation during transcode ------------------------------
  // The planner guarantees (Preserve) that every source subtitle track's
  // codec is muxable into the target container, so the tracks are copied
  // verbatim alongside the re-encoded A/V streams (spec §14: never
  // silently discard subtitles during ordinary conversion).
  std::vector<int> subInOfOut;
  std::vector<int> subOutIndex;
  if (request.subtitles.policy == SubtitlePolicy::Preserve) {
    for (unsigned i = 0; i < cleanup.in->nb_streams; ++i) {
      const AVStream *st = cleanup.in->streams[i];
      if (st->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
        continue;
      }
      AVStream *outSub = avformat_new_stream(cleanup.out, nullptr);
      if (outSub == nullptr) {
        continue;
      }
      avcodec_parameters_copy(outSub->codecpar, st->codecpar);
      outSub->codecpar->codec_tag = 0;
      av_dict_copy(&outSub->metadata, st->metadata, 0); // language etc.
      outSub->disposition = st->disposition;            // default/forced flags
      subInOfOut.push_back(int(i));
      subOutIndex.push_back(int(cleanup.out->nb_streams) - 1);
    }
  }
  // Timing offset applied to preserved tracks, in output time base.
  std::int64_t subOffsetOutUnits = 0;
  if (!subInOfOut.empty() && request.subtitles.timingOffsetMs != 0) {
    const AVRational outTb =
        cleanup.out->streams[std::size_t(subOutIndex.front())]->time_base;
    subOffsetOutUnits =
        av_rescale_q(request.subtitles.timingOffsetMs, AVRational{1, 1000},
                     outTb);
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
  std::int64_t audioSamplesOut = 0;

  // Encodes one prepared frame (already in encoder time base) and writes
  // the resulting packets. Shared by the direct, burn-in and drain paths.
  const auto sendVideoFrameToEncoder = [&](AVFrame *f) {
    if (avcodec_send_frame(cleanup.videoEnc, f) == 0) {
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
  };

  const auto drainVideoEncoder = [&]() {
    if (cleanup.videoEnc == nullptr) {
      return;
    }
    // Flush the subtitle rendering graph first so its tail frames reach
    // the encoder before EOF is signaled.
    if (burnSrc != nullptr && burnSink != nullptr) {
      if (av_buffersrc_add_frame(burnSrc, nullptr) >= 0) {
        AVFrame *filt = av_frame_alloc();
        while (av_buffersink_get_frame(burnSink, filt) >= 0) {
          filt->pict_type = AV_PICTURE_TYPE_NONE;
          filt->pts = av_rescale_q_rnd(
              filt->pts, av_buffersink_get_time_base(burnSink),
              cleanup.videoEnc->time_base,
              static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
          sendVideoFrameToEncoder(filt);
          av_frame_unref(filt);
        }
        av_frame_free(&filt);
      }
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

  bool rangeDone = false; // crossed the range end; stop reading input
  while (!writeFailed && !rangeDone &&
         av_read_frame(cleanup.in, cleanup.packet) >= 0) {
    if (params.cancel && params.cancel->isCancelled()) {
      cancelled = true;
      av_packet_unref(cleanup.packet);
      break;
    }
    const bool isVideo = cleanup.packet->stream_index == videoIn;
    const bool isAudio = cleanup.packet->stream_index == audioIn;
    // Range gate on the demux side: packets at/after the end stop the run
    // (their content belongs to the next segment); packets before the
    // start are dropped (their content is covered by earlier segments).
    if (rangeBounded) {
      const AVStream *pktStream =
          cleanup.in->streams[std::size_t(cleanup.packet->stream_index)];
      const std::int64_t pktUs =
          cleanup.packet->pts == AV_NOPTS_VALUE
              ? -1
              : av_rescale_q(cleanup.packet->pts, pktStream->time_base,
                             AVRational{1, 1000000});
      if (pktUs >= rangeEndUs && (isVideo || isAudio)) {
        rangeDone = true;
        av_packet_unref(cleanup.packet);
        break;
      }
      if (pktUs >= 0 && pktUs < rangeStartUs && (isVideo || isAudio)) {
        av_packet_unref(cleanup.packet);
        continue;
      }
    }

    if (isVideo && cleanup.videoDec != nullptr && cleanup.videoEnc != nullptr) {
      if (avcodec_send_packet(cleanup.videoDec, cleanup.packet) == 0) {
        while (avcodec_receive_frame(cleanup.videoDec, cleanup.frame) == 0) {
          const std::int64_t srcUs = frameUs(cleanup.frame, cleanup.videoDec);
          if (rangeBounded) {
            if (srcUs >= 0 && srcUs < rangeStartUs) {
              av_frame_unref(cleanup.frame); // seek overshoot content
              continue;
            }
            if (srcUs >= rangeEndUs) {
              rangeDone = true;
              av_frame_unref(cleanup.frame);
              break;
            }
          }
          if (burnSrc != nullptr && burnSink != nullptr) {
            // Burn-in path: the graph scales, renders subtitles (libass) and
            // converts to the encoder's pixel format. Frame pts are
            // normalized into the time base the buffersrc was declared with.
            if (cleanup.frame->pts != AV_NOPTS_VALUE) {
              cleanup.frame->pts = av_rescale_q_rnd(
                  cleanup.frame->pts, cleanup.videoDec->time_base, burnGraphTb,
                  static_cast<AVRounding>(
                      AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
            }
            if (av_buffersrc_add_frame_flags(
                    burnSrc, cleanup.frame,
                    AV_BUFFERSRC_FLAG_KEEP_REF) >= 0) {
              AVFrame *filt = av_frame_alloc();
              while (av_buffersink_get_frame(burnSink, filt) >= 0) {
                filt->pict_type = AV_PICTURE_TYPE_NONE;
                if (rangeBounded && filt->pts != AV_NOPTS_VALUE) {
                  // Segment-local timeline: rebase into [0, rangeEnd-start)
                  // and convert to the encoder's time base in one step.
                  const std::int64_t outUs = av_rescale_q(
                      filt->pts, av_buffersink_get_time_base(burnSink),
                      AVRational{1, 1000000});
                  filt->pts = av_rescale_q(
                      std::max<std::int64_t>(0, outUs - rangeStartUs),
                      AVRational{1, 1000000},
                      cleanup.videoEnc->time_base);
                } else {
                  filt->pts = av_rescale_q_rnd(
                      filt->pts, av_buffersink_get_time_base(burnSink),
                      cleanup.videoEnc->time_base,
                      static_cast<AVRounding>(
                          AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
                }
                sendVideoFrameToEncoder(filt);
                av_frame_unref(filt);
              }
              av_frame_free(&filt);
            }
          } else {
            av_frame_make_writable(videoFrame);
            sws_scale(cleanup.sws, cleanup.frame->data,
                      cleanup.frame->linesize, 0, cleanup.videoDec->height,
                      videoFrame->data, videoFrame->linesize);
            if (rangeBounded && cleanup.frame->pts != AV_NOPTS_VALUE) {
              // Segment-local timeline: subtract the range start (in the
              // decoder's time base) before handing pts to the encoder.
              const std::int64_t startTb = av_rescale_q(
                  rangeStartUs, AVRational{1, 1000000},
                  cleanup.videoDec->time_base);
              videoFrame->pts = av_rescale_q_rnd(
                  cleanup.frame->pts - startTb, cleanup.videoDec->time_base,
                  cleanup.videoEnc->time_base,
                  static_cast<AVRounding>(
                      AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
            } else {
              videoFrame->pts = av_rescale_q_rnd(
                  cleanup.frame->pts, cleanup.videoDec->time_base,
                  cleanup.videoEnc->time_base,
                  static_cast<AVRounding>(
                      AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
            }
            sendVideoFrameToEncoder(videoFrame);
          }
          if (params.onProgress && totalDurationUs > 0 &&
              (framesDone % 16) == 0) {
            const std::int64_t doneUs = av_rescale_q(
                cleanup.frame->pts, cleanup.videoDec->time_base,
                AVRational{1, 1000000});
            ProgressReport report;
            report.processedDurationUs = doneUs;
            report.totalDurationUs = totalDurationUs;
            if (rangeBounded && rangeEndUs > rangeStartUs) {
              // Range-relative fraction: the segmented wrapper rebases it
              // onto the whole-job timeline.
              report.fraction = std::clamp(
                  double(doneUs - rangeStartUs) /
                      double(rangeEndUs - rangeStartUs),
                  0.0, 1.0);
            } else {
              report.fraction =
                  std::clamp(double(doneUs) / double(totalDurationUs), 0.0, 1.0);
            }
            params.onProgress(report);
          }
          av_frame_unref(cleanup.frame);
        }
      }
    } else if (isAudio && cleanup.audioDec != nullptr &&
               cleanup.audioEnc != nullptr) {
      if (avcodec_send_packet(cleanup.audioDec, cleanup.packet) == 0) {
        while (avcodec_receive_frame(cleanup.audioDec, cleanup.frame) == 0) {
          if (rangeBounded) {
            const std::int64_t audioUs =
                frameUs(cleanup.frame, cleanup.audioDec);
            if (audioUs >= 0 && audioUs < rangeStartUs) {
              av_frame_unref(cleanup.frame);
              continue;
            }
            if (audioUs >= rangeEndUs) {
              rangeDone = true;
              av_frame_unref(cleanup.frame);
              break;
            }
          }
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
            // Encoder time_base is 1/sample_rate: pts in output samples.
            audioFrame->pts = audioSamplesOut;
            audioSamplesOut += converted;
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
          (void)0; /* input sample counting not needed: pts follows output samples */
          av_frame_free(&audioFrame);
          av_frame_unref(cleanup.frame);
        }
      }
    } else if (!isVideo && !isAudio) {
      // Subtitle (or other) packet: forwarded only when preserved.
      // Remove/BurnIn drop it here by explicit user policy — never a
      // silent default.
      int subOut = -1;
      for (std::size_t k = 0; k < subInOfOut.size(); ++k) {
        if (subInOfOut[k] == cleanup.packet->stream_index) {
          subOut = subOutIndex[k];
          break;
        }
      }
      if (subOut >= 0) {
        // Cues at/after the range end belong to a later segment; cues that
        // merely START before this segment keep flowing (clamped to the
        // segment-local zero) so nothing is dropped.
        bool skipSub = false;
        if (rangeBounded && cleanup.packet->pts != AV_NOPTS_VALUE) {
          const AVStream *srcSub =
              cleanup.in->streams[std::size_t(cleanup.packet->stream_index)];
          const std::int64_t subUs =
              av_rescale_q(cleanup.packet->pts, srcSub->time_base,
                           AVRational{1, 1000000});
          if (subUs >= rangeEndUs) {
            skipSub = true;
          }
        }
        if (!skipSub) {
        AVPacket *outPacket = av_packet_alloc();
        av_packet_move_ref(outPacket, cleanup.packet);
        // outPacket->stream_index is still the input stream index here
        // (move_ref preserves the fields; only the reference moved).
        const AVStream *srcStream =
            cleanup.in->streams[std::size_t(outPacket->stream_index)];
        const AVStream *dstStream =
            cleanup.out->streams[std::size_t(subOut)];
        const AVRounding rounding = static_cast<AVRounding>(
            AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
        if (outPacket->pts != AV_NOPTS_VALUE) {
          std::int64_t pts = av_rescale_q_rnd(outPacket->pts,
                                              srcStream->time_base,
                                              dstStream->time_base, rounding) +
                             subOffsetOutUnits;
          if (rangeBounded) {
            // Segment-local timeline for preserved subtitles.
            const std::int64_t startDst = av_rescale_q(
                rangeStartUs, AVRational{1, 1000000}, dstStream->time_base);
            pts -= startDst;
          }
          if (pts < 0) {
            pts = 0; // offset clamps cue starts, never emits negative pts
          }
          outPacket->pts = pts;
        }
        if (outPacket->dts != AV_NOPTS_VALUE) {
          outPacket->dts = av_rescale_q_rnd(outPacket->dts,
                                            srcStream->time_base,
                                            dstStream->time_base, rounding);
        }
        if (outPacket->duration > 0) {
          outPacket->duration =
              av_rescale_q(outPacket->duration, srcStream->time_base,
                           dstStream->time_base);
        }
        outPacket->pos = -1;
        outPacket->stream_index = subOut;
        if (av_interleaved_write_frame(cleanup.out, outPacket) < 0) {
          writeFailed = true;
        }
        av_packet_free(&outPacket);
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

// Cheap artifact validation for resume: the file must open, yield stream
// info and have a positive duration. Full validation happens on the final
// assembled output (the core's OutputValidator + independent checks).
bool segmentArtifactUsable(const std::string &path) {
  if (path.empty()) {
    return false;
  }
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) ||
      std::filesystem::file_size(path, ec) == 0) {
    return false;
  }
  AVFormatContext *probe = nullptr;
  bool usable = false;
  if (avformat_open_input(&probe, path.c_str(), nullptr, nullptr) == 0 &&
      avformat_find_stream_info(probe, nullptr) >= 0) {
    usable = probe->duration != AV_NOPTS_VALUE && probe->duration > 0;
  }
  if (probe != nullptr) {
    avformat_close_input(&probe);
  }
  return usable;
}

// Stream-copy assembly of finalized segments through the concat demuxer.
// Each segment carries its own timeline starting at 0; concat chains them
// by cumulative duration (spec §25: final assembly from validated pieces).
bool assembleSegments(const std::vector<std::string> &segments,
                      const std::string &outputPath, Container container) {
  const std::filesystem::path outFs(outputPath);
  const std::filesystem::path listPath =
      outFs.parent_path() / (outFs.filename().string() + ".concat.txt");
  {
    std::ofstream list(listPath, std::ios::binary | std::ios::trunc);
    if (!list) {
      return false;
    }
    for (const std::string &seg : segments) {
      // concat-list escaping: single quotes, with ' doubled inside.
      std::string escaped;
      for (const char c : seg) {
        if (c == '\'') {
          escaped += "''";
        } else if (c != '\n' && c != '\r') {
          escaped += c;
        }
      }
      list << "file '" << escaped << "'\n";
    }
    if (!list.good()) {
      return false;
    }
  }

  AVFormatContext *in = nullptr;
  AVFormatContext *out = nullptr;
  bool ok = false;
  do {
    const AVInputFormat *concatFormat = av_find_input_format("concat");
    if (concatFormat == nullptr ||
        avformat_open_input(&in, listPath.string().c_str(), concatFormat,
                            nullptr) < 0 ||
        avformat_find_stream_info(in, nullptr) < 0) {
      break;
    }
    if (avformat_alloc_output_context2(&out, nullptr,
                                       avFormatNameFor(container),
                                       outputPath.c_str()) < 0 ||
        out == nullptr) {
      break;
    }
    std::vector<int> outIndexOfIn;
    for (unsigned i = 0; i < in->nb_streams; ++i) {
      AVStream *outStream = avformat_new_stream(out, nullptr);
      if (outStream == nullptr) {
        break;
      }
      avcodec_parameters_copy(outStream->codecpar, in->streams[i]->codecpar);
      outStream->codecpar->codec_tag = 0;
      outIndexOfIn.push_back(outStream->index);
    }
    if (outIndexOfIn.size() != in->nb_streams) {
      break;
    }
    if (!(out->oformat->flags & AVFMT_NOFILE) &&
        avio_open(&out->pb, outputPath.c_str(), AVIO_FLAG_WRITE) < 0) {
      break;
    }
    if (avformat_write_header(out, nullptr) < 0) {
      break;
    }
    AVPacket *pkt = av_packet_alloc();
    bool writeFailed = false;
    while (!writeFailed && av_read_frame(in, pkt) >= 0) {
      const unsigned inIndex = std::size_t(pkt->stream_index);
      const AVStream *srcStream = in->streams[inIndex];
      AVStream *dstStream = out->streams[std::size_t(outIndexOfIn[inIndex])];
      pkt->stream_index = outIndexOfIn[inIndex];
      const AVRounding rounding = static_cast<AVRounding>(
          AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
      pkt->pts = av_rescale_q_rnd(pkt->pts, srcStream->time_base,
                                  dstStream->time_base, rounding);
      pkt->dts = av_rescale_q_rnd(pkt->dts, srcStream->time_base,
                                  dstStream->time_base, rounding);
      pkt->duration =
          av_rescale_q(pkt->duration, srcStream->time_base,
                       dstStream->time_base);
      pkt->pos = -1;
      if (av_interleaved_write_frame(out, pkt) < 0) {
        writeFailed = true;
      }
      av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    if (writeFailed) {
      break;
    }
    av_write_trailer(out);
    ok = true;
  } while (false);
  if (out != nullptr) {
    if (!(out->oformat->flags & AVFMT_NOFILE) && out->pb != nullptr) {
      avio_closep(&out->pb);
    }
    avformat_free_context(out);
  }
  if (in != nullptr) {
    avformat_close_input(&in);
  }
  std::error_code ec;
  std::filesystem::remove(listPath, ec);
  return ok;
}

ExecuteResult FfmpegExecutor::executeTranscodeSegmented(
    const ExecuteParams &params) {
  ExecuteResult result;
  initFfmpegLabel();
  // Whole-job duration (the decision path verified it is known and long).
  std::int64_t totalUs = 0;
  {
    const std::optional<std::string> inPath = localPathFor(params.request.source);
    if (!inPath.has_value()) {
      result.error = Error::make(ErrorCode::SourceUnavailable,
                                 "unsupported source URI", "ffmpeg");
      return result;
    }
    AVFormatContext *probe = nullptr;
    if (avformat_open_input(&probe, inPath->c_str(), nullptr, nullptr) == 0 &&
        avformat_find_stream_info(probe, nullptr) >= 0 &&
        probe->duration != AV_NOPTS_VALUE && probe->duration > 0) {
      totalUs = av_rescale_q(probe->duration, AVRational{1, AV_TIME_BASE},
                             AVRational{1, 1000000});
    }
    if (probe != nullptr) {
      avformat_close_input(&probe);
    }
    if (totalUs <= 0) {
      // Duration became unknown between the decision and now: fall back to
      // a single honest pass (no checkpoints, no resume).
      return executeTranscodeRange(params, 0, 0, false);
    }
  }

  // Durable segment directory, stable across runs of the same job.
  const std::filesystem::path outFs(params.outputPath);
  const std::filesystem::path segDir =
      outFs.parent_path() / ("velmux-seg-" + params.jobId);
  std::error_code ec;
  std::filesystem::create_directories(segDir, ec);

  // Resume: keep the ordered prefix of still-valid artifacts; drop the rest.
  struct ValidSegment {
    std::string path;
    std::int64_t endUs;
  };
  std::vector<ValidSegment> committed;
  {
    std::vector<backend::CheckpointInfo> sorted = params.resumeFrom;
    std::sort(sorted.begin(), sorted.end(),
              [](const auto &a, const auto &b) { return a.position < b.position; });
    std::int64_t expectedStart = 0;
    for (const auto &info : sorted) {
      if (info.position <= expectedStart || info.position > totalUs) {
        continue; // overlapping or out-of-range record
      }
      if (!segmentArtifactUsable(info.artifactPath)) {
        break; // prefix ends here; redo the rest
      }
      committed.push_back({info.artifactPath, info.position});
      expectedStart = info.position;
    }
  }
  std::int64_t resumeUs =
      committed.empty() ? 0 : committed.back().endUs;
  if (resumeUs >= totalUs) {
    // Everything is already committed: just assemble.
    if (committed.size() == 1) {
      std::filesystem::rename(committed.front().path, params.outputPath, ec);
      if (ec) {
        std::filesystem::copy_file(committed.front().path, params.outputPath,
                                   ec);
      }
    } else if (!assembleSegments(
                   std::vector<std::string>([](const std::vector<ValidSegment> &c) {
                     std::vector<std::string> paths;
                     paths.reserve(c.size());
                     for (const auto &v : c) {
                       paths.push_back(v.path);
                     }
                     return paths;
                   }(committed)),
                   params.outputPath, params.pipeline.container)) {
      result.error = Error::make(ErrorCode::ExecutionFailed,
                                 "cannot assemble committed segments", "ffmpeg");
      return result;
    }
    std::filesystem::remove_all(segDir, ec);
    result.success = true;
    return result;
  }

  // Progress spans the whole job (committed prefix included).
  const auto baseProgress = std::int64_t(committed.size());
  std::int64_t nextIndex = baseProgress;
  std::int64_t segStart = resumeUs;
  while (segStart < totalUs) {
    if (params.cancel && params.cancel->isCancelled()) {
      result.error = Error::make(ErrorCode::Cancelled,
                                 "transcode cancelled (segments committed)",
                                 "ffmpeg");
      return result;
    }
    const std::int64_t segEnd = std::min(segStart + kSegmentTargetUs, totalUs);
    const std::string segPath =
        (segDir / ("seg-" + std::to_string(nextIndex) + outFs.extension().string()))
            .string();
    // Per-segment progress rebasing: local fraction -> whole-job fraction.
    ExecuteParams segParams = params;
    segParams.onProgress =
        [params, segStart, segEnd,
         totalUs](const backend::ProgressReport &report) {
          if (!params.onProgress || report.fraction < 0) {
            return;
          }
          ProgressReport global;
          const double local =
              std::clamp(report.fraction, 0.0, 1.0);
          global.processedDurationUs =
              segStart +
              std::int64_t(local * double(segEnd - segStart));
          global.totalDurationUs = totalUs;
          global.fraction =
              std::clamp(double(global.processedDurationUs) / double(totalUs),
                         0.0, 1.0);
          params.onProgress(global);
        };
    const ExecuteResult seg =
        executeTranscodeRange(segParams, segStart, segEnd, true);
    if (!seg.success) {
      // Partial segment files are never committed: remove and surface the
      // error; committed segments stay durable for the next resume.
      std::error_code removeEc;
      std::filesystem::remove(segPath, removeEc);
      return seg;
    }
    if (!segmentArtifactUsable(segPath)) {
      std::error_code removeEc;
      std::filesystem::remove(segPath, removeEc);
      result.error = Error::make(ErrorCode::ExecutionFailed,
                                 "segment failed artifact validation", "ffmpeg");
      return result;
    }
    if (params.checkpoints != nullptr) {
      backend::CheckpointInfo info;
      info.stage = StageId::Encode;
      info.position = segEnd;
      info.artifactPath = segPath;
      if (!params.checkpoints->commit(info)) {
        result.error = Error::make(ErrorCode::ExecutionFailed,
                                   "checkpoint commit failed", "ffmpeg");
        return result;
      }
    }
    committed.push_back({segPath, segEnd});
    ++nextIndex;
    segStart = segEnd;
  }

  // Final assembly from validated pieces (spec §26: promote/commit safely).
  if (committed.size() == 1) {
    std::filesystem::rename(committed.front().path, params.outputPath, ec);
    if (ec) {
      std::filesystem::copy_file(committed.front().path, params.outputPath, ec);
      if (ec) {
        result.error = Error::make(ErrorCode::ExecutionFailed,
                                   "cannot promote the finished output", "ffmpeg");
        return result;
      }
    }
  } else {
    std::vector<std::string> paths;
    paths.reserve(committed.size());
    for (const auto &v : committed) {
      paths.push_back(v.path);
    }
    if (!assembleSegments(paths, params.outputPath,
                          params.pipeline.container)) {
      result.error =
          Error::make(ErrorCode::ExecutionFailed,
                      "cannot assemble the finished segments", "ffmpeg");
      return result;
    }
  }
  std::filesystem::remove_all(segDir, ec);
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
