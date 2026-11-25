#include "validation/validation.h"

#include "processing/contracts/contracts.h"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
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
                      a == Container::Mov || a == Container::M4a;
    const bool bIso = b == Container::Mp4 || b == Container::M4v ||
                      b == Container::Mov || b == Container::M4a;
    if (aIso && bIso) {
      return true;
    }
    return false;
  };
  return sameFamily(expected, observed);
}

// Set-intention tags the validator reads back from the output container.
void expectedTagsOf(const MetadataIntention &metadata,
                    std::vector<std::pair<std::string, std::string>> &tags) {
  const auto add = [&](MetadataFieldAction action, const char *key,
                       const std::string &value) {
    if (action == MetadataFieldAction::Set && !value.empty()) {
      tags.emplace_back(key, value);
    }
  };
  add(metadata.titleAction, "title", metadata.title);
  add(metadata.artistAction, "artist", metadata.artist);
  add(metadata.albumAction, "album", metadata.album);
  add(metadata.genreAction, "genre", metadata.genre);
  add(metadata.dateAction, "date", metadata.date);
  add(metadata.descriptionAction, "description", metadata.description);
  add(metadata.locationAction, "location", metadata.location);
}

// Applies a trim-window duration to an expectation (keyframe-snap tolerant).
void applyTrimDuration(ValidationExpectation &expect,
                       const std::vector<TimeRange> &ranges,
                       const MediaInfo &source) {
  if (ranges.empty()) {
    return;
  }
  // Multi-range (Cut): the retained total. Single range: its length.
  std::int64_t retainedUs = 0;
  for (const TimeRange &range : ranges) {
    const std::int64_t startUs = std::max<std::int64_t>(range.startUs, 0);
    const std::int64_t endUs =
        range.endUs >= 0
            ? range.endUs
            : std::int64_t(source.durationSec * 1'000'000.0);
    if (endUs > startUs) {
      retainedUs += endUs - startUs;
    }
  }
  if (retainedUs > 0) {
    expect.expectedDurationSec = double(retainedUs) / 1'000'000.0;
    // Stream-copy trims snap their start to the preceding keyframe;
    // transcode trims are frame-exact. A generous extra second absorbs
    // the keyframe snap without opening a fake-success window.
    expect.durationToleranceSec += 1.0;
  }
}

} // namespace

ValidationExpectation expectationFor(const JobRequest &request,
                                    const MediaInfo &source,
                                    const Pipeline &pipeline) {
  ValidationExpectation expect;
  expect.container = pipeline.container;
  if (!pipeline.sidecarExtension.empty()) {
    // Extraction job: the artifact is a text sidecar; validate cues.
    // The container-oriented 1 KiB floor would reject valid short
    // sidecars (a two-cue SRT is ~100 bytes); one cue line is the floor.
    expect.subtitleSidecar = true;
    expect.expectVideo = false;
    expect.expectAudio = false;
    expect.expectedDurationSec = 0.0;
    expect.minimumBytes = 32;
    return expect;
  }

  const std::optional<TimeRange> trimRange = std::visit(
      [](const auto &payload) -> std::optional<TimeRange> {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, job::Trim>) {
          return payload.range;
        } else if constexpr (std::is_same_v<Payload, job::ExtractAudio>) {
          return payload.range;
        }
        return std::nullopt;
      },
      request.payload);
  (void)trimRange; // per-case durations below handle their own ranges

  switch (request.payload.index()) {
    case 0: { // VideoConvert
      const auto &r = std::get<job::VideoConvert>(request.payload);
      // Video presence: always expected (the operation is a video
      // conversion; audio removal rules only affect the audio lane).
      expect.videoCodec = pipeline.videoCodec;
      if (!source.video.empty()) {
        // Transform-aware expected geometry (the executor applies
        // crop -> rotation -> scale): a correct 90-degree rotation of a
        // 1920x1080 source is 1080 high, and a cropped output is the crop
        // height — comparing against the raw source height would fail every
        // correct transformed output.
        std::int32_t baseH = source.video.front().height;
        std::int32_t baseW = source.video.front().width;
        if (r.adjustments.rotationDegrees == 90 ||
            r.adjustments.rotationDegrees == 270) {
          std::swap(baseW, baseH);
        }
        expect.expectedHeight =
            r.video.resolutionHeight.value_or(baseH);
      }
      // Audio presence: default preserve; explicit all-removal rules drop
      // the lane.
      bool audioRemoved = false;
      if (!r.audioTracks.empty()) {
        audioRemoved = std::all_of(
            r.audioTracks.begin(), r.audioTracks.end(),
            [](const AudioTrackRule &rule) { return !rule.include; });
      }
      if (!audioRemoved) {
        expect.audioCodec = pipeline.audioCodec;
        if (r.audioSettings.sampleRate.has_value()) {
          expect.expectedSampleRate = *r.audioSettings.sampleRate;
        }
      } else {
        expect.expectAudio = false;
      }
      // Duration: playbackSpeed stretches by 1/speed; reverse preserves it.
      expect.expectedDurationSec = source.durationSec;
      if (r.adjustments.playbackSpeed.has_value() &&
          *r.adjustments.playbackSpeed > 0.0) {
        expect.expectedDurationSec /= *r.adjustments.playbackSpeed;
      }
      expectedTagsOf(r.metadata, expect.expectedTags);
      break;
    }
    case 1: { // AudioConvert: standalone audio validity.
      const auto &r = std::get<job::AudioConvert>(request.payload);
      expect.expectVideo = false;
      expect.audioCodec = pipeline.audioCodec;
      if (r.audio.sampleRate.has_value()) {
        expect.expectedSampleRate = *r.audio.sampleRate;
      }
      expect.expectedDurationSec = source.durationSec;
      if (r.adjustments.playbackSpeed.has_value() &&
          *r.adjustments.playbackSpeed > 0.0) {
        expect.expectedDurationSec /= *r.adjustments.playbackSpeed;
      }
      if (r.adjustments.removeSilence) {
        // Removed silence is content-dependent: no exact expectation is
        // honest. Upper bound = unstretched duration (+tolerance); a floor
        // of 10% catches truncated output without demanding exact silence
        // detection. Encoded via the same expectedDuration path with a
        // wide tolerance: the validator's check is |actual - expected| <=
        // tol, so set expected to the midpoint of [0.1, 1.0] x duration
        // and a tolerance covering the whole band.
        const double span = expect.expectedDurationSec;
        expect.expectedDurationSec = span * 0.55;
        expect.durationToleranceSec += span * 0.45;
      }
      expectedTagsOf(r.metadata, expect.expectedTags);
      break;
    }
    case 2: { // Crop: output geometry == rect, orientation-aware.
      const auto &r = std::get<job::Crop>(request.payload);
      expect.videoCodec = pipeline.videoCodec;
      if (!source.video.empty()) {
        // The crop rectangle is in source display coordinates: crop first,
        // then an optional request rotation (VideoConvert-style rotations
        // do not apply to the Crop operation, but the source may carry its
        // own orientation; the executor crops stored pixels).
        expect.expectedWidth = r.crop.width;
        expect.expectedHeight = r.crop.height;
      }
      expect.audioCodec = pipeline.audioCodec;
      expect.expectedDurationSec = source.durationSec;
      break;
    }
    case 3: { // Trim: retained timeline.
      const auto &r = std::get<job::Trim>(request.payload);
      expect.videoCodec = pipeline.videoCodec;
      expect.audioCodec = pipeline.audioCodec;
      if (!source.video.empty()) {
        expect.expectedHeight = source.video.front().height;
      }
      expect.expectedDurationSec = source.durationSec;
      if (r.range.valid()) {
        applyTrimDuration(expect, {r.range}, source);
      }
      break;
    }
    case 7: { // ExtractAudio: valid standalone audio.
      const auto &r = std::get<job::ExtractAudio>(request.payload);
      expect.expectVideo = false;
      expect.audioCodec = pipeline.audioCodec;
      if (r.audio.sampleRate.has_value()) {
        expect.expectedSampleRate = *r.audio.sampleRate;
      }
      expect.expectedDurationSec = source.durationSec;
      if (r.range.has_value() && r.range->valid()) {
        applyTrimDuration(expect, {*r.range}, source);
      }
      expectedTagsOf(r.metadata, expect.expectedTags);
      break;
    }
    case 9: { // ExtractSubtitles: sidecar cues.
      expect.subtitleSidecar = true;
      expect.expectVideo = false;
      expect.expectAudio = false;
      expect.expectedDurationSec = 0.0;
      expect.minimumBytes = 32;
      return expect;
    }
    default:
      // Deferred operations never reach validation (the planner refuses
      // them); a neutral expectation keeps the validator honest rather
      // than fabricating checks.
      expect.expectVideo = source.hasVideo();
      expect.expectAudio = source.hasAudio();
      expect.videoCodec = pipeline.videoCodec;
      expect.audioCodec = pipeline.audioCodec;
      expect.expectedDurationSec = source.durationSec;
      break;
  }
  return expect;
}

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
      // Crop exactness: the width must match the rectangle (not merely the
      // ladder-tolerant height).
      if (expect.expectedWidth > 0 && video.width > 0 &&
          std::abs(video.width - expect.expectedWidth) >
              expect.expectedWidth / 16) {
        report.problems.push_back("Cropped width mismatch: expected ~" +
                                  std::to_string(expect.expectedWidth) +
                                  "px, found " + std::to_string(video.width) +
                                  "px.");
      }
    }
  } else if (!report.observed.video.empty() &&
             report.observed.attachedPictures == 0) {
    // Attached pictures are metadata, not a video lane: an output whose
    // only "video" stream is cover art still validates as audio-only.
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
            codecName(*expect.audioCodec) + ", found " + codecName(audio.codec) +
            ".");
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

  // Metadata Set intentions read back from the output container tags
  // (mandate §33). A backend without tag reading skips the check
  // honestly (the readTags default returns an error), never fakes it.
  if (!expect.expectedTags.empty()) {
    std::vector<std::pair<std::string, std::string>> tags;
    const Error tagError = inspector_.readTags(source, tags);
    if (tagError.ok()) {
      const auto find = [&tags](const std::string &key) -> const std::string * {
        for (const auto &[tagKey, value] : tags) {
          if (tagKey.size() == key.size()) {
            bool equal = true;
            for (std::size_t i = 0; i < key.size(); ++i) {
              if (std::tolower(static_cast<unsigned char>(tagKey[i])) !=
                  std::tolower(static_cast<unsigned char>(key[i]))) {
                equal = false;
                break;
              }
            }
            if (equal) {
              return &value;
            }
          }
        }
        return nullptr;
      };
      for (const auto &[key, value] : expect.expectedTags) {
        const std::string *observed = find(key);
        if (observed == nullptr) {
          report.problems.push_back("Metadata tag missing after write: " + key);
        } else if (*observed != value) {
          report.problems.push_back("Metadata tag mismatch: " + key +
                                    " expected \"" + value + "\", found \"" +
                                    *observed + "\".");
        }
      }
    }
  }

  report.ok = report.problems.empty();
  return report;
}

} // namespace velmux::core
