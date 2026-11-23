#include "velmux/core/validation.h"

#include "velmux/backend/contracts.h"

#include <sys/stat.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

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

// Container families that share one demuxer name on inspection: the
// distinction (WebM vs Matroska, MP4 vs M4V) is a muxer-side restriction
// that a re-parse of the output cannot always see. Family members count as
// a match; the codec checks carry the real compatibility constraints.
bool containerFamilyMatches(Container expected, Container observed) {
  const auto sameFamily = [](Container a, Container b) {
    if (a == b) {
      return true;
    }
    const bool aMkv = a == Container::Mkv || a == Container::WebM;
    const bool bMkv = b == Container::Mkv || b == Container::WebM;
    if (aMkv && bMkv) {
      return true;
    }
    const bool aIso = a == Container::Mp4 || a == Container::M4v ||
                      a == Container::Mov;
    const bool bIso = b == Container::Mp4 || b == Container::M4v ||
                      b == Container::Mov;
    if (aIso && bIso) {
      return true;
    }
    return false;
  };
  return sameFamily(expected, observed);
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

  // Subtitle-extraction jobs: the output is a text sidecar. Validate the
  // cue structure directly (independent of the code that wrote it) — the
  // inspector cannot parse SRT/VTT/ASS files as media containers.
  if (expect.subtitleSidecar) {
    if (bytes < expect.minimumBytes) {
      report.problems.push_back("Subtitle sidecar is suspiciously small (" +
                                std::to_string(bytes) + " bytes).");
    }
    std::ifstream in(outputPath, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    std::size_t cues = 0;
    for (std::size_t pos = 0; pos < text.size();) {
      const std::size_t eol = text.find('\n', pos);
      const std::string line =
          text.substr(pos, eol == std::string::npos ? std::string::npos
                                                    : eol - pos);
      // SRT/WebVTT cues carry "-->"; ASS/SSA carry "Dialogue:" lines.
      if (line.find("-->") != std::string::npos ||
          line.rfind("Dialogue:", 0) == 0) {
        ++cues;
      }
      if (eol == std::string::npos) {
        break;
      }
      pos = eol + 1;
    }
    if (cues == 0) {
      report.problems.push_back(
          "Subtitle sidecar contains no recognizable cues.");
    }
    report.ok = report.problems.empty();
    return report;
  }

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

  if (!containerFamilyMatches(expect.container, report.observed.container)) {
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
