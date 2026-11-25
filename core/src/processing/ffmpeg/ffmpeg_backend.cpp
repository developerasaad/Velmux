#include "processing/ffmpeg/ffmpeg_backend.h"
#include "processing/ffmpeg/codec_mapping.h"
#include "subtitle_tools.h"
#include "ffmpeg_util.h"

#include "compression/compression.h"
#include "catalog/features.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sys/stat.h>
#include <system_error>
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
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace velmux::ffmpeg {

using namespace velmux::core;
using velmux::backend::CancelToken;
using velmux::backend::ExecuteParams;
using velmux::backend::ExecuteResult;
using velmux::backend::ProgressCallback;
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

// Applies metadata set/clear intentions at mux time (mandate §33):
// Set writes the familiar tag, Clear deletes it (av_dict_set with a null
// value removes the entry), Unchanged leaves the copied source tag alone.
// The all-Clear intention is the moral equivalent of the retired
// stripMetadata control.
void applyMetadataIntention(AVDictionary **dict,
                            const MetadataIntention &metadata) {
  const auto apply = [&](MetadataFieldAction action, const char *key,
                         const std::string &value) {
    switch (action) {
      case velmux::core::MetadataFieldAction::Unchanged:
        break;
      case velmux::core::MetadataFieldAction::Set:
        av_dict_set(dict, key, value.c_str(), 0);
        break;
      case velmux::core::MetadataFieldAction::Clear:
        av_dict_set(dict, key, nullptr, 0);
        break;
    }
  };
  apply(metadata.titleAction, "title", metadata.title);
  apply(metadata.artistAction, "artist", metadata.artist);
  apply(metadata.albumAction, "album", metadata.album);
  apply(metadata.genreAction, "genre", metadata.genre);
  apply(metadata.dateAction, "date", metadata.date);
  apply(metadata.descriptionAction, "description", metadata.description);
  apply(metadata.locationAction, "location", metadata.location);
}

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
  // Stream-copy lanes in RANGE runs rebase onto the window-local timeline;
  // these hold the first written packet's rescaled pts/dts (the rebase
  // origin, AV_NOPTS_VALUE until the first packet lands).
  std::int64_t copyBasePts = AV_NOPTS_VALUE;
  std::int64_t copyBaseDts = AV_NOPTS_VALUE;
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
    for (AVFormatContext *&ctx : externalAudioIns) {
      if (ctx != nullptr) {
        AVFormatContext *closing = ctx;
        avformat_close_input(&closing);
        ctx = nullptr;
      }
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
  // External audio addition inputs (mandate §23 additions), opened lazily.
  std::vector<AVFormatContext *> externalAudioIns;
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

// Formats a timestamp as WebVTT time (HH:MM:SS.mmm).
std::string vttTimestamp(std::int64_t us) {
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
  std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld.%03lld",
                static_cast<long long>(h), static_cast<long long>(m),
                static_cast<long long>(s), static_cast<long long>(ms));
  return buf;
}

// Formats a timestamp as ASS time (H:MM:SS.cc).
std::string assTimestamp(std::int64_t us) {
  if (us < 0) {
    us = 0;
  }
  const std::int64_t cs = (us + 5000) / 10000; // round to centiseconds
  const std::int64_t c = cs % 100;
  const std::int64_t sTotal = cs / 100;
  const std::int64_t s = sTotal % 60;
  const std::int64_t m = (sTotal / 60) % 60;
  const std::int64_t h = sTotal / 3600;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld.%02lld",
                static_cast<long long>(h), static_cast<long long>(m),
                static_cast<long long>(s), static_cast<long long>(c));
  return buf;
}

// The text sidecar representations the extraction path writes.
enum class SidecarTarget { Srt, Vtt, Ass };

// Writes one decoded cue in the target representation. Returns false when
// the stream is unwritable (disk/environment).
bool writeCue(std::ofstream &out, SidecarTarget target,
              std::int64_t cueIndex, std::int64_t startUs, std::int64_t endUs,
              const std::string &text) {
  switch (target) {
    case SidecarTarget::Srt:
      out << cueIndex << "\n"
          << srtTimestamp(startUs) << " --> " << srtTimestamp(endUs) << "\n"
          << text << "\n\n";
      break;
    case SidecarTarget::Vtt:
      // VTT cues may carry an identifier line; the running cue number is
      // a stable, honest one.
      out << cueIndex << "\n"
          << vttTimestamp(startUs) << " --> " << vttTimestamp(endUs) << "\n"
          << text << "\n\n";
      break;
    case SidecarTarget::Ass: {
      // ASS text: newlines become \N; braces would start an override tag
      // block, so escape them (the decode path already stripped tags).
      std::string assText;
      assText.reserve(text.size());
      for (char c : text) {
        if (c == '\n') {
          assText += "\\N";
        } else {
          if (c == '{' || c == '}') {
            assText += '\\';
          }
          assText += c;
        }
      }
      out << "Dialogue: 0," << assTimestamp(startUs) << ","
          << assTimestamp(endUs) << ",Default,,0,0,0,," << assText << "\n";
      break;
    }
  }
  return out.good();
}

// The ASS file header: minimal valid v4.00+ script with one default style
// (players render it with their default font when Arial is unavailable).
const char *kAssHeader =
    "[Script Info]\n"
    "ScriptType: v4.00+\n"
    "\n"
    "[V4+ Styles]\n"
    "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
    "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, "
    "ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, "
    "MarginL, MarginR, MarginV, Encoding\n"
    "Style: Default,Arial,16,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,"
    "0,0,0,0,100,100,0,0,1,1,0,2,10,10,10,1\n"
    "\n"
    "[Events]\n"
    "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, "
    "Effect, Text\n";

// Decodes a container-internal text subtitle stream (mov_text, ttml, ...) and
// writes a text sidecar (SRT, WebVTT or ASS per `target`). Bitmap decodes
// (PGS/DVB/VobSub) produce no text rects and fail honestly. Progress and
// cancellation are honored at packet granularity (demux-level work; there is
// nothing to checkpoint).
bool decodeSubtitleStreamToSidecar(AVFormatContext *in, int streamIndex,
                                   const std::string &outPath,
                                   SidecarTarget target,
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
  if (target == SidecarTarget::Vtt) {
    out << "WEBVTT\n\n";
  } else if (target == SidecarTarget::Ass) {
    out << kAssHeader;
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
        if (!writeCue(out, target, cueIndex, startUs, endUs, text)) {
          ok = false;
        }
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
// when a timing offset must be applied first, and by extraction jobs.
// On failure, writeRc carries the output-write AVERROR code when known
// (ENOSPC lets the caller surface the terminal StorageFull error).
bool extractSubtitleStreamToFile(const std::string &inPath, int streamIndex,
                                 const std::string &outPath,
                                 int &writeRc) {
  AVFormatContext *in = nullptr;
  AVFormatContext *out = nullptr;
  bool ok = false;
  do {
    int rc = avformat_open_input(&in, inPath.c_str(), nullptr, nullptr);
    if (rc < 0 || avformat_find_stream_info(in, nullptr) < 0) {
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
    if (!(out->oformat->flags & AVFMT_NOFILE)) {
      rc = avio_open(&out->pb, outPath.c_str(), AVIO_FLAG_WRITE);
      if (rc < 0) {
        writeRc = rc;
        break;
      }
    }
    rc = avformat_write_header(out, nullptr);
    if (rc < 0) {
      writeRc = rc;
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
      const int frameRc = av_interleaved_write_frame(out, pkt);
      if (frameRc < 0) {
        writeRc = frameRc;
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

// Forward declaration: the attached-picture helper is defined further
// down (near the other cover-art helpers) but used by the transcode and
// remux setup paths. Declared at velmux::ffmpeg scope, NOT inside the
// anonymous namespace above: a declaration inside it would bind callers
// to a different (anonymous-namespace) symbol that never gets defined —
// the definition further down is at this scope, and the two must match.
AVStream *addCoverArtStream(AVFormatContext *out, const std::string &path,
                            AVPacket *coverPacket, std::string &err);

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
        // Attached pictures are cover art — METADATA, not timeline
        // content (the executor preserves/embeds them per request, spec
        // §10). They must not appear as a video lane: a cover-only MP3 is
        // an audio source, and an output with video disabled plus embedded
        // cover art must validate as "no video stream" (the old behavior
        // failed such outputs with "Expected no video stream, but found
        // one").
        if (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) {
          ++out.attachedPictures;
          break;
        }
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
                                const JobRequest &request) const {
  for (const auto &assignment : pipeline.stages) {
    if (assignment.backend != BackendId::Ffmpeg) {
      return false;
    }
  }
  if (pipeline.stages.empty()) {
    return false;
  }
  // Conversion-family operations the execution paths serve: VideoConvert,
  // AudioConvert, Crop, Trim (the trim payload of video-cutmerge),
  // ExtractAudio and ExtractSubtitles — plus the timeline operations
  // (multi-range Cut, Split, Merge, ExtractFrames) this executor now
  // implements for real (keyframe-window copy, boundary strategies,
  // compatibility-verified merge, frame identity).
  switch (request.payload.index()) {
    case 0: // VideoConvert
    case 1: // AudioConvert
    case 2: // Crop
    case 3: // Trim
    case 4: // Cut (multi-range)
    case 5: // Split
    case 6: // Merge (sequential + spatial)
    case 7: // ExtractAudio
    case 8: // ExtractFrames
    case 9: // ExtractSubtitles
      return true;
    default:
      return false;
  }
}

ExecuteResult FfmpegExecutor::execute(const ExecuteParams &params) {
  // Subtitle-extraction jobs: demux + subtitle processing only, artifact
  // is a text sidecar.
  if (std::holds_alternative<job::ExtractSubtitles>(params.request.payload)) {
    return executeSubtitleExtract(params);
  }

  // Normalize the typed payload into the internal execution view. The
  // inspection feeding the builder is a demux-level open — the execution
  // paths open the input anyway; no decoding happens here.
  const MediaSource *sourceRef = std::visit(
      [](const auto &payload) -> const MediaSource * {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, job::Merge>) {
          return payload.sources.empty() ? nullptr : &payload.sources.front();
        } else {
          return &payload.source;
        }
      },
      params.request.payload);
  if (sourceRef == nullptr) {
    ExecuteResult result;
    result.error = Error::make(ErrorCode::InvalidArgument,
                               "the request carries no source", "ffmpeg");
    return result;
  }
  MediaInfo source;
  FfmpegInspector inspector;
  {
    const Error inspectError = inspector.inspect(*sourceRef, source);
    if (!inspectError.ok()) {
      ExecuteResult result;
      result.error = inspectError;
      return result;
    }
  }
  const Result<ExecutionIntent> built =
      buildIntent(params.request, params.pipeline, source,
                  params.sampleWindow);
  if (!built.ok()) {
    ExecuteResult result;
    result.error = built.error();
    return result;
  }
  const ExecutionIntent &intent = built.value();

  // Timeline operations (mandate §27/§29): dedicated executors consume the
  // intent plus their payload rules. They dispatch BEFORE the remux/
  // transcode split — the per-window strategy (keyframe-safe copy vs exact
  // transcode) is decided per range/part inside them, not by the planned
  // pipeline's copy flags.
  if (std::holds_alternative<job::Cut>(params.request.payload)) {
    return executeCut(params, intent);
  }
  if (std::holds_alternative<job::Split>(params.request.payload)) {
    return executeSplit(params, intent);
  }
  if (std::holds_alternative<job::ExtractFrames>(params.request.payload)) {
    return executeExtractFrames(params, intent);
  }
  if (std::holds_alternative<job::Merge>(params.request.payload)) {
    return executeMerge(params, intent);
  }

  if (params.pipeline.videoStreamCopy && params.pipeline.audioStreamCopy) {
    // Burn-in rewrites video pixels; a copy pipeline can never serve it
    // (the planner enforces this; the executor refuses defensively).
    if (intent.subtitles.burnIn) {
      ExecuteResult result;
      result.error = Error::make(
          ErrorCode::InvalidArgument,
          "internal: burn-in cannot run on a stream-copy pipeline", "ffmpeg");
      return result;
    }
    return executeRemux(params, intent);
  }
  return executeTranscode(params, intent);
}

ExecuteResult FfmpegExecutor::executeSubtitleExtract(
    const ExecuteParams &params) {
  ExecuteResult result;
  initFfmpegLabel();
  const auto &request =
      std::get<job::ExtractSubtitles>(params.request.payload);
  if (params.pipeline.sidecarExtension.empty()) {
    result.error = Error::make(
        ErrorCode::InvalidArgument,
        "extraction requests must carry a sidecar pipeline",
        "ffmpeg");
    return result;
  }
  // The planner lets exactly one track through in this build (multi-track
  // sidecar sets are refused there); the executor honors the selected
  // track or the first subtitle stream.
  std::int32_t wantedTrack = -1; // subtitle-stream ordinal
  if (!request.trackIndices.empty()) {
    wantedTrack = request.trackIndices.front();
  }
  // Per-track output representation: empty/Original keeps the source
  // representation when it stands alone (packet-copy sidecar); an explicit
  // Srt/Vtt/Ass target converts through the decode path.
  const SubtitleTextTarget target =
      request.targets.empty() ? SubtitleTextTarget::Original
                              : request.targets.front();
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
  // The payload's track indices are SUBTITLE-stream ordinals (the
  // inspected source's subtitles vector), not global stream indices.
  int subStream = -1;
  int subOrdinal = 0;
  for (unsigned i = 0; i < in->nb_streams; ++i) {
    if (in->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
      if (wantedTrack < 0 || subOrdinal == wantedTrack) {
        subStream = int(i);
        break;
      }
      ++subOrdinal;
    }
  }
  if (subStream < 0) {
    avformat_close_input(&in);
    result.error = Error::make(ErrorCode::InvalidArgument,
                               "the source has no subtitle track", "ffmpeg");
    return result;
  }
  const AVCodecID cid = in->streams[subStream]->codecpar->codec_id;

  // Copy path: Original target with a native text format whose packets ARE
  // the file format.
  bool ok = false;
  int sidecarWriteRc = 0;
  if (target == SubtitleTextTarget::Original &&
      subtitleFileExtensionFor(cid) != nullptr) {
    ok = extractSubtitleStreamToFile(*inPath, subStream, params.outputPath,
                                     sidecarWriteRc);
  } else {
    // Decode path: container-internal text (mov_text, ttml, ...) and
    // explicit text targets decode to AVSubtitle text rects, written in
    // the requested representation.
    const SidecarTarget sidecar = target == SubtitleTextTarget::Vtt
                                      ? SidecarTarget::Vtt
                                  : target == SubtitleTextTarget::Ass
                                      ? SidecarTarget::Ass
                                      : SidecarTarget::Srt;
    ok = decodeSubtitleStreamToSidecar(in, subStream, params.outputPath,
                                       sidecar, params);
  }
  avformat_close_input(&in);
  if (!ok) {
    // A full disk is environmental, not a codec limitation: surface the
    // terminal StorageFull error instead of the extraction message.
    if (sidecarWriteRc != 0) {
      result.error = outputWriteError(sidecarWriteRc,
                                      "cannot write the subtitle sidecar",
                                      params.outputPath);
    } else {
      result.error = Error::make(
          ErrorCode::UnsupportedOperation,
          std::string("cannot extract subtitle codec ") +
              avcodec_get_name(cid) +
              " (bitmap formats need OCR, which is outside V1)",
          "ffmpeg");
    }
    std::error_code ec;
    std::filesystem::remove(params.outputPath, ec);
    return result;
  }
  result.success = true;
  result.framesProcessed = 1;
  return result;
}

ExecuteResult FfmpegExecutor::executeRemux(const ExecuteParams &params,
                                         const ExecutionIntent &intent) {
  ExecuteResult result;
  initFfmpegLabel();
  const std::optional<std::string> inPath = localPathFor(intent.source);
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
  // External cover art (spec §10): the stream is created before the
  // header, its encoded packet written right after it.
  AVPacket *coverPkt = nullptr;
  int coverOutIndex = -1;
  // Track selection (mandate §23): the intent's ordinals name the audio
  // lanes that survive. Ordinals count INSPECTED audio tracks (the
  // inspector skips streams whose codec is outside the catalog), matching
  // how the request model identifies tracks.
  int audioOrdinal = 0;
  for (unsigned i = 0; i < cleanup.in->nb_streams; ++i) {
    const AVStream *inStream = cleanup.in->streams[i];
    const AVMediaType type = inStream->codecpar->codec_type;
    if (type == AVMEDIA_TYPE_AUDIO) {
      if (audioCodecFromAv(inStream->codecpar->codec_id).has_value()) {
        ++audioOrdinal;
      } else {
        continue; // not an inspected lane; never a selection target
      }
    }
    const bool keepVideo = type == AVMEDIA_TYPE_VIDEO && intent.wantsVideo;
    bool keepAudio = false;
    if (type == AVMEDIA_TYPE_AUDIO && intent.wantsAudio) {
      if (intent.audioSourceOrdinals.empty() &&
          !intent.replaceAudioUri.empty()) {
        keepAudio = false; // replaced wholesale (transcode path serves it)
      } else {
        keepAudio = std::find(intent.audioSourceOrdinals.begin(),
                              intent.audioSourceOrdinals.end(),
                              audioOrdinal - 1) !=
                    intent.audioSourceOrdinals.end();
      }
    }
    const bool keepSub = type == AVMEDIA_TYPE_SUBTITLE &&
                         intent.subtitles.keepSubtitles;
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

  // Metadata (mandate §33): container-level + per-stream tags copy
  // through the remux unchanged, then the familiar-field set/clear
  // intentions apply on the container dictionary (the moral equivalent
  // of the retired stripMetadata control is the all-Clear intention).
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
  applyMetadataIntention(&cleanup.out->metadata, intent.metadata);

  // External cover art (spec §10): attached before the header (the
  // single packet rides the codecpar extradata).
  if (intent.coverArtUri.has_value()) {
    const std::optional<std::string> coverPath =
        localPathForUri(*intent.coverArtUri);
    if (!coverPath.has_value()) {
      result.error = Error::make(ErrorCode::SourceUnavailable,
                                 "cover art URI is not a local path",
                                 "ffmpeg");
      return result;
    }
    std::string coverErr;
    AVPacket *coverPacket = av_packet_alloc();
    AVStream *coverStream =
        addCoverArtStream(cleanup.out, *coverPath, coverPacket, coverErr);
    if (coverStream == nullptr) {
      av_packet_free(&coverPacket);
      result.error =
          Error::make(ErrorCode::InvalidArgument,
                      "cannot embed cover art: " + coverErr, "ffmpeg");
      return result;
    }
    coverOutIndex = coverStream->index;
    coverPkt = coverPacket; // written right after the header below
  }
  if (!(cleanup.out->oformat->flags & AVFMT_NOFILE)) {
    rc = avio_open(&cleanup.out->pb, params.outputPath.c_str(),
                   AVIO_FLAG_WRITE);
    if (rc < 0) {
      result.error = outputWriteError(rc, "open output", params.outputPath);
      return result;
    }
  }
  rc = avformat_write_header(cleanup.out, nullptr);
  if (rc < 0) {
    result.error = outputWriteError(rc, "write header", params.outputPath);
    return result;
  }
  if (coverPkt != nullptr && coverPkt->size > 0 && coverOutIndex >= 0) {
    coverPkt->stream_index = coverOutIndex;
    rc = av_write_frame(cleanup.out, coverPkt);
    av_packet_free(&coverPkt);
    coverPkt = nullptr;
    if (rc < 0) {
      result.error =
          Error::make(ErrorCode::ExecutionFailed, "write cover art", "ffmpeg");
      return result;
    }
  }

  cleanup.packet = av_packet_alloc();
  const std::int64_t totalBytes =
      cleanup.in->pb != nullptr ? avio_size(cleanup.in->pb) : 0;
  std::int64_t processedPackets = 0;
  bool cancelled = false;
  bool writeFailed = false;
  // AVERROR code of the output-write failure (0 = unknown); ENOSPC maps
  // to the terminal StorageFull error below.
  int writeFailureRc = 0;
  // Trim window (spec §15/§16): seek to the keyframe at/before the start
  // (stream copy cannot re-time compressed data, so the output begins
  // there — the honest keyframe-snapped trim) and stop at the end gate.
  const bool remuxTrimmed = intent.trim.active();
  if (remuxTrimmed && intent.trim.startUs > 0) {
    int seekStream = 0;
    for (unsigned i = 0; i < cleanup.in->nb_streams; ++i) {
      if (cleanup.in->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        seekStream = int(i);
        break;
      }
    }
    const std::int64_t target = av_rescale_q(
        intent.trim.startUs, AVRational{1, 1000000},
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
    if (remuxTrimmed && intent.trim.endUs >= 0) {
      const AVStream *pktStream =
          cleanup.in->streams[std::size_t(cleanup.packet->stream_index)];
      const std::int64_t pktUs =
          cleanup.packet->pts == AV_NOPTS_VALUE
              ? -1
              : av_rescale_q(cleanup.packet->pts, pktStream->time_base,
                             AVRational{1, 1000000});
      if (pktUs >= 0 && pktUs >= intent.trim.endUs) {
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
    // Preserved subtitle cues ride through with their source timing (the
    // v2 request model carries no per-track timing offset; the shift
    // machinery stays available for burn-in material).

    const int writeRc = av_interleaved_write_frame(cleanup.out, outPacket);
    if (writeRc < 0) {
      writeFailureRc = writeRc;
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
    result.error = outputWriteError(writeFailureRc,
                                    "write failed during remux",
                                    params.outputPath);
    return result;
  }
  result.success = true;
  return result;
}

ExecuteResult FfmpegExecutor::executeTranscode(const ExecuteParams &params,
                                             const ExecutionIntent &intent) {
  // Decide between the single-pass path (short jobs, exotic containers,
  // unknown duration) and the segmented path (long jobs: independently
  // finalized, validated, committed segments + assembly; resumable).
  const Pipeline &pipeline = params.pipeline;

  bool segmentable = intent.wantsVideo && pipeline.videoStreamCopy == false;
  std::int64_t durationUs = 0;
  std::string inPathStr;
  if (segmentable) {
    // Cheap metadata-only open: the source duration decides.
    if (const std::optional<std::string> inPath = localPathFor(intent.source);
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
    if (intent.trim.active() && durationUs > 0) {
      const std::int64_t start =
          std::min<std::int64_t>(std::max<std::int64_t>(intent.trim.startUs, 0),
                                 durationUs);
      const std::int64_t end = intent.trim.endUs >= 0
                                   ? std::min(intent.trim.endUs, durationUs)
                                   : durationUs;
      durationUs = std::max<std::int64_t>(end - start, 0);
    }
    switch (pipeline.container) {
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
  if (intent.trim.active() && !segmentable) {
    // Trimmed single-pass: honor the window directly. Timestamps stay on
    // the source timeline (matched-source-timestamp comparison, spec §20).
    // endUs < 0 (open-ended) passes the probed duration, or INT64_MAX
    // when unknown — executeTranscodeRange's packet gate stops at EOF
    // regardless.
    const std::int64_t start =
        std::max<std::int64_t>(intent.trim.startUs, 0);
    const std::int64_t end = intent.trim.endUs >= 0
                                 ? intent.trim.endUs
                                 : (durationUs > 0 ? durationUs
                                                   : INT64_MAX);
    return executeTranscodeRange(params, intent, start, end, true);
  }
  if (segmentable && !params.jobId.empty()) {
    return executeTranscodeSegmented(params, intent);
  }
  return executeTranscodeRange(params, intent, 0, 0, false);
}

ExecuteResult FfmpegExecutor::executeTranscodeRange(
    const ExecuteParams &params, const ExecutionIntent &intent,
    std::int64_t rangeStartUs, std::int64_t rangeEndUs, bool rangeActive,
    bool isSegmentRun) {
  ExecuteResult result;
  initFfmpegLabel();
  const Pipeline &pipeline = params.pipeline;

  const std::optional<std::string> inPath = localPathFor(intent.source);
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
  // Input stream indices of the INSPECTED audio tracks, in inspected
  // order: MediaInfo ordinals skip streams whose codec is outside the
  // Velmux catalog (mirroring the inspector's construction), so a
  // request-side ordinal maps through this list, not the raw stream list.
  std::vector<int> inspectedAudioStreams;
  for (unsigned i = 0; i < cleanup.in->nb_streams; ++i) {
    const AVMediaType type = cleanup.in->streams[i]->codecpar->codec_type;
    if (type == AVMEDIA_TYPE_VIDEO && videoIn < 0) {
      videoIn = int(i);
    } else if (type == AVMEDIA_TYPE_AUDIO &&
               audioCodecFromAv(cleanup.in->streams[i]->codecpar->codec_id)
                   .has_value()) {
      inspectedAudioStreams.push_back(int(i));
    }
  }
  // Track selection policy (mandate §23): the replacement wins (the main
  // input's audio is ignored); otherwise the intent's ordinals name the
  // retained main-input lanes (empty = none survive); external additions
  // open their own inputs below and ride as extra lanes.
  const std::optional<std::string> replacePath =
      intent.wantsAudio && !intent.replaceAudioUri.empty()
          ? localPathForUri(intent.replaceAudioUri)
          : std::nullopt;
  if (intent.wantsAudio && replacePath.has_value()) {
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
  } else if (intent.wantsAudio) {
    // Request ordinals are inspected-track ordinals; map them to stream
    // indices. Out-of-range ordinals were already refused by the builder.
    for (std::int32_t ordinal : intent.audioSourceOrdinals) {
      if (ordinal >= 0 && ordinal < std::int32_t(inspectedAudioStreams.size())) {
        audioStreams.push_back(inspectedAudioStreams[std::size_t(ordinal)]);
      }
    }
  }
  // External additions (mandate §23): each URI opens its own input; its
  // first audio stream becomes an extra processing lane aligned with the
  // output timeline (the intent builder refuses additions on stream-copy
  // pipelines, so these always re-encode).
  for (const std::string &uri : intent.externalAudioUris) {
    const std::optional<std::string> extPath = localPathForUri(uri);
    if (!extPath.has_value()) {
      result.error = Error::make(ErrorCode::SourceUnavailable,
                                 "external audio URI is not a local path",
                                 "ffmpeg");
      return result;
    }
    AVFormatContext *extIn = nullptr;
    if (avformat_open_input(&extIn, extPath->c_str(), nullptr, nullptr) != 0 ||
        avformat_find_stream_info(extIn, nullptr) < 0) {
      if (extIn != nullptr) {
        avformat_close_input(&extIn);
      }
      result.error = Error::make(ErrorCode::SourceUnavailable,
                                 "cannot open the external audio file",
                                 "ffmpeg");
      return result;
    }
    cleanup.externalAudioIns.push_back(extIn);
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
    // External inputs keep their own timelines: seek them too so
    // segmented runs do not re-read from zero every segment.
    for (AVFormatContext *extIn : cleanup.externalAudioIns) {
      int extAudio = -1;
      for (unsigned i = 0; i < extIn->nb_streams; ++i) {
        if (extIn->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
          extAudio = int(i);
          break;
        }
      }
      if (extAudio >= 0) {
        const std::int64_t extTarget = av_rescale_q(
            rangeStartUs, AVRational{1, 1000000},
            extIn->streams[std::size_t(extAudio)]->time_base);
        avformat_seek_file(extIn, extAudio, INT64_MIN, extTarget, INT64_MAX,
                           AVSEEK_FLAG_BACKWARD);
      }
    }
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
  // HDR -> SDR tone mapping (mandate §25 hdrToSdr): decided after the
  // decoder and encoder exist; drives the zscale/tonemap chain in the
  // video graph (real conversion, never tag removal).
  bool toneMap = false;
  if (videoIn >= 0 && intent.wantsVideo) {
    const AVStream *inVideo = cleanup.in->streams[videoIn];
    const AVCodec *decCodec =
        avcodec_find_decoder(inVideo->codecpar->codec_id);
    if (decCodec == nullptr) {
      result.error = Error::make(
          ErrorCode::UnsupportedOperation,
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
          ErrorCode::UnsupportedOperation,
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
        intent.video.crop.has_value() && intent.video.crop->active();
    const bool wantsRotation = intent.video.rotationDegrees == 90 ||
                               intent.video.rotationDegrees == 270;
    std::int32_t baseH = cleanup.videoDec->height;
    std::int32_t baseW = cleanup.videoDec->width;
    if (wantsCrop) {
      // The crop rectangle must fit entirely inside the source frame:
      // silently clamping a user rect to the overlap would produce a
      // different crop than requested (mangling, not tolerance).
      const CropRect &crop = *intent.video.crop;
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
    std::int32_t outH2 = intent.video.resolutionHeight.value_or(baseH);
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
    fpsConvert = intent.video.frameRate.has_value() &&
                 *intent.video.frameRate > 0.0 &&
                 *intent.video.frameRate < 1000.0;
    outFps = fpsConvert ? *intent.video.frameRate : 0.0;
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
    // Playback speed (mandate §25) compresses frame timestamps by 1/speed,
    // so the effective output frame rate is fps * speed. The encoder rate
    // and time base must reflect that: with the source rate kept, two
    // re-timed frames collide on one time-base tick (non-monotonic pts)
    // and the muxer rejects the stream.
    if (intent.video.playbackSpeed.has_value() &&
        *intent.video.playbackSpeed > 0.0 &&
        *intent.video.playbackSpeed != 1.0) {
      const double effectiveFps =
          av_q2d(cleanup.videoEnc->framerate) * *intent.video.playbackSpeed;
      if (effectiveFps > 0.0 && effectiveFps < 1000.0) {
        cleanup.videoEnc->framerate = av_d2q(effectiveFps, 1001000);
        cleanup.videoEnc->time_base = av_inv_q(cleanup.videoEnc->framerate);
      }
    }
    cleanup.videoEnc->pix_fmt = AV_PIX_FMT_YUV420P;
    // ---- HDR handling (mandate §25 hdrToSdr) -----------------------------
    // Source HDR signal from the decoder context (tagging authority: the
    // demuxer fills codecpar, avcodec_parameters_to_context carried it).
    const bool sourceHdr =
        cleanup.videoDec->color_trc == AVCOL_TRC_SMPTE2084 ||
        cleanup.videoDec->color_trc == AVCOL_TRC_ARIB_STD_B67 ||
        cleanup.videoDec->color_primaries == AVCOL_PRI_BT2020;
    if (sourceHdr && intent.video.hdrToSdr) {
      // Genuine HDR -> SDR tone mapping through the real zscale/tonemap
      // filter chain (libzimg present in this build; capabilities() only
      // reports hdrToneMapping after probing both filters).
      toneMap = true;
    } else if (sourceHdr && !pipeline.videoStreamCopy) {
      // Preservation was requested but this 8-bit path cannot carry HDR
      // honestly: refuse instead of encoding PQ/HLG pixels unconverted
      // (washed-out output). Stream copy keeps the bitstream intact.
      result.error = Error::make(
          ErrorCode::UnsupportedOperation,
          "this HDR source cannot be preserved through an 8-bit "
          "re-encode; enable HDR to SDR conversion or keep a stream-copy "
          "route",
          "ffmpeg");
      return result;
    }
    if (sourceHdr && !toneMap) {
      // Preserve the source HDR signaling on the output stream (only the
      // stream-copy path reaches here).
      cleanup.videoEnc->color_primaries = cleanup.videoDec->color_primaries;
      cleanup.videoEnc->color_trc = cleanup.videoDec->color_trc;
      cleanup.videoEnc->colorspace = cleanup.videoDec->colorspace;
      cleanup.videoEnc->color_range = cleanup.videoDec->color_range;
    }
    if (toneMap) {
      // The converted output is SDR BT.709 by construction; tag it
      // honestly so downstream consumers read the right colorimetry.
      cleanup.videoEnc->color_primaries = AVCOL_PRI_BT709;
      cleanup.videoEnc->color_trc = AVCOL_TRC_BT709;
      cleanup.videoEnc->colorspace = AVCOL_SPC_BT709;
      cleanup.videoEnc->color_range = AVCOL_RANGE_MPEG; // TV/narrow
    }
    if (cleanup.out->oformat->flags & AVFMT_GLOBALHEADER) {
      cleanup.videoEnc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    // Resolved rate control (mandate §20): the intent carries the policy
    // the compression derivation produced (bitrate budget or quality
    // scale).
    if (intent.rate.bitrateMode && intent.rate.bitRate > 0) {
      cleanup.videoEnc->bit_rate = intent.rate.bitRate;
      cleanup.videoEnc->rc_max_rate = intent.rate.bitRate;
      cleanup.videoEnc->rc_buffer_size = intent.rate.bitRate * 2;
    } else {
      // Quality-driven (CRF-style).
      const std::int32_t qualityPct = intent.rate.quality;
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
    // transforms AND/OR timing transforms AND/OR tone mapping
    // (crop -> rotation -> flips -> scale -> [speed] -> [reverse] ->
    //  [subtitles]).
    const bool wantsBurnIn = intent.subtitles.burnIn;
    const bool wantsTransform =
        wantsCrop || intent.video.rotationDegrees != 0 ||
        intent.video.flipHorizontal || intent.video.flipVertical;
    const bool wantsTiming = intent.video.playbackSpeed.has_value() ||
                             intent.video.reverse;
    if (wantsBurnIn || wantsTransform || wantsTiming || toneMap) {
      // Resolve the burn-in source: explicit external file, else the
      // selected embedded subtitle stream (default: the first).
      std::string subPath;
      int subGlobalIndex = -1; // stream index in the input container
      int subOrdinal = -1;     // subtitle-stream ordinal (the filter's `si`)
      if (wantsBurnIn) {
      if (!intent.subtitles.burnInExternal.empty()) {
        const std::optional<std::string> ext =
            localPathForUri(intent.subtitles.burnInExternal);
        if (!ext.has_value()) {
          result.error = Error::make(ErrorCode::SourceUnavailable,
                                     "external subtitle URI is not a local path",
                                     "ffmpeg");
          return result;
        }
        subPath = *ext;
      } else {
        // The burn-in track is an inspected subtitle ordinal (-1 = first).
        int wantedSubOrdinal = intent.subtitles.burnInTrack;
        int seenSubs = 0;
        for (unsigned i = 0; i < cleanup.in->nb_streams; ++i) {
          if (cleanup.in->streams[i]->codecpar->codec_type ==
              AVMEDIA_TYPE_SUBTITLE) {
            if (wantedSubOrdinal < 0 || seenSubs == wantedSubOrdinal) {
              subGlobalIndex = int(i);
              subOrdinal = seenSubs;
              break;
            }
            ++seenSubs;
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
                ErrorCode::UnsupportedOperation,
                "bitmap subtitle burn-in cannot be combined with crop/"
                "rotation/flip in V1 (cue positions are authored for the "
                "source frame)",
                "ffmpeg");
            return result;
          }
          if (toneMap) {
            result.error = Error::make(
                ErrorCode::UnsupportedOperation,
                "bitmap subtitle burn-in cannot be combined with HDR->SDR "
                "tone mapping in V1 (the overlay graph composites 8-bit "
                "SDR cues)",
                "ffmpeg");
            return result;
          }
          if (wantsTiming) {
            result.error = Error::make(
                ErrorCode::UnsupportedOperation,
                "bitmap subtitle burn-in cannot be combined with playback "
                "speed or reverse in V1 (cue timing is authored for the "
                "source timeline)",
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
                           0, cueReason)) {
            result.error = Error::make(
                ErrorCode::UnsupportedOperation,
                "bitmap burn-in failed: " + cueReason, "ffmpeg");
            return result;
          }
          bitmapCues = std::move(loader.cues());
          for (const BitmapCue &cue : bitmapCues) {
            overlayFrameGuard.frames.push_back(cue.overlay);
          }
        } else if (!isBurnableSubtitleCodec(subCodecId)) {
          result.error = Error::make(
              ErrorCode::UnsupportedOperation,
              std::string("burn-in of subtitle format ") +
                  avcodec_get_name(subCodecId) + " is not supported",
              "ffmpeg");
          return result;
        }
      }

      const FontSource fontSource = wantsBurnIn ? findFontSource()
                                                : FontSource{};
      if (wantsBurnIn && fontSource.dir.empty()) {
        result.error = Error::make(
            ErrorCode::UnsupportedOperation,
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
      // Graph description: [tone-map,] crop -> rotation -> flips -> scale
      // -> [subtitles] -> [setpts speed] -> [reverse]. Order matters: crop
      // uses source-frame coordinates; rotation swaps dimensions; the
      // scale lands on the encoder's (transform-aware) target geometry;
      // subtitles render on the SOURCE timeline (cues are authored there)
      // before the timing transforms re-time the burned frames.
      std::string graphDesc;
      if (toneMap) {
        // HDR -> SDR (mandate §25): the real zscale/tonemap chain (libzimg).
        graphDesc +=
            "zscale=t=linear:npl=100,format=gbrpf32le,zscale=p=bt709,"
            "tonemap=hable:desat=0,zscale=t=bt709:m=bt709:r=tv,"
            "format=yuv420p,";
      }
      if (wantsCrop) {
        const CropRect &crop = *intent.video.crop;
        graphDesc += "crop=w=" + std::to_string(baseW) +
                     ":h=" + std::to_string(baseH) +
                     ":x=" + std::to_string(crop.x) +
                     ":y=" + std::to_string(crop.y) + ",";
      }
      switch (intent.video.rotationDegrees) {
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
      if (intent.video.flipHorizontal) {
        graphDesc += "hflip,";
      }
      if (intent.video.flipVertical) {
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
      // Timing transforms (mandate §25): playback speed re-times the
      // burned/scaled frames (setpts=PTS/speed); reverse flips the
      // presentation order (whole-stream buffering, real filter).
      // Filters are comma-separated: a leading separator is required
      // whenever the graph does not already end on one.
      const auto appendFilter = [&graphDesc](const char *filter) {
        if (!graphDesc.empty() && graphDesc.back() != ',') {
          graphDesc += ',';
        }
        graphDesc += filter;
      };
      if (intent.video.playbackSpeed.has_value() &&
          *intent.video.playbackSpeed > 0.0) {
        char speedBuf[64];
        std::snprintf(speedBuf, sizeof(speedBuf), "setpts=PTS/%.4f",
                      *intent.video.playbackSpeed);
        appendFilter(speedBuf);
      }
      if (intent.video.reverse) {
        appendFilter("reverse");
      }
      if (!graphDesc.empty() && graphDesc.back() == ',') {
        graphDesc.pop_back();
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
      // Burn-in appearance (mandate §24): force_style carries the font
      // family, size, color and vertical position. SRT-like sources need
      // the family redirected to the discovered font (libass defaults to
      // "Arial", which provider-less static builds cannot match); ASS/SSA
      // keep their authored styling, but explicit appearance fields still
      // override (the user asked for them).
      if (wantsBurnIn) {
        std::string style;
        if (subIsAss == false && !fontSource.family.empty()) {
          style += "FontName=" + escapeFilterPath(fontSource.family);
        }
        if (intent.subtitles.fontSize.has_value()) {
          if (!style.empty()) {
            style += ",";
          }
          style += "FontSize=" + std::to_string(*intent.subtitles.fontSize);
        }
        if (intent.subtitles.color.has_value() &&
            intent.subtitles.color->size() == 7 &&
            (*intent.subtitles.color)[0] == '#') {
          // "#RRGGBB" -> ASS PrimaryColour &H00BBGGRR (BGR order).
          const std::string &hex = *intent.subtitles.color;
          if (!style.empty()) {
            style += ",";
          }
          style += "PrimaryColour=&H00" + hex.substr(5, 2) + hex.substr(3, 2) +
                   hex.substr(1, 2);
        }
        if (intent.subtitles.position.has_value()) {
          // 0..100 vertical position -> ASS numpad alignment bands
          // (bottom / middle / top, centered).
          const std::int32_t alignment =
              *intent.subtitles.position < 33
                  ? 2
                  : *intent.subtitles.position <= 66 ? 5 : 8;
          if (!style.empty()) {
            style += ",";
          }
          style += "Alignment=" + std::to_string(alignment);
        }
        if (!style.empty()) {
          graphDesc += ":force_style='" + style + "'";
        }
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
  // replacement audio / kept ordinals / external additions). Stream-copy
  // lanes pass packets straight to the output stream; processing lanes
  // decode, run the DSP chain, resample and encode.
  // The DSP chain runs when the operation carries audio adjustments, or
  // when the VIDEO operation's timing transforms need the audio timeline
  // to follow (atempo for playback speed, areverse for reverse).
  const bool audioSpeedWanted =
      (intent.audioDsp.has_value() &&
       intent.audioDsp->playbackSpeed.has_value()) ||
      intent.video.playbackSpeed.has_value();
  const bool audioReverseWanted =
      (intent.audioDsp.has_value() && intent.audioDsp->reverse) ||
      intent.video.reverse;
  const bool wantsAudioDspChain =
      audioSpeedWanted || audioReverseWanted ||
      (intent.audioDsp.has_value() &&
       (intent.audioDsp->gainDb.has_value() ||
        intent.audioDsp->fadeInMs.has_value() ||
        intent.audioDsp->fadeOutMs.has_value() ||
        intent.audioDsp->normalize || intent.audioDsp->removeSilence));
  // The per-lane setup, shared by main-input, replacement and addition
  // lanes (the addition inputs are appended after the main selection).
  std::vector<int> setupAudioStreams = audioStreams;
  for (std::size_t e = 0; e < cleanup.externalAudioIns.size(); ++e) {
    setupAudioStreams.push_back(-2 - int(e)); // addition-input marker
  }
  for (const int aIn : setupAudioStreams) {
    cleanup.audio.emplace_back();
    AudioTrack &track = cleanup.audio.back();
    if (aIn == -1) {
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
    } else if (aIn <= -2) {
      // Addition-input marker: its first audio stream becomes an extra
      // output track (aligned with the output timeline).
      track.input =
          cleanup.externalAudioIns[std::size_t(-2 - aIn)];
      for (unsigned i = 0; i < track.input->nb_streams; ++i) {
        if (track.input->streams[i]->codecpar->codec_type ==
            AVMEDIA_TYPE_AUDIO) {
          track.inStream = int(i);
          break;
        }
      }
      if (track.inStream < 0) {
        result.error = Error::make(ErrorCode::InvalidArgument,
                                   "the external audio has no track",
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
    // mirror the planner's canStreamCopyAudio. Addition lanes never copy
    // (their input codec is not the pipeline's source).
    const bool additionLane = track.input != cleanup.in &&
                              track.input != cleanup.audioReplaceIn;
    const bool copyable =
        pipeline.audioStreamCopy && !wantsAudioDspChain && !additionLane &&
        !intent.audioSettings.sampleRate.has_value() &&
        !intent.audioSettings.channels.has_value() &&
        !intent.audioSettings.bitRate.has_value() &&
        !intent.audioSettings.pcmBitDepth.has_value() &&
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
          ErrorCode::UnsupportedOperation,
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
        intent.audioSettings.pcmBitDepth.has_value()) {
      switch (*intent.audioSettings.pcmBitDepth) {
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
          ErrorCode::UnsupportedOperation,
          std::string("no FFmpeg encoder for output audio codec: ") +
              codecName(pipeline.audioCodec),
          "ffmpeg");
      av_frame_free(&videoFrame);
      return result;
    }
    track.enc = avcodec_alloc_context3(encCodec);
    track.enc->sample_rate =
        intent.audioSettings.sampleRate.value_or(track.dec->sample_rate);
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
    const std::int32_t channels = intent.audioSettings.channels.value_or(
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
        intent.audioSettings.bitRate.value_or(
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
      const auto dsp = intent.audioDsp; // copy: fields read below
      if (dsp.has_value() && dsp->gainDb.has_value()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "volume=volume=%.2fdB:"
                                        "precision=double,",
                      *dsp->gainDb);
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
          !rangeActive || rangeStartUs <= intent.trim.startUs;
      const bool lastWindow =
          !rangeActive || intent.trim.endUs < 0 ||
          rangeEndUs >= intent.trim.endUs;
      if (dsp.has_value() && dsp->fadeInMs.has_value() &&
          *dsp->fadeInMs > 0 && firstWindow) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "afade=t=in:st=0:d=%.3f,",
                      double(*dsp->fadeInMs) / 1000.0);
        desc += buf;
      }
      if (dsp.has_value() && dsp->fadeOutMs.has_value() &&
          *dsp->fadeOutMs > 0 && lastWindow && windowSec > 0.0) {
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "afade=t=out:st=%.3f:d=%.3f,",
                      std::max(0.0, windowSec -
                                       double(*dsp->fadeOutMs) /
                                           1000.0),
                      double(*dsp->fadeOutMs) / 1000.0);
        desc += buf;
      }
      if (dsp.has_value() && dsp->normalize) {
        desc += "loudnorm=I=-16:TP=-1.5:LRA=11,";
      }
      // Playback speed (mandate §25/§32): video operations carry the speed
      // on the video adjustments; audio operations carry it on the audio
      // adjustments. Pitch is preserved (atempo).
      if (audioSpeedWanted) {
        const double speed =
            intent.video.playbackSpeed.has_value()
                ? *intent.video.playbackSpeed
                : dsp->playbackSpeed.value_or(1.0);
        if (speed > 0.0) {
          char buf[64];
          std::snprintf(buf, sizeof(buf), "atempo=%.4f,", speed);
          desc += buf;
        }
      }
      if (audioReverseWanted) {
        desc += "areverse,";
      }
      if (dsp.has_value() && dsp->removeSilence) {
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
  // The planner guarantees (Preserve/Embed rules) that every preserved
  // source subtitle track's codec is muxable into the target container, so
  // the tracks are copied verbatim alongside the re-encoded A/V streams
  // (spec §14: never silently discard subtitles during ordinary conversion).
  std::vector<int> subInOfOut;
  std::vector<int> subOutIndex;
  if (intent.subtitles.keepSubtitles) {
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
  // Playback speed scales preserved subtitle timing (mandate §25: video,
  // audio and subtitle timing stay consistent): cue pts are multiplied by
  // 1/speed in the output time base when packets are written below.
  const bool subSpeedScale = intent.video.playbackSpeed.has_value() &&
                             intent.video.playbackSpeed > 0.0 &&
                             *intent.video.playbackSpeed != 1.0;
  const double subSpeedFactor =
      subSpeedScale ? 1.0 / *intent.video.playbackSpeed : 1.0;

  // ---- Attached pictures / cover art (spec §10) ----------------------------
  // Attached pictures are metadata, not timeline content: preserved from
  // the source (or replaced by the external image). In segmented runs the
  // FIRST segment carries them (later segments skip; assembly yields
  // exactly one copy).
  std::vector<std::pair<int, int>> picInOut; // input idx -> output idx
  const bool picsIncluded = !isSegmentRun || rangeStartUs <= 0;
  if (intent.preserveCoverArt && picsIncluded) {
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
  // External cover art (spec §10): the stream is created before the
  // header, its encoded packet written right after it.
  AVPacket *coverPkt = nullptr;
  int coverOutIndex = -1;
  if (intent.coverArtUri.has_value() && picsIncluded) {
    const std::optional<std::string> coverPath =
        localPathForUri(*intent.coverArtUri);
    if (!coverPath.has_value()) {
      result.error = Error::make(ErrorCode::SourceUnavailable,
                                 "cover art URI is not a local path",
                                 "ffmpeg");
      av_frame_free(&videoFrame);
      return result;
    }
    std::string coverErr;
    AVPacket *coverPacket = av_packet_alloc();
    AVStream *coverStream =
        addCoverArtStream(cleanup.out, *coverPath, coverPacket, coverErr);
    if (coverStream == nullptr) {
      av_packet_free(&coverPacket);
      result.error =
          Error::make(ErrorCode::InvalidArgument,
                      "cannot embed cover art: " + coverErr, "ffmpeg");
      av_frame_free(&videoFrame);
      return result;
    }
    coverOutIndex = coverStream->index;
    coverPkt = coverPacket; // written right after the header below
  }

  // Metadata (mandate §33): container-level and per-stream tags copy
  // through the transcode (re-encoding changes the essence, never the
  // descriptive metadata); the familiar-field set/clear intentions then
  // apply on the container dictionary (the all-Clear intention is the
  // moral equivalent of the retired stripMetadata control).
  av_dict_copy(&cleanup.out->metadata, cleanup.in->metadata,
               AV_DICT_DONT_OVERWRITE);
  applyMetadataIntention(&cleanup.out->metadata, intent.metadata);
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
      result.error = outputWriteError(rc, "open output", params.outputPath);
      av_frame_free(&videoFrame);
      return result;
    }
  }
  rc = avformat_write_header(cleanup.out, nullptr);
  if (rc < 0) {
    result.error = outputWriteError(rc, "write header", params.outputPath);
    av_frame_free(&videoFrame);
    return result;
  }
  if (coverPkt != nullptr && coverPkt->size > 0 && coverOutIndex >= 0) {
    coverPkt->stream_index = coverOutIndex;
    rc = av_write_frame(cleanup.out, coverPkt);
    av_packet_free(&coverPkt);
    coverPkt = nullptr;
    if (rc < 0) {
      result.error =
          Error::make(ErrorCode::ExecutionFailed, "write cover art", "ffmpeg");
      av_frame_free(&videoFrame);
      return result;
    }
  }

  // ---- Central loop ------------------------------------------------------------
  cleanup.packet = av_packet_alloc();
  cleanup.frame = av_frame_alloc();
  const std::int64_t totalDurationUs =
      cleanup.in->duration != AV_NOPTS_VALUE ? cleanup.in->duration : 0;
  std::int64_t framesDone = 0;
  bool cancelled = false;
  bool writeFailed = false;
  // AVERROR code of the output-write failure (0 = unknown); ENOSPC maps
  // to the terminal StorageFull error below. Shared by every write lane:
  // video/audio encoder packets, stream copies and subtitle packets.
  int writeFailureRc = 0;

  // Encodes one prepared frame (already in encoder time base) and writes
  // the resulting packets. Shared by the direct, burn-in and drain paths.
  const auto sendVideoFrameToEncoder = [&](AVFrame *f) {
    if (avcodec_send_frame(cleanup.videoEnc, f) == 0) {
      AVPacket *encPacket = av_packet_alloc();
      while (avcodec_receive_packet(cleanup.videoEnc, encPacket) == 0) {
        encPacket->stream_index = videoOut;
        av_packet_rescale_ts(encPacket, cleanup.videoEnc->time_base,
                             cleanup.out->streams[videoOut]->time_base);
        const int writeRc = av_interleaved_write_frame(cleanup.out, encPacket);
        if (writeRc < 0) {
          writeFailureRc = writeRc;
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
      const int writeRc = av_interleaved_write_frame(cleanup.out, encPacket);
      if (writeRc < 0) {
        writeFailureRc = writeRc;
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
      const int writeRc = av_interleaved_write_frame(cleanup.out, encPacket);
      if (writeRc < 0) {
        writeFailureRc = writeRc;
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
          const int writeRc = av_interleaved_write_frame(cleanup.out, encPacket);
          if (writeRc < 0) {
            writeFailureRc = writeRc;
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
  // ---- External-audio pump (replacement + additions, spec §9/mandate §23) -
  // Feeds external-input packets interleaved with the main timeline so the
  // muxer sees monotonically increasing dts. av_read_frame cannot unread,
  // so the one packet read too far ahead is held and consumed first on
  // the next pump. Each external lane (the replacement and every addition)
  // holds its own read-ahead packet.
  struct ExternalLane {
    AudioTrack *track = nullptr;
    AVFormatContext *input = nullptr;
    AVPacket *heldPkt = nullptr;
    std::int64_t heldUs = -1;
  };
  std::vector<ExternalLane> externalLanes;
  for (AudioTrack &track : cleanup.audio) {
    if (track.input == cleanup.in || track.input == nullptr) {
      continue; // main-input lane
    }
    ExternalLane lane;
    lane.track = &track;
    lane.input = track.input;
    externalLanes.push_back(lane);
  }
  const auto feedExternalPacket = [&](ExternalLane &lane, AVPacket *pkt) {
    AudioTrack &track = *lane.track;
    if (track.copy) {
      AVPacket *outPacket = av_packet_alloc();
      av_packet_move_ref(outPacket, pkt);
      const AVStream *srcStream =
          lane.input->streams[std::size_t(outPacket->stream_index)];
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
      const int writeRc = av_interleaved_write_frame(cleanup.out, outPacket);
      if (writeRc < 0) {
        writeFailureRc = writeRc;
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
  const auto pumpExternalLanesUpTo = [&](std::int64_t upToUs) {
    if (externalLanes.empty() || writeFailed) {
      return;
    }
    // Range gate: nothing beyond the segment/window end.
    const std::int64_t limitUs =
        rangeBounded ? std::min(upToUs, rangeEndUs) : upToUs;
    for (ExternalLane &lane : externalLanes) {
      for (;;) {
        std::int64_t nextUs = -1;
        if (lane.heldPkt != nullptr) {
          nextUs = lane.heldUs;
        }
        if (nextUs >= 0 && nextUs > limitUs && limitUs != INT64_MAX) {
          break; // still too far ahead; keep holding
        }
        AVPacket *pkt = lane.heldPkt;
        if (pkt == nullptr) {
          pkt = av_packet_alloc();
          if (av_read_frame(lane.input, pkt) < 0) {
            av_packet_free(&pkt);
            break; // external lane EOF
          }
          if (pkt->stream_index != lane.track->inStream) {
            av_packet_unref(pkt);
            av_packet_free(&pkt);
            continue; // not the selected external track
          }
          const AVStream *repStream = lane.input->streams[
              std::size_t(pkt->stream_index)];
          lane.heldUs =
              pkt->pts == AV_NOPTS_VALUE
                  ? -1
                  : av_rescale_q(pkt->pts, repStream->time_base,
                                 AVRational{1, 1000000});
        }
        if (limitUs != INT64_MAX && lane.heldUs > limitUs) {
          lane.heldPkt = pkt; // hold for the next pump
          break;
        }
        feedExternalPacket(lane, pkt);
        av_packet_free(&pkt);
        lane.heldPkt = nullptr;
        lane.heldUs = -1;
        if (writeFailed) {
          return;
        }
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
    // Presentation time of this packet in µs (the range gates and the
    // copy-lane rebase consume it; -1 when no usable timestamp exists).
    std::int64_t pktUs = -1;
    {
      const AVStream *pktStream =
          cleanup.in->streams[std::size_t(cleanup.packet->stream_index)];
      if (cleanup.packet->pts != AV_NOPTS_VALUE) {
        pktUs = av_rescale_q(cleanup.packet->pts, pktStream->time_base,
                             AVRational{1, 1000000});
      } else if (cleanup.packet->dts != AV_NOPTS_VALUE) {
        pktUs = av_rescale_q(cleanup.packet->dts, pktStream->time_base,
                             AVRational{1, 1000000});
      }
    }
    // Range gate on the demux side: packets at/after the end stop the run
    // (their content belongs to the next segment). Packets BEFORE the
    // start are NOT skipped here: after the backward seek they begin at
    // the keyframe, and the decoder must see that keyframe to establish
    // references — dropping them at the demux level makes every segment
    // that does not start exactly on a keyframe decode zero/garbled
    // frames. The decoded frames before the boundary are discarded by the
    // frame-level gates below (they exist for exactly this).
    if (rangeBounded) {
      if (pktUs >= 0 && pktUs >= rangeEndUs && (isVideo || isAudio)) {
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
      //
      // Range runs rebase onto the window-local timeline (mirroring the
      // video/encode lanes): packets before the range start belong to an
      // earlier window — dropped here, within one audio frame's tolerance
      // so the priming edge survives — and kept packets are rebased by the
      // first one that lands. Whole-file runs keep source timestamps.
      if (rangeBounded && pktUs >= 0 && pktUs < rangeStartUs - 25000) {
        av_packet_unref(cleanup.packet);
        continue;
      }
      AVPacket *outPacket = av_packet_alloc();
      av_packet_move_ref(outPacket, cleanup.packet);
      const AVStream *srcStream =
          cleanup.in->streams[std::size_t(outPacket->stream_index)];
      const AVStream *dstStream =
          cleanup.out->streams[std::size_t(audioTrack->out)];
      const AVRounding rounding = static_cast<AVRounding>(
          AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
      if (outPacket->pts != AV_NOPTS_VALUE) {
        outPacket->pts = av_rescale_q_rnd(
            outPacket->pts, srcStream->time_base, dstStream->time_base,
            rounding);
        if (rangeBounded) {
          if (audioTrack->copyBasePts == AV_NOPTS_VALUE) {
            audioTrack->copyBasePts = outPacket->pts;
          }
          outPacket->pts -= audioTrack->copyBasePts;
          if (outPacket->pts < 0) {
            outPacket->pts = 0;
          }
        }
      }
      if (outPacket->dts != AV_NOPTS_VALUE) {
        outPacket->dts = av_rescale_q_rnd(
            outPacket->dts, srcStream->time_base, dstStream->time_base,
            rounding);
        if (rangeBounded) {
          if (audioTrack->copyBaseDts == AV_NOPTS_VALUE) {
            audioTrack->copyBaseDts = outPacket->dts;
          }
          outPacket->dts -= audioTrack->copyBaseDts;
          if (outPacket->dts < 0) {
            outPacket->dts = 0;
          }
        }
      }
      outPacket->duration = av_rescale_q(
          outPacket->duration, srcStream->time_base,
          cleanup.out->streams[std::size_t(audioTrack->out)]->time_base);
      outPacket->pos = -1;
      outPacket->stream_index = audioTrack->out;
      const int writeRc = av_interleaved_write_frame(cleanup.out, outPacket);
      if (writeRc < 0) {
        writeFailureRc = writeRc;
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
        const int writeRc = av_interleaved_write_frame(cleanup.out, outPacket);
        if (writeRc < 0) {
          writeFailureRc = writeRc;
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
                                              dstStream->time_base, rounding);
          if (subSpeedScale) {
            // Playback speed scales preserved cue timing (1/speed).
            pts = std::int64_t(double(pts) * subSpeedFactor);
          }
          if (rangeBounded) {
            // Segment-local timeline for preserved subtitles.
            const std::int64_t startDst = av_rescale_q(
                rangeStartUs, AVRational{1, 1000000}, dstStream->time_base);
            pts -= startDst;
          }
          if (pts < 0) {
            pts = 0; // clamps cue starts, never emits negative pts
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
        const int writeRc = av_interleaved_write_frame(cleanup.out, outPacket);
        if (writeRc < 0) {
          writeFailureRc = writeRc;
          writeFailed = true;
        }
        av_packet_free(&outPacket);
        }
      }
    }
    // External audio pacing (replacement + additions): keep the external
    // inputs interleaved with the main timeline (up to 1 s ahead), never a
    // post-video batch — the muxer needs monotonically increasing dts
    // across streams.
    if (!externalLanes.empty() && !writeFailed) {
      const AVStream *mainStream =
          cleanup.in->streams[std::size_t(cleanup.packet->stream_index)];
      const std::int64_t mainUs =
          cleanup.packet->pts == AV_NOPTS_VALUE
              ? -1
              : av_rescale_q(cleanup.packet->pts, mainStream->time_base,
                             AVRational{1, 1000000});
      if (mainUs >= 0) {
        pumpExternalLanesUpTo(mainUs + 1000000);
      }
    }
    av_packet_unref(cleanup.packet);
  }

  // External audio tail: after the main input ends, feed everything
  // remaining (bounded by the range when segmenting).
  pumpExternalLanesUpTo(INT64_MAX);
  for (ExternalLane &lane : externalLanes) {
    av_packet_free(&lane.heldPkt);
    lane.heldPkt = nullptr;
    lane.heldUs = -1;
  }

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
    result.error = outputWriteError(writeFailureRc,
                                    "write failed during transcode",
                                    params.outputPath);
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
// On failure, writeRc carries the output-write AVERROR code when known so
// callers can surface ENOSPC as the terminal StorageFull error.
bool assembleSegments(const std::vector<std::string> &segments,
                      const std::string &outputPath, Container container,
                      int &writeRc) {
  const std::filesystem::path outFs(outputPath);
  const std::filesystem::path listPath =
      outFs.parent_path() / (outFs.filename().string() + ".concat.txt");
  {
    std::ofstream list(listPath, std::ios::binary | std::ios::trunc);
    if (!list) {
      if (errno == ENOSPC) {
        writeRc = AVERROR(ENOSPC);
      }
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
      // inpoint 0: drop codec-priming packets (e.g. the AAC encoder-delay
      // packet at a negative dts) and rebase each file's timeline to
      // zero. Pristine sources otherwise collide inside the concat
      // demuxer: file N+1's priming-shifted first dts lands BEFORE file
      // N's last packet dts (non-monotonic dts -> the muxer refuses).
      // Already-zero-based segments (Cut/Split parts) are unaffected.
      list << "inpoint 0\n";
    }
    if (!list.good()) {
      if (errno == ENOSPC) {
        writeRc = AVERROR(ENOSPC);
      }
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
    if (!(out->oformat->flags & AVFMT_NOFILE)) {
      const int ioRc = avio_open(&out->pb, outputPath.c_str(),
                                AVIO_FLAG_WRITE);
      if (ioRc < 0) {
        writeRc = ioRc;
        break;
      }
    }
    const int headerRc = avformat_write_header(out, nullptr);
    if (headerRc < 0) {
      writeRc = headerRc;
      break;
    }
    AVPacket *pkt = av_packet_alloc();
    bool writeFailed = false;
    // Monotonic dts guard per output stream: the concat demuxer rebases
    // each file onto the cumulative timeline using container durations,
    // which include codec priming/padding the actual packets do not span
    // exactly — the first packet of file N+1 can land one priming-
    // residue behind the last dts of file N (non-monotonic -> EINVAL).
    // Nudge such packets one tick forward (the established FfmpegMuxStage
    // guard pattern); the sub-millisecond jitter is inaudible and every
    // packet is kept.
    std::vector<std::int64_t> lastDts(out->nb_streams, AV_NOPTS_VALUE);
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
      const std::int64_t &streamLast = lastDts[std::size_t(dstStream->index)];
      if (pkt->dts != AV_NOPTS_VALUE && streamLast != AV_NOPTS_VALUE &&
          pkt->dts <= streamLast) {
        pkt->dts = streamLast + 1;
        if (pkt->pts != AV_NOPTS_VALUE && pkt->pts < pkt->dts) {
          pkt->pts = pkt->dts;
        }
      }
      if (pkt->dts != AV_NOPTS_VALUE) {
        lastDts[std::size_t(dstStream->index)] = pkt->dts;
      }
      const int frameRc = av_interleaved_write_frame(out, pkt);
      if (frameRc < 0) {
        writeRc = frameRc;
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
// the encoded packet is returned through coverPacket and the CALLER must
// write it with av_write_frame right after avformat_write_header — the
// mov/ipod muxer only materializes the pict track from a real sample
// (the earlier extradata-only convention produced a zero-sample track
// that no demuxer reports as attached art).
AVStream *addCoverArtStream(AVFormatContext *out, const std::string &path,
                            AVPacket *coverPacket, std::string &err) {
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
        // Hand the encoded JPEG to the caller: it must be written as a
        // real sample AFTER avformat_write_header (see the function
        // comment) — the mov muxer builds the pict track from it.
        if (av_packet_ref(coverPacket, pkt) != 0) {
          err = "cannot stage the encoded cover art packet";
          added = nullptr;
        }
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
    const ExecuteParams &params, const ExecutionIntent &intent) {
  ExecuteResult result;
  initFfmpegLabel();
  // Whole-job duration (the decision path verified it is known and long).
  std::int64_t totalUs = 0;
  {
    const std::optional<std::string> inPath = localPathFor(intent.source);
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
      return executeTranscodeRange(params, intent, 0, 0, false);
    }
  }

  // Trim window (spec §15/§16/§20): the segment tiling covers
  // [trimStart, windowEnd) on the source timeline; checkpoint positions
  // stay in source-timeline microseconds (the durable coordinate system
  // D-017 established).
  const std::int64_t trimStart =
      intent.trim.active()
          ? std::min<std::int64_t>(
                std::max<std::int64_t>(intent.trim.startUs, 0), totalUs)
          : 0;
  const std::int64_t windowEnd =
      intent.trim.endUs >= 0
          ? std::min(intent.trim.endUs, totalUs)
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
        if (ec) {
          // The rename fallback failed too (e.g. a full disk): never fall
          // through to fake success without the final output.
          result.error = outputWriteError(
              ec == std::errc::no_space_on_device ? AVERROR(ENOSPC) : 0,
              "cannot promote the finished output", params.outputPath);
          return result;
        }
      }
    } else {
      int asmRc = 0;
      const bool assembled = assembleSegments(
          std::vector<std::string>([](const std::vector<ValidSegment> &c) {
            std::vector<std::string> paths;
            paths.reserve(c.size());
            for (const auto &v : c) {
              paths.push_back(v.path);
            }
            return paths;
          }(committed)),
          params.outputPath, params.pipeline.container, asmRc);
      if (!assembled) {
        result.error = outputWriteError(asmRc,
                                        "cannot assemble committed segments",
                                        params.outputPath);
        return result;
      }
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
        executeTranscodeRange(segParams, intent, segStart, segEnd, true, true);
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
        result.error = outputWriteError(
            ec == std::errc::no_space_on_device ? AVERROR(ENOSPC) : 0,
            "cannot promote the finished output", params.outputPath);
        return result;
      }
    }
  } else {
    std::vector<std::string> paths;
    paths.reserve(committed.size());
    for (const auto &v : committed) {
      paths.push_back(v.path);
    }
    int asmRc = 0;
    if (!assembleSegments(paths, params.outputPath,
                          params.pipeline.container, asmRc)) {
      result.error = outputWriteError(asmRc,
                                      "cannot assemble the finished segments",
                                      params.outputPath);
      return result;
    }
  }
  std::filesystem::remove_all(segDir, ec);
  result.success = true;
  return result;
}

// ---------------------------------------------------------------------------
// Timeline operations (mandate §27 Cut & Split, §27.4/§27.5 Merge, §29
// Extract Frames). The helpers below build on the remux/transcode/segment
// machinery above: real keyframe-timeline analysis (packet flags, no
// decode), keyframe-safe window copies into concat-compatible segments,
// real scene detection through the select filter, and multi-output
// manifests.
// ---------------------------------------------------------------------------

// Tolerance for "boundary lands on a random-access point": below one frame
// duration at any sane rate, above time-base rounding noise.
constexpr std::int64_t kBoundaryTolUs = 1000;
// One audio frame of priming tolerance for window starts (an AAC frame at
// 44.1 kHz is ~23 ms; keeping one frame's worth of pre-boundary audio
// avoids dead air at segment heads).
constexpr std::int64_t kAudioPrimingTolUs = 25000;

// One demux pass over the video stream: keyframe presentation times
// (AV_PKT_FLAG_KEY — REAL random-access flags, never assumed), the last
// packet time and the container duration. Memory is bounded: only
// keyframe timestamps are retained (a whole-video frame list would violate
// the streaming requirement; this is packet metadata).
struct VideoTimeline {
  std::vector<std::int64_t> keyframeUs; // ascending
  std::int64_t lastPacketUs = -1;
  std::int64_t durationUs = 0;
  bool hasVideo = false;
};

bool scanVideoTimeline(const std::string &path, const CancelToken *cancel,
                       VideoTimeline &out, std::string &reason) {
  AVFormatContext *in = nullptr;
  if (avformat_open_input(&in, path.c_str(), nullptr, nullptr) != 0) {
    reason = "cannot open the source";
    return false;
  }
  if (avformat_find_stream_info(in, nullptr) < 0) {
    avformat_close_input(&in);
    reason = "cannot read stream info";
    return false;
  }
  if (in->duration != AV_NOPTS_VALUE && in->duration > 0) {
    out.durationUs = av_rescale_q(in->duration, AVRational{1, AV_TIME_BASE},
                                  AVRational{1, 1000000});
  }
  int videoStream = -1;
  for (unsigned i = 0; i < in->nb_streams; ++i) {
    if (in->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
        (in->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0) {
      videoStream = int(i);
      break;
    }
  }
  out.hasVideo = videoStream >= 0;
  if (videoStream >= 0) {
    const AVRational tb = in->streams[std::size_t(videoStream)]->time_base;
    AVPacket *pkt = av_packet_alloc();
    while (av_read_frame(in, pkt) >= 0) {
      if (cancel != nullptr && cancel->isCancelled()) {
        av_packet_unref(pkt);
        av_packet_free(&pkt);
        avformat_close_input(&in);
        reason = "cancelled";
        return false;
      }
      if (pkt->stream_index == videoStream && pkt->pts != AV_NOPTS_VALUE) {
        const std::int64_t us =
            av_rescale_q(pkt->pts, tb, AVRational{1, 1000000});
        out.lastPacketUs = std::max(out.lastPacketUs, us);
        if ((pkt->flags & AV_PKT_FLAG_KEY) != 0) {
          out.keyframeUs.push_back(us);
        }
      }
      av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
  }
  avformat_close_input(&in);
  return true;
}

// A boundary time lands on a random-access point (within rounding
// tolerance).
bool boundaryOnKeyframe(const VideoTimeline &timeline, std::int64_t us) {
  const auto it = std::lower_bound(timeline.keyframeUs.begin(),
                                   timeline.keyframeUs.end(),
                                   us - kBoundaryTolUs);
  return it != timeline.keyframeUs.end() && *it <= us + kBoundaryTolUs;
}

// A window START is copy-safe when the backward seek lands exactly on it:
// a keyframe exists at the boundary (the demuxer then begins AT the
// window, so no content before it leaks in — the honest exact start).
bool windowStartCopySafe(const VideoTimeline &timeline, std::int64_t us) {
  return timeline.hasVideo && boundaryOnKeyframe(timeline, us);
}

// A window END is copy-safe when the first packet at/after it is a
// keyframe (everything before it forms complete GOPs — no dangling
// references into the removed range) or no content exists at/after it
// (EOF: the window runs to the natural end).
bool windowEndCopySafe(const VideoTimeline &timeline, std::int64_t us) {
  if (!timeline.hasVideo) {
    return false;
  }
  if (boundaryOnKeyframe(timeline, us)) {
    return true;
  }
  return timeline.lastPacketUs >= 0 &&
         us > timeline.lastPacketUs + kBoundaryTolUs;
}

// Retained intervals = complement of the removed ranges over
// [0, durationUs] (normalized defensively here, exactly like the session
// manager and planner do; durationUs < 0 treats the tail as open-ended).
// Empty result = the removal covers the whole timeline.
std::vector<TimeRange> retainedRangesOf(const std::vector<TimeRange> &removed,
                                        std::int64_t durationUs) {
  std::vector<TimeRange> sorted = removed;
  std::sort(sorted.begin(), sorted.end(),
            [](const TimeRange &a, const TimeRange &b) {
              return a.startUs < b.startUs;
            });
  const std::int64_t totalUs =
      durationUs > 0 ? durationUs : (std::int64_t(1) << 40);
  std::vector<TimeRange> merged;
  for (const TimeRange &range : sorted) {
    if (!merged.empty() &&
        (range.startUs <= merged.back().endUs ||
         (merged.back().endUs < 0 && range.startUs <= totalUs))) {
      if (merged.back().endUs < 0 || range.endUs < 0) {
        merged.back().endUs = -1;
      } else {
        merged.back().endUs = std::max(merged.back().endUs, range.endUs);
      }
    } else {
      merged.push_back(range);
    }
  }
  std::vector<TimeRange> retained;
  std::int64_t cursor = 0;
  for (const TimeRange &range : merged) {
    const std::int64_t start = std::max<std::int64_t>(range.startUs, 0);
    const std::int64_t end = range.endUs < 0 ? totalUs : range.endUs;
    if (start > cursor) {
      retained.push_back(
          TimeRange{cursor, durationUs > 0 ? std::min(start, totalUs) : start});
    }
    cursor = std::max(cursor, end);
    if (cursor >= totalUs) {
      cursor = totalUs;
      break;
    }
  }
  if (cursor < totalUs) {
    retained.push_back(TimeRange{cursor, -1});
  }
  return retained;
}

// Packet-copies [startUs, endUs) (endUs < 0 = open-ended) into its own file
// with a REBASED, per-stream-zero timeline (concat-demuxer compatible:
// each segment starts at pts 0). Mirrors executeRemux's stream selection,
// BSF, metadata and write-error discipline; adds the window gates and the
// rebasing. Only called for keyframe-safe windows.
bool copyWindowSegment(const ExecuteParams &params,
                       const ExecutionIntent &intent,
                       const std::string &inPath, std::int64_t startUs,
                       std::int64_t endUs, const std::string &outPath,
                       bool includeAttachedPics, int &writeRc) {
  writeRc = 0;
  AVFormatContext *in = nullptr;
  AVFormatContext *out = nullptr;
  std::vector<AVBSFContext *> bitstreamFilters;
  AVPacket *pkt = av_packet_alloc();
  // Per output stream: rebase origins (first written packet's rescaled
  // pts/dts in the OUTPUT stream's time base).
  std::unordered_map<int, std::int64_t> basePts;
  std::unordered_map<int, std::int64_t> baseDts;
  bool cancelled = false;
  bool writeFailed = false;
  bool ok = false;
  do {
    if (avformat_open_input(&in, inPath.c_str(), nullptr, nullptr) != 0 ||
        avformat_find_stream_info(in, nullptr) < 0) {
      break;
    }
    if (avformat_alloc_output_context2(
            &out, nullptr, avFormatNameFor(params.pipeline.container),
            outPath.c_str()) < 0 ||
        out == nullptr) {
      break;
    }

    // Stream selection (the same policy as executeRemux: video when
    // wanted; the intent's audio ordinals; subtitles when preserved;
    // attached pictures only on window zero).
    std::vector<int> inIndexOfOut;
    int audioOrdinal = 0;
    for (unsigned i = 0; i < in->nb_streams; ++i) {
      const AVStream *inStream = in->streams[i];
      const AVMediaType type = inStream->codecpar->codec_type;
      bool keep = false;
      if (type == AVMEDIA_TYPE_VIDEO) {
        const bool isPic =
            (inStream->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0;
        keep = isPic ? (includeAttachedPics && intent.preserveCoverArt)
                     : intent.wantsVideo;
      } else if (type == AVMEDIA_TYPE_AUDIO) {
        if (audioCodecFromAv(inStream->codecpar->codec_id).has_value()) {
          keep = intent.wantsAudio &&
                 std::find(intent.audioSourceOrdinals.begin(),
                           intent.audioSourceOrdinals.end(),
                           audioOrdinal) !=
                     intent.audioSourceOrdinals.end();
          ++audioOrdinal;
        }
      } else if (type == AVMEDIA_TYPE_SUBTITLE) {
        keep = intent.subtitles.keepSubtitles;
      }
      if (!keep) {
        continue;
      }
      AVStream *outStream = avformat_new_stream(out, nullptr);
      if (outStream == nullptr) {
        continue;
      }
      avcodec_parameters_copy(outStream->codecpar, inStream->codecpar);
      outStream->codecpar->codec_tag = 0;
      // Container-mandated bitstream transformation (spec §31).
      const AVCodecID cid = inStream->codecpar->codec_id;
      const bool isTs = strcmp(out->oformat->name, "mpegts") == 0;
      if ((cid == AV_CODEC_ID_H264 || cid == AV_CODEC_ID_HEVC) && isTs) {
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
    if (inIndexOfOut.empty()) {
      break;
    }

    // Metadata copies through, then the familiar-field intentions apply.
    av_dict_copy(&out->metadata, in->metadata, AV_DICT_DONT_OVERWRITE);
    for (std::size_t k = 0;
         k < inIndexOfOut.size() && k < out->nb_streams; ++k) {
      av_dict_copy(&out->streams[k]->metadata,
                   in->streams[std::size_t(inIndexOfOut[k])]->metadata,
                   AV_DICT_DONT_OVERWRITE);
    }
    applyMetadataIntention(&out->metadata, intent.metadata);

    // Seek to the keyframe AT/BEFORE the window start (the caller verified
    // the start is keyframe-safe: the seek lands exactly on the boundary).
    if (startUs > 0) {
      int seekStream = 0;
      for (unsigned i = 0; i < in->nb_streams; ++i) {
        if (in->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
            (in->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0) {
          seekStream = int(i);
          break;
        }
      }
      const std::int64_t target = av_rescale_q(
          startUs, AVRational{1, 1000000},
          in->streams[std::size_t(seekStream)]->time_base);
      if (avformat_seek_file(in, seekStream, INT64_MIN, target, INT64_MAX,
                             AVSEEK_FLAG_BACKWARD) < 0) {
        break;
      }
    }

    if (!(out->oformat->flags & AVFMT_NOFILE)) {
      const int ioRc = avio_open(&out->pb, outPath.c_str(), AVIO_FLAG_WRITE);
      if (ioRc < 0) {
        writeRc = ioRc;
        break;
      }
    }
    if (avformat_write_header(out, nullptr) < 0) {
      writeRc = AVERROR(EIO);
      break;
    }

    const std::int64_t totalBytes =
        in->pb != nullptr ? avio_size(in->pb) : 0;
    std::int64_t processedPackets = 0;
    while (av_read_frame(in, pkt) >= 0) {
      if (params.cancel && params.cancel->isCancelled()) {
        cancelled = true;
        av_packet_unref(pkt);
        break;
      }
      int outIndex = -1;
      for (std::size_t k = 0; k < inIndexOfOut.size(); ++k) {
        if (inIndexOfOut[k] == pkt->stream_index) {
          outIndex = int(k);
          break;
        }
      }
      if (outIndex < 0) {
        av_packet_unref(pkt);
        continue;
      }
      const AVStream *srcStream = in->streams[std::size_t(pkt->stream_index)];
      const AVMediaType type = srcStream->codecpar->codec_type;
      const bool isPic =
          (srcStream->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0;
      std::int64_t pktUs = -1;
      if (pkt->pts != AV_NOPTS_VALUE) {
        pktUs = av_rescale_q(pkt->pts, srcStream->time_base,
                             AVRational{1, 1000000});
      } else if (pkt->dts != AV_NOPTS_VALUE) {
        pktUs = av_rescale_q(pkt->dts, srcStream->time_base,
                             AVRational{1, 1000000});
      }
      // Window gates. Timeline content: video strictly inside the window
      // (the start snapped to its keyframe; anything before is seek
      // overshoot), audio with one frame of priming tolerance, subtitle
      // cues that START before the end (they continue into the segment,
      // rebased and clamped to zero — the same policy as the transcode
      // path's subtitle lane). Attached pictures are metadata: copied
      // regardless of the window.
      if (!isPic) {
        if (type == AVMEDIA_TYPE_VIDEO) {
          if (pktUs >= 0 && pktUs < startUs - kBoundaryTolUs) {
            av_packet_unref(pkt);
            continue;
          }
          if (endUs >= 0 && pktUs >= endUs) {
            av_packet_unref(pkt);
            break; // past the window: packet copy is done
          }
        } else if (type == AVMEDIA_TYPE_AUDIO) {
          if (pktUs >= 0 && pktUs < startUs - kAudioPrimingTolUs) {
            av_packet_unref(pkt);
            continue;
          }
          if (endUs >= 0 && pktUs >= endUs) {
            av_packet_unref(pkt);
            break;
          }
        } else if (type == AVMEDIA_TYPE_SUBTITLE) {
          if (endUs >= 0 && pktUs >= endUs) {
            av_packet_unref(pkt);
            continue; // later windows own this cue
          }
        }
      }

      // BSF (mirrors executeRemux).
      AVPacket *toWrite = pkt;
      for (auto *bsfCtx : bitstreamFilters) {
        if (bsfCtx->par_in->codec_id == srcStream->codecpar->codec_id) {
          if (av_bsf_send_packet(bsfCtx, pkt) == 0) {
            AVPacket *filtered = av_packet_alloc();
            if (av_bsf_receive_packet(bsfCtx, filtered) == 0) {
              toWrite = filtered;
              av_packet_unref(pkt);
            } else {
              av_packet_free(&filtered);
            }
          }
          break;
        }
      }

      AVPacket *outPacket = av_packet_alloc();
      const int srcStreamIndex = toWrite->stream_index;
      av_packet_move_ref(outPacket, toWrite);
      if (toWrite != pkt) {
        av_packet_free(&toWrite); // filtered shell, now empty
      }
      outPacket->stream_index = outIndex;
      const AVStream *dstStream = out->streams[std::size_t(outIndex)];
      const AVRounding rounding = static_cast<AVRounding>(
          AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
      const bool rebase = !isPic;
      if (outPacket->pts != AV_NOPTS_VALUE) {
        std::int64_t pts =
            av_rescale_q_rnd(outPacket->pts, srcStream->time_base,
                             dstStream->time_base, rounding);
        if (rebase) {
          if (type == AVMEDIA_TYPE_SUBTITLE) {
            // Cue timing rebases by the window start (clamped at zero).
            const std::int64_t startDst = av_rescale_q(
                startUs, AVRational{1, 1000000}, dstStream->time_base);
            pts -= startDst;
            if (pts < 0) {
              pts = 0;
            }
          } else {
            const auto it = basePts.find(outIndex);
            if (it == basePts.end()) {
              basePts.emplace(outIndex, pts);
              pts = 0;
            } else {
              pts -= it->second;
              if (pts < 0) {
                pts = 0;
              }
            }
          }
        }
        outPacket->pts = pts;
      }
      if (outPacket->dts != AV_NOPTS_VALUE) {
        std::int64_t dts =
            av_rescale_q_rnd(outPacket->dts, srcStream->time_base,
                             dstStream->time_base, rounding);
        if (rebase && type != AVMEDIA_TYPE_SUBTITLE) {
          const auto it = baseDts.find(outIndex);
          if (it == baseDts.end()) {
            baseDts.emplace(outIndex, dts);
            dts = 0;
          } else {
            dts -= it->second;
            if (dts < 0) {
              dts = 0;
            }
          }
        }
        outPacket->dts = dts;
      }
      if (outPacket->duration > 0) {
        outPacket->duration =
            av_rescale_q(outPacket->duration, srcStream->time_base,
                         dstStream->time_base);
      }
      outPacket->pos = -1;
      (void)srcStreamIndex;
      const int frameRc = av_interleaved_write_frame(out, outPacket);
      if (frameRc < 0) {
        writeRc = frameRc;
        writeFailed = true;
      }
      av_packet_free(&outPacket);
      av_packet_unref(pkt);
      ++processedPackets;
      if (writeFailed) {
        break;
      }
      if (params.onProgress && totalBytes > 0 && (processedPackets % 64) == 0) {
        ProgressReport report;
        report.totalBytes = totalBytes;
        report.processedBytes = in->pb != nullptr ? avio_tell(in->pb) : 0;
        report.fraction = double(report.processedBytes) / double(totalBytes);
        params.onProgress(report);
      }
    }
    if (writeFailed || cancelled) {
      break;
    }
    av_write_trailer(out);
    ok = true;
  } while (false);
  for (auto *bsfCtx : bitstreamFilters) {
    av_bsf_free(&bsfCtx);
  }
  av_packet_free(&pkt);
  if (out != nullptr) {
    if (!(out->oformat->flags & AVFMT_NOFILE) && out->pb != nullptr) {
      avio_closep(&out->pb);
    }
    avformat_free_context(out);
  }
  if (in != nullptr) {
    avformat_close_input(&in);
  }
  if (!ok) {
    std::error_code ec;
    std::filesystem::remove(outPath, ec);
  }
  (void)cancelled; // the caller re-checks the token and maps Cancelled
  return ok;
}

// Real scene analysis (mandate §27.3): decode pass through
// select='gt(scene,THRESHOLD)' — the buffersink emits exactly the selected
// frames and their pts are the scene boundaries. Bounded memory: only
// timestamps are collected; frames flow through the graph one at a time.
bool detectSceneChanges(const std::string &path, double threshold,
                        const CancelToken *cancel,
                        std::vector<std::int64_t> &ptsUs, std::string &reason,
                        const std::function<bool()> *onFrame = nullptr) {
  AVFormatContext *in = nullptr;
  AVCodecContext *dec = nullptr;
  AVFilterGraph *graph = nullptr;
  AVFilterContext *src = nullptr;
  AVFilterContext *sink = nullptr;
  AVPacket *pkt = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  bool ok = false;
  do {
    if (avformat_open_input(&in, path.c_str(), nullptr, nullptr) != 0 ||
        avformat_find_stream_info(in, nullptr) < 0) {
      reason = "cannot open the source";
      break;
    }
    int videoStream = -1;
    for (unsigned i = 0; i < in->nb_streams; ++i) {
      if (in->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
          (in->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0) {
        videoStream = int(i);
        break;
      }
    }
    if (videoStream < 0) {
      reason = "the source has no video stream";
      break;
    }
    const AVCodec *decCodec =
        avcodec_find_decoder(in->streams[std::size_t(videoStream)]->codecpar->codec_id);
    if (decCodec == nullptr) {
      reason = "no decoder for the video stream";
      break;
    }
    dec = avcodec_alloc_context3(decCodec);
    avcodec_parameters_to_context(
        dec, in->streams[std::size_t(videoStream)]->codecpar);
    dec->pkt_timebase = in->streams[std::size_t(videoStream)]->time_base;
    if (dec->time_base.num <= 0 || dec->time_base.den <= 0) {
      dec->time_base = in->streams[std::size_t(videoStream)]->time_base;
    }
    if (avcodec_open2(dec, decCodec, nullptr) < 0) {
      reason = "cannot open the video decoder";
      break;
    }

    graph = avfilter_graph_alloc();
    const AVFilter *srcFilter = avfilter_get_by_name("buffer");
    const AVFilter *sinkFilter = avfilter_get_by_name("buffersink");
    if (graph == nullptr || srcFilter == nullptr || sinkFilter == nullptr) {
      reason = "cannot allocate the scene filter graph";
      break;
    }
    char srcArgs[512];
    AVRational sar = dec->sample_aspect_ratio;
    if (sar.num <= 0 || sar.den <= 0) {
      sar = AVRational{0, 1};
    }
    std::snprintf(srcArgs, sizeof(srcArgs),
                  "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d"
                  ":pixel_aspect=%d/%d",
                  dec->width, dec->height, dec->pix_fmt,
                  dec->time_base.num, dec->time_base.den, sar.num, sar.den);
    if (avfilter_graph_create_filter(&src, srcFilter, "in", srcArgs, nullptr,
                                     graph) < 0 ||
        avfilter_graph_create_filter(&sink, sinkFilter, "out", nullptr,
                                     nullptr, graph) < 0) {
      reason = "cannot create the scene filter endpoints";
      break;
    }
    char expr[64];
    std::snprintf(expr, sizeof(expr), "gt(scene,%.6f)", threshold);
    const std::string graphDesc = std::string("select='") + expr + "'";
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs = avfilter_inout_alloc();
    bool graphOk = outputs != nullptr && inputs != nullptr;
    if (graphOk) {
      outputs->name = av_strdup("in");
      outputs->filter_ctx = src;
      outputs->pad_idx = 0;
      outputs->next = nullptr;
      inputs->name = av_strdup("out");
      inputs->filter_ctx = sink;
      inputs->pad_idx = 0;
      inputs->next = nullptr;
      graphOk = avfilter_graph_parse_ptr(graph, graphDesc.c_str(), &inputs,
                                         &outputs, nullptr) >= 0;
    }
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (graphOk) {
      graphOk = avfilter_graph_config(graph, nullptr) >= 0;
    }
    if (!graphOk) {
      reason = "cannot configure the scene filter graph";
      break;
    }

    // Decode pass: every frame flows through the graph; the sink yields
    // exactly the scene-change frames.
    const AVRational sinkTb = av_buffersink_get_time_base(sink);
    bool cancelled = false;
    bool eof = false;
    while (!eof && !cancelled) {
      if (cancel != nullptr && cancel->isCancelled()) {
        cancelled = true;
        break;
      }
      int readRc = av_read_frame(in, pkt);
      if (readRc < 0) {
        // Demux EOF: drain the decoder, then the graph.
        eof = true;
        avcodec_send_packet(dec, nullptr);
      } else if (pkt->stream_index == videoStream) {
        if (avcodec_send_packet(dec, pkt) != 0) {
          av_packet_unref(pkt);
          continue;
        }
      }
      if (readRc >= 0) {
        av_packet_unref(pkt);
      }
      while (avcodec_receive_frame(dec, frame) == 0) {
        if (frame->pts != AV_NOPTS_VALUE) {
          av_buffersrc_add_frame_flags(src, frame,
                                       AV_BUFFERSRC_FLAG_KEEP_REF);
        } else {
          av_buffersrc_add_frame_flags(src, frame,
                                       AV_BUFFERSRC_FLAG_KEEP_REF);
        }
        AVFrame *selected = av_frame_alloc();
        while (av_buffersink_get_frame(sink, selected) >= 0) {
          if (selected->pts != AV_NOPTS_VALUE) {
            ptsUs.push_back(av_rescale_q(selected->pts, sinkTb,
                                         AVRational{1, 1000000}));
          }
          av_frame_unref(selected);
        }
        av_frame_free(&selected);
        av_frame_unref(frame);
        if (onFrame != nullptr && (*onFrame)()) {
          cancelled = true;
          break;
        }
      }
    }
    if (cancelled) {
      reason = "cancelled";
      break;
    }
    // Graph tail: the select filter holds one reference frame for the
    // score comparison; flush it.
    if (av_buffersrc_add_frame(src, nullptr) >= 0) {
      AVFrame *selected = av_frame_alloc();
      while (av_buffersink_get_frame(sink, selected) >= 0) {
        if (selected->pts != AV_NOPTS_VALUE) {
          ptsUs.push_back(av_rescale_q(selected->pts, sinkTb,
                                       AVRational{1, 1000000}));
        }
        av_frame_unref(selected);
      }
      av_frame_free(&selected);
    }
    std::sort(ptsUs.begin(), ptsUs.end());
    ok = true;
  } while (false);
  av_frame_free(&frame);
  av_packet_free(&pkt);
  avfilter_graph_free(&graph);
  avcodec_free_context(&dec);
  if (in != nullptr) {
    avformat_close_input(&in);
  }
  return ok;
}

// Chapter starts of a container, in µs (REAL AVFormatContext->chapters;
// empty when the container carries none).
std::vector<std::int64_t> chapterStartsUs(const AVFormatContext *fmt) {
  std::vector<std::int64_t> starts;
  for (unsigned i = 0; i < fmt->nb_chapters; ++i) {
    const AVChapter *chapter = fmt->chapters[i];
    if (chapter->start < 0) {
      continue;
    }
    starts.push_back(av_rescale_q(chapter->start, chapter->time_base,
                                  AVRational{1, 1000000}));
  }
  std::sort(starts.begin(), starts.end());
  return starts;
}

// Minimal JSON string escaping for the frames manifest.
std::string manifestJsonEscape(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (const char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
        break;
    }
  }
  return out;
}

// Writes a file with full-disk mapping (ENOSPC -> the caller maps it to
// the terminal StorageFull error through writeRc).
bool writeFileBytes(const std::string &path, const std::string &data,
                    int &writeRc) {
  writeRc = 0;
  std::FILE *f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) {
    writeRc = errno == ENOSPC ? AVERROR(ENOSPC) : AVERROR(EIO);
    return false;
  }
  const bool wrote = data.empty() || std::fwrite(data.data(), 1, data.size(), f) == data.size();
  const bool flushed = std::fflush(f) == 0;
  std::fclose(f);
  if (!wrote || !flushed) {
    if (errno == ENOSPC) {
      writeRc = AVERROR(ENOSPC);
    } else {
      writeRc = AVERROR(EIO);
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return false;
  }
  return true;
}

// Segment scratch directory for a job (stable across runs; unique enough
// for direct executor invocations without a job id).
std::string timelineScratchDir(const ExecuteParams &params) {
  static std::atomic<std::uint64_t> directSeq{0};
  const std::filesystem::path outFs(params.outputPath);
  std::string token = params.jobId;
  if (token.empty()) {
    token = "direct-" + std::to_string(::getpid()) + "-" +
            std::to_string(directSeq.fetch_add(1));
  }
  return (outFs.parent_path() / ("velmux-seg-" + token)).string();
}

// Wraps per-window progress into whole-job progress over the retained
// timeline (the segmented transcode uses the same pattern).
ProgressCallback windowProgressWrapper(const ExecuteParams &params,
                                        std::int64_t doneBeforeUs,
                                        std::int64_t windowLenUs,
                                        std::int64_t totalUs) {
  return [params, doneBeforeUs, windowLenUs, totalUs](
             const ProgressReport &report) {
    if (!params.onProgress || report.fraction < 0) {
      return;
    }
    ProgressReport global;
    const double local = std::clamp(report.fraction, 0.0, 1.0);
    global.processedDurationUs =
        doneBeforeUs + std::int64_t(local * double(windowLenUs));
    global.totalDurationUs = totalUs;
    global.fraction =
        totalUs > 0
            ? std::clamp(double(global.processedDurationUs) / double(totalUs),
                         0.0, 1.0)
            : -1.0;
    params.onProgress(global);
  };
}

// Commits one timeline checkpoint (Encode stage, presentation-time
// position, the finalized artifact). Returns the error path (empty = ok).
std::string commitTimelineCheckpoint(const ExecuteParams &params,
                                     std::int64_t positionUs,
                                     const std::string &artifactPath,
                                     bool &cancelledOut) {
  cancelledOut = false;
  if (params.checkpoints == nullptr) {
    return {};
  }
  backend::CheckpointInfo info;
  info.stage = StageId::Encode;
  info.position = positionUs;
  info.artifactPath = artifactPath;
  if (!params.checkpoints->commit(info)) {
    cancelledOut = params.cancel != nullptr && params.cancel->isCancelled();
    return cancelledOut ? "cancelled" : "checkpoint commit failed";
  }
  return {};
}

// Promotes a single finalized segment to the output path (rename with a
// copy fallback; the full-disk case surfaces as StorageFull).
Error promoteSingleFile(const std::string &from, const std::string &to) {
  std::error_code ec;
  std::filesystem::rename(from, to, ec);
  if (ec) {
    std::filesystem::copy_file(from, to, ec);
    if (ec) {
      return outputWriteError(
          ec == std::errc::no_space_on_device ? AVERROR(ENOSPC) : 0,
          "cannot promote the finished output", to);
    }
  }
  return {};
}

ExecuteResult FfmpegExecutor::executeCut(const ExecuteParams &params,
                                         const ExecutionIntent &intent) {
  ExecuteResult result;
  initFfmpegLabel();
  const std::optional<std::string> inPath = localPathFor(intent.source);
  if (!inPath.has_value()) {
    result.error = Error::make(ErrorCode::SourceUnavailable,
                               "unsupported source URI", "ffmpeg");
    return result;
  }

  // Removing nothing: the whole timeline, through the standard dispatch.
  if (intent.cutRemoved.empty()) {
    if (params.pipeline.videoStreamCopy && params.pipeline.audioStreamCopy) {
      return executeRemux(params, intent);
    }
    return executeTranscode(params, intent);
  }

  // Real keyframe timeline (packet flags, one demux pass).
  VideoTimeline timeline;
  {
    std::string reason;
    if (!scanVideoTimeline(*inPath, params.cancel.get(), timeline, reason)) {
      result.error = Error::make(ErrorCode::SourceUnavailable,
                                 "cannot analyze the source timeline: " +
                                     reason,
                                 "ffmpeg");
      return result;
    }
  }
  const std::int64_t durationUs = timeline.durationUs;
  const std::vector<TimeRange> retained =
      retainedRangesOf(intent.cutRemoved, durationUs);
  if (retained.empty()) {
    // Never remove the complete source (mandate §27.2; re-checked here
    // after defensive re-normalization).
    result.error = Error::make(
        ErrorCode::InvalidTimeRange,
        "cannot remove the entire timeline: at least one frame must remain",
        "ffmpeg");
    return result;
  }

  // Copy eligibility: a copy pipeline AND every retained boundary
  // keyframe-safe. A mixed copy/transcode concatenation would join
  // differently-parameterized streams (source extradata vs re-encoded
  // parameters) and produce broken output — the honest strategy is
  // uniform: all keyframe-safe copy, or all exact transcode.
  bool allCopy = params.pipeline.videoStreamCopy &&
                 params.pipeline.audioStreamCopy && timeline.hasVideo;
  if (allCopy) {
    for (const TimeRange &range : retained) {
      const std::int64_t end =
          range.endUs < 0 ? (durationUs > 0 ? durationUs : INT64_MAX)
                          : range.endUs;
      if (!windowStartCopySafe(timeline, range.startUs) ||
          !windowEndCopySafe(timeline, end)) {
        allCopy = false;
        break;
      }
    }
  }

  std::int64_t totalRetainedUs = 0;
  for (const TimeRange &range : retained) {
    const std::int64_t end =
        range.endUs < 0 ? (durationUs > 0 ? durationUs : INT64_MAX)
                        : range.endUs;
    totalRetainedUs += std::max<std::int64_t>(end - range.startUs, 0);
  }

  // Durable scratch + resume: checkpoints whose position matches a
  // retained boundary and whose artifacts are still valid cover that
  // range; everything else runs.
  const std::string segDir = timelineScratchDir(params);
  std::error_code ec;
  std::filesystem::create_directories(segDir, ec);
  const std::filesystem::path outFs(params.outputPath);
  struct CutSegment {
    std::int64_t endUs;
    std::string path;
  };
  std::vector<CutSegment> segments;
  std::vector<TimeRange> pending;
  {
    std::vector<backend::CheckpointInfo> reusable = params.resumeFrom;
    for (const TimeRange &range : retained) {
      const std::int64_t end =
          range.endUs < 0
              ? (durationUs > 0 ? durationUs : INT64_MAX)
              : std::min(range.endUs, durationUs > 0 ? durationUs : INT64_MAX);
      const std::string segPath =
          (std::filesystem::path(segDir) /
           ("cut-" + std::to_string(segments.size()) +
            outFs.extension().string()))
              .string();
      bool resumed = false;
      for (const auto &info : reusable) {
        if (info.position == end && !info.artifactPath.empty() &&
            segmentArtifactUsable(info.artifactPath)) {
          segments.push_back({end, info.artifactPath});
          resumed = true;
          break;
        }
      }
      if (!resumed) {
        pending.push_back(range);
      }
    }
  }

  std::int64_t doneUs = 0;
  for (const TimeRange &range : pending) {
    if (params.cancel && params.cancel->isCancelled()) {
      result.error = Error::make(ErrorCode::Cancelled,
                                 "cut cancelled (segments committed)",
                                 "ffmpeg");
      return result;
    }
    const std::int64_t start = std::max<std::int64_t>(range.startUs, 0);
    const std::int64_t end =
        range.endUs < 0 ? (durationUs > 0 ? durationUs : INT64_MAX)
                        : range.endUs;
    const std::int64_t windowLen = std::max<std::int64_t>(end - start, 1);
    const std::string segPath =
        (std::filesystem::path(segDir) /
         ("cut-" + std::to_string(segments.size()) +
          outFs.extension().string()))
            .string();
    ExecuteParams segParams = params;
    segParams.outputPath = segPath;
    segParams.onProgress =
        windowProgressWrapper(params, doneUs, windowLen, totalRetainedUs);
    ExecuteResult seg;
    if (allCopy) {
      int writeRc = 0;
      if (!copyWindowSegment(segParams, intent, *inPath, start, end, segPath,
                             start <= 0, writeRc)) {
        seg.error = writeRc != 0
                        ? outputWriteError(writeRc, "copy window failed",
                                           segPath)
                        : Error::make(ErrorCode::ExecutionFailed,
                                      "cannot copy the retained window",
                                      "ffmpeg");
      } else {
        seg.success = true;
      }
    } else {
      seg = executeTranscodeRange(segParams, intent, start, end, true, true);
    }
    if (!seg.success) {
      std::error_code removeEc;
      std::filesystem::remove(segPath, removeEc);
      return seg;
    }
    if (!segmentArtifactUsable(segPath)) {
      std::error_code removeEc;
      std::filesystem::remove(segPath, removeEc);
      result.error = Error::make(ErrorCode::ExecutionFailed,
                                 "cut segment failed artifact validation",
                                 "ffmpeg");
      return result;
    }
    bool checkpointCancelled = false;
    const std::string commitError = commitTimelineCheckpoint(
        params, end == INT64_MAX ? durationUs : end, segPath,
        checkpointCancelled);
    if (!commitError.empty()) {
      std::error_code removeEc;
      std::filesystem::remove(segPath, removeEc);
      result.error = Error::make(
          checkpointCancelled ? ErrorCode::Cancelled
                              : ErrorCode::ExecutionFailed,
          checkpointCancelled ? "cut cancelled (segments committed)"
                              : commitError,
          "ffmpeg");
      return result;
    }
    segments.push_back({end, segPath});
    doneUs += windowLen;
    if (params.onProgress) {
      ProgressReport report;
      report.processedDurationUs = doneUs;
      report.totalDurationUs = totalRetainedUs;
      report.fraction = totalRetainedUs > 0
                            ? std::clamp(double(doneUs) /
                                             double(totalRetainedUs),
                                         0.0, 1.0)
                            : -1.0;
      params.onProgress(report);
    }
  }

  // ONE continuous output (assembleSegments / promote a single window).
  if (segments.size() == 1) {
    const Error promoteError =
        promoteSingleFile(segments.front().path, params.outputPath);
    if (!promoteError.ok()) {
      result.error = promoteError;
      return result;
    }
  } else {
    std::vector<std::string> paths;
    paths.reserve(segments.size());
    for (const CutSegment &seg : segments) {
      paths.push_back(seg.path);
    }
    int asmRc = 0;
    if (!assembleSegments(paths, params.outputPath,
                          params.pipeline.container, asmRc)) {
      result.error = outputWriteError(asmRc,
                                      "cannot assemble the cut segments",
                                      params.outputPath);
      return result;
    }
  }
  std::filesystem::remove_all(segDir, ec);
  struct stat st{};
  if (::stat(params.outputPath.c_str(), &st) == 0) {
    result.bytesWritten = st.st_size;
  }
  result.success = true;
  result.pipelineSummary =
      "cut " + std::to_string(retained.size()) + " ranges" +
      (allCopy ? " (keyframe-safe copy)" : " (exact transcode)");
  return result;
}

ExecuteResult FfmpegExecutor::executeSplit(const ExecuteParams &params,
                                           const ExecutionIntent &intent) {
  ExecuteResult result;
  initFfmpegLabel();
  const std::optional<std::string> inPath = localPathFor(intent.source);
  if (!inPath.has_value()) {
    result.error = Error::make(ErrorCode::SourceUnavailable,
                               "unsupported source URI", "ffmpeg");
    return result;
  }
  const SplitRules &rules = intent.splitRules;

  // Probe: duration, size, chapters (real container markers).
  std::int64_t durationUs = 0;
  std::int64_t totalBytes = 0;
  std::vector<std::int64_t> chapterStarts;
  {
    AVFormatContext *probe = nullptr;
    if (avformat_open_input(&probe, inPath->c_str(), nullptr, nullptr) != 0 ||
        avformat_find_stream_info(probe, nullptr) < 0) {
      if (probe != nullptr) {
        avformat_close_input(&probe);
      }
      result.error = Error::make(ErrorCode::SourceUnavailable,
                                 "cannot open the source", "ffmpeg");
      return result;
    }
    if (probe->duration != AV_NOPTS_VALUE && probe->duration > 0) {
      durationUs = av_rescale_q(probe->duration, AVRational{1, AV_TIME_BASE},
                                AVRational{1, 1000000});
    }
    if (probe->pb != nullptr) {
      const std::int64_t size = avio_size(probe->pb);
      if (size > 0) {
        totalBytes = size;
      }
    }
    if (totalBytes <= 0) {
      struct stat st{};
      if (::stat(inPath->c_str(), &st) == 0) {
        totalBytes = st.st_size;
      }
    }
    chapterStarts = chapterStartsUs(probe);
    avformat_close_input(&probe);
  }

  // Boundaries: [0, ...interior..., duration]. Parts between consecutive
  // boundaries; the final part runs to the source end.
  std::vector<std::int64_t> boundaries{0};
  switch (rules.mode) {
    case SplitMode::EqualParts: {
      const std::int64_t parts = std::max<std::int64_t>(rules.partCount, 1);
      for (std::int64_t i = 1; i < parts; ++i) {
        boundaries.push_back(durationUs * i / parts);
      }
      break;
    }
    case SplitMode::Duration: {
      if (rules.partDurationUs > 0 && durationUs > 0) {
        const std::int64_t maxParts =
            std::max<std::int64_t>(1, durationUs / rules.partDurationUs + 1);
        for (std::int64_t t = rules.partDurationUs; t < durationUs; ++t) {
          boundaries.push_back(t);
          if (boundaries.size() > 4096) {
            break; // pathological request guard
          }
        }
        if (maxParts < 0) {
          result.error = Error::make(ErrorCode::InvalidArgument,
                                     "split duration too small", "ffmpeg");
          return result;
        }
      }
      break;
    }
    case SplitMode::MaxSize: {
      if (rules.maxSizeBytes > 0 && totalBytes > 0) {
        const std::int64_t parts = std::min<std::int64_t>(
            10000, std::max<std::int64_t>(
                       1, (totalBytes + rules.maxSizeBytes - 1) /
                              rules.maxSizeBytes));
        for (std::int64_t i = 1; i < parts; ++i) {
          boundaries.push_back(durationUs * i / parts);
        }
      }
      break;
    }
    case SplitMode::CustomTimestamps: {
      std::int64_t previous = 0;
      for (std::int64_t boundary : rules.boundaryUs) {
        if (boundary > previous &&
            (durationUs <= 0 || boundary < durationUs)) {
          boundaries.push_back(boundary);
          previous = boundary;
        }
      }
      break;
    }
    case SplitMode::Chapters: {
      if (chapterStarts.empty()) {
        // Honest infeasibility: nothing to split on.
        result.error = Error::make(ErrorCode::InvalidArgument,
                                   "source has no chapter markers", "ffmpeg");
        return result;
      }
      for (std::int64_t start : chapterStarts) {
        if (start > 0 && (durationUs <= 0 || start < durationUs)) {
          boundaries.push_back(start);
        }
      }
      break;
    }
    case SplitMode::Scenes: {
      std::vector<std::int64_t> sceneUs;
      std::string reason;
      if (!detectSceneChanges(*inPath, 0.3, params.cancel.get(), sceneUs,
                             reason)) {
        result.error = Error::make(ErrorCode::ExecutionFailed,
                                   "scene analysis failed: " + reason,
                                   "ffmpeg");
        return result;
      }
      for (std::int64_t us : sceneUs) {
        if (us > 0 && (durationUs <= 0 || us < durationUs)) {
          boundaries.push_back(us);
        }
      }
      if (boundaries.size() == 1) {
        result.error = Error::make(
            ErrorCode::ExecutionFailed,
            "no scene changes detected (threshold 0.3): nothing to split",
            "ffmpeg");
        return result;
      }
      break;
    }
  }
  if (durationUs > 0) {
    boundaries.push_back(durationUs);
  }
  std::sort(boundaries.begin(), boundaries.end());
  boundaries.erase(std::unique(boundaries.begin(), boundaries.end()),
                   boundaries.end());
  // Drop degenerate boundaries (<= 0 or >= duration).
  std::vector<std::int64_t> valid;
  for (std::int64_t b : boundaries) {
    if (b > 0 && (durationUs <= 0 || b < durationUs)) {
      valid.push_back(b);
    }
  }
  boundaries = valid;
  // The final part runs to the source end: its END boundary is the
  // duration itself (open-ended when the duration is unknown). Without
  // it the part loop below stops at the last interior boundary and
  // silently drops the timeline tail.
  boundaries.push_back(durationUs > 0 ? durationUs : -1);
  const std::size_t partCount = boundaries.size();

  // Real keyframe timeline for the per-part copy decision.
  VideoTimeline timeline;
  {
    std::string reason;
    if (!scanVideoTimeline(*inPath, params.cancel.get(), timeline, reason)) {
      result.error = Error::make(ErrorCode::SourceUnavailable,
                                 "cannot analyze the source timeline: " +
                                     reason,
                                 "ffmpeg");
      return result;
    }
  }

  // Part paths: <stem>_part_001.<ext> ... next to the planned output.
  const std::filesystem::path outFs(params.outputPath);
  const std::string stem = outFs.stem().string();
  const std::string ext = outFs.extension().string();
  const auto partPath = [&](std::size_t index) {
    char num[16];
    std::snprintf(num, sizeof(num), "%03zu", index + 1);
    return (outFs.parent_path() /
            (stem + "_part_" + num + ext))
        .string();
  };

  // Per-part copy decision + resume: a part whose checkpoint artifact is
  // still valid is not redone.
  const bool pipelineCopy = params.pipeline.videoStreamCopy &&
                            params.pipeline.audioStreamCopy;
  std::vector<std::pair<std::int64_t, std::string>> reusable = [](
      const ExecuteParams &p) {
    std::vector<std::pair<std::int64_t, std::string>> out;
    for (const auto &info : p.resumeFrom) {
      if (!info.artifactPath.empty()) {
        out.emplace_back(info.position, info.artifactPath);
      }
    }
    return out;
  }(params);

  std::vector<JobOutput> outputs;
  outputs.reserve(partCount);
  std::int64_t doneUs = 0;
  const std::int64_t totalUs = durationUs > 0 ? durationUs : 0;
  for (std::size_t i = 0; i < partCount; ++i) {
    const std::int64_t start = i == 0 ? 0 : boundaries[i - 1];
    const std::int64_t end = boundaries[i];
    const std::int64_t endForRun =
        end < 0 ? (durationUs > 0 ? durationUs : INT64_MAX) : end;
    const std::string path = partPath(i);
    const std::int64_t windowLen =
        std::max<std::int64_t>(endForRun - start, 1);

    // Resume: this part's artifact is already finalized and valid.
    bool resumed = false;
    for (const auto &[position, artifact] : reusable) {
      if (position == (end < 0 ? durationUs : end) &&
          segmentArtifactUsable(artifact)) {
        JobOutput out;
        out.path = artifact;
        struct stat st{};
        if (::stat(artifact.c_str(), &st) == 0) {
          out.sizeBytes = st.st_size;
        }
        out.kind = "media";
        outputs.push_back(std::move(out));
        resumed = true;
        break;
      }
    }
    if (resumed) {
      doneUs += windowLen;
      continue;
    }

    if (params.cancel && params.cancel->isCancelled()) {
      result.error = Error::make(ErrorCode::Cancelled,
                                 "split cancelled (parts committed)",
                                 "ffmpeg");
      return result;
    }

    // Per-part strategy: keyframe-safe copy window (independently
    // playable, own header) or exact transcode window.
    const bool partCopy = pipelineCopy &&
                          windowStartCopySafe(timeline, start) &&
                          windowEndCopySafe(timeline, endForRun);
    ExecuteParams partParams = params;
    partParams.outputPath = path;
    partParams.onProgress =
        windowProgressWrapper(params, doneUs, windowLen, totalUs);
    ExecuteResult part;
    if (partCopy) {
      int writeRc = 0;
      if (!copyWindowSegment(partParams, intent, *inPath, start, endForRun,
                             path, start <= 0, writeRc)) {
        part.error =
            writeRc != 0 ? outputWriteError(writeRc, "copy part failed", path)
                         : Error::make(ErrorCode::ExecutionFailed,
                                       "cannot copy the split part", "ffmpeg");
      } else {
        part.success = true;
      }
    } else {
      part = executeTranscodeRange(partParams, intent, start, endForRun, true,
                                   true);
    }
    if (!part.success) {
      std::error_code removeEc;
      std::filesystem::remove(path, removeEc);
      return part;
    }
    if (!segmentArtifactUsable(path)) {
      std::error_code removeEc;
      std::filesystem::remove(path, removeEc);
      result.error = Error::make(ErrorCode::ExecutionFailed,
                                 "split part failed artifact validation",
                                 "ffmpeg");
      return result;
    }
    bool checkpointCancelled = false;
    const std::string commitError = commitTimelineCheckpoint(
        params, end < 0 ? durationUs : end, path, checkpointCancelled);
    if (!commitError.empty()) {
      std::error_code removeEc;
      std::filesystem::remove(path, removeEc);
      result.error = Error::make(
          checkpointCancelled ? ErrorCode::Cancelled
                              : ErrorCode::ExecutionFailed,
          checkpointCancelled ? "split cancelled (parts committed)"
                              : commitError,
          "ffmpeg");
      return result;
    }
    JobOutput out;
    out.path = path;
    struct stat st{};
    if (::stat(path.c_str(), &st) == 0) {
      out.sizeBytes = st.st_size;
    }
    out.kind = "media";
    outputs.push_back(std::move(out));
    doneUs += windowLen;
    if (params.onProgress) {
      ProgressReport report;
      report.processedDurationUs = doneUs;
      report.totalDurationUs = totalUs;
      report.fraction = totalUs > 0
                            ? std::clamp(double(doneUs) / double(totalUs),
                                         0.0, 1.0)
                            : -1.0;
      params.onProgress(report);
    }
  }

  // MaxSize honesty (mandate §27.3): validate actual part sizes; a part
  // over the limit is a failure, never silent acceptance or fake
  // compression.
  if (rules.mode == SplitMode::MaxSize && rules.maxSizeBytes > 0) {
    std::int64_t smallest = INT64_MAX;
    for (const JobOutput &out : outputs) {
      if (out.sizeBytes > 0) {
        smallest = std::min(smallest, out.sizeBytes);
      }
    }
    for (const JobOutput &out : outputs) {
      if (out.sizeBytes > rules.maxSizeBytes) {
        result.error = Error::make(
            ErrorCode::ExecutionFailed,
            "cannot split under the size limit: part " + out.path + " is " +
                std::to_string(out.sizeBytes) + " bytes (limit " +
                std::to_string(rules.maxSizeBytes) +
                (smallest != INT64_MAX && smallest > rules.maxSizeBytes
                     ? "; even the smallest part exceeds the maximum"
                     : "") +
                "). Try a smaller part duration or a different strategy.",
            "ffmpeg");
        return result;
      }
    }
  }

  result.success = true;
  result.outputs = std::move(outputs);
  result.pipelineSummary = "split into " + std::to_string(partCount) +
                           " parts";
  return result;
}

// ---------------------------------------------------------------------------
// Merge (mandate §27.4 sequential, §27.5 spatial)
// ---------------------------------------------------------------------------

// REAL compatibility verification across merge sources: codec names alone
// are insufficient. Compares per-stream codec ids, profile/level, geometry,
// sample aspect, pixel format, average frame rate, audio shape, stream
// time bases, extradata BYTES, and the per-index (type, codec, language)
// layout that the concat demuxer's index mapping relies on (the mandate's
// "map by codec + language tag").
struct MergeCompatibility {
  bool ok = false;
  std::string reason;
};

bool mergeSourcesCompatible(const std::vector<AVFormatContext *> &sources,
                            MergeCompatibility &compat) {
  const AVFormatContext *first = sources.front();
  for (std::size_t s = 1; s < sources.size(); ++s) {
    const AVFormatContext *other = sources[s];
    if (other->nb_streams != first->nb_streams) {
      compat.reason = "the sources have different stream layouts";
      return false;
    }
    for (unsigned i = 0; i < first->nb_streams; ++i) {
      const AVStream *a = first->streams[i];
      const AVStream *b = other->streams[i];
      if (a->codecpar->codec_type != b->codecpar->codec_type ||
          a->codecpar->codec_id != b->codecpar->codec_id) {
        compat.reason = "stream layouts differ between the sources";
        return false;
      }
      const char *langA = "";
      const char *langB = "";
      if (const AVDictionaryEntry *e =
              av_dict_get(a->metadata, "language", nullptr, 0)) {
        langA = e->value;
      }
      if (const AVDictionaryEntry *e =
              av_dict_get(b->metadata, "language", nullptr, 0)) {
        langB = e->value;
      }
      if (strcmp(langA, langB) != 0) {
        compat.reason = "track languages differ between the sources";
        return false;
      }
      const bool isVideo = a->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
                           (a->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0;
      if (isVideo) {
        if (a->codecpar->profile != b->codecpar->profile) {
          compat.reason = "video codec profiles differ";
          return false;
        }
        if (a->codecpar->level != b->codecpar->level) {
          compat.reason = "video codec levels differ";
          return false;
        }
        if (a->codecpar->width != b->codecpar->width ||
            a->codecpar->height != b->codecpar->height) {
          compat.reason = "video resolutions differ";
          return false;
        }
        if (a->codecpar->format != b->codecpar->format) {
          compat.reason = "video pixel formats differ";
          return false;
        }
        if (a->codecpar->sample_aspect_ratio.num !=
                b->codecpar->sample_aspect_ratio.num ||
            a->codecpar->sample_aspect_ratio.den !=
                b->codecpar->sample_aspect_ratio.den) {
          compat.reason = "video sample aspects differ";
          return false;
        }
        if (a->avg_frame_rate.num != b->avg_frame_rate.num ||
            a->avg_frame_rate.den != b->avg_frame_rate.den) {
          compat.reason = "video frame rates differ";
          return false;
        }
      } else if (a->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
        if (a->codecpar->sample_rate != b->codecpar->sample_rate ||
            a->codecpar->ch_layout.nb_channels !=
                b->codecpar->ch_layout.nb_channels) {
          compat.reason = "audio formats differ";
          return false;
        }
      }
      if (a->time_base.num != b->time_base.num ||
          a->time_base.den != b->time_base.den) {
        compat.reason = "stream time bases differ";
        return false;
      }
      // Extradata compatibility for the concat path: byte-identical
      // parameter sets (different SPS/PPS across segments decode as
      // garbage even when the codec id matches).
      const bool extradataA =
          a->codecpar->extradata != nullptr && a->codecpar->extradata_size > 0;
      const bool extradataB =
          b->codecpar->extradata != nullptr && b->codecpar->extradata_size > 0;
      if (extradataA != extradataB ||
          (extradataA && (a->codecpar->extradata_size !=
                              b->codecpar->extradata_size ||
                          std::memcmp(a->codecpar->extradata,
                                      b->codecpar->extradata,
                                      std::size_t(a->codecpar->extradata_size)) != 0))) {
        compat.reason = "codec parameter sets (extradata) differ";
        return false;
      }
    }
  }
  compat.ok = true;
  compat.reason = "compatible";
  return true;
}

// One NORMALIZED merge segment: source -> scale+pad to the reference
// geometry (aspect preserved, never stretched) -> encoder; audio resampled
// to the reference shape (silence fill when the source lacks audio or its
// audio ends early); subtitle lanes copied with segment-local timing.
// Every segment starts at pts 0 with the SAME encoder parameters, so the
// concat assembly yields uniform streams.
bool transcodeMergeSegment(const ExecuteParams &params,
                           const ExecutionIntent &intent,
                           AVFormatContext *in, std::int64_t durationUs,
                           const std::string &segPath, std::int32_t targetW,
                           std::int32_t targetH, AVRational targetRate,
                           bool wantAudio, int audioInStream,
                           std::int32_t audioRate, std::int32_t audioChannels,
                           bool keepSubtitles, int &writeRc,
                           std::string &reason) {
  struct MergeCleanup {
    ~MergeCleanup() {
      av_frame_free(&frame);
      av_packet_free(&packet);
      avfilter_graph_free(&graph);
      avcodec_free_context(&videoDec);
      avcodec_free_context(&videoEnc);
      avcodec_free_context(&audioDec);
      avcodec_free_context(&audioEnc);
      swr_free(&swr);
      if (fifo != nullptr) {
        av_audio_fifo_free(fifo);
      }
      if (out != nullptr) {
        if (!(out->oformat->flags & AVFMT_NOFILE) && out->pb != nullptr) {
          avio_closep(&out->pb);
        }
        avformat_free_context(out);
      }
    }
    AVFormatContext *out = nullptr;
    AVCodecContext *videoDec = nullptr;
    AVCodecContext *videoEnc = nullptr;
    AVCodecContext *audioDec = nullptr;
    AVCodecContext *audioEnc = nullptr;
    SwrContext *swr = nullptr;
    AVAudioFifo *fifo = nullptr;
    AVFilterGraph *graph = nullptr;
    AVFrame *frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
  } cleanup;

  int videoStream = -1;
  for (unsigned i = 0; i < in->nb_streams; ++i) {
    if (in->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
        (in->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0) {
      videoStream = int(i);
      break;
    }
  }
  if (videoStream < 0 && !wantAudio && !keepSubtitles) {
    reason = "the source has no convertible streams";
    return false;
  }

  if (avformat_alloc_output_context2(&cleanup.out, nullptr,
                                     avFormatNameFor(params.pipeline.container),
                                     segPath.c_str()) < 0 ||
      cleanup.out == nullptr) {
    reason = "cannot allocate the segment output";
    return false;
  }

  int videoOut = -1;
  AVFilterContext *vsrc = nullptr;
  AVFilterContext *vsink = nullptr;
  std::int64_t videoFirstUs = AV_NOPTS_VALUE;
  std::int64_t lastVideoUs = 0;
  if (videoStream >= 0) {
    const AVCodec *decCodec =
        avcodec_find_decoder(in->streams[std::size_t(videoStream)]->codecpar->codec_id);
    if (decCodec == nullptr) {
      reason = "no decoder for the source video";
      return false;
    }
    cleanup.videoDec = avcodec_alloc_context3(decCodec);
    avcodec_parameters_to_context(
        cleanup.videoDec, in->streams[std::size_t(videoStream)]->codecpar);
    cleanup.videoDec->pkt_timebase =
        in->streams[std::size_t(videoStream)]->time_base;
    if (cleanup.videoDec->time_base.num <= 0 ||
        cleanup.videoDec->time_base.den <= 0) {
      cleanup.videoDec->time_base =
          in->streams[std::size_t(videoStream)]->time_base;
    }
    if (avcodec_open2(cleanup.videoDec, decCodec, nullptr) < 0) {
      reason = "cannot open the source video decoder";
      return false;
    }
    const char *encName = encoderNameFor(params.pipeline.videoCodec);
    const AVCodec *encCodec = encName != nullptr
                                  ? avcodec_find_encoder_by_name(encName)
                                  : nullptr;
    if (encCodec == nullptr) {
      encCodec = avcodec_find_encoder(avCodecFor(params.pipeline.videoCodec));
    }
    if (encCodec == nullptr) {
      reason = "no encoder for the normalized video";
      return false;
    }
    cleanup.videoEnc = avcodec_alloc_context3(encCodec);
    cleanup.videoEnc->width = targetW;
    cleanup.videoEnc->height = targetH;
    cleanup.videoEnc->pix_fmt = AV_PIX_FMT_YUV420P;
    if (targetRate.num > 0 && targetRate.den > 0) {
      cleanup.videoEnc->framerate = targetRate;
      cleanup.videoEnc->time_base = av_inv_q(targetRate);
    } else {
      cleanup.videoEnc->framerate = AVRational{30, 1};
      cleanup.videoEnc->time_base = AVRational{1, 30};
    }
    if (cleanup.out->oformat->flags & AVFMT_GLOBALHEADER) {
      cleanup.videoEnc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    if (intent.rate.bitrateMode && intent.rate.bitRate > 0) {
      cleanup.videoEnc->bit_rate = intent.rate.bitRate;
      cleanup.videoEnc->rc_max_rate = intent.rate.bitRate;
      cleanup.videoEnc->rc_buffer_size = intent.rate.bitRate * 2;
    } else {
      const int crf = qualityToCrf(params.pipeline.videoCodec,
                                   intent.rate.quality);
      av_opt_set(cleanup.videoEnc->priv_data, "crf",
                 std::to_string(crf).c_str(), 0);
      if (params.pipeline.videoCodec == VideoCodec::AV1) {
        av_opt_set(cleanup.videoEnc->priv_data, "preset", "8", 0);
      }
    }
    if (avcodec_open2(cleanup.videoEnc, encCodec, nullptr) < 0) {
      reason = "cannot open the normalized video encoder";
      return false;
    }
    videoOut = int(cleanup.out->nb_streams);
    AVStream *outVideo = avformat_new_stream(cleanup.out, nullptr);
    avcodec_parameters_from_context(outVideo->codecpar, cleanup.videoEnc);
    outVideo->time_base = cleanup.videoEnc->time_base;

    // Aspect-preserving fit into the reference geometry + black pad.
    cleanup.graph = avfilter_graph_alloc();
    const AVFilter *bufSrc = avfilter_get_by_name("buffer");
    const AVFilter *bufSink = avfilter_get_by_name("buffersink");
    if (cleanup.graph == nullptr || bufSrc == nullptr || bufSink == nullptr) {
      reason = "cannot allocate the normalization graph";
      return false;
    }
    AVRational sar = cleanup.videoDec->sample_aspect_ratio;
    if (sar.num <= 0 || sar.den <= 0) {
      sar = AVRational{0, 1};
    }
    char srcArgs[512];
    std::snprintf(srcArgs, sizeof(srcArgs),
                  "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d"
                  ":pixel_aspect=%d/%d",
                  cleanup.videoDec->width, cleanup.videoDec->height,
                  cleanup.videoDec->pix_fmt,
                  cleanup.videoDec->time_base.num,
                  cleanup.videoDec->time_base.den, sar.num, sar.den);
    if (avfilter_graph_create_filter(&vsrc, bufSrc, "in", srcArgs, nullptr,
                                     cleanup.graph) < 0 ||
        avfilter_graph_create_filter(&vsink, bufSink, "out", nullptr, nullptr,
                                     cleanup.graph) < 0) {
      reason = "cannot create the normalization endpoints";
      return false;
    }
    const enum AVPixelFormat sinkFmts[] = {AV_PIX_FMT_YUV420P,
                                           AV_PIX_FMT_NONE};
    if (av_opt_set_bin(vsink, "pix_fmts",
                       reinterpret_cast<const uint8_t *>(sinkFmts),
                       sizeof(sinkFmts) - sizeof(sinkFmts[0]),
                       AV_OPT_SEARCH_CHILDREN) < 0) {
      reason = "cannot constrain the normalization output format";
      return false;
    }
    const std::string graphDesc =
        "scale=w=" + std::to_string(targetW) + ":h=" + std::to_string(targetH) +
        ":force_original_aspect_ratio=decrease,pad=" + std::to_string(targetW) +
        ":" + std::to_string(targetH) +
        ":(ow-iw)/2:(oh-ih)/2:color=black,setsar=1";
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs = avfilter_inout_alloc();
    bool graphOk = outputs != nullptr && inputs != nullptr;
    if (graphOk) {
      outputs->name = av_strdup("in");
      outputs->filter_ctx = vsrc;
      outputs->pad_idx = 0;
      outputs->next = nullptr;
      inputs->name = av_strdup("out");
      inputs->filter_ctx = vsink;
      inputs->pad_idx = 0;
      inputs->next = nullptr;
      graphOk = avfilter_graph_parse_ptr(cleanup.graph, graphDesc.c_str(),
                                         &inputs, &outputs, nullptr) >= 0;
    }
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (graphOk) {
      graphOk = avfilter_graph_config(cleanup.graph, nullptr) >= 0;
    }
    if (!graphOk) {
      reason = "cannot configure the normalization graph (" + graphDesc + ")";
      return false;
    }
  }

  // Audio lane: decode + resample to the reference shape, or silence when
  // the source carries no audio.
  int audioOut = -1;
  std::int64_t audioSamplesOut = 0;
  const AVStream *audioInStreamRef = nullptr;
  if (wantAudio) {
    if (audioInStream >= 0) {
      audioInStreamRef = in->streams[std::size_t(audioInStream)];
      const AVCodec *decCodec =
          avcodec_find_decoder(audioInStreamRef->codecpar->codec_id);
      if (decCodec == nullptr) {
        reason = "no decoder for the source audio";
        return false;
      }
      cleanup.audioDec = avcodec_alloc_context3(decCodec);
      avcodec_parameters_to_context(cleanup.audioDec,
                                    audioInStreamRef->codecpar);
      if (avcodec_open2(cleanup.audioDec, decCodec, nullptr) < 0) {
        reason = "cannot open the source audio decoder";
        return false;
      }
    }
    const char *encName = encoderNameFor(params.pipeline.audioCodec);
    const AVCodec *encCodec = encName != nullptr
                                  ? avcodec_find_encoder_by_name(encName)
                                  : nullptr;
    if (encCodec == nullptr) {
      encCodec = avcodec_find_encoder(avCodecFor(params.pipeline.audioCodec));
    }
    if (encCodec == nullptr) {
      reason = "no encoder for the normalized audio";
      return false;
    }
    cleanup.audioEnc = avcodec_alloc_context3(encCodec);
    cleanup.audioEnc->sample_rate = audioRate;
    if (encCodec->supported_samplerates != nullptr) {
      bool supported = false;
      for (int i = 0; encCodec->supported_samplerates[i] != 0; ++i) {
        if (encCodec->supported_samplerates[i] == audioRate) {
          supported = true;
          break;
        }
      }
      if (!supported) {
        reason = "the normalized audio rate is not encodable";
        return false;
      }
    }
    av_channel_layout_default(&cleanup.audioEnc->ch_layout,
                              std::clamp(audioChannels, 1, 8));
    if (encCodec->sample_fmts != nullptr) {
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
    cleanup.audioEnc->bit_rate = 128000;
    if (cleanup.out->oformat->flags & AVFMT_GLOBALHEADER) {
      cleanup.audioEnc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    if (avcodec_open2(cleanup.audioEnc, encCodec, nullptr) < 0) {
      reason = "cannot open the normalized audio encoder";
      return false;
    }
    audioOut = int(cleanup.out->nb_streams);
    AVStream *outAudio = avformat_new_stream(cleanup.out, nullptr);
    avcodec_parameters_from_context(outAudio->codecpar, cleanup.audioEnc);
    outAudio->time_base = AVRational{1, cleanup.audioEnc->sample_rate};

    if (cleanup.audioDec != nullptr) {
      cleanup.swr = swr_alloc();
      AVChannelLayout inLayout;
      av_channel_layout_copy(&inLayout, &cleanup.audioDec->ch_layout);
      if (swr_alloc_set_opts2(&cleanup.swr, &cleanup.audioEnc->ch_layout,
                              cleanup.audioEnc->sample_fmt,
                              cleanup.audioEnc->sample_rate, &inLayout,
                              static_cast<AVSampleFormat>(
                                  cleanup.audioDec->sample_fmt),
                              cleanup.audioDec->sample_rate > 0
                                  ? cleanup.audioDec->sample_rate
                                  : audioRate,
                              0, nullptr) < 0) {
        av_channel_layout_uninit(&inLayout);
        swr_free(&cleanup.swr);
      } else {
        av_channel_layout_uninit(&inLayout);
      }
      if (cleanup.swr == nullptr || swr_init(cleanup.swr) < 0) {
        reason = "cannot create the audio resampler";
        return false;
      }
    }
    cleanup.fifo = av_audio_fifo_alloc(
        cleanup.audioEnc->sample_fmt, cleanup.audioEnc->ch_layout.nb_channels,
        1);
    if (cleanup.fifo == nullptr) {
      reason = "cannot create the audio fifo";
      return false;
    }
  }

  // Subtitle lanes: copied with segment-local timing (the concat assembly
  // then offsets them by each source's start — the "cue offsets per source
  // start" semantics).
  std::vector<std::pair<int, int>> subInOut;
  if (keepSubtitles) {
    for (unsigned i = 0; i < in->nb_streams; ++i) {
      if (in->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
        continue;
      }
      AVStream *outSub = avformat_new_stream(cleanup.out, nullptr);
      if (outSub == nullptr) {
        continue;
      }
      avcodec_parameters_copy(outSub->codecpar, in->streams[i]->codecpar);
      outSub->codecpar->codec_tag = 0;
      av_dict_copy(&outSub->metadata, in->streams[i]->metadata, 0);
      outSub->disposition = in->streams[i]->disposition;
      subInOut.emplace_back(int(i), int(cleanup.out->nb_streams) - 1);
    }
  }

  av_dict_copy(&cleanup.out->metadata, in->metadata, AV_DICT_DONT_OVERWRITE);
  if (!(cleanup.out->oformat->flags & AVFMT_NOFILE)) {
    const int ioRc = avio_open(&cleanup.out->pb, segPath.c_str(),
                               AVIO_FLAG_WRITE);
    if (ioRc < 0) {
      writeRc = ioRc;
      reason = "cannot open the segment output";
      return false;
    }
  }
  if (avformat_write_header(cleanup.out, nullptr) < 0) {
    writeRc = AVERROR(EIO);
    reason = "cannot write the segment header";
    return false;
  }

  bool writeFailed = false;
  const auto writePacket = [&](AVPacket *pkt, int outIndex) {
    pkt->stream_index = outIndex;
    const int rc = av_interleaved_write_frame(cleanup.out, pkt);
    if (rc < 0) {
      writeRc = rc;
      writeFailed = true;
    }
  };
  const auto encodeVideoFrame = [&](AVFrame *f) {
    if (avcodec_send_frame(cleanup.videoEnc, f) == 0) {
      AVPacket *encPacket = av_packet_alloc();
      while (avcodec_receive_packet(cleanup.videoEnc, encPacket) == 0) {
        av_packet_rescale_ts(encPacket, cleanup.videoEnc->time_base,
                             cleanup.out->streams[std::size_t(videoOut)]
                                 ->time_base);
        writePacket(encPacket, videoOut);
        av_packet_unref(encPacket);
      }
      av_packet_free(&encPacket);
    }
  };
  const auto feedAudioFifo = [&](bool flush) {
    if (cleanup.fifo == nullptr || cleanup.audioEnc == nullptr) {
      return;
    }
    const int frameSize = cleanup.audioEnc->frame_size;
    while (true) {
      const int have = av_audio_fifo_size(cleanup.fifo);
      int want = frameSize > 0 ? frameSize : have;
      if (want <= 0) {
        return;
      }
      if (have < want) {
        if (!flush || have == 0) {
          return;
        }
        want = have;
      }
      AVFrame *audioFrame = av_frame_alloc();
      audioFrame->format = cleanup.audioEnc->sample_fmt;
      audioFrame->sample_rate = cleanup.audioEnc->sample_rate;
      av_channel_layout_copy(&audioFrame->ch_layout,
                             &cleanup.audioEnc->ch_layout);
      audioFrame->nb_samples = want;
      if (av_frame_get_buffer(audioFrame, 0) < 0 ||
          av_audio_fifo_read(cleanup.fifo,
                             reinterpret_cast<void **>(audioFrame->data),
                             want) < want) {
        av_frame_free(&audioFrame);
        return;
      }
      audioFrame->pts = audioSamplesOut;
      audioSamplesOut += want;
      if (avcodec_send_frame(cleanup.audioEnc, audioFrame) == 0) {
        AVPacket *encPacket = av_packet_alloc();
        while (avcodec_receive_packet(cleanup.audioEnc, encPacket) == 0) {
          av_packet_rescale_ts(encPacket, cleanup.audioEnc->time_base,
                               cleanup.out->streams[std::size_t(audioOut)]
                                   ->time_base);
          writePacket(encPacket, audioOut);
          av_packet_unref(encPacket);
        }
        av_packet_free(&encPacket);
      }
      av_frame_free(&audioFrame);
      if (frameSize <= 0 || (flush && av_audio_fifo_size(cleanup.fifo) == 0)) {
        return;
      }
    }
  };
  const auto encodeAudioFrame = [&](AVFrame *srcFrame) {
    if (cleanup.swr == nullptr) {
      return;
    }
    const int maxOut = swr_get_out_samples(cleanup.swr, srcFrame->nb_samples);
    if (maxOut <= 0) {
      return;
    }
    uint8_t **buffer = nullptr;
    if (av_samples_alloc_array_and_samples(
            &buffer, nullptr, cleanup.audioEnc->ch_layout.nb_channels, maxOut,
            cleanup.audioEnc->sample_fmt, 0) < 0 ||
        buffer == nullptr) {
      return;
    }
    const int converted = swr_convert(
        cleanup.swr, buffer, maxOut,
        const_cast<const uint8_t **>(srcFrame->data), srcFrame->nb_samples);
    if (converted > 0 &&
        av_audio_fifo_write(cleanup.fifo, reinterpret_cast<void **>(buffer),
                            converted) < converted) {
      writeFailed = true;
    }
    if (buffer != nullptr) {
      av_freep(&buffer[0]);
      av_freep(&buffer);
    }
    feedAudioFifo(false);
  };

  const AVRational sinkTb =
      vsink != nullptr ? av_buffersink_get_time_base(vsink) : AVRational{0, 1};
  const AVRational decTb =
      cleanup.videoDec != nullptr ? cleanup.videoDec->time_base
                                  : AVRational{1, 1000000};
  bool cancelled = false;
  bool demuxEof = false;
  while (!writeFailed && !cancelled && !demuxEof) {
    if (params.cancel && params.cancel->isCancelled()) {
      cancelled = true;
      break;
    }
    if (av_read_frame(in, cleanup.packet) < 0) {
      demuxEof = true;
    }
    if (!demuxEof) {
      const int streamIndex = cleanup.packet->stream_index;
      if (streamIndex == videoStream && cleanup.videoDec != nullptr &&
          cleanup.videoEnc != nullptr) {
        if (avcodec_send_packet(cleanup.videoDec, cleanup.packet) == 0) {
          while (avcodec_receive_frame(cleanup.videoDec, cleanup.frame) == 0) {
            if (vsrc != nullptr && vsink != nullptr) {
              av_buffersrc_add_frame_flags(vsrc, cleanup.frame,
                                           AV_BUFFERSRC_FLAG_KEEP_REF);
              AVFrame *filt = av_frame_alloc();
              while (av_buffersink_get_frame(vsink, filt) >= 0) {
                std::int64_t graphUs = AV_NOPTS_VALUE;
                if (filt->pts != AV_NOPTS_VALUE) {
                  graphUs = av_rescale_q(filt->pts, sinkTb,
                                         AVRational{1, 1000000});
                }
                if (graphUs != AV_NOPTS_VALUE) {
                  if (videoFirstUs == AV_NOPTS_VALUE) {
                    videoFirstUs = graphUs;
                  }
                  const std::int64_t localUs =
                      std::max<std::int64_t>(0, graphUs - videoFirstUs);
                  lastVideoUs = std::max(lastVideoUs, localUs);
                  filt->pts = av_rescale_q(localUs, AVRational{1, 1000000},
                                           cleanup.videoEnc->time_base);
                }
                filt->pict_type = AV_PICTURE_TYPE_NONE;
                encodeVideoFrame(filt);
                av_frame_unref(filt);
              }
              av_frame_free(&filt);
            }
            av_frame_unref(cleanup.frame);
            if (params.onProgress && durationUs > 0) {
              ProgressReport report;
              report.processedDurationUs =
                  std::min(lastVideoUs, durationUs);
              report.totalDurationUs = durationUs;
              report.fraction = std::clamp(
                  double(report.processedDurationUs) / double(durationUs),
                  0.0, 1.0);
              params.onProgress(report);
            }
          }
        }
      } else if (wantAudio && cleanup.audioDec != nullptr &&
                 audioInStreamRef != nullptr &&
                 streamIndex == audioInStreamRef->index) {
        if (avcodec_send_packet(cleanup.audioDec, cleanup.packet) == 0) {
          while (avcodec_receive_frame(cleanup.audioDec, cleanup.frame) == 0) {
            encodeAudioFrame(cleanup.frame);
            av_frame_unref(cleanup.frame);
          }
        }
      } else {
        int subOut = -1;
        for (const auto &pair : subInOut) {
          if (pair.first == streamIndex) {
            subOut = pair.second;
            break;
          }
        }
        if (subOut >= 0) {
          AVPacket *outPacket = av_packet_alloc();
          av_packet_move_ref(outPacket, cleanup.packet);
          const AVStream *srcSub = in->streams[std::size_t(streamIndex)];
          const AVStream *dstSub =
              cleanup.out->streams[std::size_t(subOut)];
          const AVRounding rounding = static_cast<AVRounding>(
              AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
          if (outPacket->pts != AV_NOPTS_VALUE) {
            std::int64_t pts = av_rescale_q_rnd(outPacket->pts,
                                                srcSub->time_base,
                                                dstSub->time_base, rounding);
            if (pts < 0) {
              pts = 0;
            }
            outPacket->pts = pts;
          }
          if (outPacket->dts != AV_NOPTS_VALUE) {
            outPacket->dts = av_rescale_q_rnd(
                outPacket->dts, srcSub->time_base, dstSub->time_base, rounding);
            if (outPacket->dts < 0) {
              outPacket->dts = 0;
            }
          }
          if (outPacket->duration > 0) {
            outPacket->duration =
                av_rescale_q(outPacket->duration, srcSub->time_base,
                             dstSub->time_base);
          }
          outPacket->pos = -1;
          writePacket(outPacket, subOut);
          av_packet_free(&outPacket);
          av_packet_unref(cleanup.packet);
          continue;
        }
      }
      av_packet_unref(cleanup.packet);
    }
  }
  (void)decTb;
  if (cancelled) {
    reason = "cancelled";
    return false;
  }
  if (writeFailed) {
    return false;
  }

  // Drains: decoder -> graph -> encoder, then the audio tail.
  if (cleanup.videoDec != nullptr) {
    avcodec_send_packet(cleanup.videoDec, nullptr);
    while (avcodec_receive_frame(cleanup.videoDec, cleanup.frame) == 0) {
      if (vsrc != nullptr) {
        av_buffersrc_add_frame_flags(vsrc, cleanup.frame,
                                     AV_BUFFERSRC_FLAG_KEEP_REF);
      }
      av_frame_unref(cleanup.frame);
    }
    if (vsrc != nullptr) {
      av_buffersrc_add_frame(vsrc, nullptr);
      AVFrame *filt = av_frame_alloc();
      while (av_buffersink_get_frame(vsink, filt) >= 0) {
        std::int64_t graphUs = AV_NOPTS_VALUE;
        if (filt->pts != AV_NOPTS_VALUE) {
          graphUs =
              av_rescale_q(filt->pts, sinkTb, AVRational{1, 1000000});
        }
        if (graphUs != AV_NOPTS_VALUE) {
          if (videoFirstUs == AV_NOPTS_VALUE) {
            videoFirstUs = graphUs;
          }
          const std::int64_t localUs =
              std::max<std::int64_t>(0, graphUs - videoFirstUs);
          lastVideoUs = std::max(lastVideoUs, localUs);
          filt->pts = av_rescale_q(localUs, AVRational{1, 1000000},
                                   cleanup.videoEnc->time_base);
        }
        filt->pict_type = AV_PICTURE_TYPE_NONE;
        encodeVideoFrame(filt);
        av_frame_unref(filt);
      }
      av_frame_free(&filt);
    }
    avcodec_send_frame(cleanup.videoEnc, nullptr);
    AVPacket *encPacket = av_packet_alloc();
    while (avcodec_receive_packet(cleanup.videoEnc, encPacket) == 0) {
      av_packet_rescale_ts(encPacket, cleanup.videoEnc->time_base,
                           cleanup.out->streams[std::size_t(videoOut)]
                               ->time_base);
      writePacket(encPacket, videoOut);
      av_packet_unref(encPacket);
    }
    av_packet_free(&encPacket);
  }

  if (wantAudio) {
    if (cleanup.swr != nullptr && cleanup.fifo != nullptr) {
      while (true) {
        const int estimate = swr_get_out_samples(cleanup.swr, 0);
        if (estimate <= 0) {
          break;
        }
        uint8_t **buffer = nullptr;
        if (av_samples_alloc_array_and_samples(
                &buffer, nullptr, cleanup.audioEnc->ch_layout.nb_channels,
                estimate, cleanup.audioEnc->sample_fmt, 0) < 0 ||
            buffer == nullptr) {
          break;
        }
        const int converted =
            swr_convert(cleanup.swr, buffer, estimate, nullptr, 0);
        if (converted > 0) {
          av_audio_fifo_write(cleanup.fifo, reinterpret_cast<void **>(buffer),
                              converted);
        }
        av_freep(&buffer[0]);
        av_freep(&buffer);
        if (converted <= 0) {
          break;
        }
      }
    }
    // Silence fill: tracks align on the video timeline (mandate §27.4 —
    // missing audio in a source becomes an anullsrc-equivalent padded
    // track; a short audio tail extends with silence to the video end).
    if (durationUs > 0 || lastVideoUs > 0) {
      const std::int64_t targetSamples =
          std::max<std::int64_t>(
              lastVideoUs, cleanup.videoDec != nullptr ? 0 : durationUs) *
          cleanup.audioEnc->sample_rate / 1000000;
      while (audioSamplesOut < targetSamples && !writeFailed) {
        const int frameSize = cleanup.audioEnc->frame_size;
        const int want = frameSize > 0
                             ? std::min<int>(frameSize,
                                             int(targetSamples -
                                                 audioSamplesOut))
                             : int(targetSamples - audioSamplesOut);
        if (want <= 0) {
          break;
        }
        AVFrame *silence = av_frame_alloc();
        silence->format = cleanup.audioEnc->sample_fmt;
        silence->sample_rate = cleanup.audioEnc->sample_rate;
        av_channel_layout_copy(&silence->ch_layout,
                               &cleanup.audioEnc->ch_layout);
        silence->nb_samples = want;
        if (av_frame_get_buffer(silence, 0) < 0) {
          av_frame_free(&silence);
          break;
        }
        av_samples_set_silence(silence->data, 0, want,
                               cleanup.audioEnc->ch_layout.nb_channels,
                               cleanup.audioEnc->sample_fmt);
        silence->pts = audioSamplesOut;
        audioSamplesOut += want;
        if (avcodec_send_frame(cleanup.audioEnc, silence) == 0) {
          AVPacket *encPacket = av_packet_alloc();
          while (avcodec_receive_packet(cleanup.audioEnc, encPacket) == 0) {
            av_packet_rescale_ts(encPacket, cleanup.audioEnc->time_base,
                                 cleanup.out->streams[std::size_t(audioOut)]
                                     ->time_base);
            writePacket(encPacket, audioOut);
            av_packet_unref(encPacket);
          }
          av_packet_free(&encPacket);
        }
        av_frame_free(&silence);
      }
    }
    feedAudioFifo(true);
    avcodec_send_frame(cleanup.audioEnc, nullptr);
    AVPacket *encPacket = av_packet_alloc();
    while (avcodec_receive_packet(cleanup.audioEnc, encPacket) == 0) {
      av_packet_rescale_ts(encPacket, cleanup.audioEnc->time_base,
                           cleanup.out->streams[std::size_t(audioOut)]
                               ->time_base);
      writePacket(encPacket, audioOut);
      av_packet_unref(encPacket);
    }
    av_packet_free(&encPacket);
  }

  if (writeFailed) {
    return false;
  }
  av_write_trailer(cleanup.out);
  return true;
}

ExecuteResult FfmpegExecutor::executeMerge(const ExecuteParams &params,
                                           const ExecutionIntent &intent) {
  ExecuteResult result;
  initFfmpegLabel();
  const auto &merge = std::get<job::Merge>(params.request.payload);
  if (merge.sources.size() < 2) {
    result.error = Error::make(ErrorCode::InvalidArgument,
                               "merge needs at least two sources", "ffmpeg");
    return result;
  }
  if (merge.rules.layout != MergeLayout::Sequential) {
    return executeMergeSpatial(params, intent);
  }

  // Open every source (all demuxable — a readable container check).
  std::vector<AVFormatContext *> inputs;
  std::vector<std::string> paths;
  for (const MediaSource &source : merge.sources) {
    const std::optional<std::string> path = localPathFor(source);
    if (!path.has_value()) {
      result.error = Error::make(ErrorCode::SourceUnavailable,
                                 "unsupported merge source URI", "ffmpeg");
      break;
    }
    AVFormatContext *in = nullptr;
    if (avformat_open_input(&in, path->c_str(), nullptr, nullptr) != 0 ||
        avformat_find_stream_info(in, nullptr) < 0) {
      if (in != nullptr) {
        avformat_close_input(&in);
      }
      result.error =
          Error::make(ErrorCode::SourceUnavailable,
                      "cannot open merge source " + source.displayName,
                      "ffmpeg");
      break;
    }
    inputs.push_back(in);
    paths.push_back(*path);
  }
  if (!result.error.ok()) {
    for (AVFormatContext *in : inputs) {
      avformat_close_input(&in);
    }
    return result;
  }

  // Whole-job progress denominator: total duration.
  std::int64_t totalUs = 0;
  for (const AVFormatContext *in : inputs) {
    if (in->duration != AV_NOPTS_VALUE && in->duration > 0) {
      totalUs += av_rescale_q(in->duration, AVRational{1, AV_TIME_BASE},
                              AVRational{1, 1000000});
    }
  }

  MergeCompatibility compat;
  const bool concatCopy = params.pipeline.videoStreamCopy &&
                          params.pipeline.audioStreamCopy &&
                          mergeSourcesCompatible(inputs, compat);
  if (concatCopy) {
    // Packet concat through the concat demuxer (list file, safe=0,
    // escaped paths — the assembleSegments machinery).
    if (params.onProgress) {
      ProgressReport report;
      report.fraction = 0.0;
      report.totalDurationUs = totalUs;
      params.onProgress(report);
    }
    int writeRc = 0;
    if (!assembleSegments(paths, params.outputPath,
                          params.pipeline.container, writeRc)) {
      for (AVFormatContext *in : inputs) {
        avformat_close_input(&in);
      }
      result.error = outputWriteError(writeRc, "cannot concat the sources",
                                      params.outputPath);
      return result;
    }
    for (AVFormatContext *in : inputs) {
      avformat_close_input(&in);
    }
    if (params.onProgress) {
      ProgressReport report;
      report.fraction = 1.0;
      report.processedDurationUs = totalUs;
      report.totalDurationUs = totalUs;
      params.onProgress(report);
    }
    struct stat st{};
    if (::stat(params.outputPath.c_str(), &st) == 0) {
      result.bytesWritten = st.st_size;
    }
    result.success = true;
    result.pipelineSummary =
        "merge " + std::to_string(inputs.size()) + " sources (concat stream copy)";
    return result;
  }

  // Normalization: every source transcoded to the FIRST source's
  // characteristics (scale+pad, audio resample, silence fill), then the
  // segments concatenate.
  // Reference video: the first source's first non-pic video stream.
  const AVFormatContext *first = inputs.front();
  int refVideo = -1;
  for (unsigned i = 0; i < first->nb_streams; ++i) {
    if (first->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
        (first->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0) {
      refVideo = int(i);
      break;
    }
  }
  std::int32_t targetW = 0;
  std::int32_t targetH = 0;
  AVRational targetRate{30, 1};
  if (refVideo >= 0) {
    const AVStream *ref = first->streams[std::size_t(refVideo)];
    targetW = std::max(2, ref->codecpar->width & ~1);
    targetH = std::max(2, ref->codecpar->height & ~1);
    if (ref->avg_frame_rate.num > 0 && ref->avg_frame_rate.den > 0) {
      targetRate = ref->avg_frame_rate;
    }
  }
  // Audio reference: the first source carrying audio (missing audio in
  // other sources becomes silence — never a silent track drop).
  std::int32_t audioRate = 48000;
  std::int32_t audioChannels = 2;
  bool wantAudio = false;
  for (const AVFormatContext *in : inputs) {
    for (unsigned i = 0; i < in->nb_streams; ++i) {
      if (in->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
          audioCodecFromAv(in->streams[i]->codecpar->codec_id).has_value()) {
        audioRate = in->streams[i]->codecpar->sample_rate > 0
                        ? in->streams[i]->codecpar->sample_rate
                        : audioRate;
        audioChannels = std::clamp(
            in->streams[i]->codecpar->ch_layout.nb_channels, 1, 8);
        wantAudio = true;
        break;
      }
    }
    if (wantAudio) {
      break;
    }
  }
  // Subtitle policy: carry each source's subtitle lanes when their layout
  // matches the first source's (concat maps by index); drop conflicting
  // ones and report it in the summary.
  bool subsDropped = false;
  std::vector<bool> keepSubs(inputs.size(), false);
  if (first->nb_streams > 0) {
    std::vector<AVCodecID> firstSubs;
    for (unsigned i = 0; i < first->nb_streams; ++i) {
      if (first->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
        firstSubs.push_back(first->streams[i]->codecpar->codec_id);
      }
    }
    keepSubs[0] = !firstSubs.empty();
    for (std::size_t s = 1; s < inputs.size(); ++s) {
      std::vector<AVCodecID> subs;
      for (unsigned i = 0; i < inputs[s]->nb_streams; ++i) {
        if (inputs[s]->streams[i]->codecpar->codec_type ==
            AVMEDIA_TYPE_SUBTITLE) {
          subs.push_back(inputs[s]->streams[i]->codecpar->codec_id);
        }
      }
      if (subs == firstSubs) {
        keepSubs[s] = !subs.empty();
      } else if (!subs.empty()) {
        subsDropped = true;
      }
    }
  }

  const std::string segDir = timelineScratchDir(params);
  std::error_code ec;
  std::filesystem::create_directories(segDir, ec);
  const std::filesystem::path outFs(params.outputPath);
  std::vector<std::string> segmentPaths;
  std::int64_t doneUs = 0;
  for (std::size_t s = 0; s < inputs.size(); ++s) {
    if (params.cancel && params.cancel->isCancelled()) {
      for (AVFormatContext *in : inputs) {
        avformat_close_input(&in);
      }
      result.error = Error::make(ErrorCode::Cancelled,
                                 "merge cancelled (segments committed)",
                                 "ffmpeg");
      return result;
    }
    std::int64_t durationUs = 0;
    if (inputs[s]->duration != AV_NOPTS_VALUE && inputs[s]->duration > 0) {
      durationUs = av_rescale_q(inputs[s]->duration, AVRational{1, AV_TIME_BASE},
                                AVRational{1, 1000000});
    }
    int audioInStream = -1;
    if (wantAudio) {
      for (unsigned i = 0; i < inputs[s]->nb_streams; ++i) {
        if (inputs[s]->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
            audioCodecFromAv(inputs[s]->streams[i]->codecpar->codec_id)
                .has_value()) {
          audioInStream = int(i);
          break;
        }
      }
    }
    const std::string segPath =
        (std::filesystem::path(segDir) /
         ("merge-" + std::to_string(s) + outFs.extension().string()))
            .string();
    ExecuteParams segParams = params;
    segParams.outputPath = segPath;
    segParams.onProgress =
        windowProgressWrapper(params, doneUs, durationUs, totalUs);
    int writeRc = 0;
    std::string reason;
    if (!transcodeMergeSegment(segParams, intent, inputs[s], durationUs,
                               segPath, targetW, targetH, targetRate, wantAudio,
                               audioInStream, audioRate, audioChannels,
                               keepSubs[s], writeRc, reason)) {
      std::error_code removeEc;
      std::filesystem::remove(segPath, removeEc);
      for (AVFormatContext *in : inputs) {
        avformat_close_input(&in);
      }
      std::filesystem::remove_all(segDir, ec);
      result.error =
          reason == "cancelled"
              ? Error::make(ErrorCode::Cancelled, "merge cancelled", "ffmpeg")
              : writeRc != 0
                    ? outputWriteError(writeRc,
                                       "normalized merge segment failed",
                                       segPath)
                    : Error::make(ErrorCode::ExecutionFailed,
                                  "normalized merge segment failed: " + reason,
                                  "ffmpeg");
      return result;
    }
    if (!segmentArtifactUsable(segPath)) {
      std::error_code removeEc;
      std::filesystem::remove(segPath, removeEc);
      for (AVFormatContext *in : inputs) {
        avformat_close_input(&in);
      }
      result.error = Error::make(ErrorCode::ExecutionFailed,
                                 "merge segment failed artifact validation",
                                 "ffmpeg");
      return result;
    }
    bool checkpointCancelled = false;
    const std::string commitError = commitTimelineCheckpoint(
        params, s + 1, segPath, checkpointCancelled);
    if (!commitError.empty()) {
      std::error_code removeEc;
      std::filesystem::remove(segPath, removeEc);
      for (AVFormatContext *in : inputs) {
        avformat_close_input(&in);
      }
      result.error = Error::make(
          checkpointCancelled ? ErrorCode::Cancelled
                              : ErrorCode::ExecutionFailed,
          checkpointCancelled ? "merge cancelled (segments committed)"
                              : commitError,
          "ffmpeg");
      return result;
    }
    segmentPaths.push_back(segPath);
    doneUs += durationUs;
  }
  for (AVFormatContext *in : inputs) {
    avformat_close_input(&in);
  }

  if (segmentPaths.size() == 1) {
    const Error promoteError =
        promoteSingleFile(segmentPaths.front(), params.outputPath);
    if (!promoteError.ok()) {
      result.error = promoteError;
      return result;
    }
  } else {
    int asmRc = 0;
    if (!assembleSegments(segmentPaths, params.outputPath,
                          params.pipeline.container, asmRc)) {
      std::filesystem::remove_all(segDir, ec);
      result.error = outputWriteError(asmRc,
                                      "cannot assemble the merged segments",
                                      params.outputPath);
      return result;
    }
  }
  std::filesystem::remove_all(segDir, ec);
  struct stat st{};
  if (::stat(params.outputPath.c_str(), &st) == 0) {
    result.bytesWritten = st.st_size;
  }
  result.success = true;
  result.pipelineSummary =
      "merge " + std::to_string(inputs.size()) +
      " sources (normalized transcode" +
      (subsDropped ? "; later sources' conflicting subtitle tracks dropped)"
                   : ")");
  return result;
}

// Spatial merge (mandate §27.5): two sources, side-by-side or top-bottom
// composition with aspect-preserving scale + pad, duration policy (longer
// by default with a black-padded tail, shorter alternative), and audio
// selection (First/Second/Both with mixing headroom/None).
ExecuteResult FfmpegExecutor::executeMergeSpatial(
    const ExecuteParams &params, const ExecutionIntent &intent) {
  ExecuteResult result;
  initFfmpegLabel();
  const auto &merge = std::get<job::Merge>(params.request.payload);
  if (merge.sources.size() != 2) {
    result.error = Error::make(ErrorCode::InvalidArgument,
                               "spatial merge requires exactly two sources",
                               "ffmpeg");
    return result;
  }
  const bool sideBySide = merge.rules.layout == MergeLayout::SideBySide;
  const std::optional<std::string> path0 = localPathFor(merge.sources[0]);
  const std::optional<std::string> path1 = localPathFor(merge.sources[1]);
  if (!path0.has_value() || !path1.has_value()) {
    result.error = Error::make(ErrorCode::SourceUnavailable,
                               "unsupported merge source URI", "ffmpeg");
    return result;
  }

  struct SpatialCleanup {
    ~SpatialCleanup() {
      av_frame_free(&frame);
      av_packet_free(&packet);
      avfilter_graph_free(&graph);
      avfilter_graph_free(&audioGraph);
      avcodec_free_context(&dec0);
      avcodec_free_context(&dec1);
      avcodec_free_context(&videoEnc);
      avcodec_free_context(&audioDec0);
      avcodec_free_context(&audioDec1);
      avcodec_free_context(&audioEnc);
      swr_free(&swr0);
      swr_free(&swr1);
      if (fifo != nullptr) {
        av_audio_fifo_free(fifo);
      }
      if (out != nullptr) {
        if (!(out->oformat->flags & AVFMT_NOFILE) && out->pb != nullptr) {
          avio_closep(&out->pb);
        }
        avformat_free_context(out);
      }
      if (in0 != nullptr) {
        avformat_close_input(&in0);
      }
      if (in1 != nullptr) {
        avformat_close_input(&in1);
      }
    }
    AVFormatContext *in0 = nullptr;
    AVFormatContext *in1 = nullptr;
    AVFormatContext *out = nullptr;
    AVCodecContext *dec0 = nullptr;
    AVCodecContext *dec1 = nullptr;
    AVCodecContext *videoEnc = nullptr;
    AVCodecContext *audioDec0 = nullptr;
    AVCodecContext *audioDec1 = nullptr;
    AVCodecContext *audioEnc = nullptr;
    SwrContext *swr0 = nullptr;
    SwrContext *swr1 = nullptr;
    AVAudioFifo *fifo = nullptr;
    AVFilterGraph *graph = nullptr;
    AVFilterGraph *audioGraph = nullptr;
    AVFrame *frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
  } cleanup;

  std::string reason;
  const auto fail = [&](ErrorCode code, const std::string &msg) {
    result.error = Error::make(code, msg, "ffmpeg");
    return result;
  };
  if (avformat_open_input(&cleanup.in0, path0->c_str(), nullptr, nullptr) != 0 ||
      avformat_find_stream_info(cleanup.in0, nullptr) < 0 ||
      avformat_open_input(&cleanup.in1, path1->c_str(), nullptr, nullptr) != 0 ||
      avformat_find_stream_info(cleanup.in1, nullptr) < 0) {
    return fail(ErrorCode::SourceUnavailable, "cannot open the merge sources");
  }
  int videoIn0 = -1;
  int videoIn1 = -1;
  for (unsigned i = 0; i < cleanup.in0->nb_streams; ++i) {
    if (cleanup.in0->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
        (cleanup.in0->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0) {
      videoIn0 = int(i);
      break;
    }
  }
  for (unsigned i = 0; i < cleanup.in1->nb_streams; ++i) {
    if (cleanup.in1->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
        (cleanup.in1->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0) {
      videoIn1 = int(i);
      break;
    }
  }
  if (videoIn0 < 0 || videoIn1 < 0) {
    return fail(ErrorCode::InvalidArgument,
                "spatial merge requires decodable video in both sources");
  }
  std::int64_t duration0Us = 0;
  std::int64_t duration1Us = 0;
  if (cleanup.in0->duration != AV_NOPTS_VALUE && cleanup.in0->duration > 0) {
    duration0Us = av_rescale_q(cleanup.in0->duration, AVRational{1, AV_TIME_BASE},
                               AVRational{1, 1000000});
  }
  if (cleanup.in1->duration != AV_NOPTS_VALUE && cleanup.in1->duration > 0) {
    duration1Us = av_rescale_q(cleanup.in1->duration, AVRational{1, AV_TIME_BASE},
                               AVRational{1, 1000000});
  }
  const std::int64_t longerUs = std::max(duration0Us, duration1Us);
  const std::int64_t shorterUs = std::min(duration0Us, duration1Us);
  if (merge.rules.longerDuration && (duration0Us <= 0 || duration1Us <= 0)) {
    return fail(ErrorCode::InspectionFailed,
                "cannot determine both source durations for the spatial "
                "merge duration policy");
  }

  // Decoders.
  const auto openDec = [&](AVFormatContext *in, int streamIndex,
                           AVCodecContext *&dec) -> bool {
    const AVCodec *decCodec =
        avcodec_find_decoder(in->streams[std::size_t(streamIndex)]->codecpar->codec_id);
    if (decCodec == nullptr) {
      reason = "no decoder for the source video";
      return false;
    }
    dec = avcodec_alloc_context3(decCodec);
    avcodec_parameters_to_context(
        dec, in->streams[std::size_t(streamIndex)]->codecpar);
    dec->pkt_timebase = in->streams[std::size_t(streamIndex)]->time_base;
    if (dec->time_base.num <= 0 || dec->time_base.den <= 0) {
      dec->time_base = in->streams[std::size_t(streamIndex)]->time_base;
    }
    if (avcodec_open2(dec, decCodec, nullptr) < 0) {
      reason = "cannot open the source video decoder";
      return false;
    }
    return true;
  };
  if (!openDec(cleanup.in0, videoIn0, cleanup.dec0) ||
      !openDec(cleanup.in1, videoIn1, cleanup.dec1)) {
    return fail(ErrorCode::ExecutionFailed, reason);
  }

  // Composition geometry (mandate §27.5: common height = min for SBS,
  // common width = min for TB — never unnecessary upscaling). Each input
  // scales aspect-preserving into its slot and pads with black.
  const std::int32_t w0 = cleanup.dec0->width;
  const std::int32_t h0 = cleanup.dec0->height;
  const std::int32_t w1 = cleanup.dec1->width;
  const std::int32_t h1 = cleanup.dec1->height;
  std::int32_t totalW = 0;
  std::int32_t totalH = 0;
  std::int32_t slot0W = 0;
  std::int32_t slot0H = 0;
  std::int32_t slot1W = 0;
  std::int32_t slot1H = 0;
  if (sideBySide) {
    slot0H = slot1H = std::max(2, std::min(h0, h1) & ~1);
    slot0W = std::max(2, int(std::lround(double(w0) * slot0H / std::max(h0, 1))) & ~1);
    slot1W = std::max(2, int(std::lround(double(w1) * slot1H / std::max(h1, 1))) & ~1);
    totalW = slot0W + slot1W;
    totalH = slot0H;
  } else {
    slot0W = slot1W = std::max(2, std::min(w0, w1) & ~1);
    slot0H = std::max(2, int(std::lround(double(h0) * slot0W / std::max(w0, 1))) & ~1);
    slot1H = std::max(2, int(std::lround(double(h1) * slot1W / std::max(w1, 1))) & ~1);
    totalW = slot0W;
    totalH = slot0H + slot1H;
  }

  // Output + encoder.
  if (avformat_alloc_output_context2(&cleanup.out, nullptr,
                                     avFormatNameFor(params.pipeline.container),
                                     params.outputPath.c_str()) < 0 ||
      cleanup.out == nullptr) {
    return fail(ErrorCode::ExecutionFailed, "cannot allocate the output");
  }
  const char *encName = encoderNameFor(params.pipeline.videoCodec);
  const AVCodec *encCodec =
      encName != nullptr ? avcodec_find_encoder_by_name(encName) : nullptr;
  if (encCodec == nullptr) {
    encCodec = avcodec_find_encoder(avCodecFor(params.pipeline.videoCodec));
  }
  if (encCodec == nullptr) {
    return fail(ErrorCode::UnsupportedOperation,
                "no encoder for the spatial merge output");
  }
  cleanup.videoEnc = avcodec_alloc_context3(encCodec);
  cleanup.videoEnc->width = totalW;
  cleanup.videoEnc->height = totalH;
  cleanup.videoEnc->pix_fmt = AV_PIX_FMT_YUV420P;
  AVRational rate{30, 1};
  const AVStream *rateRef = cleanup.in0->streams[std::size_t(videoIn0)];
  if (rateRef->avg_frame_rate.num > 0 && rateRef->avg_frame_rate.den > 0) {
    rate = rateRef->avg_frame_rate;
  }
  cleanup.videoEnc->framerate = rate;
  cleanup.videoEnc->time_base = av_inv_q(rate);
  if (cleanup.out->oformat->flags & AVFMT_GLOBALHEADER) {
    cleanup.videoEnc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }
  if (intent.rate.bitrateMode && intent.rate.bitRate > 0) {
    cleanup.videoEnc->bit_rate = intent.rate.bitRate;
    cleanup.videoEnc->rc_max_rate = intent.rate.bitRate;
    cleanup.videoEnc->rc_buffer_size = intent.rate.bitRate * 2;
  } else {
    const int crf =
        qualityToCrf(params.pipeline.videoCodec, intent.rate.quality);
    av_opt_set(cleanup.videoEnc->priv_data, "crf",
               std::to_string(crf).c_str(), 0);
    if (params.pipeline.videoCodec == VideoCodec::AV1) {
      av_opt_set(cleanup.videoEnc->priv_data, "preset", "8", 0);
    }
  }
  if (avcodec_open2(cleanup.videoEnc, encCodec, nullptr) < 0) {
    return fail(ErrorCode::ExecutionFailed,
                "cannot open the spatial merge encoder");
  }
  const int videoOut = int(cleanup.out->nb_streams);
  AVStream *outVideo = avformat_new_stream(cleanup.out, nullptr);
  avcodec_parameters_from_context(outVideo->codecpar, cleanup.videoEnc);
  outVideo->time_base = cleanup.videoEnc->time_base;

  // Audio selection.
  const MergeAudioSelection audioSel = merge.rules.audio;
  int audioIn0 = -1;
  int audioIn1 = -1;
  for (unsigned i = 0; i < cleanup.in0->nb_streams; ++i) {
    if (cleanup.in0->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
        audioCodecFromAv(cleanup.in0->streams[i]->codecpar->codec_id).has_value()) {
      audioIn0 = int(i);
      break;
    }
  }
  for (unsigned i = 0; i < cleanup.in1->nb_streams; ++i) {
    if (cleanup.in1->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
        audioCodecFromAv(cleanup.in1->streams[i]->codecpar->codec_id).has_value()) {
      audioIn1 = int(i);
      break;
    }
  }
  const bool wantAudio = audioSel == MergeAudioSelection::First ||
                         audioSel == MergeAudioSelection::Second ||
                         audioSel == MergeAudioSelection::Both;
  if (audioSel == MergeAudioSelection::First && audioIn0 < 0) {
    return fail(ErrorCode::InvalidArgument,
                "audio selection First: the first source has no audio track");
  }
  if (audioSel == MergeAudioSelection::Second && audioIn1 < 0) {
    return fail(ErrorCode::InvalidArgument,
                "audio selection Second: the second source has no audio track");
  }
  if (audioSel == MergeAudioSelection::Both &&
      (audioIn0 < 0 || audioIn1 < 0)) {
    return fail(ErrorCode::InvalidArgument,
                "audio selection Both needs an audio track in both sources");
  }
  // Audio reference shape: the selected source (First/Second), else the
  // first source's.
  const AVStream *audioRef =
      audioSel == MergeAudioSelection::Second && audioIn1 >= 0
          ? cleanup.in1->streams[std::size_t(audioIn1)]
          : (audioIn0 >= 0 ? cleanup.in0->streams[std::size_t(audioIn0)]
                           : nullptr);
  const std::int32_t audioRate =
      audioRef != nullptr && audioRef->codecpar->sample_rate > 0
          ? audioRef->codecpar->sample_rate
          : 48000;
  const std::int32_t audioChannels =
      audioRef != nullptr
          ? std::clamp(audioRef->codecpar->ch_layout.nb_channels, 1, 2)
          : 2;

  int audioOut = -1;
  std::int64_t audioSamplesOut = 0;
  AVFilterContext *aSrc0 = nullptr;
  AVFilterContext *aSrc1 = nullptr;
  AVFilterContext *aSink = nullptr;
  if (wantAudio) {
    const char *aEncName = encoderNameFor(params.pipeline.audioCodec);
    const AVCodec *aEncCodec =
        aEncName != nullptr ? avcodec_find_encoder_by_name(aEncName) : nullptr;
    if (aEncCodec == nullptr) {
      aEncCodec = avcodec_find_encoder(avCodecFor(params.pipeline.audioCodec));
    }
    if (aEncCodec == nullptr) {
      return fail(ErrorCode::UnsupportedOperation,
                  "no encoder for the spatial merge audio");
    }
    cleanup.audioEnc = avcodec_alloc_context3(aEncCodec);
    cleanup.audioEnc->sample_rate = audioRate;
    if (aEncCodec->supported_samplerates != nullptr) {
      bool supported = false;
      for (int i = 0; aEncCodec->supported_samplerates[i] != 0; ++i) {
        if (aEncCodec->supported_samplerates[i] == audioRate) {
          supported = true;
          break;
        }
      }
      if (!supported) {
        return fail(ErrorCode::ExecutionFailed,
                    "the spatial merge audio rate is not encodable");
      }
    }
    av_channel_layout_default(&cleanup.audioEnc->ch_layout, audioChannels);
    if (aEncCodec->sample_fmts != nullptr) {
      cleanup.audioEnc->sample_fmt = aEncCodec->sample_fmts[0];
      for (int i = 0; aEncCodec->sample_fmts[i] != -1; ++i) {
        if (aEncCodec->sample_fmts[i] == AV_SAMPLE_FMT_FLTP) {
          cleanup.audioEnc->sample_fmt = AV_SAMPLE_FMT_FLTP;
          break;
        }
      }
    } else {
      cleanup.audioEnc->sample_fmt = AV_SAMPLE_FMT_FLTP;
    }
    cleanup.audioEnc->bit_rate = 128000;
    if (cleanup.out->oformat->flags & AVFMT_GLOBALHEADER) {
      cleanup.audioEnc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    if (avcodec_open2(cleanup.audioEnc, aEncCodec, nullptr) < 0) {
      return fail(ErrorCode::ExecutionFailed,
                  "cannot open the spatial merge audio encoder");
    }
    audioOut = int(cleanup.out->nb_streams);
    AVStream *outAudio = avformat_new_stream(cleanup.out, nullptr);
    avcodec_parameters_from_context(outAudio->codecpar, cleanup.audioEnc);
    outAudio->time_base = AVRational{1, cleanup.audioEnc->sample_rate};
    cleanup.fifo =
        av_audio_fifo_alloc(cleanup.audioEnc->sample_fmt, audioChannels, 1);
    if (cleanup.fifo == nullptr) {
      return fail(ErrorCode::ExecutionFailed, "cannot create the audio fifo");
    }
  }

  // Video composition graph: two aspect-fitting scale+pad chains, a
  // duration-policy pad/trim on the shorter/longer input, then hstack/
  // vstack.
  cleanup.graph = avfilter_graph_alloc();
  const AVFilter *bufSrc = avfilter_get_by_name("buffer");
  const AVFilter *bufSink = avfilter_get_by_name("buffersink");
  if (cleanup.graph == nullptr || bufSrc == nullptr || bufSink == nullptr) {
    return fail(ErrorCode::ExecutionFailed, "cannot allocate the composition graph");
  }
  const auto makeSrc = [&](AVCodecContext *dec, const char *name) {
    AVRational sar = dec->sample_aspect_ratio;
    if (sar.num <= 0 || sar.den <= 0) {
      sar = AVRational{0, 1};
    }
    char srcArgs[512];
    std::snprintf(srcArgs, sizeof(srcArgs),
                  "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d"
                  ":pixel_aspect=%d/%d",
                  dec->width, dec->height, dec->pix_fmt, dec->time_base.num,
                  dec->time_base.den, sar.num, sar.den);
    return avfilter_graph_create_filter(nullptr, bufSrc, name, srcArgs,
                                        nullptr, cleanup.graph) >= 0;
  };
  AVFilterContext *vsrc0 = nullptr;
  AVFilterContext *vsrc1 = nullptr;
  AVFilterContext *vsink = nullptr;
  {
    AVRational sar0 = cleanup.dec0->sample_aspect_ratio;
    if (sar0.num <= 0 || sar0.den <= 0) {
      sar0 = AVRational{0, 1};
    }
    char srcArgs0[512];
    std::snprintf(srcArgs0, sizeof(srcArgs0),
                  "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d"
                  ":pixel_aspect=%d/%d",
                  cleanup.dec0->width, cleanup.dec0->height,
                  cleanup.dec0->pix_fmt, cleanup.dec0->time_base.num,
                  cleanup.dec0->time_base.den, sar0.num, sar0.den);
    AVRational sar1 = cleanup.dec1->sample_aspect_ratio;
    if (sar1.num <= 0 || sar1.den <= 0) {
      sar1 = AVRational{0, 1};
    }
    char srcArgs1[512];
    std::snprintf(srcArgs1, sizeof(srcArgs1),
                  "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d"
                  ":pixel_aspect=%d/%d",
                  cleanup.dec1->width, cleanup.dec1->height,
                  cleanup.dec1->pix_fmt, cleanup.dec1->time_base.num,
                  cleanup.dec1->time_base.den, sar1.num, sar1.den);
    if (avfilter_graph_create_filter(&vsrc0, bufSrc, "in0", srcArgs0, nullptr,
                                     cleanup.graph) < 0 ||
        avfilter_graph_create_filter(&vsrc1, bufSrc, "in1", srcArgs1, nullptr,
                                     cleanup.graph) < 0 ||
        avfilter_graph_create_filter(&vsink, bufSink, "out", nullptr, nullptr,
                                     cleanup.graph) < 0) {
      return fail(ErrorCode::ExecutionFailed,
                  "cannot create the composition endpoints");
    }
  }
  (void)makeSrc;
  const enum AVPixelFormat sinkFmts[] = {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE};
  if (av_opt_set_bin(vsink, "pix_fmts",
                     reinterpret_cast<const uint8_t *>(sinkFmts),
                     sizeof(sinkFmts) - sizeof(sinkFmts[0]),
                     AV_OPT_SEARCH_CHILDREN) < 0) {
    return fail(ErrorCode::ExecutionFailed,
                "cannot constrain the composition output format");
  }
  const auto slotChain = [&](std::int32_t w, std::int32_t h) {
    return "scale=w=" + std::to_string(w) + ":h=" + std::to_string(h) +
           ":force_original_aspect_ratio=decrease,pad=" + std::to_string(w) +
           ":" + std::to_string(h) +
           ":(ow-iw)/2:(oh-ih)/2:color=black,setsar=1";
  };
  std::string chain0 = slotChain(slot0W, slot0H);
  std::string chain1 = slotChain(slot1W, slot1H);
  bool shortestPolicy = false;
  if (merge.rules.longerDuration) {
    // Black-pad the shorter input's tail to the longer duration (tpad).
    const double deltaSec =
        std::max<std::int64_t>(longerUs - shorterUs, 0) / 1000000.0;
    char padArgs[96];
    std::snprintf(padArgs, sizeof(padArgs),
                  ",tpad=stop_mode=add:color=black:stop_duration=%.3f",
                  deltaSec);
    if (duration0Us <= duration1Us) {
      chain0 += padArgs;
    } else {
      chain1 += padArgs;
    }
  } else if (duration0Us != duration1Us) {
    shortestPolicy = true;
  }
  const std::string stackName = sideBySide ? "hstack" : "vstack";
  const std::string graphDesc =
      "[in0]" + chain0 + "[x0];[in1]" + chain1 + "[x1];[x0][x1]" + stackName +
      "=inputs=2" + (shortestPolicy ? ":shortest=1" : "") + "[stack]";
  AVFilterInOut *gOutputs = avfilter_inout_alloc();
  AVFilterInOut *gInputs = avfilter_inout_alloc();
  bool graphOk = gOutputs != nullptr && gInputs != nullptr;
  if (graphOk) {
    gOutputs->name = av_strdup("in0");
    gOutputs->filter_ctx = vsrc0;
    gOutputs->pad_idx = 0;
    AVFilterInOut *second = avfilter_inout_alloc();
    second->name = av_strdup("in1");
    second->filter_ctx = vsrc1;
    second->pad_idx = 0;
    second->next = nullptr;
    gOutputs->next = second;
    gInputs->name = av_strdup("stack");
    gInputs->filter_ctx = vsink;
    gInputs->pad_idx = 0;
    gInputs->next = nullptr;
    graphOk = avfilter_graph_parse_ptr(cleanup.graph, graphDesc.c_str(),
                                       &gInputs, &gOutputs, nullptr) >= 0;
  }
  avfilter_inout_free(&gInputs);
  avfilter_inout_free(&gOutputs);
  if (graphOk) {
    graphOk = avfilter_graph_config(cleanup.graph, nullptr) >= 0;
  }
  if (!graphOk) {
    return fail(ErrorCode::ExecutionFailed,
                "cannot configure the composition graph (" + graphDesc + ")");
  }

  // Audio "Both": amix without normalization, both inputs pre-scaled by
  // 0.5 (clipping protection headroom).
  if (audioSel == MergeAudioSelection::Both) {
    cleanup.audioGraph = avfilter_graph_alloc();
    const AVFilter *abuf = avfilter_get_by_name("abuffer");
    const AVFilter *amix = avfilter_get_by_name("amix");
    const AVFilter *abufSink = avfilter_get_by_name("abuffersink");
    if (cleanup.audioGraph == nullptr || abuf == nullptr || amix == nullptr ||
        abufSink == nullptr) {
      return fail(ErrorCode::ExecutionFailed, "cannot allocate the mixing graph");
    }
    const auto abufferArgs = [&](AVFormatContext *in, int streamIndex,
                                 char *buf, std::size_t size) {
      const AVCodecParameters *par = in->streams[std::size_t(streamIndex)]->codecpar;
      char layout[64];
      AVChannelLayout chLayout;
      av_channel_layout_copy(&chLayout, &par->ch_layout);
      if (av_channel_layout_describe(&chLayout, layout, sizeof(layout)) < 0) {
        std::snprintf(layout, sizeof(layout), "stereo");
      }
      av_channel_layout_uninit(&chLayout);
      std::snprintf(buf, size,
                    "time_base=1/%d:sample_rate=%d:sample_fmt=%s"
                    ":channel_layout=%s",
                    par->sample_rate > 0 ? par->sample_rate : 48000,
                    par->sample_rate > 0 ? par->sample_rate : 48000,
                    av_get_sample_fmt_name(
                        static_cast<AVSampleFormat>(par->format)),
                    layout);
    };
    char args0[256];
    char args1[256];
    abufferArgs(cleanup.in0, audioIn0, args0, sizeof(args0));
    abufferArgs(cleanup.in1, audioIn1, args1, sizeof(args1));
    if (avfilter_graph_create_filter(&aSrc0, abuf, "a0", args0, nullptr,
                                     cleanup.audioGraph) < 0 ||
        avfilter_graph_create_filter(&aSrc1, abuf, "a1", args1, nullptr,
                                     cleanup.audioGraph) < 0 ||
        avfilter_graph_create_filter(&aSink, abufSink, "aout", nullptr,
                                     nullptr, cleanup.audioGraph) < 0) {
      return fail(ErrorCode::ExecutionFailed, "cannot create the mixing endpoints");
    }
    char layoutDesc[64];
    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, audioChannels);
    av_channel_layout_describe(&outLayout, layoutDesc, sizeof(layoutDesc));
    av_channel_layout_uninit(&outLayout);
    const std::string mixDesc =
        "[a0]aresample=" + std::to_string(audioRate) +
        ",aformat=sample_fmts=fltp:channel_layouts=" +
        std::string(layoutDesc) + "[m0];[a1]aresample=" +
        std::to_string(audioRate) + ",aformat=sample_fmts=fltp:channel_layouts=" +
        std::string(layoutDesc) + "[m1];[m0][m1]amix=inputs=2:normalize=0:"
        "weights=0.5 0.5[mout]";
    AVFilterInOut *aOuts = avfilter_inout_alloc();
    AVFilterInOut *aIns = avfilter_inout_alloc();
    bool aOk = aOuts != nullptr && aIns != nullptr;
    if (aOk) {
      aOuts->name = av_strdup("a0");
      aOuts->filter_ctx = aSrc0;
      aOuts->pad_idx = 0;
      AVFilterInOut *second = avfilter_inout_alloc();
      second->name = av_strdup("a1");
      second->filter_ctx = aSrc1;
      second->pad_idx = 0;
      second->next = nullptr;
      aOuts->next = second;
      aIns->name = av_strdup("mout");
      aIns->filter_ctx = aSink;
      aIns->pad_idx = 0;
      aIns->next = nullptr;
      aOk = avfilter_graph_parse_ptr(cleanup.audioGraph, mixDesc.c_str(),
                                     &aIns, &aOuts, nullptr) >= 0;
    }
    avfilter_inout_free(&aIns);
    avfilter_inout_free(&aOuts);
    if (aOk) {
      aOk = avfilter_graph_config(cleanup.audioGraph, nullptr) >= 0;
    }
    if (!aOk) {
      return fail(ErrorCode::ExecutionFailed, "cannot configure the mixing graph");
    }
  } else if (wantAudio) {
    // Single audio lane: resampler straight into the fifo.
    const AVStream *audioStream =
        audioSel == MergeAudioSelection::Second
            ? cleanup.in1->streams[std::size_t(audioIn1)]
            : cleanup.in0->streams[std::size_t(audioIn0)];
    SwrContext **swr =
        audioSel == MergeAudioSelection::Second ? &cleanup.swr1 : &cleanup.swr0;
    *swr = swr_alloc();
    AVChannelLayout inLayout;
    av_channel_layout_copy(&inLayout, &audioStream->codecpar->ch_layout);
    if (swr_alloc_set_opts2(swr, &cleanup.audioEnc->ch_layout,
                            cleanup.audioEnc->sample_fmt,
                            cleanup.audioEnc->sample_rate, &inLayout,
                            static_cast<AVSampleFormat>(
                                audioStream->codecpar->format),
                            audioStream->codecpar->sample_rate > 0
                                ? audioStream->codecpar->sample_rate
                                : audioRate,
                            0, nullptr) < 0) {
      av_channel_layout_uninit(&inLayout);
      swr_free(swr);
    } else {
      av_channel_layout_uninit(&inLayout);
    }
    if (*swr == nullptr || swr_init(*swr) < 0) {
      return fail(ErrorCode::ExecutionFailed,
                  "cannot create the spatial merge audio resampler");
    }
    const AVCodec *decCodec =
        avcodec_find_decoder(audioStream->codecpar->codec_id);
    AVCodecContext *&decCtx =
        audioSel == MergeAudioSelection::Second ? cleanup.audioDec1
                                                : cleanup.audioDec0;
    decCtx = decCodec != nullptr ? avcodec_alloc_context3(decCodec) : nullptr;
    if (decCtx == nullptr) {
      return fail(ErrorCode::ExecutionFailed, "no audio decoder");
    }
    avcodec_parameters_to_context(decCtx, audioStream->codecpar);
    if (avcodec_open2(decCtx, decCodec, nullptr) < 0) {
      return fail(ErrorCode::ExecutionFailed, "cannot open the audio decoder");
    }
  }
  if (audioSel == MergeAudioSelection::Both) {
    // Decoders for both lanes.
    for (int lane = 0; lane < 2; ++lane) {
      const AVStream *audioStream =
          lane == 0 ? cleanup.in0->streams[std::size_t(audioIn0)]
                    : cleanup.in1->streams[std::size_t(audioIn1)];
      const AVCodec *decCodec =
          avcodec_find_decoder(audioStream->codecpar->codec_id);
      AVCodecContext *&decCtx =
          lane == 0 ? cleanup.audioDec0 : cleanup.audioDec1;
      decCtx = decCodec != nullptr ? avcodec_alloc_context3(decCodec) : nullptr;
      if (decCtx == nullptr) {
        return fail(ErrorCode::ExecutionFailed, "no audio decoder");
      }
      avcodec_parameters_to_context(decCtx, audioStream->codecpar);
      if (avcodec_open2(decCtx, decCodec, nullptr) < 0) {
        return fail(ErrorCode::ExecutionFailed, "cannot open the audio decoder");
      }
    }
  }

  if (!(cleanup.out->oformat->flags & AVFMT_NOFILE)) {
    const int ioRc =
        avio_open(&cleanup.out->pb, params.outputPath.c_str(), AVIO_FLAG_WRITE);
    if (ioRc < 0) {
      result.error = outputWriteError(ioRc, "open output", params.outputPath);
      return result;
    }
  }
  if (avformat_write_header(cleanup.out, nullptr) < 0) {
    result.error =
        outputWriteError(AVERROR(EIO), "write header", params.outputPath);
    return result;
  }

  bool writeFailed = false;
  int writeRc = 0;
  const AVRational vSinkTb = av_buffersink_get_time_base(vsink);
  std::int64_t firstStackUs = AV_NOPTS_VALUE;
  std::int64_t lastStackUs = 0;
  const auto encodeVideoFrame = [&](AVFrame *f) {
    if (avcodec_send_frame(cleanup.videoEnc, f) == 0) {
      AVPacket *encPacket = av_packet_alloc();
      while (avcodec_receive_packet(cleanup.videoEnc, encPacket) == 0) {
        encPacket->stream_index = videoOut;
        av_packet_rescale_ts(encPacket, cleanup.videoEnc->time_base,
                             cleanup.out->streams[std::size_t(videoOut)]
                                 ->time_base);
        const int rc = av_interleaved_write_frame(cleanup.out, encPacket);
        if (rc < 0) {
          writeRc = rc;
          writeFailed = true;
        }
        av_packet_unref(encPacket);
      }
      av_packet_free(&encPacket);
    }
  };
  const auto drainVideoSink = [&]() {
    AVFrame *stacked = av_frame_alloc();
    while (av_buffersink_get_frame(vsink, stacked) >= 0) {
      std::int64_t us = AV_NOPTS_VALUE;
      if (stacked->pts != AV_NOPTS_VALUE) {
        us = av_rescale_q(stacked->pts, vSinkTb, AVRational{1, 1000000});
      }
      if (us != AV_NOPTS_VALUE) {
        if (firstStackUs == AV_NOPTS_VALUE) {
          firstStackUs = us;
        }
        lastStackUs = std::max(lastStackUs, us - firstStackUs);
        stacked->pts = av_rescale_q(std::max<std::int64_t>(0, us - firstStackUs),
                                    AVRational{1, 1000000},
                                    cleanup.videoEnc->time_base);
      }
      stacked->pict_type = AV_PICTURE_TYPE_NONE;
      encodeVideoFrame(stacked);
      av_frame_unref(stacked);
      if (params.onProgress && longerUs > 0 && lastStackUs > 0) {
        ProgressReport report;
        report.processedDurationUs = lastStackUs;
        report.totalDurationUs = longerUs;
        report.fraction =
            std::clamp(double(lastStackUs) / double(longerUs), 0.0, 1.0);
        params.onProgress(report);
      }
    }
    av_frame_free(&stacked);
  };
  const auto feedAudioFifo = [&](bool flush) {
    if (cleanup.fifo == nullptr || cleanup.audioEnc == nullptr) {
      return;
    }
    const int frameSize = cleanup.audioEnc->frame_size;
    while (true) {
      const int have = av_audio_fifo_size(cleanup.fifo);
      int want = frameSize > 0 ? frameSize : have;
      if (want <= 0) {
        return;
      }
      if (have < want) {
        if (!flush || have == 0) {
          return;
        }
        want = have;
      }
      AVFrame *audioFrame = av_frame_alloc();
      audioFrame->format = cleanup.audioEnc->sample_fmt;
      audioFrame->sample_rate = cleanup.audioEnc->sample_rate;
      av_channel_layout_copy(&audioFrame->ch_layout,
                             &cleanup.audioEnc->ch_layout);
      audioFrame->nb_samples = want;
      if (av_frame_get_buffer(audioFrame, 0) < 0 ||
          av_audio_fifo_read(cleanup.fifo,
                             reinterpret_cast<void **>(audioFrame->data),
                             want) < want) {
        av_frame_free(&audioFrame);
        return;
      }
      audioFrame->pts = audioSamplesOut;
      audioSamplesOut += want;
      if (avcodec_send_frame(cleanup.audioEnc, audioFrame) == 0) {
        AVPacket *encPacket = av_packet_alloc();
        while (avcodec_receive_packet(cleanup.audioEnc, encPacket) == 0) {
          encPacket->stream_index = audioOut;
          av_packet_rescale_ts(encPacket, cleanup.audioEnc->time_base,
                               cleanup.out->streams[std::size_t(audioOut)]
                                   ->time_base);
          const int rc = av_interleaved_write_frame(cleanup.out, encPacket);
          if (rc < 0) {
            writeRc = rc;
            writeFailed = true;
          }
          av_packet_unref(encPacket);
        }
        av_packet_free(&encPacket);
      }
      av_frame_free(&audioFrame);
      if (frameSize <= 0 ||
          (flush && av_audio_fifo_size(cleanup.fifo) == 0)) {
        return;
      }
    }
  };
  const auto pushAudioToLane = [&](int lane, AVFrame *decoded) {
    if (audioSel == MergeAudioSelection::Both) {
      AVFilterContext *src = lane == 0 ? aSrc0 : aSrc1;
      if (src != nullptr) {
        av_buffersrc_add_frame_flags(src, decoded, AV_BUFFERSRC_FLAG_KEEP_REF);
        AVFrame *mixed = av_frame_alloc();
        while (aSink != nullptr &&
               av_buffersink_get_frame(aSink, mixed) >= 0) {
          // Resample the mix output into the fifo.
          if (cleanup.swr0 == nullptr) {
            cleanup.swr0 = swr_alloc();
            AVChannelLayout inLayout;
            av_channel_layout_copy(&inLayout, &mixed->ch_layout);
            if (swr_alloc_set_opts2(&cleanup.swr0, &cleanup.audioEnc->ch_layout,
                                    cleanup.audioEnc->sample_fmt,
                                    cleanup.audioEnc->sample_rate, &inLayout,
                                    static_cast<AVSampleFormat>(mixed->format),
                                    mixed->sample_rate > 0
                                        ? mixed->sample_rate
                                        : cleanup.audioEnc->sample_rate,
                                    0, nullptr) < 0) {
              av_channel_layout_uninit(&inLayout);
              swr_free(&cleanup.swr0);
            } else {
              av_channel_layout_uninit(&inLayout);
            }
            if (cleanup.swr0 == nullptr || swr_init(cleanup.swr0) < 0) {
              av_frame_free(&mixed);
              return;
            }
          }
          const int maxOut = swr_get_out_samples(cleanup.swr0, mixed->nb_samples);
          if (maxOut > 0) {
            uint8_t **buffer = nullptr;
            if (av_samples_alloc_array_and_samples(
                    &buffer, nullptr, cleanup.audioEnc->ch_layout.nb_channels,
                    maxOut, cleanup.audioEnc->sample_fmt, 0) == 0 &&
                buffer != nullptr) {
              const int converted = swr_convert(
                  cleanup.swr0, buffer, maxOut,
                  const_cast<const uint8_t **>(mixed->data),
                  mixed->nb_samples);
              if (converted > 0 &&
                  av_audio_fifo_write(cleanup.fifo,
                                      reinterpret_cast<void **>(buffer),
                                      converted) < converted) {
                writeFailed = true;
              }
              av_freep(&buffer[0]);
              av_freep(&buffer);
            }
          }
          feedAudioFifo(false);
          av_frame_unref(mixed);
        }
        av_frame_free(&mixed);
      }
      return;
    }
    // Single lane: through this lane's resampler.
    SwrContext *resampler = lane == 0 ? cleanup.swr0 : cleanup.swr1;
    if (resampler == nullptr) {
      return;
    }
    const int maxOut = swr_get_out_samples(resampler, decoded->nb_samples);
    if (maxOut <= 0) {
      return;
    }
    uint8_t **buffer = nullptr;
    if (av_samples_alloc_array_and_samples(
            &buffer, nullptr, cleanup.audioEnc->ch_layout.nb_channels, maxOut,
            cleanup.audioEnc->sample_fmt, 0) < 0 ||
        buffer == nullptr) {
      return;
    }
    const int converted = swr_convert(
        resampler, buffer, maxOut,
        const_cast<const uint8_t **>(decoded->data), decoded->nb_samples);
    if (converted > 0 &&
        av_audio_fifo_write(cleanup.fifo, reinterpret_cast<void **>(buffer),
                            converted) < converted) {
      writeFailed = true;
    }
    av_freep(&buffer[0]);
    av_freep(&buffer);
    feedAudioFifo(false);
  };

  // Central loop: pump both inputs, decode, feed the composition graph.
  bool cancelled = false;
  bool eof0 = false;
  bool eof1 = false;
  while ((!eof0 || !eof1) && !writeFailed && !cancelled) {
    if (params.cancel && params.cancel->isCancelled()) {
      cancelled = true;
      break;
    }
    for (int lane = 0; lane < 2 && !writeFailed; ++lane) {
      AVFormatContext *in = lane == 0 ? cleanup.in0 : cleanup.in1;
      bool &eof = lane == 0 ? eof0 : eof1;
      int videoIn = lane == 0 ? videoIn0 : videoIn1;
      int audioIn = lane == 0 ? audioIn0 : audioIn1;
      AVCodecContext *videoDec = lane == 0 ? cleanup.dec0 : cleanup.dec1;
      AVCodecContext *audioDec =
          lane == 0 ? cleanup.audioDec0 : cleanup.audioDec1;
      if (eof) {
        continue;
      }
      if (av_read_frame(in, cleanup.packet) < 0) {
        eof = true;
        avcodec_send_packet(videoDec, nullptr);
        while (avcodec_receive_frame(videoDec, cleanup.frame) == 0) {
          av_buffersrc_add_frame_flags(lane == 0 ? vsrc0 : vsrc1,
                                       cleanup.frame,
                                       AV_BUFFERSRC_FLAG_KEEP_REF);
          av_frame_unref(cleanup.frame);
        }
        if (audioDec != nullptr) {
          avcodec_send_packet(audioDec, nullptr);
          while (avcodec_receive_frame(audioDec, cleanup.frame) == 0) {
            pushAudioToLane(lane, cleanup.frame);
            av_frame_unref(cleanup.frame);
          }
        }
        av_buffersrc_add_frame(lane == 0 ? vsrc0 : vsrc1, nullptr);
        continue;
      }
      if (cleanup.packet->stream_index == videoIn) {
        if (avcodec_send_packet(videoDec, cleanup.packet) == 0) {
          while (avcodec_receive_frame(videoDec, cleanup.frame) == 0) {
            av_buffersrc_add_frame_flags(lane == 0 ? vsrc0 : vsrc1,
                                         cleanup.frame,
                                         AV_BUFFERSRC_FLAG_KEEP_REF);
            av_frame_unref(cleanup.frame);
            drainVideoSink();
          }
        }
      } else if (wantAudio && cleanup.packet->stream_index == audioIn &&
                 audioDec != nullptr) {
        if (avcodec_send_packet(audioDec, cleanup.packet) == 0) {
          while (avcodec_receive_frame(audioDec, cleanup.frame) == 0) {
            pushAudioToLane(lane, cleanup.frame);
            av_frame_unref(cleanup.frame);
          }
        }
      }
      av_packet_unref(cleanup.packet);
      drainVideoSink();
    }
  }
  if (cancelled) {
    result.error = Error::make(ErrorCode::Cancelled, "merge cancelled", "ffmpeg");
    return result;
  }
  if (writeFailed) {
    result.error = outputWriteError(writeRc, "write failed during merge",
                                    params.outputPath);
    return result;
  }

  // Drains: graph tails, then the encoders.
  drainVideoSink();
  avcodec_send_frame(cleanup.videoEnc, nullptr);
  {
    AVPacket *encPacket = av_packet_alloc();
    while (avcodec_receive_packet(cleanup.videoEnc, encPacket) == 0) {
      encPacket->stream_index = videoOut;
      av_packet_rescale_ts(encPacket, cleanup.videoEnc->time_base,
                           cleanup.out->streams[std::size_t(videoOut)]->time_base);
      const int rc = av_interleaved_write_frame(cleanup.out, encPacket);
      if (rc < 0) {
        writeRc = rc;
        writeFailed = true;
      }
      av_packet_unref(encPacket);
    }
    av_packet_free(&encPacket);
  }
  if (wantAudio) {
    if (audioSel == MergeAudioSelection::Both && aSrc0 != nullptr) {
      av_buffersrc_add_frame(aSrc0, nullptr);
      av_buffersrc_add_frame(aSrc1, nullptr);
      AVFrame *mixed = av_frame_alloc();
      while (aSink != nullptr && av_buffersink_get_frame(aSink, mixed) >= 0) {
        if (cleanup.swr0 != nullptr) {
          const int maxOut = swr_get_out_samples(cleanup.swr0, mixed->nb_samples);
          if (maxOut > 0) {
            uint8_t **buffer = nullptr;
            if (av_samples_alloc_array_and_samples(
                    &buffer, nullptr, cleanup.audioEnc->ch_layout.nb_channels,
                    maxOut, cleanup.audioEnc->sample_fmt, 0) == 0 &&
                buffer != nullptr) {
              const int converted = swr_convert(
                  cleanup.swr0, buffer, maxOut,
                  const_cast<const uint8_t **>(mixed->data),
                  mixed->nb_samples);
              if (converted > 0) {
                av_audio_fifo_write(cleanup.fifo,
                                    reinterpret_cast<void **>(buffer),
                                    converted);
              }
              av_freep(&buffer[0]);
              av_freep(&buffer);
            }
          }
        }
        av_frame_unref(mixed);
      }
      av_frame_free(&mixed);
    }
    feedAudioFifo(true);
    avcodec_send_frame(cleanup.audioEnc, nullptr);
    AVPacket *encPacket = av_packet_alloc();
    while (avcodec_receive_packet(cleanup.audioEnc, encPacket) == 0) {
      encPacket->stream_index = audioOut;
      av_packet_rescale_ts(encPacket, cleanup.audioEnc->time_base,
                           cleanup.out->streams[std::size_t(audioOut)]
                               ->time_base);
      const int rc = av_interleaved_write_frame(cleanup.out, encPacket);
      if (rc < 0) {
        writeRc = rc;
        writeFailed = true;
      }
      av_packet_unref(encPacket);
    }
    av_packet_free(&encPacket);
  }
  if (writeFailed) {
    result.error = outputWriteError(writeRc, "write failed during merge",
                                    params.outputPath);
    return result;
  }
  av_write_trailer(cleanup.out);
  struct stat st{};
  if (::stat(params.outputPath.c_str(), &st) == 0) {
    result.bytesWritten = st.st_size;
  }
  result.success = true;
  result.pipelineSummary = std::string("merge ") +
                           (sideBySide ? "side-by-side" : "top-bottom") +
                           " (2 sources" +
                           (audioSel == MergeAudioSelection::None
                                ? ", no audio)"
                                : audioSel == MergeAudioSelection::Both
                                      ? ", mixed audio)"
                                      : ")");
  return result;
}

// ---------------------------------------------------------------------------
// FfmpegBackend
// ---------------------------------------------------------------------------

// Velmux frame extraction (mandate §29): real decoded frame identity,
// JPEG/PNG/still-WebP output, orientation resolution, manifest.
// Appended into velmux::ffmpeg (ffmpeg_backend.cpp) before
// FfmpegBackend::initialize().
ExecuteResult FfmpegExecutor::executeExtractFrames(
    const ExecuteParams &params, const ExecutionIntent &intent) {
  ExecuteResult result;
  const auto &payload = std::get<job::ExtractFrames>(params.request.payload);
  const FrameSelection &selection = intent.frameSelection;
  const ImageOutputRules &rules = intent.imageRules;

  const auto fail = [&](ErrorCode code, const std::string &msg) {
    result.error = Error::make(code, msg, "ffmpeg");
    return result;
  };
  const auto escapeJson = [](const std::string &text) {
    std::string out;
    for (unsigned char c : text) {
      switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", unsigned(c));
          out += buf;
        } else {
          out.push_back(char(c));
        }
      }
    }
    return out;
  };
  const auto modeName = [](FrameSelectionMode mode) -> const char * {
    switch (mode) {
    case FrameSelectionMode::Current:
      return "current";
    case FrameSelectionMode::Selected:
      return "selected";
    case FrameSelectionMode::Interval:
      return "interval";
    case FrameSelectionMode::FrameRate:
      return "frame-rate";
    case FrameSelectionMode::Count:
      return "count";
    case FrameSelectionMode::AllInRange:
      return "all";
    case FrameSelectionMode::Keyframes:
      return "keyframes";
    case FrameSelectionMode::SceneChanges:
      return "scene-changes";
    }
    return "unknown";
  };

  // Output layout: the primary output path is the manifest location;
  // images live beside it.
  const std::string dir = [p = params.outputPath]() {
    const auto slash = p.rfind('/');
    return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
  }();
  const std::string stem = [p = params.outputPath]() {
    auto base = p;
    const auto slash = base.rfind('/');
    if (slash != std::string::npos) {
      base = base.substr(slash + 1);
    }
    const auto dot = base.rfind('.');
    if (dot != std::string::npos) {
      base = base.substr(0, dot);
    }
    return base;
  }();
  const char *imageExt = rules.format == ImageOutputRules::Format::Jpeg
                             ? "jpg"
                             : (rules.format == ImageOutputRules::Format::Png
                                    ? "png"
                                    : "webp");
  const std::string manifestPath = dir + "/" + stem + "_manifest.json";

  struct FramesCleanup {
    ~FramesCleanup() {
      avformat_close_input(&in);
      avcodec_free_context(&dec);
      avcodec_free_context(&imgEnc);
      sws_freeContext(toRgba);
      sws_freeContext(toTarget);
      av_frame_free(&frame);
      av_frame_free(&rgba);
      av_frame_free(&best);
      av_frame_free(&converted);
      av_packet_free(&packet);
      if (convertedBuffers[0] != nullptr) {
        av_freep(&convertedBuffers[0]);
      }
      if (rgbaBuffer != nullptr) {
        av_free(rgbaBuffer);
      }
    }
    AVFormatContext *in = nullptr;
    AVCodecContext *dec = nullptr;
    AVCodecContext *imgEnc = nullptr;
    SwsContext *toRgba = nullptr;
    SwsContext *toTarget = nullptr;
    AVFrame *frame = av_frame_alloc();
    AVFrame *rgba = av_frame_alloc();
    AVFrame *best = av_frame_alloc();
    AVFrame *converted = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
    uint8_t *rgbaBuffer = nullptr;
    uint8_t *convertedBuffers[4] = {nullptr, nullptr, nullptr, nullptr};
    int convertedLinesizes[4] = {0, 0, 0, 0};
  } cleanup;

  if (avformat_open_input(&cleanup.in, payload.source.uri.c_str(), nullptr,
                          nullptr) != 0 ||
      avformat_find_stream_info(cleanup.in, nullptr) < 0) {
    return fail(ErrorCode::SourceUnavailable,
                "cannot open the frame extraction source");
  }
  int videoIn = -1;
  for (unsigned i = 0; i < cleanup.in->nb_streams; ++i) {
    AVStream *stream = cleanup.in->streams[i];
    if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
        (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0) {
      videoIn = int(i);
      break;
    }
  }
  if (videoIn < 0) {
    return fail(ErrorCode::UnsupportedOperation,
                "the source has no timed video stream to extract frames "
                "from");
  }
  AVStream *videoStream = cleanup.in->streams[videoIn];
  const AVCodec *decoder =
      avcodec_find_decoder(videoStream->codecpar->codec_id);
  if (decoder == nullptr) {
    return fail(ErrorCode::DecoderUnavailable,
                "no decoder for the source video codec");
  }
  cleanup.dec = avcodec_alloc_context3(decoder);
  if (avcodec_parameters_to_context(cleanup.dec, videoStream->codecpar) < 0 ||
      avcodec_open2(cleanup.dec, decoder, nullptr) < 0) {
    return fail(ErrorCode::DecoderUnavailable,
                "cannot open the source video decoder");
  }

  // Orientation: resolve the display rotation ONCE (mandate §29.3).
  int rotation = 0;
  {
    const uint8_t *data = av_stream_get_side_data(
        videoStream, AV_PKT_DATA_DISPLAYMATRIX, nullptr);
    if (data != nullptr) {
      const double degrees =
          -av_display_rotation_get(reinterpret_cast<const int32_t *>(data));
      if (degrees > 315 || degrees <= -315) {
        rotation = 0;
      } else if (degrees > 45 && degrees <= 135) {
        rotation = 90;
      } else if (degrees > 135 || degrees <= -45) {
        rotation = 180;
      } else {
        rotation = 270;
      }
    }
  }

  // Range resolution.
  std::int64_t durationUs = 0;
  if (videoStream->duration > 0) {
    durationUs = av_rescale_q(videoStream->duration, videoStream->time_base,
                              AVRational{1, 1000000});
  } else if (cleanup.in->duration > 0) {
    durationUs = std::int64_t(cleanup.in->duration * 1000000.0);
  }
  std::int64_t rangeStartUs = selection.range.startUs;
  std::int64_t rangeEndUs =
      selection.range.endUs < 0 ? durationUs : selection.range.endUs;
  if (rangeEndUs <= rangeStartUs) {
    return fail(ErrorCode::InvalidTimeRange, "frame selection range is "
                                             "empty");
  }

  // Image encoder setup.
  const int srcW = cleanup.dec->width;
  const int srcH = cleanup.dec->height;
  if (srcW <= 0 || srcH <= 0) {
    return fail(ErrorCode::UnsupportedOperation,
                "the source video has no dimensions");
  }
  // Target dimensions: original by default; custom bounds scale down
  // preserving the aspect ratio (never stretch, never upscale).
  int targetW = srcW;
  int targetH = srcH;
  if (rules.maxWidth.has_value() || rules.maxHeight.has_value()) {
    double scale = 1.0;
    if (rules.maxWidth.has_value() && srcW > *rules.maxWidth) {
      scale = std::min(scale, double(*rules.maxWidth) / double(srcW));
    }
    if (rules.maxHeight.has_value() && srcH > *rules.maxHeight) {
      scale = std::min(scale, double(*rules.maxHeight) / double(srcH));
    }
    targetW = std::max(2, int(double(srcW) * scale) / 2 * 2);
    targetH = std::max(2, int(double(srcH) * scale) / 2 * 2);
  }
  // Rotation swaps the written image's axis for quarter turns.
  const bool rotatedQuarter = rotation == 90 || rotation == 270;
  const int imgW = rotatedQuarter ? targetH : targetW;
  const int imgH = rotatedQuarter ? targetW : targetH;

  const char *encoderName = rules.format == ImageOutputRules::Format::Jpeg
                                ? "mjpeg"
                                : (rules.format == ImageOutputRules::Format::Png
                                       ? "png"
                                       : "libwebp");
  const AVCodec *imgCodec = avcodec_find_encoder_by_name(encoderName);
  if (imgCodec == nullptr) {
    imgCodec = avcodec_find_encoder(
        rules.format == ImageOutputRules::Format::Jpeg
            ? AV_CODEC_ID_MJPEG
            : (rules.format == ImageOutputRules::Format::Png
                   ? AV_CODEC_ID_PNG
                   : AV_CODEC_ID_WEBP));
  }
  if (imgCodec == nullptr) {
    return fail(ErrorCode::EncoderUnavailable,
                std::string("no image encoder for ") + imageExt);
  }
  cleanup.imgEnc = avcodec_alloc_context3(imgCodec);
  if (rules.format == ImageOutputRules::Format::Png) {
    cleanup.imgEnc->pix_fmt = AV_PIX_FMT_RGBA;
  } else {
    // mjpeg uses the full-range yuvj420p; libwebp takes yuv420p.
    cleanup.imgEnc->pix_fmt = rules.format == ImageOutputRules::Format::Jpeg
                                  ? AV_PIX_FMT_YUVJ420P
                                  : AV_PIX_FMT_YUV420P;
  }
  if (rules.quality.has_value() &&
      rules.format != ImageOutputRules::Format::Png) {
    // Quality 1..100 (higher = better). mjpeg maps it to qscale 1..31;
    // libwebp reads global_quality in lambda units directly.
    const int q = std::clamp(*rules.quality, 1, 100);
    if (rules.format == ImageOutputRules::Format::WebP) {
      cleanup.imgEnc->global_quality = q * FF_QP2LAMBDA;
    } else {
      const int qscale = 1 + (100 - q) * 30 / 99;
      cleanup.imgEnc->global_quality = qscale * FF_QP2LAMBDA;
    }
    cleanup.imgEnc->flags |= AV_CODEC_FLAG_QSCALE;
  }
  cleanup.imgEnc->time_base = AVRational{1, 1000000};
  // Image encoders validate their dimensions at open time: the target
  // geometry (rotation-aware) must be set on the context BEFORE the open.
  cleanup.imgEnc->width = imgW;
  cleanup.imgEnc->height = imgH;
  if (avcodec_open2(cleanup.imgEnc, imgCodec, nullptr) < 0) {
    return fail(ErrorCode::EncoderUnavailable, "cannot open the image "
                                               "encoder");
  }

  // Persistent conversion buffers.
  const int rgbaLinesize = imgW * 4;
  cleanup.rgbaBuffer =
      static_cast<uint8_t *>(av_malloc(size_t(rgbaLinesize) * imgH));
  if (cleanup.rgbaBuffer == nullptr) {
    return fail(ErrorCode::InternalError, "out of memory (rgba buffer)");
  }
  const int convertedSize = av_image_get_buffer_size(
      cleanup.imgEnc->pix_fmt, imgW, imgH, 16);
  if (convertedSize <= 0 ||
      av_image_alloc(cleanup.convertedBuffers, cleanup.convertedLinesizes,
                     imgW, imgH, cleanup.imgEnc->pix_fmt, 16) < 0) {
    return fail(ErrorCode::InternalError, "out of memory (image buffer)");
  }

  struct ManifestEntry {
    std::int64_t timestampUs;
    std::string file;
    std::int64_t sizeBytes;
  };
  std::vector<ManifestEntry> manifest;
  std::int64_t sequence = 0;

  // Frame write helper: decoded -> RGBA (scaled) -> rotate -> encode.
  const auto writeImage = [&](AVFrame *decoded,
                              std::int64_t ptsUs) -> bool {
    if (cleanup.toRgba == nullptr) {
      cleanup.toRgba = sws_getContext(
          srcW, srcH, cleanup.dec->pix_fmt, targetW, targetH,
          AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
      if (cleanup.toRgba == nullptr) {
        return false;
      }
    }
    {
      uint8_t *dst[4] = {cleanup.rgbaBuffer, nullptr, nullptr, nullptr};
      int dstLinesize[4] = {rgbaLinesize, 0, 0, 0};
      if (sws_scale(cleanup.toRgba, decoded->data, decoded->linesize, 0,
                    srcH, dst, dstLinesize) <= 0) {
        return false;
      }
    }
    // Rotation on the RGBA buffer (90 cw / 180 / 270 cw).
    if (rotation != 0) {
      const int w = imgW; // pre-rotation geometry for the pixel math:
                          // 90/270 wrote the swapped dims already? No:
      // the scale above produced targetW x targetH; quarter turns swap
      // the axes in the output image.
      (void)w;
      const int preW = targetW;
      const int preH = targetH;
      const int postW = rotatedQuarter ? preH : preW;
      const int postH = rotatedQuarter ? preW : preH;
      const size_t rotatedBytes =
          size_t(postW) * size_t(postH) * 4;
      uint8_t *rotated =
          static_cast<uint8_t *>(av_malloc(rotatedBytes));
      if (rotated == nullptr) {
        return false;
      }
      const uint8_t *srcBuf = cleanup.rgbaBuffer;
      for (int y = 0; y < preH; ++y) {
        for (int x = 0; x < preW; ++x) {
          const size_t srcIdx = (size_t(y) * preW + x) * 4;
          int outX;
          int outY;
          if (rotation == 180) {
            outX = preW - 1 - x;
            outY = preH - 1 - y;
          } else if (rotation == 90) {
            outX = preH - 1 - y;
            outY = x;
          } else { // 270
            outX = y;
            outY = preW - 1 - x;
          }
          const size_t dstIdx = (size_t(outY) * postW + outX) * 4;
          rotated[dstIdx + 0] = srcBuf[srcIdx + 0];
          rotated[dstIdx + 1] = srcBuf[srcIdx + 1];
          rotated[dstIdx + 2] = srcBuf[srcIdx + 2];
          rotated[dstIdx + 3] = srcBuf[srcIdx + 3];
        }
      }
      av_free(cleanup.rgbaBuffer);
      cleanup.rgbaBuffer = rotated;
    }
    const int finalW = imgW;
    const int finalH = imgH;
    const int finalLinesize = finalW * 4;
    if (cleanup.toTarget == nullptr) {
      cleanup.toTarget = sws_getContext(
          finalW, finalH, AV_PIX_FMT_RGBA, finalW, finalH,
          cleanup.imgEnc->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
      if (cleanup.toTarget == nullptr) {
        return false;
      }
    }
    {
      uint8_t *src[4] = {cleanup.rgbaBuffer, nullptr, nullptr, nullptr};
      int srcLinesize[4] = {finalLinesize, 0, 0, 0};
      if (sws_scale(cleanup.toTarget, src, srcLinesize, 0, finalH,
                    cleanup.convertedBuffers, cleanup.convertedLinesizes) <=
          0) {
        return false;
      }
    }
    cleanup.converted->format = cleanup.imgEnc->pix_fmt;
    cleanup.converted->width = finalW;
    cleanup.converted->height = finalH;
    cleanup.converted->pict_type = AV_PICTURE_TYPE_NONE;
    av_image_fill_arrays(
        cleanup.converted->data, cleanup.converted->linesize,
        cleanup.convertedBuffers[0], cleanup.imgEnc->pix_fmt, finalW,
        finalH, 16);
    if (avcodec_send_frame(cleanup.imgEnc, cleanup.converted) < 0) {
      return false;
    }
    AVPacket *encPacket = av_packet_alloc();
    bool wrote = false;
    std::int64_t writtenBytes = 0;
    std::string filePath;
    while (avcodec_receive_packet(cleanup.imgEnc, encPacket) >= 0) {
      if (!wrote) {
        ++sequence;
        char nameBuf[256];
        if (rules.sequentialNaming) {
          std::snprintf(nameBuf, sizeof(nameBuf), "%s_%05lld.%s",
                        stem.c_str(), static_cast<long long>(sequence),
                        imageExt);
        } else {
          std::snprintf(nameBuf, sizeof(nameBuf), "%s_%lldus.%s",
                        stem.c_str(), static_cast<long long>(ptsUs),
                        imageExt);
        }
        filePath = dir + "/" + nameBuf;
      }
      FILE *out = fopen(filePath.c_str(), "wb");
      if (out == nullptr) {
        av_packet_free(&encPacket);
        return false;
      }
      const size_t wroteNow = fwrite(encPacket->data, 1,
                                     size_t(encPacket->size), out);
      const int closeRc = fclose(out);
      if (wroteNow != size_t(encPacket->size) || closeRc != 0) {
        av_packet_free(&encPacket);
        result.error = Error::make(ErrorCode::StorageFull,
                                   "image write failed (disk full?)",
                                   "ffmpeg");
        return false;
      }
      writtenBytes += encPacket->size;
      wrote = true;
    }
    av_packet_free(&encPacket);
    if (!wrote) {
      return false;
    }
    manifest.push_back(ManifestEntry{ptsUs, filePath, writtenBytes});
    result.bytesWritten += writtenBytes;
    return true;
  };

  const auto ptsToUs = [&](AVFrame *frame) -> std::int64_t {
    if (frame->pts == AV_NOPTS_VALUE) {
      return -1;
    }
    return av_rescale_q(frame->pts, videoStream->time_base,
                        AVRational{1, 1000000});
  };
  const auto cancelled = [&]() {
    return params.cancel != nullptr && params.cancel->isCancelled();
  };
  const auto reportProgress = [&](std::int64_t processedUs) {
    if (params.onProgress == nullptr) {
      return;
    }
    const std::int64_t totalUs = rangeEndUs - rangeStartUs;
    if (totalUs <= 0) {
      return;
    }
    backend::ProgressReport report;
    report.processedDurationUs = processedUs - rangeStartUs;
    report.totalDurationUs = totalUs;
    report.fraction = std::clamp(
        double(processedUs - rangeStartUs) / double(totalUs), 0.0, 1.0);
    params.onProgress(report);
  };

  // Streaming decode over the range: writes every frame whose pts falls
  // in [start, end) — AllInRange mode.
  const auto decodeRange = [&](std::int64_t startUs,
                               std::int64_t endUs) -> bool {
    if (startUs > 0) {
      const std::int64_t seekTs =
          av_rescale_q(startUs, AVRational{1, 1000000},
                       videoStream->time_base);
      avformat_seek_file(cleanup.in, videoIn, INT64_MIN, seekTs, INT64_MAX,
                         AVSEEK_FLAG_BACKWARD);
      avcodec_flush_buffers(cleanup.dec);
    }
    bool eof = false;
    std::int64_t lastReported = -1;
    while (!eof) {
      if (cancelled()) {
        return false;
      }
      if (av_read_frame(cleanup.in, cleanup.packet) < 0) {
        eof = true;
        avcodec_send_packet(cleanup.dec, nullptr);
      } else if (cleanup.packet->stream_index == videoIn) {
        avcodec_send_packet(cleanup.dec, cleanup.packet);
      }
      if (cleanup.packet->stream_index == videoIn || eof) {
        while (avcodec_receive_frame(cleanup.dec, cleanup.frame) >= 0) {
          const std::int64_t us = ptsToUs(cleanup.frame);
          if (us < 0) {
            av_frame_unref(cleanup.frame);
            continue;
          }
          if (us >= endUs) {
            av_frame_unref(cleanup.frame);
            return true;
          }
          if (us >= startUs) {
            const bool ok = writeImage(cleanup.frame, us);
            av_frame_unref(cleanup.frame);
            if (!ok) {
              return false;
            }
          } else {
            av_frame_unref(cleanup.frame);
          }
          if (lastReported < 0 || us - lastReported >= 250000) {
            lastReported = us;
            reportProgress(us);
          }
        }
      }
      av_packet_unref(cleanup.packet);
    }
    return true;
  };

  // Resolve ONE actual frame for a requested timestamp: seek back to the
  // keyframe at/before it and decode forward to the nearest frame at or
  // before the request (mandate §29.1: real presentation-frame identity,
  // never the nearest keyframe picture).
  const auto resolveFrameAt = [&](std::int64_t requestedUs) -> bool {
    const std::int64_t seekUs = std::max<std::int64_t>(0, requestedUs - 1000000);
    if (seekUs > 0) {
      const std::int64_t seekTs = av_rescale_q(
          seekUs, AVRational{1, 1000000}, videoStream->time_base);
      if (avformat_seek_file(cleanup.in, videoIn, INT64_MIN, seekTs,
                             INT64_MAX, AVSEEK_FLAG_BACKWARD) < 0) {
        return false;
      }
      avcodec_flush_buffers(cleanup.dec);
    }
    av_frame_unref(cleanup.best);
    bool found = false;
    bool eof = false;
    while (!eof) {
      if (cancelled()) {
        return false;
      }
      if (av_read_frame(cleanup.in, cleanup.packet) < 0) {
        eof = true;
        avcodec_send_packet(cleanup.dec, nullptr);
      } else if (cleanup.packet->stream_index == videoIn) {
        avcodec_send_packet(cleanup.dec, cleanup.packet);
      }
      if (cleanup.packet->stream_index == videoIn || eof) {
        while (avcodec_receive_frame(cleanup.dec, cleanup.frame) >= 0) {
          const std::int64_t us = ptsToUs(cleanup.frame);
          if (us < 0) {
            av_frame_unref(cleanup.frame);
            continue;
          }
          if (us <= requestedUs) {
            av_frame_unref(cleanup.best);
            av_frame_ref(cleanup.best, cleanup.frame);
            found = true;
            av_frame_unref(cleanup.frame);
          } else {
            // First frame past the request: resolve now.
            bool ok;
            if (found) {
              const std::int64_t bestUs = av_rescale_q(
                  cleanup.best->pts, videoStream->time_base,
                  AVRational{1, 1000000});
              ok = writeImage(cleanup.best, bestUs);
            } else {
              ok = writeImage(cleanup.frame, us);
            }
            av_frame_unref(cleanup.frame);
            return ok;
          }
        }
      }
      av_packet_unref(cleanup.packet);
    }
    if (found) {
      const std::int64_t bestUs = av_rescale_q(
          cleanup.best->pts, videoStream->time_base, AVRational{1, 1000000});
      return writeImage(cleanup.best, bestUs);
    }
    return false;
  };

  // Scene-change timestamps (real analysis, only when requested):
  // decode through the select='gt(scene,0.3)' filter graph.
  const auto detectSceneTimestamps = [&]() -> std::vector<std::int64_t> {
    std::vector<std::int64_t> stamps;
    AVFilterGraph *graph = avfilter_graph_alloc();
    if (graph == nullptr) {
      return stamps;
    }
    struct GraphCleanup {
      ~GraphCleanup() { avfilter_graph_free(&graph); }
      AVFilterGraph *graph;
    } graphCleanup{graph};
    const AVFilter *buffersrc = avfilter_get_by_name("buffer");
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");
    const AVFilter *selectF = avfilter_get_by_name("select");
    if (buffersrc == nullptr || buffersink == nullptr ||
        selectF == nullptr) {
      return stamps;
    }
    AVFilterContext *src = nullptr;
    AVFilterContext *sel = nullptr;
    AVFilterContext *sink = nullptr;
    char args[512];
    std::snprintf(args, sizeof(args),
                  "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d"
                  ":pixel_aspect=%d/%d",
                  cleanup.dec->width, cleanup.dec->height,
                  int(cleanup.dec->pix_fmt), videoStream->time_base.num,
                  videoStream->time_base.den, 0, 1);
    if (avfilter_graph_create_filter(&src, buffersrc, "in", args, nullptr,
                                     graph) < 0 ||
        avfilter_graph_create_filter(&sel, selectF, "sel",
                                     "gt(scene\\,0.3)", nullptr, graph) <
            0 ||
        avfilter_graph_create_filter(&sink, buffersink, "out", nullptr,
                                     nullptr, graph) < 0) {
      return stamps;
    }
    if (avfilter_link(src, 0, sel, 0) != 0 ||
        avfilter_link(sel, 0, sink, 0) != 0 ||
        avfilter_graph_config(graph, nullptr) < 0) {
      return stamps;
    }
    // Rewind + decode the whole range through the graph.
    avformat_seek_file(cleanup.in, videoIn, INT64_MIN, 0, INT64_MAX,
                       AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(cleanup.dec);
    bool eof = false;
    while (!eof) {
      if (cancelled()) {
        break;
      }
      if (av_read_frame(cleanup.in, cleanup.packet) < 0) {
        eof = true;
        av_buffersrc_add_frame(src, nullptr);
      } else if (cleanup.packet->stream_index == videoIn) {
        avcodec_send_packet(cleanup.dec, cleanup.packet);
      }
      if (cleanup.packet->stream_index == videoIn || eof) {
        while (avcodec_receive_frame(cleanup.dec, cleanup.frame) >= 0) {
          AVFrame *graphFrame = av_frame_clone(cleanup.frame);
          av_frame_unref(cleanup.frame);
          if (graphFrame != nullptr) {
            if (av_buffersrc_add_frame_flags(
                    src, graphFrame, AV_BUFFERSRC_FLAG_KEEP_REF) >= 0) {
              AVFrame *selectedFrame = av_frame_alloc();
              while (av_buffersink_get_frame(sink, selectedFrame) >= 0) {
                if (selectedFrame->pts != AV_NOPTS_VALUE) {
                  stamps.push_back(av_rescale_q(
                      selectedFrame->pts, av_buffersink_get_time_base(sink),
                      AVRational{1, 1000000}));
                }
                av_frame_unref(selectedFrame);
              }
              av_frame_free(&selectedFrame);
            }
            av_frame_free(&graphFrame);
          }
        }
      }
      av_packet_unref(cleanup.packet);
    }
    avcodec_flush_buffers(cleanup.dec);
    return stamps;
  };

  bool ok = false;
  switch (selection.mode) {
  case FrameSelectionMode::AllInRange:
    ok = decodeRange(rangeStartUs, rangeEndUs);
    break;
  case FrameSelectionMode::Current:
  case FrameSelectionMode::Selected: {
    ok = true;
    for (const std::int64_t ts : selection.timestampsUs) {
      if (cancelled()) {
        ok = false;
        break;
      }
      if (ts >= rangeStartUs && ts < rangeEndUs) {
        ok = resolveFrameAt(ts);
        if (!ok) {
          break;
        }
      }
    }
    break;
  }
  case FrameSelectionMode::Interval:
  case FrameSelectionMode::FrameRate:
  case FrameSelectionMode::Count: {
    std::vector<std::int64_t> requested;
    if (selection.mode == FrameSelectionMode::Interval) {
      if (selection.intervalUs <= 0) {
        return fail(ErrorCode::InvalidArgument,
                    "invalid extraction interval");
      }
      for (std::int64_t ts = rangeStartUs; ts < rangeEndUs;
           ts += selection.intervalUs) {
        requested.push_back(ts);
      }
    } else if (selection.mode == FrameSelectionMode::FrameRate) {
      if (!selection.frameRate.valid()) {
        return fail(ErrorCode::InvalidArgument, "invalid sampling rate");
      }
      const std::int64_t stepUs = 1000000LL * selection.frameRate.den /
                                  selection.frameRate.num;
      if (stepUs <= 0) {
        return fail(ErrorCode::InvalidArgument, "invalid sampling rate");
      }
      for (std::int64_t ts = rangeStartUs; ts < rangeEndUs; ts += stepUs) {
        requested.push_back(ts);
      }
    } else {
      if (selection.count <= 0) {
        return fail(ErrorCode::InvalidArgument, "invalid frame count");
      }
      const std::int64_t span = rangeEndUs - rangeStartUs;
      for (std::int32_t k = 0; k < selection.count; ++k) {
        const std::int64_t ts = selection.count == 1
                                    ? rangeStartUs + span / 2
                                    : rangeStartUs +
                                          span * k / (selection.count - 1);
        requested.push_back(ts);
      }
    }
    // Deduplicate resolved frames by identity: consecutive requests may
    // resolve to the same actual frame; write each actual frame once
    // (never fabricate duplicates — mandate §29.2).
    std::int64_t lastWrittenPts = -1;
    ok = true;
    for (const std::int64_t ts : requested) {
      if (cancelled()) {
        ok = false;
        break;
      }
      const std::size_t before = manifest.size();
      ok = resolveFrameAt(ts);
      if (!ok) {
        break;
      }
      if (manifest.size() > before) {
        const std::int64_t writtenPts = manifest.back().timestampUs;
        if (writtenPts == lastWrittenPts) {
          std::remove(manifest.back().file.c_str());
          manifest.pop_back();
        } else {
          lastWrittenPts = writtenPts;
        }
      }
      reportProgress(ts);
    }
    break;
  }
  case FrameSelectionMode::Keyframes: {
    // Packet-level pass (no decode): the ACTUAL random-access flags
    // (mandate §29.2); only selected keyframe images decode.
    ok = true;
    std::int64_t lastReported = -1;
    bool eof = false;
    while (!eof && ok) {
      if (cancelled()) {
        ok = false;
        break;
      }
      if (av_read_frame(cleanup.in, cleanup.packet) < 0) {
        eof = true;
      } else if (cleanup.packet->stream_index == videoIn) {
        const std::int64_t us =
            cleanup.packet->pts == AV_NOPTS_VALUE
                ? -1
                : av_rescale_q(cleanup.packet->pts, videoStream->time_base,
                               AVRational{1, 1000000});
        if (us >= rangeStartUs && us < rangeEndUs &&
            (cleanup.packet->flags & AV_PKT_FLAG_KEY) != 0) {
          // A keyframe packet decodes standalone.
          avcodec_send_packet(cleanup.dec, cleanup.packet);
          if (avcodec_receive_frame(cleanup.dec, cleanup.frame) >= 0) {
            ok = writeImage(cleanup.frame, us >= 0 ? us : 0);
            av_frame_unref(cleanup.frame);
          }
          avcodec_flush_buffers(cleanup.dec);
        }
        if (us > 0 && (lastReported < 0 || us - lastReported >= 250000)) {
          lastReported = us;
          reportProgress(us);
        }
      }
      av_packet_unref(cleanup.packet);
    }
    break;
  }
  case FrameSelectionMode::SceneChanges: {
    const std::vector<std::int64_t> sceneStamps = detectSceneTimestamps();
    if (sceneStamps.empty()) {
      return fail(ErrorCode::UnsupportedOperation,
                  "no scene changes detected in the selection range");
    }
    ok = true;
    std::int64_t lastWrittenPts = -1;
    for (const std::int64_t ts : sceneStamps) {
      if (cancelled()) {
        ok = false;
        break;
      }
      const std::size_t before = manifest.size();
      ok = resolveFrameAt(ts);
      if (!ok) {
        break;
      }
      if (manifest.size() > before &&
          manifest.back().timestampUs == lastWrittenPts) {
        std::remove(manifest.back().file.c_str());
        manifest.pop_back();
      } else if (manifest.size() > before) {
        lastWrittenPts = manifest.back().timestampUs;
      }
    }
    break;
  }
  }

  if (!ok) {
    if (cancelled()) {
      result.error = Error::make(ErrorCode::Cancelled, "cancelled", "ffmpeg");
    } else if (result.error.code == ErrorCode::None) {
      result.error =
          Error::make(ErrorCode::ExecutionFailed, "frame extraction failed",
                      "ffmpeg");
    }
    return result;
  }
  if (manifest.empty()) {
    return fail(ErrorCode::UnsupportedOperation,
                "the source cannot provide frames in the selected range");
  }

  // Manifest file (mandate §29.4): frame identity, timestamp, file.
  {
    std::string json = "{\"source\":\"" + escapeJson(payload.source.displayName) +
                       "\",\"mode\":\"" + modeName(selection.mode) +
                       "\",\"count\":" + std::to_string(manifest.size()) +
                       ",\"frames\":[";
    for (std::size_t i = 0; i < manifest.size(); ++i) {
      if (i > 0) {
        json += ",";
      }
      json += "{\"timestampUs\":" + std::to_string(manifest[i].timestampUs) +
              ",\"file\":\"" + escapeJson(manifest[i].file) + "\"}";
    }
    json += "]}";
    FILE *mf = fopen(manifestPath.c_str(), "wb");
    if (mf == nullptr ||
        fwrite(json.data(), 1, json.size(), mf) != json.size() ||
        fclose(mf) != 0) {
      return fail(ErrorCode::InvalidOutputDestination,
                  "cannot write the frames manifest");
    }
  }

  result.success = true;
  result.framesProcessed = std::int64_t(manifest.size());
  JobOutput manifestOut;
  manifestOut.path = manifestPath;
  manifestOut.kind = "manifest";
  result.outputs.push_back(std::move(manifestOut));
  const std::size_t imageCap = 5000;
  for (std::size_t i = 0; i < manifest.size() && i < imageCap; ++i) {
    JobOutput imageOut;
    imageOut.path = manifest[i].file;
    imageOut.sizeBytes = manifest[i].sizeBytes;
    imageOut.kind = "image";
    result.outputs.push_back(std::move(imageOut));
  }
  return result;
}

Error FfmpegBackend::initialize() {
  initFfmpegLabel();
  implementationLabel_ = g_ffmpegVersion;
  // HDR tone-map availability (mandate §25): probe the ACTUAL avfilter
  // registry at startup — both zscale (libzimg colorspace converter) and
  // tonemap must exist for the real conversion chain. Never a hardcoded
  // claim; capability follows the compiled library.
  hdrToneMappingAvailable_ = avfilter_get_by_name("zscale") != nullptr &&
                             avfilter_get_by_name("tonemap") != nullptr;
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
  // HDR -> SDR tone mapping: real when the compiled avfilter carries BOTH
  // zscale (libzimg) and tonemap — probed at initialize(). HDR
  // PRESERVATION works via stream copy regardless.
  caps.hdrToneMapping = hdrToneMappingAvailable_;
  // Timeline operations (mandate §27/§29): multi-range Cut, Split with all
  // boundary strategies (real chapter reading, real scene detection), Merge
  // with compatibility verification + normalization, and frame extraction
  // with real frame identity. All implemented by this executor.
  caps.timelineOps = true;

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
