#include <velmux/types.h>
#include <velmux/configuration.h>
#include <velmux/jobs.h>

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
    case ErrorCode::None: return "none";
    case ErrorCode::InvalidArgument: return "invalid-argument";
    case ErrorCode::UnknownFormat: return "unknown-format";
    case ErrorCode::UnsupportedOperation: return "unsupported-operation";
    case ErrorCode::IncompatibleFormat: return "incompatible-format";
    case ErrorCode::IncompatibleCodec: return "incompatible-codec";
    case ErrorCode::EncoderUnavailable: return "encoder-unavailable";
    case ErrorCode::DecoderUnavailable: return "decoder-unavailable";
    case ErrorCode::InvalidResolution: return "invalid-resolution";
    case ErrorCode::InvalidFrameRate: return "invalid-frame-rate";
    case ErrorCode::InvalidSampleRate: return "invalid-sample-rate";
    case ErrorCode::InvalidChannelLayout: return "invalid-channel-layout";
    case ErrorCode::InvalidTrackIdentity: return "invalid-track-identity";
    case ErrorCode::InvalidTimeRange: return "invalid-time-range";
    case ErrorCode::InvalidCrop: return "invalid-crop";
    case ErrorCode::InfeasibleTargetSize: return "infeasible-target-size";
    case ErrorCode::StaleRevision: return "stale-revision";
    case ErrorCode::RelatedChangeRequired: return "related-change-required";
    case ErrorCode::DuplicateSubmission: return "duplicate-submission";
    case ErrorCode::SourceUnavailable: return "source-unavailable";
    case ErrorCode::SourceChanged: return "source-changed";
    case ErrorCode::InspectionFailed: return "inspection-failed";
    case ErrorCode::NoCompatiblePipeline: return "no-compatible-pipeline";
    case ErrorCode::ExecutionFailed: return "execution-failed";
    case ErrorCode::BackendFailure: return "backend-failure";
    case ErrorCode::OutputValidationFailed: return "output-validation-failed";
    case ErrorCode::TargetNotMet: return "target-not-met";
    case ErrorCode::StorageFull: return "storage-full";
    case ErrorCode::InvalidOutputDestination: return "invalid-output-destination";
    case ErrorCode::Cancelled: return "cancelled";
    case ErrorCode::Interrupted: return "interrupted";
    case ErrorCode::InternalError: return "internal-error";
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



// ---------------------------------------------------------------------------
// Operation / configuration vocabulary (configuration.h)
// ---------------------------------------------------------------------------

static constexpr std::array<std::pair<Operation, const char *>, 8>
    kOperationNames = {{
        {Operation::VideoConvert, "video-convert"},
        {Operation::VideoCrop, "video-crop"},
        {Operation::VideoCutMerge, "video-cutmerge"},
        {Operation::ExtractAudio, "extract-audio"},
        {Operation::ExtractFrames, "extract-frames"},
        {Operation::ExtractSubtitles, "extract-subtitles"},
        {Operation::AudioConvert, "audio-convert"},
        {Operation::AudioCutMerge, "audio-cutmerge"},
}};

const char *operationName(Operation op) {
  for (const auto &entry : kOperationNames) {
    if (entry.first == op) {
      return entry.second;
    }
  }
  return "unknown-operation";
}

std::optional<Operation> operationFromName(const std::string &name) {
  for (const auto &entry : kOperationNames) {
    if (name == entry.second) {
      return entry.first;
    }
  }
  return std::nullopt;
}

static constexpr std::array<std::pair<TimelineMode, const char *>, 4>
    kTimelineModeNames = {{
        {TimelineMode::Trim, "trim"},
        {TimelineMode::Cut, "cut"},
        {TimelineMode::Split, "split"},
        {TimelineMode::Merge, "merge"},
}};

const char *timelineModeName(TimelineMode mode) {
  for (const auto &entry : kTimelineModeNames) {
    if (entry.first == mode) {
      return entry.second;
    }
  }
  return "unknown-timeline-mode";
}

std::optional<TimelineMode> timelineModeFromName(const std::string &name) {
  for (const auto &entry : kTimelineModeNames) {
    if (name == entry.second) {
      return entry.first;
    }
  }
  return std::nullopt;
}

static constexpr std::array<std::pair<CompressionPreset, const char *>, 5>
    kCompressionPresetNames = {{
        {CompressionPreset::HighQuality, "high-quality"},
        {CompressionPreset::Balanced, "balanced"},
        {CompressionPreset::SmallerFile, "smaller-file"},
        {CompressionPreset::TargetSize, "target-size"},
        {CompressionPreset::Custom, "custom"},
}};

const char *compressionPresetName(CompressionPreset preset) {
  for (const auto &entry : kCompressionPresetNames) {
    if (entry.first == preset) {
      return entry.second;
    }
  }
  return "unknown-preset";
}

std::optional<CompressionPreset> compressionPresetFromName(
    const std::string &name) {
  for (const auto &entry : kCompressionPresetNames) {
    if (name == entry.second) {
      return entry.first;
    }
  }
  return std::nullopt;
}

static constexpr std::array<std::pair<FieldId, const char *>, 33>
    kFieldIdNames = {{
        {FieldId::Format, "format"},
        {FieldId::Codec, "codec"},
        {FieldId::Resolution, "resolution"},
        {FieldId::FrameRate, "frame-rate"},
        {FieldId::CompressionEnabled, "compression.enabled"},
        {FieldId::CompressionPreset, "compression.preset"},
        {FieldId::CompressionQuality, "compression.quality"},
        {FieldId::CompressionVideoBitrate, "compression.video-bitrate"},
        {FieldId::CompressionAudioBitrate, "compression.audio-bitrate"},
        {FieldId::CompressionTargetSize, "compression.target-size"},
        {FieldId::AudioCodec, "audio.codec"},
        {FieldId::AudioBitrate, "audio.bitrate"},
        {FieldId::AudioSampleRate, "audio.sample-rate"},
        {FieldId::AudioChannels, "audio.channels"},
        {FieldId::AudioTracks, "audio.tracks"},
        {FieldId::Subtitles, "subtitles"},
        {FieldId::Adjustments, "adjustments"},
        {FieldId::Metadata, "metadata"},
        {FieldId::OutputName, "output.name"},
        {FieldId::CropRect, "crop.rect"},
        {FieldId::TimelineMode, "timeline.mode"},
        {FieldId::TimelineRanges, "timeline.ranges"},
        {FieldId::SplitRules, "split.rules"},
        {FieldId::MergeLayout, "merge.layout"},
        {FieldId::MergeAudio, "merge.audio"},
        {FieldId::FrameSelection, "frames.selection"},
        {FieldId::ImageFormat, "image.format"},
        {FieldId::ImageQuality, "image.quality"},
        {FieldId::ImageDimensions, "image.dimensions"},
        {FieldId::SubtitleTracks, "subtitle.tracks"},
        {FieldId::SubtitleTarget, "subtitle.target"},
        {FieldId::ExtractAudioTracks, "extract-audio.tracks"},
        {FieldId::ExtractAudioRange, "extract-audio.range"},
}};

const char *fieldIdName(FieldId field) {
  for (const auto &entry : kFieldIdNames) {
    if (entry.first == field) {
      return entry.second;
    }
  }
  return "unknown-field";
}

std::optional<FieldId> fieldIdFromName(const std::string &name) {
  for (const auto &entry : kFieldIdNames) {
    if (name == entry.second) {
      return entry.first;
    }
  }
  return std::nullopt;
}

static constexpr std::array<std::pair<ValueOrigin, const char *>, 4>
    kValueOriginNames = {{
        {ValueOrigin::Automatic, "automatic"},
        {ValueOrigin::Default, "default"},
        {ValueOrigin::Preset, "preset"},
        {ValueOrigin::User, "user"},
}};

const char *valueOriginName(ValueOrigin origin) {
  for (const auto &entry : kValueOriginNames) {
    if (entry.first == origin) {
      return entry.second;
    }
  }
  return "unknown-origin";
}

static constexpr std::array<std::pair<SessionState, const char *>, 5>
    kSessionStateNames = {{
        {SessionState::Created, "created"},
        {SessionState::Preparing, "preparing"},
        {SessionState::Ready, "ready"},
        {SessionState::Failed, "failed"},
        {SessionState::Closed, "closed"},
}};

const char *sessionStateName(SessionState state) {
  for (const auto &entry : kSessionStateNames) {
    if (entry.first == state) {
      return entry.second;
    }
  }
  return "unknown-session-state";
}

// Job states (jobs.h) — the old state names stay stable for persistence.
static constexpr std::array<std::pair<JobState, const char *>, 14>
    kJobStateNames = {{
        {JobState::Queued, "queued"},
        {JobState::Preparing, "preparing"},
        {JobState::Processing, "running"},
        {JobState::Pausing, "pausing"},
        {JobState::Paused, "paused"},
        {JobState::Canceling, "canceling"},
        {JobState::Interrupted, "interrupted"},
        {JobState::Recovering, "recovering"},
        {JobState::Finalizing, "finalizing"},
        {JobState::Validating, "validating"},
        {JobState::Completed, "completed"},
        {JobState::TargetNotMet, "target-not-met"},
        {JobState::Failed, "failed"},
        {JobState::Canceled, "canceled"},
}};

const char *jobStateName(JobState state) {
  for (const auto &entry : kJobStateNames) {
    if (entry.first == state) {
      return entry.second;
    }
  }
  return "unknown";
}

std::optional<JobState> jobStateFromName(const std::string &name) {
  for (const auto &entry : kJobStateNames) {
    if (name == entry.second) {
      return entry.first;
    }
  }
  return std::nullopt;
}

} // namespace velmux::core
