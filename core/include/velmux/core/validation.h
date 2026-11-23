// Velmux core — output validation.
//
// Never mark a conversion complete merely because the pipeline returned
// success. The output is re-inspected through the same inspector machinery
// (independent parse of the produced file) and checked against what the
// user asked for: container parses, required streams exist, expected
// codecs, duration, timestamp continuity, resolution, sample rate.
#pragma once

#include "velmux/core/job.h"
#include "velmux/core/media_model.h"

#include <string>
#include <vector>

namespace velmux::backend {
class IInspector; // backend contracts
} // namespace velmux::backend

namespace velmux::core {

using velmux::backend::IInspector;

struct ValidationReport {
  bool ok = false;
  std::vector<std::string> problems;
  // Observed properties of the output (for history/diagnostics).
  MediaInfo observed;
  std::int64_t outputBytes = -1;
};

struct ValidationExpectation {
  Container container = Container::Mp4;
  std::optional<VideoCodec> videoCodec;
  std::optional<AudioCodec> audioCodec;
  // Expected stream presence. When video is expected but absent -> problem.
  bool expectVideo = true;
  bool expectAudio = true;
  std::int32_t expectedHeight = 0; // 0 = unchecked
  std::int32_t expectedSampleRate = 0; // 0 = unchecked
  double expectedDurationSec = 0.0; // 0 = unchecked
  // Tolerance for duration comparison (container overhead, rounding).
  double durationToleranceSec = 0.75;
  // Minimum output size: a "successful" transcode of real media producing
  // a near-empty file is a failure.
  std::int64_t minimumBytes = 1024;
  // Subtitle-extraction jobs: the output is a text sidecar (SRT/VTT/ASS),
  // not a media container. Validation checks cue structure instead.
  bool subtitleSidecar = false;
};

class OutputValidator {
public:
  explicit OutputValidator(IInspector &inspector) : inspector_(inspector) {}

  ValidationReport validate(const std::string &outputPath,
                            const ValidationExpectation &expectation);

private:
  IInspector &inspector_;
};

} // namespace velmux::core
