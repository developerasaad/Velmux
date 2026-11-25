// FFmpeg backend — the internal execution view of a typed request.
//
// FfmpegExecutor normalizes each operation's payload into this intent at
// the TOP of execute() (spec: executeRemux/executeTranscode consume the
// intent, never the payload). The builder resolves the compression policy
// into concrete rate control and flattens the typed fields into the lanes
// the execution paths already understand.
//
// Deferred operations: Split / Merge / multi-range Cut / ExtractFrames are
// carried in full (split/merge/image rules) for the future executors, but
// canExecute refuses them in this build — they never reach the execution
// paths.
#pragma once

#include <velmux/jobs.h>
#include <velmux/media.h>

#include "compression/compression.h"
#include "processing/pipeline.h"

#include <optional>
#include <string>
#include <vector>

namespace velmux::ffmpeg {

using velmux::core::AudioAdjustments;
using velmux::core::AudioSettings;
using velmux::core::Container;
using velmux::core::CropIntent;
using velmux::core::Error;
using velmux::core::FrameSelection;
using velmux::core::ImageOutputRules;
using velmux::core::JobRequest;
using velmux::core::MediaInfo;
using velmux::core::MediaSource;
using velmux::core::MergeRules;
using velmux::core::MetadataIntention;
using velmux::core::Pipeline;
using velmux::core::Result;
using velmux::core::SplitRules;
using velmux::core::TimeRange;

// A crop rectangle in source display coordinates (mirrors the execution
// paths' vocabulary).
struct CropRect {
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t width = 0;
  std::int32_t height = 0;
  bool active() const { return width >= 2 && height >= 2; }
};

struct VideoIntent {
  bool enabled = true;
  std::optional<std::int32_t> resolutionHeight;
  std::optional<double> frameRate;
  std::optional<CropRect> crop;
  std::int32_t rotationDegrees = 0;
  bool flipHorizontal = false;
  bool flipVertical = false;
  // Timing transforms (transcode only; refused with stream copy).
  std::optional<double> playbackSpeed;
  bool reverse = false;
  bool hdrToSdr = false;
};

// Resolved rate control (mandate §20): bitrate mode or quality mode.
struct RateControlIntent {
  bool bitrateMode = false;
  std::int64_t bitRate = 0;   // bits per second (bitrate mode)
  std::int32_t quality = 65;  // 0..100 user scale (quality mode)
};

// Subtitle lanes flattened for the execution paths.
struct SubtitleIntent {
  // Preserve/embed: source subtitle tracks ride through to the output.
  bool keepSubtitles = false;
  // Burn-in: the burn-in rule (external file, else the source track).
  bool burnIn = false;
  std::string burnInExternal;    // local-file URI of the external subtitles
  std::int32_t burnInTrack = -1; // source subtitle ordinal (audio-free list)
  // Burn-in appearance (force_style additions).
  std::optional<std::int32_t> fontSize;
  std::optional<std::string> color;
  std::optional<std::int32_t> position;
};

// The timeline window the execution paths honor (half-open, microseconds;
// endUs < 0 = open-ended). One window: multi-range cuts are deferred.
struct TrimIntent {
  std::int64_t startUs = 0;
  std::int64_t endUs = -1;
  bool active() const { return startUs != 0 || endUs != -1; }
};

struct ExecutionIntent {
  MediaSource source;
  Container outputContainer = Container::Mp4;
  bool wantsVideo = true;
  bool wantsAudio = true;

  VideoIntent video;
  RateControlIntent rate;

  // Audio lanes: retained source tracks (audio-only ordinals, 0-based),
  // the replacement file (v1: replaces the whole audio lane) and external
  // additions (each becomes an extra track aligned at video start).
  std::vector<std::int32_t> audioSourceOrdinals;
  std::string replaceAudioUri;
  std::vector<std::string> externalAudioUris;
  // Settings applied to every processing lane.
  AudioSettings audioSettings;
  // The audio DSP chain (audio-only operations carry it).
  std::optional<AudioAdjustments> audioDsp;

  SubtitleIntent subtitles;
  TrimIntent trim;
  MetadataIntention metadata;
  // Artwork: the metadata Set intention (external image to attach).
  std::optional<std::string> coverArtUri;
  bool preserveCoverArt = true;

  // Deferred operations (Split/Merge/Cut/ExtractFrames): carried verbatim
  // for the future executors; canExecute refuses them in this build.
  SplitRules splitRules;
  MergeRules mergeRules;
  std::vector<TimeRange> cutRemoved;
  FrameSelection frameSelection;
  ImageOutputRules imageRules;

  // The ExtractSubtitles operation (sidecar extraction).
  bool subtitleExtract = false;
  std::vector<std::int32_t> extractSubtitleTracks;
};

// Builds the execution view of a typed request against the planned
// pipeline and the inspected source. `sampleWindow` (core-internal, sample
// generation) replaces the timeline selection when present. Fails honestly
// on shapes the execution paths cannot serve (e.g. mixed additions with
// stream copy).
Result<ExecutionIntent> buildIntent(
    const JobRequest &request, const Pipeline &pipeline,
    const MediaInfo &source,
    const std::optional<TimeRange> &sampleWindow = std::nullopt);

} // namespace velmux::ffmpeg
