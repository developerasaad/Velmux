// Velmux core — output validation.
//
// Never mark a conversion complete merely because the pipeline returned
// success. The output is re-inspected through the same inspector machinery
// (independent parse of the produced file) and checked against what the
// user asked for: container parses, required streams exist, expected
// codecs, duration, timestamp continuity, resolution, sample rate.
// Per-operation expectations (mandate §39) derive from the typed request.
#pragma once

#include <velmux/jobs.h>
#include <velmux/media.h>
#include "processing/pipeline.h"

#include <string>
#include <utility>
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
  std::int32_t expectedWidth = 0;  // 0 = unchecked (Crop exactness)
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
  // Frame-extraction jobs: the output is the frames manifest (JSON) and
  // the images it references. Validation checks the manifest structure
  // and that every referenced image exists with real content.
  bool framesManifest = false;
  // Metadata Set intentions read back from the output container tags
  // (mandate §33): key + expected value. Checked when the inspector can
  // read tags; skipped honestly otherwise.
  std::vector<std::pair<std::string, std::string>> expectedTags;
};

class OutputValidator {
public:
  explicit OutputValidator(IInspector &inspector) : inspector_(inspector) {}

  ValidationReport validate(const std::string &outputPath,
                            const ValidationExpectation &expectation);

private:
  IInspector &inspector_;
};

// Derives the per-operation expectation from the accepted request, the
// inspected sources (all of them: Merge sums durations across sources)
// and the planned pipeline. Geometry is
// transform-aware (crop -> rotation -> scale), durations are
// trim/speed/silence-aware, audio presence follows the track rules.
ValidationExpectation expectationFor(const JobRequest &request,
                                     const std::vector<MediaInfo> &sources,
                                     const Pipeline &pipeline);

} // namespace velmux::core
