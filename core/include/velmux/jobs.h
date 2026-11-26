// Velmux core — job model.
//
// Durable typed operation requests and observable job state (mandate §15,
// §37). The accepted configuration is immutable after submission; retry
// preserves the accepted intent; reuse settings creates a new editable
// configuration through a fresh session.
#pragma once

#include <velmux/configuration.h>
#include <velmux/media.h>
#include <velmux/types.h>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace velmux::core {

struct JobRequest;

namespace job {
using VideoConvert = VideoConvertRequest;
using AudioConvert = AudioConvertRequest;
using Crop = CropRequest;
using Trim = TrimRequest;
using Cut = CutRequest;
using Split = SplitRequest;
using Merge = MergeRequest;
using ExtractAudio = ExtractAudioRequest;
using ExtractFrames = ExtractFramesRequest;
using ExtractSubtitles = ExtractSubtitlesRequest;
} // namespace job

// The durable accepted intent: one typed payload per approved operation
// (mandate §15: cohesive typed models, fields that do not belong to the
// requested operation are rejected at the boundary).
using JobRequestPayload =
    std::variant<job::VideoConvert, job::AudioConvert, job::Crop, job::Trim,
                 job::Cut, job::Split, job::Merge, job::ExtractAudio,
                 job::ExtractFrames, job::ExtractSubtitles>;

struct JobRequest {
  Operation operation = Operation::VideoConvert;
  JobRequestPayload payload;
};

// ---------------------------------------------------------------------------
// Observable job state (mandate §37.2)
// ---------------------------------------------------------------------------

enum class JobState : std::uint8_t {
  Queued,
  Preparing,
  Processing,
  Pausing,
  Paused,
  Canceling,
  Interrupted,
  Recovering,
  Finalizing,
  Validating,
  Completed,
  TargetNotMet,  // valid output that exceeded a target-size objective
  Failed,
  Canceled,
};

const char *jobStateName(JobState state);
std::optional<JobState> jobStateFromName(const std::string &name);

inline bool isTerminalJobState(JobState state) {
  switch (state) {
    case JobState::Completed:
    case JobState::TargetNotMet:
    case JobState::Failed:
    case JobState::Canceled:
      return true;
    default:
      return false;
  }
}

inline bool isActiveJobState(JobState state) {
  switch (state) {
    case JobState::Queued:
    case JobState::Preparing:
    case JobState::Processing:
    case JobState::Pausing:
    case JobState::Canceling:
    case JobState::Recovering:
    case JobState::Finalizing:
    case JobState::Validating:
      return true;
    default:
      return false;
  }
}

// One produced output file. Multi-output operations (Split, Extract
// Frames, batch extraction) expose their complete output manifest.
struct JobOutput {
  std::string path;
  std::int64_t sizeBytes = -1;
  // Output kind: "media", "image", "subtitle", "manifest".
  std::string kind;
};

struct JobSnapshot {
  std::string id;
  std::string submissionId;
  Operation operation = Operation::VideoConvert;
  JobState state = JobState::Queued;
  // 0..1, or negative when unknown (never fabricated).
  double progress = -1.0;
  std::string sourceName;
  std::string outputName;
  std::string outputPath;  // primary output (first manifest entry)
  std::vector<JobOutput> outputs;
  std::string error;
  std::string errorCode;  // stable canonical error identifier
  std::int64_t outputSizeBytes = -1;
  std::int64_t inputSizeBytes = -1;
  std::string pipelineSummary;
  std::int64_t createdAtMs = 0;
  std::int64_t finishedAtMs = 0;
};

// Durable job record persisted in SQLite (schema v4). Everything needed to
// reconstruct the job after process termination (mandate §37.1).
struct JobRecord {
  std::string id;
  std::string submissionId;
  JobState state = JobState::Queued;
  JobRequest request;
  std::vector<MediaInfo> sourceInfo;  // inspection results at submit time
  // Selected pipeline (empty until planned).
  struct {
    bool planned = false;
    std::string summary;
    bool usesHardware = false;
  } pipeline;
  std::string outputPath;
  std::vector<JobOutput> outputs;
  std::int64_t inputSizeBytes = -1;
  std::int64_t outputSizeBytes = -1;
  double progress = -1.0;
  std::string error;
  std::string errorCode;
  std::int64_t createdAtMs = 0;
  std::int64_t updatedAtMs = 0;
  std::int64_t startedAtMs = 0;
  std::int64_t finishedAtMs = 0;
  // Schema version of this record (for forward migrations).
  std::int32_t schemaVersion = 4;
};

// Result of sample generation (mandate §22): a short preview transcode of
// a representative window using the session's resolved settings.
struct SampleResult {
  std::string outputPath;
  std::int64_t startUs = 0;
  std::int64_t endUs = 0;
  std::int64_t sizeBytes = -1;
};

} // namespace velmux::core
