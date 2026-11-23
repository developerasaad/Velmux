#include "velmux/core/types.h"

#include <array>
#include <cstring>

namespace velmux::core {

// ---------------------------------------------------------------------------
// Video codecs
// ---------------------------------------------------------------------------

struct VideoCodecName {
  VideoCodec codec;
  const char *name;
};

static constexpr std::array<VideoCodecName, 15> kVideoCodecNames = {{
    {VideoCodec::H264, "h264"},
    {VideoCodec::HEVC, "hevc"},
    {VideoCodec::AV1, "av1"},
    {VideoCodec::VP8, "vp8"},
    {VideoCodec::VP9, "vp9"},
    {VideoCodec::Mpeg4Part2, "mpeg4"},
    {VideoCodec::Mpeg2, "mpeg2video"},
    {VideoCodec::Mpeg1, "mpeg1video"},
    {VideoCodec::H263, "h263"},
    {VideoCodec::Vc1, "vc1"},
    {VideoCodec::Wmv, "wmv"},
    {VideoCodec::ProRes, "prores"},
    {VideoCodec::Dnxhd, "dnxhd"},
    {VideoCodec::Ffv1, "ffv1"},
    {VideoCodec::Mjpeg, "mjpeg"},
}};

const char *codecName(VideoCodec codec) {
  for (const auto &entry : kVideoCodecNames) {
    if (entry.codec == codec) {
      return entry.name;
    }
  }
  return "unknown-video";
}

std::optional<VideoCodec> videoCodecFromName(const std::string &name) {
  for (const auto &entry : kVideoCodecNames) {
    if (name == entry.name) {
      return entry.codec;
    }
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Audio codecs
// ---------------------------------------------------------------------------

struct AudioCodecName {
  AudioCodec codec;
  const char *name;
};

static constexpr std::array<AudioCodecName, 17> kAudioCodecNames = {{
    {AudioCodec::Aac, "aac"},
    {AudioCodec::HeAac, "he-aac"},
    {AudioCodec::Mp3, "mp3"},
    {AudioCodec::Opus, "opus"},
    {AudioCodec::Vorbis, "vorbis"},
    {AudioCodec::Flac, "flac"},
    {AudioCodec::Alac, "alac"},
    {AudioCodec::Pcm, "pcm"},
    {AudioCodec::Ac3, "ac3"},
    {AudioCodec::Eac3, "eac3"},
    {AudioCodec::Dts, "dts"},
    {AudioCodec::TrueHd, "truehd"},
    {AudioCodec::Wma, "wma"},
    {AudioCodec::WavPack, "wavpack"},
    {AudioCodec::Ape, "ape"},
    {AudioCodec::AmrNb, "amrnb"},
    {AudioCodec::AmrWb, "amrwb"},
}};

const char *codecName(AudioCodec codec) {
  for (const auto &entry : kAudioCodecNames) {
    if (entry.codec == codec) {
      return entry.name;
    }
  }
  return "unknown-audio";
}

std::optional<AudioCodec> audioCodecFromName(const std::string &name) {
  for (const auto &entry : kAudioCodecNames) {
    if (name == entry.name) {
      return entry.codec;
    }
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Containers
// ---------------------------------------------------------------------------

struct ContainerName {
  Container container;
  const char *name;
};

static constexpr std::array<ContainerName, 15> kContainerNames = {{
    {Container::Mp4, "mp4"},
    {Container::M4v, "m4v"},
    {Container::Mov, "mov"},
    {Container::Mkv, "mkv"},
    {Container::WebM, "webm"},
    {Container::Avi, "avi"},
    {Container::MpegTs, "mpegts"},
    {Container::Flv, "flv"},
    {Container::ThreeGp, "3gp"},
    {Container::M4a, "m4a"},
    {Container::Ogg, "ogg"},
    {Container::Mp3, "mp3"},
    {Container::Wav, "wav"},
    {Container::Flac, "flac"},
    {Container::Asf, "asf"},
}};

const char *containerName(Container container) {
  for (const auto &entry : kContainerNames) {
    if (entry.container == container) {
      return entry.name;
    }
  }
  return "unknown-container";
}

std::optional<Container> containerFromName(const std::string &name) {
  for (const auto &entry : kContainerNames) {
    if (name == entry.name) {
      return entry.container;
    }
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Subtitle formats
// ---------------------------------------------------------------------------

struct SubtitleFormatName {
  SubtitleFormat format;
  const char *name;
};

static constexpr std::array<SubtitleFormatName, 11> kSubtitleFormatNames = {{
    {SubtitleFormat::Srt, "srt"},
    {SubtitleFormat::WebVtt, "vtt"},
    {SubtitleFormat::Ass, "ass"},
    {SubtitleFormat::Ssa, "ssa"},
    {SubtitleFormat::MovText, "mov_text"},
    {SubtitleFormat::Ttml, "ttml"},
    {SubtitleFormat::Cea608, "cea-608"},
    {SubtitleFormat::Cea708, "cea-708"},
    {SubtitleFormat::Pgs, "pgs"},
    {SubtitleFormat::VobSub, "vobsub"},
    {SubtitleFormat::Dvb, "dvb"},
}};

const char *subtitleFormatName(SubtitleFormat format) {
  for (const auto &entry : kSubtitleFormatNames) {
    if (entry.format == format) {
      return entry.name;
    }
  }
  return "unknown-subtitle";
}

std::optional<SubtitleFormat> subtitleFormatFromName(const std::string &name) {
  for (const auto &entry : kSubtitleFormatNames) {
    if (name == entry.name) {
      return entry.format;
    }
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

const char *errorCodeName(ErrorCode code) {
  switch (code) {
    case ErrorCode::None:
      return "none";
    case ErrorCode::InvalidArgument:
      return "invalid-argument";
    case ErrorCode::SourceUnavailable:
      return "source-unavailable";
    case ErrorCode::UnsupportedFeature:
      return "unsupported-feature";
    case ErrorCode::NoCompatiblePipeline:
      return "no-compatible-pipeline";
    case ErrorCode::InspectionFailed:
      return "inspection-failed";
    case ErrorCode::ExecutionFailed:
      return "execution-failed";
    case ErrorCode::BackendFailure:
      return "backend-failure";
    case ErrorCode::OutputValidationFailed:
      return "output-validation-failed";
    case ErrorCode::StorageFull:
      return "storage-full";
    case ErrorCode::Cancelled:
      return "cancelled";
    case ErrorCode::Interrupted:
      return "interrupted";
    case ErrorCode::InternalError:
      return "internal-error";
  }
  return "unknown-error";
}

// ---------------------------------------------------------------------------
// Pixel formats
// ---------------------------------------------------------------------------

namespace {
struct PixelFormatEntry {
  PixelFormat format;
  const char *name;
};
const PixelFormatEntry kPixelFormatNames[] = {
    {PixelFormat::Yuv420P, "yuv420p"},
    {PixelFormat::Yuv422P, "yuv422p"},
    {PixelFormat::Yuv444P, "yuv444p"},
    {PixelFormat::Yuv420P10Le, "yuv420p10le"},
    {PixelFormat::Yuv422P10Le, "yuv422p10le"},
    {PixelFormat::Yuv444P10Le, "yuv444p10le"},
};
} // namespace

const char *pixelFormatName(PixelFormat format) {
  for (const auto &entry : kPixelFormatNames) {
    if (entry.format == format) {
      return entry.name;
    }
  }
  return "unknown-pixel-format";
}

std::optional<PixelFormat> pixelFormatFromName(const std::string &name) {
  for (const auto &entry : kPixelFormatNames) {
    if (name == entry.name) {
      return entry.format;
    }
  }
  return std::nullopt;
}

} // namespace velmux::core
