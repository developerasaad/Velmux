// Velmux core — job model.
//
// Durable conversion request and job snapshot. Mirrors the React Native
// bridge contract (apps/mobile/src/native/NativeVelmux.ts). React Native is
// never the source of truth: jobs live in SQLite through JobManager.
#pragma once

#include "velmux/core/media_model.h"
#include "velmux/core/pipeline_fwd.h"
#include "velmux/core/types.h"

#include <optional>
#include <string>

namespace velmux::core {

enum class JobState : std::uint8_t {
  Queued,
  Preparing,
  Running,
  Checkpointing,
  Paused,
  Recovering,
  Interrupted,
  Failed,
  Canceled,
  Validating,
  Completed,
};

const char *jobStateName(JobState state);
std::optional<JobState> jobStateFromName(const std::string &name);

// ---------------------------------------------------------------------------
// User intent
// ---------------------------------------------------------------------------

enum class GoalKind : std::uint8_t {
  Recommended,
  SmallerFile,
  HighQuality,
  FitUnderSize,
  Custom,
};

enum class RateControl : std::uint8_t { Auto, Bitrate, Quality };

enum class SubtitlePolicy : std::uint8_t {
  Preserve,
  Remove,
  Extract,
  BurnIn,
};

enum class ProcessingMode : std::uint8_t {
  Auto,
  PreferNative,
  PreferSoftware,
  Balanced,
};

struct CompressionGoal {
  GoalKind kind = GoalKind::Recommended;
  // Set for FitUnderSize.
  std::int64_t targetSizeBytes = 0;
};

struct VideoRequest {
  bool enabled = true;
  std::optional<VideoCodec> codec;
  std::optional<std::int32_t> resolutionHeight;
  std::optional<double> frameRate;
  RateControl rateControl = RateControl::Auto;
  std::optional<std::int64_t> bitRate; // bits per second
  // Encoder quality parameter, 0..100 scale (higher = better quality).
  std::optional<std::int32_t> quality;
};

struct AudioRequest {
  bool enabled = true;
  std::optional<AudioCodec> codec;
  std::optional<std::int64_t> bitRate;
  std::optional<std::int32_t> sampleRate;
  std::optional<std::int32_t> channels;
};

struct SubtitleRequest {
  SubtitlePolicy policy = SubtitlePolicy::Preserve;
  // External subtitle source for burn-in / mux-in, when provided.
  std::optional<std::string> externalUri;
  // Timing offset applied to subtitle cues, milliseconds.
  std::int32_t timingOffsetMs = 0;
};

struct ConversionRequest {
  MediaSource source;
  std::optional<std::string> outputName;
  std::optional<Container> outputContainer;
  CompressionGoal goal;
  VideoRequest video;
  AudioRequest audio;
  SubtitleRequest subtitles;
  ProcessingMode processingMode = ProcessingMode::Auto;
};

// ---------------------------------------------------------------------------
// Observable job state
// ---------------------------------------------------------------------------

struct JobSnapshot {
  std::string id;
  JobState state = JobState::Queued;
  // 0..1, or negative when unknown (never fabricated).
  double progress = -1.0;
  std::string sourceName;
  std::string outputName;
  // Absolute path of the produced file once known (for open/share).
  std::string outputPath;
  std::string error;
  std::int64_t outputSizeBytes = -1;
  std::int64_t inputSizeBytes = -1;
  std::string pipelineSummary;
};

// Durable job record persisted in SQLite. Everything needed to reconstruct
// the job after process termination (spec §24).
struct JobRecord {
  std::string id;
  JobState state = JobState::Queued;
  ConversionRequest request;
  MediaInfo sourceInfo; // inspection result at submit time
  Pipeline pipeline;   // selected pipeline (empty until planned)
  std::string outputPath;
  std::int64_t inputSizeBytes = -1;
  std::int64_t outputSizeBytes = -1;
  double progress = -1.0;
  std::string error;
  std::int64_t createdAtMs = 0;
  std::int64_t updatedAtMs = 0;
  std::int64_t startedAtMs = 0;
  std::int64_t finishedAtMs = 0;
  // Schema version of this record (for forward migrations).
  std::int32_t schemaVersion = 1;
};

} // namespace velmux::core
