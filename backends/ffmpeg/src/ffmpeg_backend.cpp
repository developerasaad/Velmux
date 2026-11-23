#include "velmux/ffmpeg/ffmpeg_backend.h"
#include "velmux/ffmpeg/codec_mapping.h"
#include "subtitle_tools.h"
#include "ffmpeg_util.h"

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
#include <libavutil/audio_fifo.h>
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

// Shared helpers (avErrorString, makeAvError, qualityToCrf,
// encoderNameFor) live in ffmpeg_util.h.

// Maps a request-model pixel format (spec §18 advanced controls) to the
// libavutil pixel format. AV_PIX_FMT_NONE means "unknown" (refused).
AVPixelFormat avPixFmtFor(core::PixelFormat format) {
  using core::PixelFormat;
  switch (format) {
    case PixelFormat::Yuv420P:
      return AV_PIX_FMT_YUV420P;
    case PixelFormat::Yuv422P:
      return AV_PIX_FMT_YUV422P;
    case PixelFormat::Yuv444P:
      return AV_PIX_FMT_YUV444P;
    case PixelFormat::Yuv420P10Le:
      return AV_PIX_FMT_YUV420P10LE;
    case PixelFormat::Yuv422P10Le:
      return AV_PIX_FMT_YUV422P10LE;
    case PixelFormat::Yuv444P10Le:
      return AV_PIX_FMT_YUV444P10LE;
  }
  return AV_PIX_FMT_NONE;
}

// Maps a user-facing profile name to the codec's FF_PROFILE value.
// Returns -1 when the name is unknown or does not apply to the codec.
int ffmpegProfileFor(core::VideoCodec codec, const std::string &name) {
  using core::VideoCodec;
  // Case-insensitive compare against the accepted spellings.
  std::string lower;
  lower.reserve(name.size());
  for (char c : name) {
    lower.push_back(char(std::tolower(static_cast<unsigned char>(c))));
  }
  switch (codec) {
    case VideoCodec::H264:
      if (lower == "baseline") {
        return FF_PROFILE_H264_BASELINE;
      }
      if (lower == "main") {
        return FF_PROFILE_H264_MAIN;
      }
      if (lower == "high") {
        return FF_PROFILE_H264_HIGH;
      }
      if (lower == "high10") {
        return FF_PROFILE_H264_HIGH_10;
      }
      if (lower == "high422") {
        return FF_PROFILE_H264_HIGH_422;
      }
      return -1;
    case VideoCodec::HEVC:
      if (lower == "main") {
        return FF_PROFILE_HEVC_MAIN;
      }
      if (lower == "main10") {
        return FF_PROFILE_HEVC_MAIN_10;
      }
      if (lower == "main-still-picture") {
        return FF_PROFILE_HEVC_MAIN_STILL_PICTURE;
      }
      return -1;
    case VideoCodec::VP9:
      if (lower == "0" || lower == "profile0") {
        return FF_PROFILE_VP9_0;
      }
      if (lower == "1" || lower == "profile1") {
        return FF_PROFILE_VP9_1;
      }
      if (lower == "2" || lower == "profile2") {
        return FF_PROFILE_VP9_2;
      }
      if (lower == "3" || lower == "profile3") {
        return FF_PROFILE_VP9_3;
      }
      return -1;
    case VideoCodec::AV1:
      if (lower == "main") {
        return FF_PROFILE_AV1_MAIN;
      }
      if (lower == "high") {
        return FF_PROFILE_AV1_HIGH;
      }
      if (lower == "professional") {
        return FF_PROFILE_AV1_PROFESSIONAL;
      }
      return -1;
    default:
      return -1; // profiles are not meaningful for the remaining codecs
  }
}

// Per-track audio pipeline state (multi-track audio, spec §9). Every
// selected source audio stream gets its own decode -> [DSP] -> resample ->
// encode chain (or a pure stream-copy lane); the central loop dispatches
// packets by input stream index.
struct AudioTrack {
  // Input side. `input` points at the owning AVFormatContext (the main
  // input, or the replacement input for replace-audio requests).
  AVFormatContext *input = nullptr;
  int inStream = -1;
  bool copy = false; // stream-copy lane: packets pass straight to `out`
  // Processing chain (unused for copy lanes).
  AVCodecContext *dec = nullptr;
  AVCodecContext *enc = nullptr;
  AVAudioFifo *fifo = nullptr;
  SwrContext *swr = nullptr;
  AVFilterGraph *graph = nullptr; // DSP chain (gain/fades/loudness/...)
  AVFilterContext *src = nullptr;
  AVFilterContext *sink = nullptr;
  // Output side.
  int out = -1;
  std::int64_t samplesOut = 0; // pts bookkeeping in output samples
};

struct CodecCleanup {
  ~CodecCleanup() {
    av_frame_free(&frame);
    av_packet_free(&packet);
    sws_freeContext(sws);
    for (AudioTrack &track : audio) {
      swr_free(&track.swr);
      if (track.fifo != nullptr) {
        av_audio_fifo_free(track.fifo);
      }
      avfilter_graph_free(&track.graph);
      avcodec_free_context(&track.dec);
      avcodec_free_context(&track.enc);
    }
    avfilter_graph_free(&filterGraph);
    if (audioReplaceIn != nullptr) {
      AVFormatContext *ctx = audioReplaceIn;
      avformat_close_input(&ctx);
    }
    avcodec_free_context(&videoDec);
    avcodec_free_context(&videoEnc);
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
  // Replacement audio input (spec §9 replace-audio); opened lazily.
  AVFormatContext *audioReplaceIn = nullptr;
  AVCodecContext *videoDec = nullptr;
  AVCodecContext *videoEnc = nullptr;
  SwsContext *sws = nullptr;
  // Selected audio tracks, in output order.
  std::vector<AudioTrack> audio;
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
      return false; // handled by the bitmap path below when applicable
  }
}

// Bitmap subtitle codec ids decodable into AVSubtitle pict rects and
// composited through the overlay-based burn-in path.
bool isBitmapSubtitleCodec(AVCodecID id) {
  switch (id) {
    case AV_CODEC_ID_HDMV_PGS_SUBTITLE:
    case AV_CODEC_ID_DVB_SUBTITLE:
    case AV_CODEC_ID_DVD_SUBTITLE:
      return true;
    default:
      return false;
  }
}

// One pre-decoded bitmap cue: an overlay frame (YUVA420P, full output
// geometry, alpha 0 = transparent outside the rects) valid for
// [startUs, endUs) on the source timeline.
struct BitmapCue {
  std::int64_t startUs = 0;
  std::int64_t endUs = 0;
  AVFrame *overlay = nullptr; // owned by the vector's owner
};

// Composites one decoded subtitle's rects into a full-geometry YUVA frame.
// Rect positions/sizes map linearly from the source video size to the
// output geometry (burn-in may downscale; transforms are refused earlier).
bool compositeSubtitleRects(const AVSubtitle &sub, std::int32_t srcW,
                            std::int32_t srcH, std::int32_t outW,
                            std::int32_t outH, AVFrame *frame) {
  const double sx = double(outW) / double(std::max(srcW, 1));
  const double sy = double(outH) / double(std::max(srcH, 1));
  for (unsigned r = 0; r < sub.num_rects; ++r) {
    const AVSubtitleRect *rect = sub.rects[r];
    if (rect->type != SUBTITLE_BITMAP || rect->w <= 0 || rect->h <= 0 ||
        rect->data[0] == nullptr || rect->data[1] == nullptr) {
      continue;
    }
    const std::int32_t rw = std::max(2, std::int32_t(rect->w * sx) & ~1);
    const std::int32_t rh = std::max(2, std::int32_t(rect->h * sy) & ~1);
    const std::int32_t rx =
        std::clamp(std::int32_t(rect->x * sx), 0, std::max(0, outW - rw));
    const std::int32_t ry =
        std::clamp(std::int32_t(rect->y * sy), 0, std::max(0, outH - rh));
    // PAL8 (indices + palette) -> YUVA420P directly into the sub-region.
    SwsContext *sws = sws_getContext(
        rect->w, rect->h, AV_PIX_FMT_PAL8, rw, rh, AV_PIX_FMT_YUVA420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (sws == nullptr) {
      return false;
    }
    const uint8_t *srcSlice[4] = {rect->data[0], rect->data[1], nullptr,
                                  nullptr};
    const int srcStride[4] = {rect->linesize[0], 0, 0, 0};
    // Per-plane destination offsets for the sub-region (Y, U, V, A).
    uint8_t *dst[4] = {
        frame->data[0] + std::size_t(ry) * frame->linesize[0] + rx,
        frame->data[1] + std::size_t(ry / 2) * frame->linesize[1] + rx / 2,
        frame->data[2] + std::size_t(ry / 2) * frame->linesize[2] + rx / 2,
        frame->data[3] + std::size_t(ry) * frame->linesize[3] + rx,
    };
    const int dstStride[4] = {frame->linesize[0], frame->linesize[1],
                              frame->linesize[2], frame->linesize[3]};
    sws_scale(sws, srcSlice, srcStride, 0, rect->h, dst, dstStride);
    sws_freeContext(sws);
  }
  return true;
}

// Pre-decodes every bitmap cue of a subtitle stream (embedded in the
// source, or a standalone external file: .sup, .idx/.sub, ...) into
// full-geometry overlay frames. Bitmap burn-in composites these through
// the overlay filter: libass cannot render them.
class BitmapCueLoader {
public:
  ~BitmapCueLoader() {
    for (auto &cue : cues_) {
      if (cue.overlay != nullptr) {
        av_frame_free(&cue.overlay);
      }
    }
  }

  // Returns false with a reason on failure. `durationUs` bounds the last
  // open cue (PGS cues end at the NEXT cue; a trailing one would else
  // never end).
  bool load(const std::string &path, std::int32_t srcW, std::int32_t srcH,
            std::int32_t outW, std::int32_t outH, std::int64_t durationUs,
            std::int64_t offsetMs, std::string &reason) {
    AVFormatContext *in = nullptr;
    if (avformat_open_input(&in, path.c_str(), nullptr, nullptr) < 0 ||
        avformat_find_stream_info(in, nullptr) < 0) {
      if (in != nullptr) {
        AVFormatContext *ctx = in;
        avformat_close_input(&ctx);
      }
      reason = "cannot open the bitmap subtitle source";
      return false;
    }
    int subStream = -1;
    for (unsigned i = 0; i < in->nb_streams; ++i) {
      if (in->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
        subStream = int(i);
        break;
      }
    }
    if (subStream < 0) {
      avformat_close_input(&in);
      reason = "the source has no subtitle track";
      return false;
    }
    const AVCodecID cid = in->streams[subStream]->codecpar->codec_id;
    if (!isBitmapSubtitleCodec(cid)) {
      avformat_close_input(&in);
      reason = std::string("codec ") + avcodec_get_name(cid) +
               " is not a bitmap subtitle format";
      return false;
    }
    const AVCodec *dec = avcodec_find_decoder(cid);
    AVCodecContext *decCtx = dec != nullptr ? avcodec_alloc_context3(dec)
                                            : nullptr;
    if (decCtx == nullptr ||
        avcodec_parameters_to_context(decCtx,
                                      in->streams[subStream]->codecpar) < 0 ||
        avcodec_open2(decCtx, dec, nullptr) < 0) {
      if (decCtx != nullptr) {
        avcodec_free_context(&decCtx);
      }
      avformat_close_input(&in);
      reason = "cannot open the bitmap subtitle decoder";
      return false;
    }
    const AVRational subTb = in->streams[subStream]->time_base;
    const std::int64_t offsetUs = offsetMs * 1000;
    AVPacket *pkt = av_packet_alloc();
    AVSubtitle sub = {0};
    std::vector<BitmapCue> raw; // cues before end-time resolution
    while (av_read_frame(in, pkt) >= 0) {
      if (pkt->stream_index != subStream) {
        av_packet_unref(pkt);
        continue;
      }
      int gotSub = 0;
      if (avcodec_decode_subtitle2(decCtx, &sub, &gotSub, pkt) >= 0 &&
          gotSub != 0) {
        const std::int64_t baseUs =
            pkt->pts != AV_NOPTS_VALUE
                ? av_rescale_q(pkt->pts, subTb, AVRational{1, 1000000})
                : 0;
        if (sub.num_rects == 0) {
          // Clear marker (PGS empty display set): closes the previous cue.
          if (!raw.empty() && raw.back().endUs < 0) {
            raw.back().endUs = baseUs;
          }
        } else {
          BitmapCue cue;
          cue.startUs =
              baseUs + std::int64_t(sub.start_display_time) * 1000;
          cue.endUs = sub.end_display_time == UINT32_MAX
                          ? std::int64_t(-1) // closed by the next cue
                          : baseUs +
                                std::int64_t(sub.end_display_time) * 1000;
          cue.overlay = av_frame_alloc();
          cue.overlay->format = AV_PIX_FMT_YUVA420P;
          cue.overlay->width = outW;
          cue.overlay->height = outH;
          if (av_frame_get_buffer(cue.overlay, 0) < 0) {
            av_frame_free(&cue.overlay);
            reason = "cannot allocate the cue overlay frame";
            avsubtitle_free(&sub);
            av_packet_unref(pkt);
            avcodec_free_context(&decCtx);
            avformat_close_input(&in);
            av_packet_free(&pkt);
            return false;
          }
          // Transparent background: alpha 0 everywhere; the rects overwrite
          // their sub-regions. get_buffer does NOT zero the planes.
          for (int p = 0; p < 4; ++p) {
            const int planeH = p == 1 || p == 2 ? (outH + 1) / 2 : outH;
            if (cue.overlay->data[p] != nullptr) {
              std::memset(cue.overlay->data[p], 0,
                          std::size_t(cue.overlay->linesize[p]) *
                              std::size_t(planeH));
            }
          }
          if (!compositeSubtitleRects(sub, srcW, srcH, outW, outH,
                                      cue.overlay)) {
            av_frame_free(&cue.overlay);
            reason = "cannot composite the bitmap cue";
            avsubtitle_free(&sub);
            av_packet_unref(pkt);
            avcodec_free_context(&decCtx);
            avformat_close_input(&in);
            av_packet_free(&pkt);
            return false;
          }
          raw.push_back(std::move(cue));
        }
        avsubtitle_free(&sub);
      }
      av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avcodec_free_context(&decCtx);
    avformat_close_input(&in);

    if (raw.empty()) {
      reason = "no bitmap cues found in the subtitle source";
      return false;
    }
    const std::int64_t boundUs =
        durationUs > 0 ? durationUs + 1000000 : INT64_MAX / 4;
    for (std::size_t i = 0; i < raw.size(); ++i) {
      BitmapCue cue = std::move(raw[i]);
      if (cue.endUs < 0) {
        cue.endUs = i + 1 < raw.size() ? raw[i + 1].startUs : boundUs;
      }
      // User timing offset + sanity clamps.
      cue.startUs = std::max<std::int64_t>(cue.startUs + offsetUs, 0);
      cue.endUs = std::min<std::int64_t>(cue.endUs + offsetUs, boundUs);
      if (cue.endUs > cue.startUs) {
        cues_.push_back(std::move(cue));
      }
    }
    return !cues_.empty();
  }

  std::vector<BitmapCue> &cues() { return cues_; }

private:
  std::vector<BitmapCue> cues_;
};

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

// Formats a timestamp as SRT time (HH:MM:SS,mmm).
std::string srtTimestamp(std::int64_t us) {
  if (us < 0) {
    us = 0;
  }
  const std::int64_t msTotal = us / 1000;
  const std::int64_t ms = msTotal % 1000;
  const std::int64_t sTotal = msTotal / 1000;
  const std::int64_t s = sTotal % 60;
  const std::int64_t m = (sTotal / 60) % 60;
  const std::int64_t h = sTotal / 3600;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld,%03lld",
                static_cast<long long>(h), static_cast<long long>(m),
                static_cast<long long>(s), static_cast<long long>(ms));
  return buf;
}

// Decodes a container-internal text subtitle stream (mov_text, ttml, ...)
// and writes a valid SRT sidecar. Bitmap decodes (PGS/DVB/VobSub) produce
// no text rects and fail honestly. Progress and cancellation are honored
// at packet granularity (demux-level work; there is nothing to
// checkpoint).
bool decodeSubtitleStreamToSrt(AVFormatContext *in, int streamIndex,
                               const std::string &outPath,
                               const velmux::backend::ExecuteParams &params) {
  const AVCodec *codec =
      avcodec_find_decoder(in->streams[streamIndex]->codecpar->codec_id);
  if (codec == nullptr) {
    return false;
  }
  AVCodecContext *dec = avcodec_alloc_context3(codec);
  if (dec == nullptr) {
    return false;
  }
  if (avcodec_parameters_to_context(dec, in->streams[streamIndex]->codecpar) <
          0 ||
      avcodec_open2(dec, codec, nullptr) < 0) {
    avcodec_free_context(&dec);
    return false;
  }
  std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
  if (!out.good()) {
    avcodec_free_context(&dec);
    return false;
  }
  AVPacket *pkt = av_packet_alloc();
  AVSubtitle sub = {0};
  std::int64_t cueIndex = 0;
  bool ok = true;
  bool cancelled = false;
  while (ok && av_read_frame(in, pkt) >= 0) {
    if (params.cancel && params.cancel->isCancelled()) {
      cancelled = true;
      av_packet_unref(pkt);
      break;
    }
    if (pkt->stream_index != streamIndex) {
      av_packet_unref(pkt);
      continue;
    }
    int gotSub = 0;
    const int consumed =
        avcodec_decode_subtitle2(dec, &sub, &gotSub, pkt);
    if (consumed < 0) {
      // Damaged packet: skip it, keep the rest (isolate malformed input).
      av_packet_unref(pkt);
      continue;
    }
    if (gotSub != 0) {
      const AVRational tb = in->streams[streamIndex]->time_base;
      const std::int64_t startUs =
          sub.pts != AV_NOPTS_VALUE
              ? av_rescale_q(sub.pts, dec->pkt_timebase.num > 0
                                         ? dec->pkt_timebase
                                         : tb,
                             AVRational{1, 1000000})
              : 0;
      std::int64_t endUs = startUs;
      if (sub.end_display_time > 0) {
        endUs = startUs + std::int64_t(sub.end_display_time) * 1000;
      } else {
        endUs = startUs + 2'000'000; // sane default cue length
      }
      std::string text;
      for (unsigned r = 0; r < sub.num_rects; ++r) {
        const AVSubtitleRect *rect = sub.rects[r];
        if (rect->type == SUBTITLE_TEXT && rect->text != nullptr) {
          if (!text.empty()) {
            text += "\n";
          }
          text += rect->text;
        } else if (rect->type == SUBTITLE_ASS && rect->ass != nullptr) {
          // "Dialogue: 0,start,end,style,,0,0,0,,text" - take the last
          // field; strip the trivial override tags.
          const char *ass = rect->ass;
          const char *lastComma = strrchr(ass, ',');
          if (lastComma != nullptr) {
            std::string chunk(lastComma + 1);
            std::size_t pos = 0;
            while ((pos = chunk.find('{', pos)) != std::string::npos) {
              const std::size_t close = chunk.find('}', pos);
              if (close == std::string::npos) {
                chunk.erase(pos);
                break;
              }
              chunk.erase(pos, close - pos + 1);
            }
            if (!text.empty()) {
              text += "\n";
            }
            text += chunk;
          }
        }
      }
      if (!text.empty()) {
        ++cueIndex;
        out << cueIndex << "\n"
            << srtTimestamp(startUs) << " --> " << srtTimestamp(endUs)
            << "\n"
            << text << "\n\n";
      }
      avsubtitle_free(&sub);
    }
    av_packet_unref(pkt);
  }
  av_packet_free(&pkt);
  avcodec_free_context(&dec);
  out.flush();
  ok = out.good() && cueIndex > 0;
  if (cancelled) {
    return false;
  }
  return ok;
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

  // CEA-608/708 detection (spec §11): captions ride the video elementary
  // stream as A53 side data on packets. A bounded demux-level probe (no
  // decoding) checks the first video stream's leading packets; presence
  // flags the stream so the planner can warn that transcoding drops them
  // while stream copy keeps them.
  if (!out.video.empty()) {
    int firstVideo = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
      if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        firstVideo = int(i);
        break;
      }
    }
    if (firstVideo >= 0) {
      AVPacket *probePkt = av_packet_alloc();
      int videoPackets = 0;
      bool sawCc = false;
      while (videoPackets < 64 && !sawCc &&
             av_read_frame(fmt, probePkt) >= 0) {
        if (probePkt->stream_index == firstVideo) {
          ++videoPackets;
          if (av_packet_get_side_data(
                  probePkt, AV_PKT_DATA_A53_CC, nullptr) != nullptr) {
            sawCc = true;
          }
        }
        av_packet_unref(probePkt);
      }
      av_packet_free(&probePkt);
      if (sawCc) {
        out.video.front().hasClosedCaptions = true;
      }
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

bool FfmpegInspector::representativeWindow(const MediaSource &source,
                                          std::int32_t seconds,
                                          std::int64_t &startUs,
                                          std::int64_t &endUs) {
  initFfmpegLabel();
  if (seconds <= 0) {
    return false;
  }
  const std::optional<std::string> path = localPathFor(source);
  if (!path.has_value()) {
    return false;
  }
  AVFormatContext *fmt = nullptr;
  if (avformat_open_input(&fmt, path->c_str(), nullptr, nullptr) != 0 ||
      fmt == nullptr) {
    return false;
  }
  if (avformat_find_stream_info(fmt, nullptr) < 0) {
    avformat_close_input(&fmt);
    return false;
  }
  std::int64_t durationUs = 0;
  if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0) {
    durationUs = av_rescale_q(fmt->duration, AVRational{1, AV_TIME_BASE},
                              AVRational{1, 1000000});
  }
  if (durationUs <= std::int64_t(seconds) * 1000000) {
    // Whole file fits: window = everything.
    startUs = 0;
    endUs = durationUs;
    avformat_close_input(&fmt);
    return durationUs > 0;
  }

  // Single demux pass: per-second compressed byte totals over A/V packets.
  // More bytes per second == more motion/detail at the same codec settings.
  const std::int64_t totalSec = durationUs / 1000000 + 1;
  if (totalSec > (1 << 24)) { // absurd metadata; refuse honestly
    avformat_close_input(&fmt);
    return false;
  }
  std::vector<std::int64_t> bytesPerSec(std::size_t(totalSec), 0);
  AVPacket *pkt = av_packet_alloc();
  while (av_read_frame(fmt, pkt) >= 0) {
    const AVStream *stream = fmt->streams[std::size_t(pkt->stream_index)];
    const AVMediaType type = stream->codecpar->codec_type;
    if ((type == AVMEDIA_TYPE_VIDEO || type == AVMEDIA_TYPE_AUDIO) &&
        pkt->pts != AV_NOPTS_VALUE) {
      const std::int64_t us = av_rescale_q(pkt->pts, stream->time_base,
                                           AVRational{1, 1000000});
      const std::int64_t sec = us / 1000000;
      if (sec >= 0 && sec < totalSec) {
        bytesPerSec[std::size_t(sec)] += pkt->size;
      }
    }
    av_packet_unref(pkt);
  }
  av_packet_free(&pkt);
  avformat_close_input(&fmt);

  // Slide the window; pick the densest (ties: earliest — deterministic).
  const std::size_t win = std::size_t(seconds);
  std::int64_t best = 0;
  for (std::size_t i = 0; i < win && i < bytesPerSec.size(); ++i) {
    best += bytesPerSec[i];
  }
  std::int64_t bestStart = 0;
  std::int64_t rolling = best;
  for (std::size_t i = win; i < bytesPerSec.size(); ++i) {
    rolling += bytesPerSec[i] - bytesPerSec[i - win];
    if (rolling > best) {
      best = rolling;
      bestStart = std::int64_t(i - win) + 1;
    }
  }
  startUs = bestStart * 1000000;
  endUs = std::min(startUs + std::int64_t(seconds) * 1000000, durationUs);
  return true;
}

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
  // Subtitle-extraction jobs: demux + subtitle processing only, artifact
  // is a text sidecar.
  if (params.request.subtitles.policy == SubtitlePolicy::Extract) {
    return executeSubtitleExtract(params);
  }
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

ExecuteResult FfmpegExecutor::executeSubtitleExtract(
    const ExecuteParams &params) {
  ExecuteResult result;
  initFfmpegLabel();
  const ConversionRequest &request = params.request;
  if (params.pipeline.sidecarExtension.empty() || request.video.enabled ||
      request.audio.enabled) {
    result.error = Error::make(
        ErrorCode::InvalidArgument,
        "extraction requests must be subtitle-only with a sidecar pipeline",
        "ffmpeg");
    return result;
  }
  const std::optional<std::string> inPath = localPathFor(request.source);
  if (!inPath.has_value()) {
    result.error = Error::make(ErrorCode::SourceUnavailable,
                               "unsupported source URI", "ffmpeg");
    return result;
  }

  AVFormatContext *in = nullptr;
  if (avformat_open_input(&in, inPath->c_str(), nullptr, nullptr) < 0 ||
      avformat_find_stream_info(in, nullptr) < 0) {
    if (in != nullptr) {
      AVFormatContext *ctx = in;
      avformat_close_input(&ctx);
    }
    result.error = Error::make(ErrorCode::SourceUnavailable,
                               "cannot open the source", "ffmpeg");
    return result;
  }
  int subStream = -1;
  for (unsigned i = 0; i < in->nb_streams; ++i) {
    if (in->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
      subStream = int(i);
      break;
    }
  }
  if (subStream < 0) {
    avformat_close_input(&in);
    result.error = Error::make(ErrorCode::InvalidArgument,
                               "the source has no subtitle track", "ffmpeg");
    return result;
  }
  const AVCodecID cid = in->streams[subStream]->codecpar->codec_id;

  // Copy path: native text formats whose packets ARE the file format.
  bool ok = false;
  if (subtitleFileExtensionFor(cid) != nullptr) {
    ok = extractSubtitleStreamToFile(*inPath, subStream, params.outputPath);
  } else {
    // Decode path: container-internal text (mov_text, ttml, ...) decodes
    // to AVSubtitle text rects, written as SRT cues.
    ok = decodeSubtitleStreamToSrt(in, subStream, params.outputPath,
                                    params);
  }
  avformat_close_input(&in);
  if (!ok) {
    result.error = Error::make(
        ErrorCode::UnsupportedFeature,
        std::string("cannot extract subtitle codec ") +
            avcodec_get_name(cid) +
            " (bitmap formats need OCR, which is outside V1)",
        "ffmpeg");
    std::error_code ec;
    std::filesystem::remove(params.outputPath, ec);
    return result;
  }
  result.success = true;
  result.framesProcessed = 1;
  return result;
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
  // Explicit track selection (spec §9): keep only the selected audio
  // stream; allTracks (and the default) keep every audio stream.
  int audioOrdinal = 0;
  const bool selectSingleAudioTrack =
      params.request.audio.enabled &&
      params.request.audio.sourceTrackIndex >= 0 &&
      !params.request.audio.allTracks;
  for (unsigned i = 0; i < cleanup.in->nb_streams; ++i) {
    const AVStream *inStream = cleanup.in->streams[i];
    const AVMediaType type = inStream->codecpar->codec_type;
    if (type == AVMEDIA_TYPE_AUDIO) {
      ++audioOrdinal;
    }
    const bool keepVideo = type == AVMEDIA_TYPE_VIDEO &&
                           params.request.video.enabled;
    const bool keepAudio = type == AVMEDIA_TYPE_AUDIO &&
                           params.request.audio.enabled &&
                           (!selectSingleAudioTrack ||
                            audioOrdinal - 1 ==
                                params.request.audio.sourceTrackIndex);
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

  // Metadata preservation (spec): container-level + per-stream tags copy
  // through the remux unchanged. stripMetadata (advanced control) drops
  // them instead.
  if (!params.request.stripMetadata) {
    av_dict_copy(&cleanup.out->metadata, cleanup.in->metadata,
                 AV_DICT_DONT_OVERWRITE);
    for (std::size_t k = 0; k < inIndexOfOut.size() &&
                             k < cleanup.out->nb_streams;
         ++k) {
      av_dict_copy(&cleanup.out->streams[k]->metadata,
                   cleanup.in->streams[std::size_t(inIndexOfOut[k])]
                       ->metadata,
                   AV_DICT_DONT_OVERWRITE);
    }
  }

  // External cover art (spec §10): attached before the header (the
  // single packet rides the codecpar extradata).
  if (params.request.audio.coverArtUri.has_value()) {
    const std::optional<std::string> coverPath =
        localPathForUri(*params.request.audio.coverArtUri);
    if (!coverPath.has_value()) {
      result.error = Error::make(ErrorCode::SourceUnavailable,
                                 "cover art URI is not a local path",
                                 "ffmpeg");
      return result;
    }
    std::string coverErr;
    if (addCoverArtStream(cleanup.out, *coverPath, coverErr) == nullptr) {
      result.error =
          Error::make(ErrorCode::InvalidArgument,
                      "cannot embed cover art: " + coverErr, "ffmpeg");
      return result;
    }
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
  // Trim window (spec §15/§16): seek to the keyframe at/before the start
  // (stream copy cannot re-time compressed data, so the output begins
  // there — the honest keyframe-snapped trim) and stop at the end gate.
  const bool remuxTrimmed = params.request.trim.active();
  if (remuxTrimmed && params.request.trim.startUs > 0) {
    int seekStream = 0;
    for (unsigned i = 0; i < cleanup.in->nb_streams; ++i) {
      if (cleanup.in->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        seekStream = int(i);
        break;
      }
    }
    const std::int64_t target = av_rescale_q(
        params.request.trim.startUs, AVRational{1, 1000000},
        cleanup.in->streams[std::size_t(seekStream)]->time_base);
    if (avformat_seek_file(cleanup.in, seekStream, INT64_MIN, target,
                           INT64_MAX, AVSEEK_FLAG_BACKWARD) < 0) {
      result.error = Error::make(ErrorCode::ExecutionFailed,
                                 "cannot seek to the trim boundary", "ffmpeg");
      return result;
    }
  }
  while (av_read_frame(cleanup.in, cleanup.packet) >= 0) {
    if (params.cancel && params.cancel->isCancelled()) {
      cancelled = true;
      av_packet_unref(cleanup.packet);
      break;
    }
    if (remuxTrimmed && params.request.trim.endUs >= 0) {
      const AVStream *pktStream =
          cleanup.in->streams[std::size_t(cleanup.packet->stream_index)];
      const std::int64_t pktUs =
          cleanup.packet->pts == AV_NOPTS_VALUE
              ? -1
              : av_rescale_q(cleanup.packet->pts, pktStream->time_base,
                             AVRational{1, 1000000});
      if (pktUs >= 0 && pktUs >= params.request.trim.endUs) {
        av_packet_unref(cleanup.packet);
        break; // past the window: stream copy is done
      }
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
    // Trim window: the SEGMENTABLE decision and the segment tiling use the
    // window length, not the whole source length.
    if (request.trim.active() && durationUs > 0) {
      const std::int64_t start =
          std::min<std::int64_t>(std::max<std::int64_t>(request.trim.startUs, 0),
                                 durationUs);
      const std::int64_t end = request.trim.endUs >= 0
                                   ? std::min(request.trim.endUs, durationUs)
                                   : durationUs;
      durationUs = std::max<std::int64_t>(end - start, 0);
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
  if (request.trim.active() && !segmentable) {
    // Trimmed single-pass: honor the window directly. Timestamps stay on
    // the source timeline (matched-source-timestamp comparison, spec §20).
    // endUs < 0 (open-ended) passes the probed duration, or INT64_MAX
    // when unknown — executeTranscodeRange's packet gate stops at EOF
    // regardless.
    const std::int64_t start =
        std::max<std::int64_t>(request.trim.startUs, 0);
    const std::int64_t end = request.trim.endUs >= 0
                                 ? request.trim.endUs
                                 : (durationUs > 0 ? durationUs
                                                   : INT64_MAX);
    return executeTranscodeRange(params, start, end, true);
  }
  if (segmentable && !params.jobId.empty()) {
    return executeTranscodeSegmented(params);
  }
  return executeTranscodeRange(params, 0, 0, false);
}

ExecuteResult FfmpegExecutor::executeTranscodeRange(
    const ExecuteParams &params, std::int64_t rangeStartUs,
    std::int64_t rangeEndUs, bool rangeActive, bool isSegmentRun) {
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

  int videoIn = -1;
  std::vector<int> audioStreams; // selected main-input audio streams
  std::vector<int> audioOrdinals; // subtitle-free ordinal per selection
  for (unsigned i = 0; i < cleanup.in->nb_streams; ++i) {
    const AVMediaType type = cleanup.in->streams[i]->codecpar->codec_type;
    if (type == AVMEDIA_TYPE_VIDEO && videoIn < 0) {
      videoIn = int(i);
    } else if (type == AVMEDIA_TYPE_AUDIO) {
      audioOrdinals.push_back(int(i));
    }
  }
  // Track selection policy (spec §9): replacement audio wins; allTracks
  // selects every audio stream; an explicit index selects that track;
  // otherwise the first.
  const std::optional<std::string> replacePath =
      request.audio.enabled && request.audio.replaceAudioUri.has_value()
          ? localPathForUri(*request.audio.replaceAudioUri)
          : std::nullopt;
  if (request.audio.enabled && replacePath.has_value()) {
    // Replacement input: opened here, its first audio stream feeds the
    // (single) audio pipeline. The main input's audio is ignored.
    if (avformat_open_input(&cleanup.audioReplaceIn, replacePath->c_str(),
                            nullptr, nullptr) != 0 ||
        avformat_find_stream_info(cleanup.audioReplaceIn, nullptr) < 0) {
      if (cleanup.audioReplaceIn != nullptr) {
        AVFormatContext *ctx = cleanup.audioReplaceIn;
        avformat_close_input(&ctx);
        cleanup.audioReplaceIn = nullptr;
      }
      result.error = Error::make(ErrorCode::SourceUnavailable,
                                 "cannot open the replacement audio file",
                                 "ffmpeg");
      return result;
    }
    int replaceAudio = -1;
    for (unsigned i = 0; i < cleanup.audioReplaceIn->nb_streams; ++i) {
      if (cleanup.audioReplaceIn->streams[i]->codecpar->codec_type ==
          AVMEDIA_TYPE_AUDIO) {
        replaceAudio = int(i);
        break;
      }
    }
    if (replaceAudio < 0) {
      result.error = Error::make(ErrorCode::InvalidArgument,
                                 "the replacement audio file has no audio "
                                 "track",
                                 "ffmpeg");
      return result;
    }
    audioStreams.push_back(-1); // marker: the replacement input's track
  } else if (request.audio.enabled && request.audio.allTracks) {
    audioStreams = audioOrdinals;
  } else if (request.audio.enabled && !audioOrdinals.empty()) {
    const int pick = request.audio.sourceTrackIndex >= 0
                         ? request.audio.sourceTrackIndex
                         : 0;
    audioStreams.push_back(audioOrdinals[std::size_t(pick)]);
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
    // The replacement input keeps its own timeline: seek it too so
    // segmented runs do not re-read from zero every segment.
    if (cleanup.audioReplaceIn != nullptr) {
      int repAudio = -1;
      for (unsigned i = 0; i < cleanup.audioReplaceIn->nb_streams; ++i) {
        if (cleanup.audioReplaceIn->streams[i]->codecpar->codec_type ==
            AVMEDIA_TYPE_AUDIO) {
          repAudio = int(i);
          break;
        }
      }
      if (repAudio >= 0) {
        const std::int64_t repTarget = av_rescale_q(
            rangeStartUs, AVRational{1, 1000000},
            cleanup.audioReplaceIn->streams[std::size_t(repAudio)]->time_base);
        avformat_seek_file(cleanup.audioReplaceIn, repAudio, INT64_MIN,
                           repTarget, INT64_MAX, AVSEEK_FLAG_BACKWARD);
      }
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
  // Output geometry (assigned by the video setup below; the bitmap event
  // timeline and the direct sws path consume it at function scope).
  std::int32_t outW = 0;
  std::int32_t outH = 0;
  // Burn-in routing: when subtitles are burned, decoded frames pass through
  // a libavfilter graph (scale -> subtitles[libass] -> buffersink) instead of
  // the direct sws path. Owned by cleanup.filterGraph.
  AVFilterContext *burnSrc = nullptr;
  AVFilterContext *burnSink = nullptr;
  // Bitmap burn-in overlay input (spec §11: PGS/VobSub/DVB); active only
  // for bitmap cues (text burn-in uses the libass subtitles filter).
  AVFilterContext *overlaySrc = nullptr;
  // Pre-decoded bitmap cues (spec §11); populated by the video setup when
  // bitmap burn-in is requested, consumed by the frame loop below.
  std::vector<BitmapCue> bitmapCues;
  // Owns every overlay frame (transparent pre-roll + all cue frames) for
  // the whole transcode; freed on any exit path.
  struct OverlayFrameGuard {
    std::vector<AVFrame *> frames;
    ~OverlayFrameGuard() {
      for (AVFrame *f : frames) {
        av_frame_free(&f);
      }
    }
  } overlayFrameGuard;
  // Time base the buffersrc was declared with; pushed frames' pts are
  // normalized into it (decoders may adjust their own time base mid-stream).
  AVRational burnGraphTb{0, 1};
  // Frame-rate conversion state (spec §9): output grid in segment-local
  // microseconds. fpsConvert/outFps are finalized once the video encoder
  // exists; the grid cursor advances per emitted frame.
  bool fpsConvert = false;
  double outFps = 0.0;
  double outFrameDurUs = 0.0;
  double fpsNextOutUs = 0.0;
  double srcFrameDurUs = 33333.0; // 30 fps fallback when unknown
  // HDR -> SDR tone mapping (spec §9): decided after the decoder and
  // encoder exist; drives the colorspace filter in the video graph.
  bool toneMap = false;
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

    // Target geometry: requested resolution or the TRANSFORMED source size;
    // never upscale. Crop applies first (source-frame pixels), then rotation
    // (odd multiples of 90 swap width/height); scale fits inside the
    // transformed rectangle preserving aspect.
    const bool wantsCrop =
        request.video.crop.has_value() && request.video.crop->active();
    const bool wantsRotation = request.video.rotationDegrees == 90 ||
                               request.video.rotationDegrees == 270;
    std::int32_t baseH = cleanup.videoDec->height;
    std::int32_t baseW = cleanup.videoDec->width;
    if (wantsCrop) {
      // The crop rectangle must fit entirely inside the source frame:
      // silently clamping a user rect to the overlap would produce a
      // different crop than requested (mangling, not tolerance).
      const CropRect &crop = *request.video.crop;
      const bool cropValid =
          crop.x >= 0 && crop.y >= 0 &&
          crop.x + crop.width <= cleanup.videoDec->width &&
          crop.y + crop.height <= cleanup.videoDec->height &&
          crop.width >= 2 && crop.height >= 2;
      if (!cropValid) {
        result.error = Error::make(
            ErrorCode::InvalidArgument,
            "crop rectangle does not fit inside the source frame", "ffmpeg");
        return result;
      }
      baseW = crop.width;
      baseH = crop.height;
    }
    if (wantsRotation) {
      std::swap(baseW, baseH);
    }
    std::int32_t outH2 = request.video.resolutionHeight.value_or(baseH);
    if (baseH > 0) {
      outH2 = std::min(outH2, baseH);
    }
    outH2 = std::max(outH2 & ~1, 16);
    const double aspect =
        baseW > 0 && baseH > 0 ? double(baseW) / double(baseH) : 16.0 / 9.0;
    std::int32_t outW2 = std::int32_t(std::lround(outH2 * aspect)) & ~1;
    outW2 = std::max(outW2, 16);
    outW = outW2;
    outH = outH2;
    cleanup.videoEnc->width = outW;
    cleanup.videoEnc->height = outH;
    // Frame-rate conversion (spec §9): a requested output rate re-times
    // the video onto a constant output grid (duplication/dropping in the
    // frame loop below); the encoder carries the OUTPUT rate.
    fpsConvert = request.video.frameRate.has_value() &&
                 *request.video.frameRate > 0.0 &&
                 *request.video.frameRate < 1000.0;
    outFps = fpsConvert ? *request.video.frameRate : 0.0;
    if (fpsConvert) {
      outFrameDurUs = 1000000.0 / outFps;
    }
    // Source frame duration fallback (used when frames carry no duration).
    if (inVideo->avg_frame_rate.den > 0 && inVideo->avg_frame_rate.num > 0) {
      srcFrameDurUs = 1000000.0 * double(inVideo->avg_frame_rate.den) /
                      double(inVideo->avg_frame_rate.num);
    }
    if (fpsConvert) {
      cleanup.videoEnc->framerate = av_d2q(outFps, 1001000);
      cleanup.videoEnc->time_base = av_inv_q(cleanup.videoEnc->framerate);
    } else if (inVideo->avg_frame_rate.den > 0 &&
               inVideo->avg_frame_rate.num > 0) {
      cleanup.videoEnc->framerate = inVideo->avg_frame_rate;
      cleanup.videoEnc->time_base = AVRational{inVideo->avg_frame_rate.den,
                                               inVideo->avg_frame_rate.num};
    } else {
      cleanup.videoEnc->framerate = AVRational{30, 1};
      cleanup.videoEnc->time_base = AVRational{1, 30};
    }
    cleanup.videoEnc->pix_fmt = AV_PIX_FMT_YUV420P;
    // ---- Advanced encoder controls (spec §18) ------------------------------
    // Pixel format: verify the concrete encoder accepts it before opening
    // (honest refusal, never a silent fallback to 4:2:0 8-bit).
    if (request.video.pixelFormat.has_value()) {
      const AVPixelFormat wanted = avPixFmtFor(*request.video.pixelFormat);
      if (wanted == AV_PIX_FMT_NONE) {
        result.error = Error::make(ErrorCode::InvalidArgument,
                                   "unknown output pixel format", "ffmpeg");
        return result;
      }
      bool supported = false;
      if (encCodec->pix_fmts != nullptr) {
        for (int i = 0; encCodec->pix_fmts[i] != AV_PIX_FMT_NONE; ++i) {
          if (encCodec->pix_fmts[i] == wanted) {
            supported = true;
            break;
          }
        }
      }
      if (!supported) {
        result.error = Error::make(
            ErrorCode::UnsupportedFeature,
            std::string("encoder ") + encCodec->name +
                " does not support pixel format " +
                pixelFormatName(*request.video.pixelFormat),
            "ffmpeg");
        return result;
      }
      cleanup.videoEnc->pix_fmt = wanted;
    }
    // Profile: codec-specific FF_PROFILE values. Unknown names are request
    // errors, never silently ignored.
    if (request.video.profile.has_value()) {
      const int profile =
          ffmpegProfileFor(pipeline.videoCodec, *request.video.profile);
      if (profile < 0) {
        result.error = Error::make(
            ErrorCode::InvalidArgument,
            "unknown or inapplicable profile \"" + *request.video.profile +
                "\" for codec " + codecName(pipeline.videoCodec),
            "ffmpeg");
        return result;
      }
      cleanup.videoEnc->profile = profile;
    }
    // Level: libx264 takes the level as a private option ("4.0", "31", ...);
    // the generic form is the numeric level_idc (major*10 + minor).
    if (request.video.level.has_value()) {
      const std::string &levelStr = *request.video.level;
      bool applied = false;
      if (std::string(encCodec->name) == "libx264") {
        applied = av_opt_set(cleanup.videoEnc->priv_data, "level",
                             levelStr.c_str(), 0) >= 0;
      }
      if (!applied) {
        double levelNum = 0.0;
        if (std::sscanf(levelStr.c_str(), "%lf", &levelNum) == 1 &&
            levelNum > 0.0 && levelNum < 100.0) {
          cleanup.videoEnc->level = int(levelNum * 10.0 + 0.5);
          applied = true;
        }
      }
      if (!applied) {
        result.error = Error::make(
            ErrorCode::InvalidArgument,
            "cannot parse level \"" + levelStr + "\"", "ffmpeg");
        return result;
      }
    }
    // GOP size and B-frame count (advanced controls, spec §18).
    if (request.video.gopSize.has_value()) {
      cleanup.videoEnc->gop_size = *request.video.gopSize;
    }
    if (request.video.bFrames.has_value()) {
      cleanup.videoEnc->max_b_frames = *request.video.bFrames;
    }
    // ---- HDR handling (spec §9) ---------------------------------------------
    // Source HDR signal from the decoder context (tagging authority: the
    // demuxer fills codecpar, avcodec_parameters_to_context carried it).
    const bool sourceHdr =
        cleanup.videoDec->color_trc == AVCOL_TRC_SMPTE2084 ||
        cleanup.videoDec->color_trc == AVCOL_TRC_ARIB_STD_B67 ||
        cleanup.videoDec->color_primaries == AVCOL_PRI_BT2020;
    // Tone map when explicitly requested, or automatically when the
    // source is HDR but the output cannot carry it (the pinned software
    // encoders are 8-bit 4:2:0). Preserving HDR signaling through a
    // compatible path keeps it untouched.
    const bool outputCanCarryHdr =
        cleanup.videoEnc->pix_fmt == AV_PIX_FMT_YUV420P10LE ||
        cleanup.videoEnc->pix_fmt == AV_PIX_FMT_YUV422P10LE ||
        cleanup.videoEnc->pix_fmt == AV_PIX_FMT_YUV444P10LE;
    toneMap = sourceHdr && !pipeline.videoStreamCopy &&
              (request.video.hdrPolicy == HdrPolicy::ToneMapToSdr ||
               !outputCanCarryHdr);
    if (toneMap) {
      // SDR BT.709 output tags (the colorspace filter produces them).
      cleanup.videoEnc->color_primaries = AVCOL_PRI_BT709;
      cleanup.videoEnc->color_trc = AVCOL_TRC_BT709;
      cleanup.videoEnc->colorspace = AVCOL_SPC_BT709;
      cleanup.videoEnc->color_range = AVCOL_RANGE_MPEG;
    } else if (sourceHdr) {
      // Preserve the source HDR signaling on the output stream.
      cleanup.videoEnc->color_primaries = cleanup.videoDec->color_primaries;
      cleanup.videoEnc->color_trc = cleanup.videoDec->color_trc;
      cleanup.videoEnc->colorspace = cleanup.videoDec->colorspace;
      cleanup.videoEnc->color_range = cleanup.videoDec->color_range;
    }
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

    // Unified video filter graph: burn-in rendering AND/OR geometry
    // transforms (crop -> rotation -> flips -> scale -> [subtitles]).
    const bool wantsBurnIn =
        request.subtitles.policy == SubtitlePolicy::BurnIn;
    const bool wantsTransform =
        wantsCrop || request.video.rotationDegrees != 0 ||
        request.video.flipHorizontal || request.video.flipVertical;
    if (wantsBurnIn || wantsTransform || toneMap) {
      // Resolve the burn-in source: explicit external file, else the first
      // embedded subtitle stream.
      std::string subPath;
      int subGlobalIndex = -1; // stream index in the input container
      int subOrdinal = -1;     // subtitle-stream ordinal (the filter's `si`)
      if (wantsBurnIn) {
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
      } // wantsBurnIn (subtitle source resolution)

      // Classify the burn-in material: bitmap (PGS/DVB/VobSub) composites
      // through the overlay path; text renders through libass.
      bool bitmapBurn = false;
      if (wantsBurnIn) {
        AVCodecID subCodecId = AV_CODEC_ID_NONE;
        if (subGlobalIndex >= 0) {
          subCodecId =
              cleanup.in->streams[subGlobalIndex]->codecpar->codec_id;
        } else {
          AVFormatContext *probeCtx = nullptr;
          if (avformat_open_input(&probeCtx, subPath.c_str(), nullptr,
                                  nullptr) >= 0) {
            avformat_find_stream_info(probeCtx, nullptr);
            for (unsigned i = 0; i < probeCtx->nb_streams; ++i) {
              if (probeCtx->streams[i]->codecpar->codec_type ==
                  AVMEDIA_TYPE_SUBTITLE) {
                subCodecId = probeCtx->streams[i]->codecpar->codec_id;
                break;
              }
            }
            avformat_close_input(&probeCtx);
          }
        }
        if (isBitmapSubtitleCodec(subCodecId)) {
          if (wantsTransform) {
            result.error = Error::make(
                ErrorCode::UnsupportedFeature,
                "bitmap subtitle burn-in cannot be combined with crop/"
                "rotation/flip in V1 (cue positions are authored for the "
                "source frame)",
                "ffmpeg");
            return result;
          }
          if (toneMap) {
            result.error = Error::make(
                ErrorCode::UnsupportedFeature,
                "bitmap subtitle burn-in cannot be combined with HDR->SDR "
                "tone mapping in V1 (the overlay graph composites 8-bit "
                "SDR cues)",
                "ffmpeg");
            return result;
          }
          if (request.video.pixelFormat.has_value() &&
              *request.video.pixelFormat != core::PixelFormat::Yuv420P) {
            result.error = Error::make(
                ErrorCode::UnsupportedFeature,
                "bitmap subtitle burn-in requires 8-bit 4:2:0 output in V1 "
                "(the overlay graph composites yuv420 cues)",
                "ffmpeg");
            return result;
          }
          bitmapBurn = true;
          BitmapCueLoader loader;
          std::string cueReason;
          const std::int64_t durationUs =
              cleanup.in->duration != AV_NOPTS_VALUE &&
                      cleanup.in->duration > 0
                  ? cleanup.in->duration
                  : 0;
          if (!loader.load(subPath, cleanup.videoDec->width,
                           cleanup.videoDec->height, outW, outH, durationUs,
                           request.subtitles.timingOffsetMs, cueReason)) {
            result.error = Error::make(
                ErrorCode::UnsupportedFeature,
                "bitmap burn-in failed: " + cueReason, "ffmpeg");
            return result;
          }
          bitmapCues = std::move(loader.cues());
          for (const BitmapCue &cue : bitmapCues) {
            overlayFrameGuard.frames.push_back(cue.overlay);
          }
        } else if (!isBurnableSubtitleCodec(subCodecId)) {
          result.error = Error::make(
              ErrorCode::UnsupportedFeature,
              std::string("burn-in of subtitle format ") +
                  avcodec_get_name(subCodecId) + " is not supported",
              "ffmpeg");
          return result;
        }
      }

      // Timing offset: materialize a shifted copy before rendering
      // (spec §14: adjust subtitle timing — never silently ignored).
      // Bitmap cues took the offset inside the loader.
      if (wantsBurnIn && !bitmapBurn &&
          request.subtitles.timingOffsetMs != 0) {
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

      const FontSource fontSource = wantsBurnIn ? findFontSource()
                                                : FontSource{};
      if (wantsBurnIn && fontSource.dir.empty()) {
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
      if (bitmapBurn) {
        // Bitmap burn-in graph (manually linked; no parse labels):
        //   main -> scale -> overlay(main)   over -> overlay(side)
        //   overlay -> sink
        // The overlay layer is YUVA420P at the OUTPUT geometry; the pre-
        // decoded cue frames were composited at exactly that size. A
        // transparent pre-roll frame at t=0 keeps framesync from holding
        // main frames before the first cue arrives.
        AVFilterContext *scaleF = nullptr;
        AVFilterContext *overlayF = nullptr;
        char overArgs[512];
        std::snprintf(overArgs, sizeof(overArgs),
                      "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d"
                      ":pixel_aspect=%d/%d",
                      outW, outH, AV_PIX_FMT_YUVA420P,
                      cleanup.videoDec->time_base.num,
                      cleanup.videoDec->time_base.den, sar.num, sar.den);
        // Filter args are option lists WITHOUT the filter-name prefix.
        const std::string scaleArgsStr =
            "w=" + std::to_string(outW) + ":h=" + std::to_string(outH);
        int rc = avfilter_graph_create_filter(
            &overlaySrc, srcFilter, "over", overArgs, nullptr,
            cleanup.filterGraph);
        if (rc >= 0) {
          rc = avfilter_graph_create_filter(
              &scaleF, avfilter_get_by_name("scale"), "sc",
              scaleArgsStr.c_str(), nullptr, cleanup.filterGraph);
        }
        if (rc >= 0) {
          rc = avfilter_graph_create_filter(
              &overlayF, avfilter_get_by_name("overlay"), "ov",
              "eof_action=pass:format=yuv420", nullptr,
              cleanup.filterGraph);
        }
        if (rc >= 0) {
          rc = avfilter_link(burnSrc, 0, scaleF, 0);
        }
        if (rc >= 0) {
          rc = avfilter_link(scaleF, 0, overlayF, 0);
        }
        if (rc >= 0) {
          rc = avfilter_link(overlaySrc, 0, overlayF, 1);
        }
        if (rc >= 0) {
          rc = avfilter_link(overlayF, 0, burnSink, 0);
        }
        if (rc < 0 || overlaySrc == nullptr) {
          result.error = Error::make(
              ErrorCode::ExecutionFailed,
              "cannot build the bitmap overlay graph: " + avErrorString(rc),
              "ffmpeg");
          return result;
        }
        if (avfilter_graph_config(cleanup.filterGraph, nullptr) < 0) {
          result.error = Error::make(ErrorCode::ExecutionFailed,
                                     "cannot configure the bitmap overlay "
                                     "graph",
                                     "ffmpeg");
          return result;
        }
      } else {
      // Graph description: crop -> rotation -> flips -> scale, then the
      // subtitles filter when burning in. Order matters: crop uses
      // source-frame coordinates; rotation swaps dimensions; the scale
      // lands on the encoder's (transform-aware) target geometry.
      std::string graphDesc;
      // HDR -> SDR (spec §9): the native colorspace filter converts the
      // source's gamut AND transfer function (e.g. BT.2020/PQ) to SDR
      // BT.709 with proper transfer math; specular highlights clip at SDR
      // white. It runs FIRST so it always sees the decoder's tagged
      // frames (later filters may not preserve color metadata).
      if (toneMap) {
        graphDesc += "colorspace=all=bt709:range=tv,";
      }
      if (wantsCrop) {
        const CropRect &crop = *request.video.crop;
        graphDesc += "crop=w=" + std::to_string(baseW) +
                     ":h=" + std::to_string(baseH) +
                     ":x=" + std::to_string(crop.x) +
                     ":y=" + std::to_string(crop.y) + ",";
      }
      switch (request.video.rotationDegrees) {
        case 90:
          graphDesc += "transpose=1,";
          break;
        case 180:
          graphDesc += "transpose=1,transpose=1,";
          break;
        case 270:
          graphDesc += "transpose=2,";
          break;
        default:
          break;
      }
      if (request.video.flipHorizontal) {
        graphDesc += "hflip,";
      }
      if (request.video.flipVertical) {
        graphDesc += "vflip,";
      }
      graphDesc +=
          "scale=w=" + std::to_string(outW) + ":h=" + std::to_string(outH);
      if (wantsBurnIn) {
        graphDesc += ",subtitles=filename='" + escapeFilterPath(subPath) +
                     "':fontsdir='" + escapeFilterPath(fontSource.dir) + "'";
        if (subOrdinal >= 0) {
          graphDesc += ":si=" + std::to_string(subOrdinal);
        }
      }
      // SRT-like sources render with libass's default style (family
      // "Arial"), which never matches the fontsdir contents on a provider-
      // less static build. Redirect the family to the discovered font so
      // text actually renders. ASS/SSA keep their authored styling.
      const bool subIsAss =
          wantsBurnIn &&
          (subGlobalIndex >= 0
               ? (cleanup.in->streams[subGlobalIndex]->codecpar->codec_id ==
                      AV_CODEC_ID_ASS ||
                  cleanup.in->streams[subGlobalIndex]->codecpar->codec_id ==
                      AV_CODEC_ID_SSA)
               : (subPath.size() > 4 &&
                  (subPath.compare(subPath.size() - 4, 4, ".ass") == 0 ||
                   subPath.compare(subPath.size() - 4, 4, ".ssa") == 0 ||
                   subPath.compare(subPath.size() - 4, 4, ".ASS") == 0 ||
                   subPath.compare(subPath.size() - 4, 4, ".SSA") == 0)));
      if (subIsAss == false && wantsBurnIn && !fontSource.family.empty()) {
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

  // ---- Audio setup (multi-track, spec §9) -----------------------------------
  // One pipeline per selected audio stream (selection happened above:
  // replacement audio / allTracks / explicit index / first). Stream-copy
  // lanes pass packets straight to the output stream; processing lanes
  // decode, run the DSP chain, resample and encode.
  const bool wantsAudioDspChain =
      request.audio.gainDb.has_value() ||
      request.audio.fadeInMs.has_value() ||
      request.audio.fadeOutMs.has_value() ||
      request.audio.normalizeLoudness ||
      request.audio.playbackSpeed.has_value() ||
      request.audio.removeSilence;
  for (const int aIn : audioStreams) {
    cleanup.audio.emplace_back();
    AudioTrack &track = cleanup.audio.back();
    if (aIn < 0) {
      // Replacement-input marker: its first audio stream.
      track.input = cleanup.audioReplaceIn;
      for (unsigned i = 0; i < cleanup.audioReplaceIn->nb_streams; ++i) {
        if (cleanup.audioReplaceIn->streams[i]->codecpar->codec_type ==
            AVMEDIA_TYPE_AUDIO) {
          track.inStream = int(i);
          break;
        }
      }
      if (track.inStream < 0) {
        result.error = Error::make(ErrorCode::InvalidArgument,
                                   "the replacement audio has no track",
                                   "ffmpeg");
        av_frame_free(&videoFrame);
        return result;
      }
    } else {
      track.input = cleanup.in;
      track.inStream = aIn;
    }
    const AVStream *inAudio = track.input->streams[std::size_t(track.inStream)];

    // Stream-copy lane (fixes the mixed transcode+audio-copy case: the
    // planner's "audio copy" promise is honored literally). Requirements
    // mirror the planner's canStreamCopyAudio.
    const bool copyable =
        pipeline.audioStreamCopy && !wantsAudioDspChain &&
        !request.audio.sampleRate.has_value() &&
        !request.audio.channels.has_value() &&
        !request.audio.bitRate.has_value() &&
        !request.audio.pcmBitDepth.has_value() &&
        inAudio->codecpar->codec_id == avCodecFor(pipeline.audioCodec);
    if (copyable) {
      track.copy = true;
      track.out = int(cleanup.out->nb_streams);
      AVStream *outAudio = avformat_new_stream(cleanup.out, nullptr);
      avcodec_parameters_copy(outAudio->codecpar, inAudio->codecpar);
      outAudio->codecpar->codec_tag = 0;
      outAudio->time_base = inAudio->time_base;
      av_dict_copy(&outAudio->metadata, inAudio->metadata, 0);
      outAudio->disposition = inAudio->disposition;
      continue;
    }

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
    track.dec = avcodec_alloc_context3(decCodec);
    avcodec_parameters_to_context(track.dec, inAudio->codecpar);
    if (avcodec_open2(track.dec, decCodec, nullptr) < 0) {
      result.error = Error::make(ErrorCode::ExecutionFailed,
                                 "cannot open audio decoder", "ffmpeg");
      av_frame_free(&videoFrame);
      return result;
    }

    const char *encName = encoderNameFor(pipeline.audioCodec);
    // PCM bit depth (spec §10): the user-selectable depth picks the exact
    // PCM encoder variant; lossless codecs ignore it.
    std::string pcmOverride;
    if (pipeline.audioCodec == AudioCodec::Pcm &&
        request.audio.pcmBitDepth.has_value()) {
      switch (*request.audio.pcmBitDepth) {
        case 24:
          pcmOverride = "pcm_s24le";
          break;
        case 32:
          pcmOverride = "pcm_s32le";
          break;
        default:
          break; // 16-bit stays the default pcm_s16le mapping
      }
      if (!pcmOverride.empty()) {
        encName = pcmOverride.c_str();
      }
    }
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
    track.enc = avcodec_alloc_context3(encCodec);
    track.enc->sample_rate =
        request.audio.sampleRate.value_or(track.dec->sample_rate);
    if (track.enc->sample_rate <= 0) {
      track.enc->sample_rate = 48000;
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
        if (candidate == track.enc->sample_rate) {
          supported = true;
          break;
        }
        const int distance =
            std::abs(candidate - track.enc->sample_rate);
        if (best == 0 || distance < bestDistance) {
          best = candidate;
          bestDistance = distance;
        }
      }
      if (!supported && best > 0) {
        track.enc->sample_rate = best;
      }
    }
    const std::int32_t channels = request.audio.channels.value_or(
        track.dec->ch_layout.nb_channels);
    av_channel_layout_default(&track.enc->ch_layout,
                              std::clamp(channels, 1, 8));
    if (encCodec->sample_fmts != nullptr) {
      // Prefer planar float when available (best supported by pinned encs).
      track.enc->sample_fmt = encCodec->sample_fmts[0];
      for (int i = 0; encCodec->sample_fmts[i] != -1; ++i) {
        if (encCodec->sample_fmts[i] == AV_SAMPLE_FMT_FLTP) {
          track.enc->sample_fmt = AV_SAMPLE_FMT_FLTP;
          break;
        }
      }
    } else {
      track.enc->sample_fmt = AV_SAMPLE_FMT_FLTP;
    }
    const std::int64_t defaultAudioBitrate =
        track.dec->bit_rate > 0 ? track.dec->bit_rate : 128000;
    track.enc->bit_rate =
        request.audio.bitRate.value_or(
            std::clamp<std::int64_t>(defaultAudioBitrate, 64000, 320000));
    if (cleanup.out->oformat->flags & AVFMT_GLOBALHEADER) {
      track.enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    if (avcodec_open2(track.enc, encCodec, nullptr) < 0) {
      result.error = Error::make(ErrorCode::ExecutionFailed,
                                 "cannot open audio encoder", "ffmpeg");
      av_frame_free(&videoFrame);
      return result;
    }
    track.out = int(cleanup.out->nb_streams);
    AVStream *outAudio = avformat_new_stream(cleanup.out, nullptr);
    avcodec_parameters_from_context(outAudio->codecpar, track.enc);
    outAudio->time_base = AVRational{1, track.enc->sample_rate};
    av_dict_copy(&outAudio->metadata, inAudio->metadata, 0);
    outAudio->disposition = inAudio->disposition;

    track.swr = swr_alloc();
    if (track.swr != nullptr &&
        swr_alloc_set_opts2(&track.swr,
                            &track.enc->ch_layout,
                            track.enc->sample_fmt,
                            track.enc->sample_rate,
                            &track.dec->ch_layout,
                            static_cast<AVSampleFormat>(
                                track.dec->sample_fmt),
                            track.dec->sample_rate > 0
                                ? track.dec->sample_rate
                                : 48000,
                            0, nullptr) < 0) {
      swr_free(&track.swr);
    }
    if (track.swr == nullptr || swr_init(track.swr) < 0) {
      result.error = Error::make(ErrorCode::ExecutionFailed,
                                 "cannot create audio resampler", "ffmpeg");
      av_frame_free(&videoFrame);
      return result;
    }
    // Encoder-side sample accumulator: frame-based encoders (AAC) require
    // EXACTLY frame_size samples per non-final frame, but the DSP graph
    // and the resampler emit arbitrary chunk sizes. The fifo absorbs the
    // mismatch; the encoder is fed only complete frames (plus the final
    // partial one at drain).
    track.fifo = av_audio_fifo_alloc(
        track.enc->sample_fmt,
        track.enc->ch_layout.nb_channels, 1);
    if (track.fifo == nullptr) {
      result.error = Error::make(ErrorCode::ExecutionFailed,
                                 "cannot create audio fifo", "ffmpeg");
      av_frame_free(&videoFrame);
      return result;
    }

    // Audio DSP chain (spec §10): gain -> fades -> loudness -> speed ->
    // silence removal, applied between decode and the resampler. The sink
    // is constrained to the DECODER's format/rate/layout so the existing
    // swr configuration stays valid regardless of what the chain does
    // internally (avfilter auto-inserts resampling where needed).
    if (wantsAudioDspChain) {
      const AVFilter *aSrc = avfilter_get_by_name("abuffer");
      const AVFilter *aSink = avfilter_get_by_name("abuffersink");
      track.graph = avfilter_graph_alloc();
      if (aSrc == nullptr || aSink == nullptr || track.graph == nullptr) {
        result.error = Error::make(ErrorCode::ExecutionFailed,
                                   "cannot allocate audio DSP graph",
                                   "ffmpeg");
        av_frame_free(&videoFrame);
        return result;
      }
      const int decRate = track.dec->sample_rate > 0
                              ? track.dec->sample_rate
                              : 48000;
      char layoutDesc[64];
      if (av_channel_layout_describe(&track.dec->ch_layout,
                                     layoutDesc, sizeof(layoutDesc)) < 0) {
        std::snprintf(layoutDesc, sizeof(layoutDesc), "stereo");
      }
      char aSrcArgs[256];
      std::snprintf(aSrcArgs, sizeof(aSrcArgs),
                    "time_base=1/%d:sample_rate=%d:sample_fmt=%s"
                    ":channel_layout=%s",
                    decRate, decRate,
                    av_get_sample_fmt_name(static_cast<AVSampleFormat>(
                        track.dec->sample_fmt)),
                    layoutDesc);
      const int srcRc = avfilter_graph_create_filter(
          &track.src, aSrc, "ain", aSrcArgs, nullptr, track.graph);
      const int sinkRc =
          srcRc >= 0
              ? avfilter_graph_create_filter(&track.sink, aSink, "aout",
                                             nullptr, nullptr,
                                             track.graph)
              : srcRc;
      if (srcRc < 0 || sinkRc < 0) {
        result.error = Error::make(
            ErrorCode::ExecutionFailed,
            "cannot create audio DSP endpoints (" + std::string(aSrcArgs) +
                ")",
            "ffmpeg");
        av_frame_free(&videoFrame);
        return result;
      }
      // Pin the sink to the decoder's negotiated space.
      const int sinkRates[] = {decRate, -1};
      av_opt_set_bin(track.sink, "sample_rates",
                     reinterpret_cast<const uint8_t *>(sinkRates),
                     sizeof(sinkRates) - sizeof(sinkRates[0]),
                     AV_OPT_SEARCH_CHILDREN);
      const enum AVSampleFormat sinkFmts[] = {
          static_cast<AVSampleFormat>(track.dec->sample_fmt),
          AV_SAMPLE_FMT_NONE};
      av_opt_set_bin(track.sink, "sample_fmts",
                     reinterpret_cast<const uint8_t *>(sinkFmts),
                     sizeof(sinkFmts) - sizeof(sinkFmts[0]),
                     AV_OPT_SEARCH_CHILDREN);
      av_opt_set(track.sink, "ch_layouts", layoutDesc,
                 AV_OPT_SEARCH_CHILDREN);

      std::string desc;
      if (request.audio.gainDb.has_value()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "volume=volume=%.2fdB:"
                                        "precision=double,",
                      *request.audio.gainDb);
        desc += buf;
      }
      // Total audio span for fade positioning (trim window or source).
      double spanSec = 0.0;
      if (track.input->duration != AV_NOPTS_VALUE &&
          track.input->duration > 0) {
        spanSec = double(track.input->duration) / double(AV_TIME_BASE);
      }
      // Window this range covers on the output timeline: segment-local for
      // segmented runs, the whole output for single-pass. Fades position
      // inside the window and fire only in the FIRST/LAST window (a
      // fade-in repeating every 10 s segment would be wrong output, not a
      // cosmetic issue).
      const double windowSec =
          rangeActive && rangeEndUs > rangeStartUs
              ? double(rangeEndUs - rangeStartUs) / 1000000.0
              : spanSec;
      const bool firstWindow =
          !rangeActive || rangeStartUs <= request.trim.startUs;
      const bool lastWindow =
          !rangeActive || request.trim.endUs < 0 ||
          rangeEndUs >= request.trim.endUs;
      if (request.audio.fadeInMs.has_value() && *request.audio.fadeInMs > 0 &&
          firstWindow) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "afade=t=in:st=0:d=%.3f,",
                      double(*request.audio.fadeInMs) / 1000.0);
        desc += buf;
      }
      if (request.audio.fadeOutMs.has_value() && *request.audio.fadeOutMs > 0 &&
          lastWindow && windowSec > 0.0) {
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "afade=t=out:st=%.3f:d=%.3f,",
                      std::max(0.0, windowSec -
                                       double(*request.audio.fadeOutMs) /
                                           1000.0),
                      double(*request.audio.fadeOutMs) / 1000.0);
        desc += buf;
      }
      if (request.audio.normalizeLoudness) {
        desc += "loudnorm=I=-16:TP=-1.5:LRA=11,";
      }
      if (request.audio.playbackSpeed.has_value() &&
          *request.audio.playbackSpeed > 0.0) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "atempo=%.4f,",
                      *request.audio.playbackSpeed);
        desc += buf;
      }
      if (request.audio.removeSilence) {
        // Remove mid-stream silences longer than 100 ms below -45 dB,
        // keeping 50 ms of each for natural transitions.
        desc += "silenceremove=stop_periods=-1:stop_duration=0.1:"
                "stop_threshold=-45dB:stop_silence=0.05,";
      }
      if (desc.empty()) {
        desc = "anull";
      } else {
        desc.pop_back(); // trailing comma
      }
      AVFilterInOut *aOuts = avfilter_inout_alloc();
      AVFilterInOut *aIns = avfilter_inout_alloc();
      bool graphOk = aOuts != nullptr && aIns != nullptr;
      if (graphOk) {
        // Entry names MUST be "in"/"out": libavfilter matches unlabeled
        // graph strings against these conventional labels (anything else
        // leaves the abuffer unwired and graph_config fails).
        aOuts->name = av_strdup("in");
        aOuts->filter_ctx = track.src;
        aOuts->pad_idx = 0;
        aOuts->next = nullptr;
        aIns->name = av_strdup("out");
        aIns->filter_ctx = track.sink;
        aIns->pad_idx = 0;
        aIns->next = nullptr;
        graphOk = avfilter_graph_parse_ptr(track.graph, desc.c_str(),
                                           &aIns, &aOuts, nullptr) >= 0;
      }
      avfilter_inout_free(&aIns);
      avfilter_inout_free(&aOuts);
      if (graphOk) {
        graphOk = avfilter_graph_config(track.graph, nullptr) >= 0;
      }
      if (!graphOk) {
        result.error = Error::make(
            ErrorCode::ExecutionFailed,
            "cannot initialize the audio DSP graph (" + desc + ")",
            "ffmpeg");
        av_frame_free(&videoFrame);
        return result;
      }
    }
  }

  if (videoOut < 0 && cleanup.audio.empty()) {
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

  // ---- Attached pictures / cover art (spec §10) ----------------------------
  // Attached pictures are metadata, not timeline content: preserved from
  // the source (or replaced by the external image). In segmented runs the
  // FIRST segment carries them (later segments skip; assembly yields
  // exactly one copy).
  std::vector<std::pair<int, int>> picInOut; // input idx -> output idx
  const bool picsIncluded = !isSegmentRun || rangeStartUs <= 0;
  if (request.audio.preserveCoverArt && picsIncluded) {
    for (unsigned i = 0; i < cleanup.in->nb_streams; ++i) {
      const AVStream *st = cleanup.in->streams[i];
      if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
          (st->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0) {
        AVStream *outPic = avformat_new_stream(cleanup.out, nullptr);
        if (outPic == nullptr) {
          continue;
        }
        avcodec_parameters_copy(outPic->codecpar, st->codecpar);
        outPic->codecpar->codec_tag = 0;
        av_dict_copy(&outPic->metadata, st->metadata, 0);
        outPic->disposition = st->disposition;
        picInOut.emplace_back(int(i), int(cleanup.out->nb_streams) - 1);
      }
    }
  }
  if (request.audio.coverArtUri.has_value() && picsIncluded) {
    const std::optional<std::string> coverPath =
        localPathForUri(*request.audio.coverArtUri);
    if (!coverPath.has_value()) {
      result.error = Error::make(ErrorCode::SourceUnavailable,
                                 "cover art URI is not a local path",
                                 "ffmpeg");
      av_frame_free(&videoFrame);
      return result;
    }
    std::string coverErr;
    if (addCoverArtStream(cleanup.out, *coverPath, coverErr) == nullptr) {
      result.error =
          Error::make(ErrorCode::InvalidArgument,
                      "cannot embed cover art: " + coverErr, "ffmpeg");
      av_frame_free(&videoFrame);
      return result;
    }
  }

  // Metadata preservation (spec video ops: "metadata preservation where
  // possible"): container-level and per-stream tags copy through the
  // transcode. Re-encoding changes the essence, never the descriptive
  // metadata (title/artist/creation date...). Encoder-written entries
  // (extradata-adjacent tags) do not exist yet at header time.
  // stripMetadata (advanced control) drops them instead.
  if (!request.stripMetadata) {
    av_dict_copy(&cleanup.out->metadata, cleanup.in->metadata,
                 AV_DICT_DONT_OVERWRITE);
  }
  {
    if (videoIn >= 0 && videoOut >= 0) {
      av_dict_copy(&cleanup.out->streams[std::size_t(videoOut)]->metadata,
                   cleanup.in->streams[std::size_t(videoIn)]->metadata,
                   AV_DICT_DONT_OVERWRITE);
    }
    // Audio tracks copied their metadata at stream creation (copy lanes
    // and processing lanes both call av_dict_copy on the source stream).
    // Preserved subtitle streams copy their metadata at creation too.
    // Replacement audio metadata travels with the replacement input's
    // stream, already copied during setup.
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

  // Frame-rate conversion (spec §9): resamples the source timeline onto a
  // constant output grid. A source frame covering N output slots is sent N
  // times (duplication); source frames not covering any free slot are
  // dropped. The grid cursor is segment-local microseconds and snaps
  // forward across gaps (e.g. after the segment seek overshoot).
  const auto emitFpsConverted = [&](AVFrame *f, std::int64_t startLocalUs,
                                    std::int64_t durUs) {
    if (fpsNextOutUs < double(startLocalUs) - 0.5) {
      fpsNextOutUs = std::ceil(std::max<double>(double(startLocalUs), 0.0) /
                               outFrameDurUs) *
                     outFrameDurUs;
    }
    const double spanEnd =
        double(startLocalUs) +
        double(durUs > 0 ? durUs : std::int64_t(srcFrameDurUs));
    bool emitted = false;
    for (double t = fpsNextOutUs; t < spanEnd - 0.5; t += outFrameDurUs) {
      f->pts = av_rescale_q(std::int64_t(std::llround(t)),
                            AVRational{1, 1000000},
                            cleanup.videoEnc->time_base);
      sendVideoFrameToEncoder(f);
      fpsNextOutUs = t + outFrameDurUs;
      emitted = true;
    }
    return emitted;
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
  const auto drainAudioEncoder = [&](AudioTrack &track) {
    if (track.enc == nullptr) {
      return;
    }
    avcodec_send_frame(track.enc, nullptr);
    AVPacket *encPacket = av_packet_alloc();
    while (avcodec_receive_packet(track.enc, encPacket) == 0) {
      encPacket->stream_index = track.out;
      av_packet_rescale_ts(encPacket, track.enc->time_base,
                           cleanup.out->streams[std::size_t(track.out)]->time_base);
      if (av_interleaved_write_frame(cleanup.out, encPacket) < 0) {
        writeFailed = true;
      }
      av_packet_unref(encPacket);
    }
    av_packet_free(&encPacket);
  };

  bool rangeDone = false; // crossed the range end; stop reading input
  // Bitmap burn-in event timeline: (timeUs, frame) pairs pushed into the
  // overlay input as the video timeline advances. A transparent pre-roll
  // at t=0 (and after every cue end) keeps framesync flowing.
  struct OverlayEvent {
    std::int64_t us = 0;
    bool isEnd = false; // end events sort before starts at equal times
    AVFrame *frame = nullptr;
  };
  std::vector<OverlayEvent> overlayEvents;
  std::size_t overlayCursor = 0;
  if (overlaySrc != nullptr) {
    AVFrame *transparent = av_frame_alloc();
    transparent->format = AV_PIX_FMT_YUVA420P;
    transparent->width = outW;
    transparent->height = outH;
    if (av_frame_get_buffer(transparent, 0) >= 0) {
      for (int p = 0; p < 4; ++p) {
        const int planeH = p == 1 || p == 2 ? (outH + 1) / 2 : outH;
        if (transparent->data[p] != nullptr) {
          std::memset(transparent->data[p], 0,
                      std::size_t(transparent->linesize[p]) *
                          std::size_t(planeH));
        }
      }
      overlayEvents.push_back(OverlayEvent{0, true, transparent});
      overlayFrameGuard.frames.push_back(transparent);
    } else {
      av_frame_free(&transparent);
      result.error = Error::make(ErrorCode::ExecutionFailed,
                                 "cannot allocate the transparent overlay",
                                 "ffmpeg");
      av_frame_free(&videoFrame);
      return result;
    }
    for (const BitmapCue &cue : bitmapCues) {
      overlayEvents.push_back(
          OverlayEvent{cue.startUs, false, cue.overlay});
      overlayEvents.push_back(OverlayEvent{cue.endUs, true, transparent});
    }
    std::stable_sort(overlayEvents.begin(), overlayEvents.end(),
                     [](const OverlayEvent &a, const OverlayEvent &b) {
                       if (a.us != b.us) {
                         return a.us < b.us;
                       }
                       return a.isEnd && !b.isEnd;
                     });
    // Far-future transparent sentinels: framesync only emits a main frame
    // once the overlay input carries frames BEYOND it (empirically TWO
    // ahead). Real events end at the last cue; without sentinels the
    // video tail (and any sparse-cue timeline) would stall until the
    // overlay EOF and then flush UNBLENDED. Transparent sentinels never
    // change the visible result; they only guarantee the scheduler can
    // always look ahead. They sort last, preserving push order.
    overlayEvents.push_back(
        OverlayEvent{3600LL * 1000000LL, true, transparent});
    overlayEvents.push_back(
        OverlayEvent{7200LL * 1000000LL, true, transparent});
  }
  const auto pushOverlayEventsUpTo = [&](std::int64_t upToUs) {
    if (overlaySrc == nullptr) {
      return;
    }
    // framesync invariant (established empirically with standalone graph
    // probes): a main frame at T is only emitted once the overlay input
    // carries frames BEYOND T - effectively two ahead. Starved, it
    // buffers every main frame and eof_action=pass then flushes them
    // UNBLENDED. Keep two pushed overlay events ahead of the main
    // timeline: push all events <= T, then two more (the far-future
    // sentinels guarantee the supply never runs dry).
    std::int64_t lastPushedUs = INT64_MIN;
    std::int64_t secondLastPushedUs = INT64_MIN;
    while (overlayCursor < overlayEvents.size() &&
           (overlayEvents[overlayCursor].us <= upToUs ||
            lastPushedUs <= upToUs || secondLastPushedUs <= upToUs)) {
      const OverlayEvent &event = overlayEvents[overlayCursor];
      // av_frame_clone, NOT a struct copy: pushing a shallow stack copy
      // fails inside av_frame_ref with ENOMEM (-12) — found via a
      // standalone baseline in the same process. The clone shares the
      // cached frame's buffers; KEEP_REF makes buffersrc take its own
      // reference, so the clone is freed immediately after the push.
      AVFrame *push = av_frame_clone(event.frame);
      if (push != nullptr) {
        push->pts =
            av_rescale_q(event.us, AVRational{1, 1000000}, burnGraphTb);
        av_buffersrc_add_frame_flags(overlaySrc, push,
                                     AV_BUFFERSRC_FLAG_KEEP_REF);
        av_frame_free(&push);
      }
      secondLastPushedUs = lastPushedUs;
      lastPushedUs = event.us;
      ++overlayCursor;
    }
  };
  // Feeds the encoder from the fifo: only complete frame_size frames (the
  // final drain allows a partial tail). PCM-style encoders (frame_size 0)
  // consume whatever is available.
  const auto feedEncoderFromFifo = [&](AudioTrack &track, bool flush) {
    if (track.fifo == nullptr || track.enc == nullptr) {
      return;
    }
    const int frameSize = track.enc->frame_size;
    while (true) {
      const int have = av_audio_fifo_size(track.fifo);
      int want = frameSize > 0 ? frameSize : have;
      if (want <= 0) {
        return;
      }
      if (have < want) {
        if (!flush || have == 0) {
          return; // wait for more input (or nothing left to flush)
        }
        want = have; // final partial frame
      }
      AVFrame *audioFrame = av_frame_alloc();
      audioFrame->format = track.enc->sample_fmt;
      audioFrame->sample_rate = track.enc->sample_rate;
      av_channel_layout_copy(&audioFrame->ch_layout,
                             &track.enc->ch_layout);
      audioFrame->nb_samples = want;
      if (av_frame_get_buffer(audioFrame, 0) < 0 ||
          av_audio_fifo_read(track.fifo,
                             reinterpret_cast<void **>(audioFrame->data),
                             want) < want) {
        av_frame_free(&audioFrame);
        return;
      }
      // Encoder time_base is 1/sample_rate: pts in output samples.
      audioFrame->pts = track.samplesOut;
      track.samplesOut += want;
      if (avcodec_send_frame(track.enc, audioFrame) == 0) {
        AVPacket *encPacket = av_packet_alloc();
        while (avcodec_receive_packet(track.enc, encPacket) == 0) {
          encPacket->stream_index = track.out;
          av_packet_rescale_ts(encPacket, track.enc->time_base,
                               cleanup.out->streams[std::size_t(track.out)]->time_base);
          if (av_interleaved_write_frame(cleanup.out, encPacket) < 0) {
            writeFailed = true;
          }
          av_packet_unref(encPacket);
        }
        av_packet_free(&encPacket);
      }
      av_frame_free(&audioFrame);
      if (frameSize <= 0 || (flush && av_audio_fifo_size(track.fifo) == 0)) {
        return;
      }
    }
  };
  // Resamples one decoded (possibly DSP-filtered) frame into the fifo,
  // then feeds the encoder whatever complete frames accumulated.
  const auto encodeAudioFrame = [&](AudioTrack &track, AVFrame *srcFrame) {
    if (track.swr == nullptr || track.fifo == nullptr) {
      return;
    }
    // Output capacity: the resampler's worst case for this input size.
    const int maxOut =
        swr_get_out_samples(track.swr, srcFrame->nb_samples);
    if (maxOut <= 0) {
      return;
    }
    uint8_t **buffer = nullptr;
    if (av_samples_alloc_array_and_samples(
            &buffer, nullptr, track.enc->ch_layout.nb_channels,
            maxOut, track.enc->sample_fmt, 0) < 0 ||
        buffer == nullptr) {
      return;
    }
    const int converted = swr_convert(
        track.swr, buffer, maxOut,
        const_cast<const uint8_t **>(srcFrame->data),
        srcFrame->nb_samples);
    if (converted > 0) {
      if (av_audio_fifo_write(
              track.fifo, reinterpret_cast<void **>(buffer),
              converted) < converted) {
        writeFailed = true;
      }
    }
    if (buffer != nullptr) {
      av_freep(&buffer[0]);
      av_freep(&buffer);
    }
    feedEncoderFromFifo(track, false);
  };
  // Pushes one decoded audio frame through the DSP graph when active and
  // encodes every pulled output frame (the graph may emit multiple frames
  // per input or none, e.g. after silence removal).
  const auto processAudioFrame = [&](AudioTrack &track, AVFrame *decoded) {
    if (track.src != nullptr && track.sink != nullptr) {
      if (av_buffersrc_add_frame_flags(track.src, decoded,
                                       AV_BUFFERSRC_FLAG_KEEP_REF) >= 0) {
        AVFrame *dsp = av_frame_alloc();
        while (av_buffersink_get_frame(track.sink, dsp) >= 0) {
          encodeAudioFrame(track, dsp);
          av_frame_unref(dsp);
        }
        av_frame_free(&dsp);
      }
    } else {
      encodeAudioFrame(track, decoded);
    }
  };
  // Flushes the DSP graph's tail (atempo/loudnorm/silenceremove hold
  // buffered samples) and encodes everything it emits.
  const auto flushAudioGraph = [&](AudioTrack &track) {
    if (track.src == nullptr || track.sink == nullptr) {
      return;
    }
    if (av_buffersrc_add_frame(track.src, nullptr) >= 0) {
      AVFrame *dsp = av_frame_alloc();
      while (av_buffersink_get_frame(track.sink, dsp) >= 0) {
        encodeAudioFrame(track, dsp);
        av_frame_unref(dsp);
      }
      av_frame_free(&dsp);
    }
  };
  // ---- Replacement-audio pump (spec §9 replace audio) ----------------------
  // Feeds replacement packets interleaved with the main timeline so the
  // muxer sees monotonically increasing dts. av_read_frame cannot unread,
  // so the one packet read too far ahead is held and consumed first on
  // the next pump.
  AudioTrack *replaceTrackOfInput = nullptr;
  for (AudioTrack &track : cleanup.audio) {
    if (track.input == cleanup.audioReplaceIn &&
        cleanup.audioReplaceIn != nullptr) {
      replaceTrackOfInput = &track;
      break;
    }
  }
  AVPacket *heldReplacePkt = nullptr;
  std::int64_t heldReplaceUs = -1;
  const auto feedReplacementPacket = [&](AVPacket *pkt) {
    AudioTrack &track = *replaceTrackOfInput;
    if (track.copy) {
      AVPacket *outPacket = av_packet_alloc();
      av_packet_move_ref(outPacket, pkt);
      const AVStream *srcStream =
          cleanup.audioReplaceIn->streams[std::size_t(outPacket->stream_index)];
      const AVRounding rounding = static_cast<AVRounding>(
          AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
      outPacket->pts = av_rescale_q_rnd(
          outPacket->pts, srcStream->time_base,
          cleanup.out->streams[std::size_t(track.out)]->time_base, rounding);
      outPacket->dts = av_rescale_q_rnd(
          outPacket->dts, srcStream->time_base,
          cleanup.out->streams[std::size_t(track.out)]->time_base, rounding);
      outPacket->duration = av_rescale_q(
          outPacket->duration, srcStream->time_base,
          cleanup.out->streams[std::size_t(track.out)]->time_base);
      outPacket->pos = -1;
      outPacket->stream_index = track.out;
      if (av_interleaved_write_frame(cleanup.out, outPacket) < 0) {
        writeFailed = true;
      }
      av_packet_free(&outPacket);
    } else if (track.dec != nullptr &&
               avcodec_send_packet(track.dec, pkt) == 0) {
      while (avcodec_receive_frame(track.dec, cleanup.frame) == 0) {
        if (rangeBounded) {
          const std::int64_t repUs = frameUs(cleanup.frame, track.dec);
          if (repUs >= 0 && repUs < rangeStartUs) {
            av_frame_unref(cleanup.frame);
            continue;
          }
          if (repUs >= rangeEndUs) {
            av_frame_unref(cleanup.frame);
            break;
          }
        }
        processAudioFrame(track, cleanup.frame);
        av_frame_unref(cleanup.frame);
      }
    }
  };
  const auto pumpReplacementUpTo = [&](std::int64_t upToUs) {
    if (cleanup.audioReplaceIn == nullptr || replaceTrackOfInput == nullptr ||
        writeFailed) {
      return;
    }
    // Range gate: nothing beyond the segment/window end.
    const std::int64_t limitUs =
        rangeBounded ? std::min(upToUs, rangeEndUs) : upToUs;
    for (;;) {
      std::int64_t nextUs = -1;
      if (heldReplacePkt != nullptr) {
        nextUs = heldReplaceUs;
      }
      if (nextUs >= 0 && nextUs > limitUs && limitUs != INT64_MAX) {
        return; // still too far ahead; keep holding
      }
      AVPacket *pkt = heldReplacePkt;
      if (pkt == nullptr) {
        pkt = av_packet_alloc();
        if (av_read_frame(cleanup.audioReplaceIn, pkt) < 0) {
          av_packet_free(&pkt);
          return; // replacement EOF
        }
        if (pkt->stream_index != replaceTrackOfInput->inStream) {
          av_packet_unref(pkt);
          av_packet_free(&pkt);
          continue; // not the selected replacement track
        }
        const AVStream *repStream = cleanup.audioReplaceIn->streams[
            std::size_t(pkt->stream_index)];
        heldReplaceUs =
            pkt->pts == AV_NOPTS_VALUE
                ? -1
                : av_rescale_q(pkt->pts, repStream->time_base,
                               AVRational{1, 1000000});
      }
      if (limitUs != INT64_MAX && heldReplaceUs > limitUs) {
        heldReplacePkt = pkt; // hold for the next pump
        return;
      }
      feedReplacementPacket(pkt);
      av_packet_free(&pkt);
      heldReplacePkt = nullptr;
      heldReplaceUs = -1;
      if (writeFailed) {
        return;
      }
    }
  };
  while (!writeFailed && !rangeDone &&
         av_read_frame(cleanup.in, cleanup.packet) >= 0) {
    if (params.cancel && params.cancel->isCancelled()) {
      cancelled = true;
      av_packet_unref(cleanup.packet);
      break;
    }
    const bool isVideo = cleanup.packet->stream_index == videoIn;
    // Multi-track dispatch: which selected audio track (if any) owns this
    // packet's stream index.
    AudioTrack *audioTrack = nullptr;
    for (AudioTrack &track : cleanup.audio) {
      if (track.input == cleanup.in &&
          track.inStream == cleanup.packet->stream_index) {
        audioTrack = &track;
        break;
      }
    }
    const bool isAudio = audioTrack != nullptr;
    // Range gate on the demux side: packets at/after the end stop the run
    // (their content belongs to the next segment). Packets BEFORE the
    // start are NOT skipped here: after the backward seek they begin at
    // the keyframe, and the decoder must see that keyframe to establish
    // references — dropping them at the demux level makes every segment
    // that does not start exactly on a keyframe decode zero/garbled
    // frames. The decoded frames before the boundary are discarded by the
    // frame-level gates below (they exist for exactly this).
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
            // Burn-in path: the graph scales, renders subtitles (libass for
            // text; the overlay filter for bitmap cues) and converts to the
            // encoder's pixel format. Frame pts are normalized into the time
            // base the buffersrc was declared with.
            if (overlaySrc != nullptr) {
              // Advance the bitmap overlay timeline to this frame first:
              // events must precede the main frame they apply to.
              std::int64_t localUs = srcUs;
              if (rangeBounded && srcUs >= 0) {
                localUs = std::max<std::int64_t>(0, srcUs - rangeStartUs);
              }
              pushOverlayEventsUpTo(localUs);
            }
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
                if (fpsConvert) {
                  // Frame-rate conversion composes with burn-in: grid-emit
                  // on the segment-local timeline.
                  std::int64_t filtUs = std::int64_t(
                      std::llround(fpsNextOutUs)); // NOPTS fallback: continue
                  if (filt->pts != AV_NOPTS_VALUE) {
                    const std::int64_t graphUs = av_rescale_q(
                        filt->pts, av_buffersink_get_time_base(burnSink),
                        AVRational{1, 1000000});
                    if (graphUs != AV_NOPTS_VALUE) {
                      filtUs = rangeBounded
                                   ? std::max<std::int64_t>(
                                         0, graphUs - rangeStartUs)
                                   : graphUs;
                    }
                  }
                  std::int64_t filtDurUs = std::int64_t(srcFrameDurUs);
                  if (filt->duration > 0) {
                    filtDurUs = av_rescale_q(
                        filt->duration, av_buffersink_get_time_base(burnSink),
                        AVRational{1, 1000000});
                  }
                  emitFpsConverted(filt, filtUs, filtDurUs);
                } else if (rangeBounded && filt->pts != AV_NOPTS_VALUE) {
                  // Segment-local timeline: rebase into [0, rangeEnd-start)
                  // and convert to the encoder's time base in one step.
                  const std::int64_t outUs = av_rescale_q(
                      filt->pts, av_buffersink_get_time_base(burnSink),
                      AVRational{1, 1000000});
                  filt->pts = av_rescale_q(
                      std::max<std::int64_t>(0, outUs - rangeStartUs),
                      AVRational{1, 1000000},
                      cleanup.videoEnc->time_base);
                  sendVideoFrameToEncoder(filt);
                } else {
                  filt->pts = av_rescale_q_rnd(
                      filt->pts, av_buffersink_get_time_base(burnSink),
                      cleanup.videoEnc->time_base,
                      static_cast<AVRounding>(
                          AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
                  sendVideoFrameToEncoder(filt);
                }
                av_frame_unref(filt);
              }
              av_frame_free(&filt);
            }
          } else {
            av_frame_make_writable(videoFrame);
            sws_scale(cleanup.sws, cleanup.frame->data,
                      cleanup.frame->linesize, 0, cleanup.videoDec->height,
                      videoFrame->data, videoFrame->linesize);
            if (fpsConvert) {
              // Frame-rate conversion on the segment-local timeline.
              std::int64_t localUs = std::int64_t(std::llround(fpsNextOutUs));
              if (srcUs >= 0) {
                localUs = rangeBounded
                              ? std::max<std::int64_t>(0, srcUs - rangeStartUs)
                              : srcUs;
              }
              std::int64_t durUs = std::int64_t(srcFrameDurUs);
              if (cleanup.frame->duration > 0) {
                durUs = av_rescale_q(cleanup.frame->duration,
                                     cleanup.videoDec->time_base,
                                     AVRational{1, 1000000});
              }
              emitFpsConverted(videoFrame, localUs, durUs);
            } else if (rangeBounded &&
                       cleanup.frame->pts != AV_NOPTS_VALUE) {
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
              sendVideoFrameToEncoder(videoFrame);
            } else {
              videoFrame->pts = av_rescale_q_rnd(
                  cleanup.frame->pts, cleanup.videoDec->time_base,
                  cleanup.videoEnc->time_base,
                  static_cast<AVRounding>(
                      AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
              sendVideoFrameToEncoder(videoFrame);
            }
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
    } else if (isAudio && audioTrack->copy) {
      // Stream-copy lane: packets pass straight to the output stream.
      // Ownership follows the remux path's move_ref discipline.
      AVPacket *outPacket = av_packet_alloc();
      av_packet_move_ref(outPacket, cleanup.packet);
      const AVStream *srcStream =
          cleanup.in->streams[std::size_t(outPacket->stream_index)];
      const AVRounding rounding = static_cast<AVRounding>(
          AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
      outPacket->pts = av_rescale_q_rnd(
          outPacket->pts, srcStream->time_base,
          cleanup.out->streams[std::size_t(audioTrack->out)]->time_base,
          rounding);
      outPacket->dts = av_rescale_q_rnd(
          outPacket->dts, srcStream->time_base,
          cleanup.out->streams[std::size_t(audioTrack->out)]->time_base,
          rounding);
      outPacket->duration = av_rescale_q(
          outPacket->duration, srcStream->time_base,
          cleanup.out->streams[std::size_t(audioTrack->out)]->time_base);
      outPacket->pos = -1;
      outPacket->stream_index = audioTrack->out;
      if (av_interleaved_write_frame(cleanup.out, outPacket) < 0) {
        writeFailed = true;
      }
      av_packet_free(&outPacket);
    } else if (isAudio && audioTrack->dec != nullptr &&
               audioTrack->enc != nullptr) {
      if (avcodec_send_packet(audioTrack->dec, cleanup.packet) == 0) {
        while (avcodec_receive_frame(audioTrack->dec, cleanup.frame) == 0) {
          if (rangeBounded) {
            const std::int64_t audioUs =
                frameUs(cleanup.frame, audioTrack->dec);
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
          processAudioFrame(*audioTrack, cleanup.frame);
          av_frame_unref(cleanup.frame);
        }
      }
    } else if (!isVideo && !isAudio) {
      // Attached-picture packet (spec §10 cover art preservation): copied
      // to its mapped output stream.
      int picOut = -1;
      for (const auto &pair : picInOut) {
        if (pair.first == cleanup.packet->stream_index) {
          picOut = pair.second;
          break;
        }
      }
      if (picOut >= 0) {
        AVPacket *outPacket = av_packet_alloc();
        av_packet_move_ref(outPacket, cleanup.packet);
        const AVStream *srcStream =
            cleanup.in->streams[std::size_t(outPacket->stream_index)];
        const AVRounding rounding = static_cast<AVRounding>(
            AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
        outPacket->pts = av_rescale_q_rnd(
            outPacket->pts, srcStream->time_base,
            cleanup.out->streams[std::size_t(picOut)]->time_base, rounding);
        outPacket->dts = av_rescale_q_rnd(
            outPacket->dts, srcStream->time_base,
            cleanup.out->streams[std::size_t(picOut)]->time_base, rounding);
        outPacket->duration = av_rescale_q(
            outPacket->duration, srcStream->time_base,
            cleanup.out->streams[std::size_t(picOut)]->time_base);
        outPacket->pos = -1;
        outPacket->stream_index = picOut;
        if (av_interleaved_write_frame(cleanup.out, outPacket) < 0) {
          writeFailed = true;
        }
        av_packet_free(&outPacket);
        av_packet_unref(cleanup.packet);
        continue;
      }
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
    // Replacement audio pacing: keep the replacement input interleaved
    // with the main timeline (up to 1 s ahead), never a post-video batch —
    // the muxer needs monotonically increasing dts across streams.
    if (cleanup.audioReplaceIn != nullptr && !writeFailed) {
      const AVStream *mainStream =
          cleanup.in->streams[std::size_t(cleanup.packet->stream_index)];
      const std::int64_t mainUs =
          cleanup.packet->pts == AV_NOPTS_VALUE
              ? -1
              : av_rescale_q(cleanup.packet->pts, mainStream->time_base,
                             AVRational{1, 1000000});
      if (mainUs >= 0) {
        pumpReplacementUpTo(mainUs + 1000000);
      }
    }
    av_packet_unref(cleanup.packet);
  }

  // Replacement audio tail: after the main input ends, feed everything
  // remaining (bounded by the range when segmenting).
  pumpReplacementUpTo(INT64_MAX);
  av_packet_free(&heldReplacePkt);
  heldReplacePkt = nullptr;

  // Bitmap burn-in: flush the overlay input FIRST (eof_action=pass lets
  // remaining main frames through the overlay once its stream ends), then
  // the main graph input, then the audio DSP tails, then the resamplers.
  if (overlaySrc != nullptr) {
    av_buffersrc_add_frame(overlaySrc, nullptr);
  }
  drainVideoEncoder();
  for (AudioTrack &track : cleanup.audio) {
    if (track.copy) {
      continue;
    }
    // Flush the DSP graph's tail BEFORE the encoder drain: atempo/
    // loudnorm/silenceremove hold buffered samples that must still be
    // encoded.
    flushAudioGraph(track);
    // Drain the resampler's own delay into the fifo, then encode the
    // final (possibly partial) frame. Only after that may the encoder be
    // flushed.
    if (track.swr != nullptr && track.fifo != nullptr) {
      while (true) {
        const int estimate = swr_get_out_samples(track.swr, 0);
        if (estimate <= 0) {
          break;
        }
        uint8_t **buffer = nullptr;
        if (av_samples_alloc_array_and_samples(
                &buffer, nullptr, track.enc->ch_layout.nb_channels,
                estimate, track.enc->sample_fmt, 0) < 0 ||
            buffer == nullptr) {
          break;
        }
        const int converted =
            swr_convert(track.swr, buffer, estimate, nullptr, 0);
        if (converted > 0) {
          av_audio_fifo_write(track.fifo,
                              reinterpret_cast<void **>(buffer), converted);
        }
        av_freep(&buffer[0]);
        av_freep(&buffer);
        if (converted <= 0) {
          break;
        }
      }
    }
    feedEncoderFromFifo(track, true);
    drainAudioEncoder(track);
  }
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
    // The list carries absolute artifact paths; the concat demuxer's safe
    // mode (default on) rejects those with "Unsafe file name". The paths
    // are core-generated, not user input, so safe=0 is correct here.
    AVDictionary *openOpts = nullptr;
    av_dict_set(&openOpts, "safe", "0", 0);
    const int openRc =
        avformat_open_input(&in, listPath.string().c_str(), concatFormat,
                            &openOpts);
    av_dict_free(&openOpts);
    if (concatFormat == nullptr || openRc < 0 ||
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

// Adds an attached-picture stream for external cover art (spec §10).
// Decodes the image (mjpeg/png/bmp/...), converts to yuvj420p and encodes
// a single JPEG packet. Stream creation must happen before write_header;
// the packet write happens right after (writeCoverArtPacket).
AVStream *addCoverArtStream(AVFormatContext *out, const std::string &path,
                            std::string &err) {
  AVFormatContext *img = nullptr;
  if (avformat_open_input(&img, path.c_str(), nullptr, nullptr) != 0 ||
      avformat_find_stream_info(img, nullptr) < 0) {
    if (img != nullptr) {
      avformat_close_input(&img);
    }
    err = "cannot open the cover art image";
    return nullptr;
  }
  int imgStream = -1;
  for (unsigned i = 0; i < img->nb_streams; ++i) {
    if (img->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      imgStream = int(i);
      break;
    }
  }
  const AVCodec *imgDec = imgStream >= 0
                              ? avcodec_find_decoder(
                                    img->streams[std::size_t(imgStream)]
                                        ->codecpar->codec_id)
                              : nullptr;
  AVCodecContext *dec = nullptr;
  AVFrame *frame = nullptr;
  AVPacket *pkt = nullptr;
  SwsContext *sws = nullptr;
  AVStream *added = nullptr;
  do {
    if (imgStream < 0 || imgDec == nullptr) {
      err = "no decodable image stream in the cover art file";
      break;
    }
    dec = avcodec_alloc_context3(imgDec);
    avcodec_parameters_to_context(
        dec, img->streams[std::size_t(imgStream)]->codecpar);
    if (avcodec_open2(dec, imgDec, nullptr) < 0) {
      err = "cannot open the cover art decoder";
      break;
    }
    frame = av_frame_alloc();
    pkt = av_packet_alloc();
    bool got = false;
    while (!got && av_read_frame(img, pkt) >= 0) {
      if (pkt->stream_index == imgStream &&
          avcodec_send_packet(dec, pkt) == 0) {
        while (avcodec_receive_frame(dec, frame) == 0) {
          got = true;
          break;
        }
      }
      av_packet_unref(pkt);
    }
    if (!got || frame->width <= 0 || frame->height <= 0) {
      err = "the cover art image has no decodable frame";
      break;
    }
    const int w = frame->width & ~1;
    const int h = frame->height & ~1;
    const AVCodec *mjpeg = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (mjpeg == nullptr || w <= 0 || h <= 0) {
      err = "no mjpeg encoder for cover art";
      break;
    }
    AVCodecContext *enc = avcodec_alloc_context3(mjpeg);
    enc->width = w;
    enc->height = h;
    enc->pix_fmt = AV_PIX_FMT_YUVJ420P;
    enc->time_base = AVRational{1, 25};
    if (avcodec_open2(enc, mjpeg, nullptr) < 0) {
      avcodec_free_context(&enc);
      err = "cannot open the cover art encoder";
      break;
    }
    sws = sws_getContext(frame->width, frame->height,
                         static_cast<AVPixelFormat>(frame->format), w, h,
                         AV_PIX_FMT_YUVJ420P, SWS_BILINEAR, nullptr, nullptr,
                         nullptr);
    AVFrame *jpg = av_frame_alloc();
    jpg->format = AV_PIX_FMT_YUVJ420P;
    jpg->width = w;
    jpg->height = h;
    bool okPath = sws != nullptr && av_frame_get_buffer(jpg, 0) >= 0;
    if (okPath) {
      sws_scale(sws, frame->data, frame->linesize, 0, frame->height,
                jpg->data, jpg->linesize);
      jpg->pts = 0;
      okPath = avcodec_send_frame(enc, jpg) == 0;
    }
    if (okPath && avcodec_receive_packet(enc, pkt) >= 0 && pkt->size > 0) {
      added = avformat_new_stream(out, nullptr);
      if (added != nullptr) {
        added->id = int(out->nb_streams) - 1;
        added->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        added->codecpar->codec_id = AV_CODEC_ID_MJPEG;
        added->codecpar->width = w;
        added->codecpar->height = h;
        added->codecpar->format = AV_PIX_FMT_YUVJ420P;
        added->codecpar->bit_rate = 0;
        added->time_base = AVRational{1, 25};
        added->disposition |= AV_DISPOSITION_ATTACHED_PIC;
        // The single packet rides IN the codecpar extradata (the
        // convention for attached pictures): muxers write it at header
        // time, no separate packet write needed.
        added->codecpar->extradata_size = pkt->size;
        added->codecpar->extradata = static_cast<uint8_t *>(
            av_memdup(pkt->data, std::size_t(pkt->size)));
      }
    } else {
      err = "cannot encode the cover art";
    }
    av_frame_free(&jpg);
    avcodec_free_context(&enc);
  } while (false);
  sws_freeContext(sws);
  av_packet_free(&pkt);
  av_frame_free(&frame);
  avcodec_free_context(&dec);
  avformat_close_input(&img);
  return added;
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

  // Trim window (spec §15/§16/§20): the segment tiling covers
  // [trimStart, windowEnd) on the source timeline; checkpoint positions
  // stay in source-timeline microseconds (the durable coordinate system
  // D-017 established).
  const std::int64_t trimStart =
      params.request.trim.active()
          ? std::min<std::int64_t>(
                std::max<std::int64_t>(params.request.trim.startUs, 0), totalUs)
          : 0;
  const std::int64_t windowEnd =
      params.request.trim.endUs >= 0
          ? std::min(params.request.trim.endUs, totalUs)
          : totalUs;
  const std::int64_t windowLen =
      std::max<std::int64_t>(windowEnd - trimStart, 0);
  if (windowLen <= 0) {
    result.error = Error::make(ErrorCode::InvalidArgument,
                               "trim window is outside the source", "ffmpeg");
    return result;
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
    std::int64_t expectedStart = trimStart;
    for (const auto &info : sorted) {
      if (info.position <= expectedStart || info.position > totalUs) {
        continue; // overlapping or out-of-range record
      }
      if (info.position - expectedStart > 2 * kSegmentTargetUs) {
        break; // suspicious gap: the tiling is not contiguous, redo it
      }
      if (!segmentArtifactUsable(info.artifactPath)) {
        break; // prefix ends here; redo the rest
      }
      committed.push_back({info.artifactPath, info.position});
      expectedStart = info.position;
    }
  }
  std::int64_t resumeUs =
      committed.empty() ? trimStart : committed.back().endUs;
  if (resumeUs >= windowEnd) {
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
  while (segStart < windowEnd) {
    if (params.cancel && params.cancel->isCancelled()) {
      result.error = Error::make(ErrorCode::Cancelled,
                                 "transcode cancelled (segments committed)",
                                 "ffmpeg");
      return result;
    }
    const std::int64_t segEnd = std::min(segStart + kSegmentTargetUs, windowEnd);
    const std::string segPath =
        (segDir / ("seg-" + std::to_string(nextIndex) + outFs.extension().string()))
            .string();
    // Per-segment execution: the range run writes ITS OWN artifact file
    // (validated + committed below); the final output only appears at
    // assembly. Without this redirect every segment would overwrite the
    // final output and the artifact validation would fail on a file that
    // was never written.
    ExecuteParams segParams = params;
    segParams.outputPath = segPath;
    // Per-segment progress rebasing: local fraction -> whole-job fraction.
    segParams.onProgress =
        [params, segStart, segEnd, trimStart,
         windowLen](const backend::ProgressReport &report) {
          if (!params.onProgress || report.fraction < 0) {
            return;
          }
          ProgressReport global;
          const double local =
              std::clamp(report.fraction, 0.0, 1.0);
          global.processedDurationUs =
              segStart +
              std::int64_t(local * double(segEnd - segStart));
          global.totalDurationUs = windowLen;
          global.fraction =
              std::clamp(double(global.processedDurationUs - trimStart) /
                             double(windowLen),
                         0.0, 1.0);
          params.onProgress(global);
        };
    const ExecuteResult seg =
        executeTranscodeRange(segParams, segStart, segEnd, true, true);
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
        // A commit that lands while the token fires is a cancel, not a
        // durability failure; report it as such (the state machine
        // finalizes Canceled either way, the message keeps logs honest).
        const bool tokenFired =
            params.cancel && params.cancel->isCancelled();
        result.error = Error::make(
            tokenFired ? ErrorCode::Cancelled : ErrorCode::ExecutionFailed,
            tokenFired ? "transcode cancelled (segments committed)"
                       : "checkpoint commit failed",
            "ffmpeg");
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
  // libswscale/libswresample handle geometry and audio format changes; the
  // subtitle streams copy through containers and libass burns text in.
  caps.videoScaling = true;
  caps.timeTrim = true;
  caps.audioResampling = true;
  // The audio DSP chain (gain/fades/loudnorm/atempo/silenceremove) runs as
  // a libavfilter graph between decode and the resampler.
  caps.audioDsp = true;
  caps.subtitlePassthrough = true;
  caps.subtitleBurnIn = true;
  // Extraction: text formats copy to sidecars natively; container-internal
  // text (mov_text/ttml) decodes to SRT. Bitmap formats (PGS/DVB/VobSub)
  // need OCR and refuse honestly.
  caps.subtitleExtract = true;
  // Milestone 4: the FFmpeg stage workers decode to Yuv420P through
  // swscale and encode/mux from CPU frames.
  caps.stageDecodeToCpuFrames = true;
  caps.stageEncodeFromCpuFrames = true;
  // Advanced encoder controls (spec §18): profile, level, pixel format,
  // GOP and B-frame count are applied to the concrete encoder context.
  caps.videoEncoderTuning = true;
  // Multi-track audio: every selected source track decodes/encodes (or
  // copies) with its language metadata; explicit track selection too.
  caps.multiTrackAudio = true;
  // Audio replacement: an external audio file feeds the audio pipeline.
  caps.audioReplacement = true;
  // Cover art: attached pictures carry through; external images embed as
  // attached-picture streams.
  caps.coverArt = true;
  // HDR -> SDR tone mapping during transcode (gamut conversion + peak
  // compression through the native filter set; no zimg in the pinned
  // build, so linear-light precision is limited and documented).
  caps.hdrToneMapping = true;

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
