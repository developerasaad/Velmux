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

// A window on the source timeline (spec §15/§16: trimming; §20: sample
// generation). Microseconds. startUs must be >= 0; endUs < 0 means
// open-ended (until end of stream). Transcoded output keeps the source
// timeline's timestamps (matched-source-timestamp comparison, spec §20);
// stream-copied output snaps its start to the keyframe at/before startUs
// (compressed data cannot be re-timed without re-encoding).
struct TimeRange {
  std::int64_t startUs = 0;
  std::int64_t endUs = -1;
  bool active() const { return startUs > 0 || endUs >= 0; }
};

// Crop rectangle on the SOURCE frame (pre-rotation coordinates, pixels,
// top-left origin). width/height <= 0 means no crop.
struct CropRect {
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t width = 0;
  std::int32_t height = 0;
  bool active() const { return width > 0 && height > 0; }
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
  // Geometry transforms (spec §9). Order: crop -> rotation -> flips.
  // Rotation is clockwise, one of 0/90/180/270 (odd values swap width and
  // height).
  std::optional<CropRect> crop;
  std::int32_t rotationDegrees = 0;
  bool flipHorizontal = false;
  bool flipVertical = false;
  // Advanced encoder controls (spec §18). All optional: nullopt keeps
  // the encoder default. Only backends reporting `videoEncoderTuning`
  // receive requests that set any of these (planner-enforced; hybrid
  // stage contracts do not carry them in v1).
  std::optional<std::string> profile; // e.g. "baseline"/"main"/"high"
  std::optional<std::string> level;   // e.g. "4.0"/"5.1"
  std::optional<PixelFormat> pixelFormat;
  std::optional<std::int32_t> gopSize; // >= 1 keyframe interval in frames
  std::optional<std::int32_t> bFrames; // >= 0 B-frames between references
  // HDR handling for transcodes (spec §9).
  HdrPolicy hdrPolicy = HdrPolicy::Auto;
};

struct AudioRequest {
  bool enabled = true;
  std::optional<AudioCodec> codec;
  std::optional<std::int64_t> bitRate;
  std::optional<std::int32_t> sampleRate;
  std::optional<std::int32_t> channels;
  // Audio DSP chain (spec §10), applied before the output encoder by
  // backends that report audioDsp. Order: gain -> fades -> loudness
  // normalization -> speed -> silence removal.
  std::optional<double> gainDb; // -60..+60 dB volume adjustment
  std::optional<std::int32_t> fadeInMs;
  std::optional<std::int32_t> fadeOutMs;
  bool normalizeLoudness = false; // single-pass loudness normalization
  // Pitch-preserving time stretching, 0.5..2.0 (1.0 = unchanged). Changes
  // the output duration; validation expectations account for it.
  std::optional<double> playbackSpeed;
  bool removeSilence = false; // shortens pauses; duration-only checks relax
  // PCM output bit depth (16/24/32); meaningful for PCM/WAV output.
  std::optional<std::int32_t> pcmBitDepth;
  // Multi-track audio (spec §9): -1 selects the first source audio track;
  // a valid index selects that track. With allTracks, every source audio
  // track is processed (encoded or copied) with its language metadata.
  std::int32_t sourceTrackIndex = -1;
  bool allTracks = false;
  // Replace the source's audio with this external audio file (spec §9).
  // The output timeline follows the video stream; a shorter/longer
  // replacement is honestly reflected in the result.
  std::optional<std::string> replaceAudioUri;
  // Embed this external image as cover art / attached picture (spec §10).
  std::optional<std::string> coverArtUri;
  // Carry the source's attached pictures into the output (when the
  // container supports them).
  bool preserveCoverArt = true;
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
  // Process only this window of the source (trimming, sample generation).
  TimeRange trim;
  // Metadata policy (spec advanced controls): false preserves container
  // and stream tags through transcode/remux; true strips them.
  bool stripMetadata = false;
};

// Result of sample generation (spec §20): a short preview transcode of a
// representative window using the requested settings. The window keeps
// source-timeline timestamps so the UI can compare original vs sample at
// matched positions.
struct SampleResult {
  std::string outputPath;
  std::int64_t startUs = 0;
  std::int64_t endUs = 0;
  std::int64_t sizeBytes = -1;
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
