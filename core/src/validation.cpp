#include "velmux/core/validation.h"

#include "velmux/backend/contracts.h"

#include <sys/stat.h>

#include <cmath>
#include <cstring>

namespace velmux::core {

namespace {

bool fileExists(const std::string &path, std::int64_t *sizeOut = nullptr) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
    return false;
  }
  if (sizeOut != nullptr) {
    *sizeOut = st.st_size;
  }
  return true;
}

} // namespace

ValidationReport OutputValidator::validate(const std::string &outputPath,
                                           const ValidationExpectation &expect) {
  ValidationReport report;

  std::int64_t bytes = 0;
  if (!fileExists(outputPath, &bytes)) {
    report.problems.push_back("Output file does not exist: " + outputPath);
    return report;
  }
  report.outputBytes = bytes;
  if (bytes < expect.minimumBytes) {
    report.problems.push_back("Output file is suspiciously small (" +
                              std::to_string(bytes) + " bytes).");
  }

  // Independent parse of the produced file through the inspector machinery.
  MediaSource source{outputPath, outputPath};
  const Error inspectError = inspector_.inspect(source, report.observed);
  if (!inspectError.ok()) {
    report.problems.push_back("Output cannot be parsed: " + inspectError.message);
    return report;
  }

  if (report.observed.container != expect.container) {
    report.problems.push_back(
        std::string("Container mismatch: expected ") +
        containerName(expect.container) + ", found " +
        containerName(report.observed.container) + ".");
  }

  if (expect.expectVideo) {
    if (report.observed.video.empty()) {
      report.problems.push_back("Expected a video stream, found none.");
    } else {
      const VideoStream &video = report.observed.video.front();
      if (expect.videoCodec.has_value() && video.codec != *expect.videoCodec) {
        report.problems.push_back(
            std::string("Video codec mismatch: expected ") +
            codecName(*expect.videoCodec) + ", found " + codecName(video.codec) +
            ".");
      }
      if (expect.expectedHeight > 0 && video.height > 0 &&
          std::abs(video.height - expect.expectedHeight) >
              expect.expectedHeight / 16) {
        report.problems.push_back("Resolution mismatch: expected ~" +
                                  std::to_string(expect.expectedHeight) +
                                  "p, found " + std::to_string(video.height) +
                                  "p.");
      }
    }
  } else if (!report.observed.video.empty()) {
    report.problems.push_back("Expected no video stream, but found one.");
  }

  if (expect.expectAudio) {
    if (report.observed.audio.empty()) {
      report.problems.push_back("Expected an audio stream, found none.");
    } else {
      const AudioStream &audio = report.observed.audio.front();
      if (expect.audioCodec.has_value() && audio.codec != *expect.audioCodec) {
        report.problems.push_back(
            std::string("Audio codec mismatch: expected ") +
            codecName(*expect.audioCodec) + ", found " +
            codecName(audio.codec) + ".");
      }
      if (expect.expectedSampleRate > 0 && audio.sampleRate > 0 &&
          audio.sampleRate != expect.expectedSampleRate) {
        report.problems.push_back("Sample rate mismatch: expected " +
                                  std::to_string(expect.expectedSampleRate) +
                                  " Hz, found " +
                                  std::to_string(audio.sampleRate) + " Hz.");
      }
    }
  }

  if (expect.expectedDurationSec > 0.0 && report.observed.durationSec > 0.0) {
    const double delta =
        std::abs(report.observed.durationSec - expect.expectedDurationSec);
    if (delta > expect.durationToleranceSec) {
      report.problems.push_back(
          "Duration mismatch: expected ~" +
          std::to_string(expect.expectedDurationSec) + "s, found " +
          std::to_string(report.observed.durationSec) + "s.");
    }
  }

  report.ok = report.problems.empty();
  return report;
}

} // namespace velmux::core
