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

// Retained intervals = the complement of the removed ranges over the
// source timeline (the Cut payload's view; normalized the same way the
// planner normalizes before the timeTrim gate).
std::vector<TimeRange> retainedRangesOf(const std::vector<TimeRange> &removed,
                                       std::int64_t totalUs) {
  std::vector<TimeRange> sorted = removed;
  std::sort(sorted.begin(), sorted.end(),
            [](const TimeRange &a, const TimeRange &b) {
              return a.startUs < b.startUs;
            });
  std::vector<TimeRange> merged;
  for (const TimeRange &range : sorted) {
    if (!merged.empty() &&
        (range.startUs <= merged.back().endUs ||
         (merged.back().endUs < 0 && range.startUs <= totalUs))) {
      if (merged.back().endUs < 0 || range.endUs < 0) {
        merged.back().endUs = -1; // extends to the end
      } else {
        merged.back().endUs = std::max(merged.back().endUs, range.endUs);
      }
    } else {
      merged.push_back(range);
    }
  }
  std::vector<TimeRange> retained;
  std::int64_t cursor = 0;
  for (const TimeRange &range : merged) {
    const std::int64_t start = std::max<std::int64_t>(range.startUs, 0);
    const std::int64_t end = range.endUs < 0 ? totalUs : range.endUs;
    if (start > cursor) {
      retained.push_back(TimeRange{cursor, std::min(start, totalUs)});
    }
    cursor = std::max(cursor, end);
    if (cursor >= totalUs) {
      cursor = totalUs;
      break;
    }
  }
  if (cursor < totalUs) {
    retained.push_back(TimeRange{cursor, -1}); // to the end
  }
  return retained;
}

} // namespace

ValidationExpectation expectationFor(const JobRequest &request,
                                     const std::vector<MediaInfo> &sources,
                                     const Pipeline &pipeline) {
  const MediaInfo source = sources.empty() ? MediaInfo{} : sources.front();
  ValidationExpectation expect;
  expect.container = pipeline.container;
  if (!pipeline.sidecarExtension.empty()) {
    if (pipeline.sidecarExtension == "json") {
      // Frame-extraction job: the artifact is the frames manifest; every
      // image it references must exist on disk (the manifest is the index
      // the UI renders from — dangling entries are a broken artifact).
      expect.framesManifest = true;
      expect.expectVideo = false;
      expect.expectAudio = false;
      expect.expectedDurationSec = 0.0;
      expect.minimumBytes = 32;
      return expect;
    }
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
    case 4: { // Cut: retained timeline (complement of the removed ranges).
      const auto &r = std::get<job::Cut>(request.payload);
      expect.expectVideo = source.hasVideo();
      expect.expectAudio = source.hasAudio();
      expect.videoCodec = pipeline.videoCodec;
      expect.audioCodec = pipeline.audioCodec;
      if (!source.video.empty()) {
        expect.expectedHeight = source.video.front().height;
      }
      expect.expectedDurationSec = source.durationSec;
      if (!r.removed.empty()) {
        const std::int64_t totalUs =
            source.durationSec > 0.0
                ? std::int64_t(source.durationSec * 1'000'000.0)
                : INT64_MAX / 4;
        applyTrimDuration(expect, retainedRangesOf(r.removed, totalUs),
                          source);
      }
      break;
    }
    case 5: { // Split: the primary artifact is the first PART; part
      // durations vary by boundary strategy (equal/custom/chapters/
      // scenes), so no single duration expectation is honest — the parts
      // are validated structurally and cross-checked per part.
      expect.expectVideo = source.hasVideo();
      expect.expectAudio = source.hasAudio();
      expect.videoCodec = pipeline.videoCodec;
      expect.audioCodec = pipeline.audioCodec;
      if (!source.video.empty()) {
        expect.expectedHeight = source.video.front().height;
      }
      expect.expectedDurationSec = 0.0; // per-part; no single expectation
      break;
    }
    case 6: { // Merge: sequential = the sum; spatial = the duration policy.
      const auto &r = std::get<job::Merge>(request.payload);
      expect.expectVideo = source.hasVideo();
      if (r.rules.layout == MergeLayout::Sequential) {
        // Sequential keeps the first source's characteristics (copy when
        // compatible, else normalized to it): geometry from source 0;
        // audio when ANY source carries it (normalization fills silence).
        expect.expectAudio = std::any_of(sources.begin(), sources.end(),
                                         [](const MediaInfo &s) {
                                           return s.hasAudio();
                                         });
        expect.videoCodec = pipeline.videoCodec;
        expect.audioCodec = pipeline.audioCodec;
        if (!source.video.empty()) {
          expect.expectedHeight = source.video.front().height;
        }
        double sum = 0.0;
        bool known = !sources.empty();
        for (const MediaInfo &info : sources) {
          if (info.durationSec <= 0.0) {
            known = false;
            break;
          }
          sum += info.durationSec;
        }
        expect.expectedDurationSec = known ? sum : 0.0;
      } else {
        // Spatial composition: slot geometry is aspect-fitted per layout
        // (side-by-side stacks widths at a common height, top-bottom
        // stacks heights at a common width) — honest no-expectation on
        // geometry; the duration follows the explicit longer/shorter
        // policy; the audio lane follows the selection.
        expect.expectedHeight = 0;
        expect.videoCodec = pipeline.videoCodec;
        if (r.rules.audio == MergeAudioSelection::None) {
          expect.expectAudio = false;
        } else {
          expect.expectAudio = true;
          expect.audioCodec = pipeline.audioCodec;
        }
        if (sources.size() == 2 && sources[0].durationSec > 0.0 &&
            sources[1].durationSec > 0.0) {
          expect.expectedDurationSec =
              r.rules.longerDuration
                  ? std::max(sources[0].durationSec, sources[1].durationSec)
                  : std::min(sources[0].durationSec, sources[1].durationSec);
        } else {
          expect.expectedDurationSec = 0.0;
        }
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

  // Frame-extraction jobs: the output is the frames manifest. Validate it
  // as an index (independent of the executor that wrote it): every image
  // file it references must exist with real content — a dangling manifest
  // entry is a broken artifact the UI would render as a hole.
  if (expect.framesManifest) {
    if (bytes < expect.minimumBytes) {
      report.problems.push_back("Frames manifest is suspiciously small (" +
                                std::to_string(bytes) + " bytes).");
    }
    std::ifstream in(outputPath, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    if (text.find("\"frames\":[") == std::string::npos) {
      report.problems.push_back("Frames manifest carries no frame array.");
    } else {
      std::size_t images = 0;
      for (std::size_t pos = text.find("\"file\":\"");
           pos != std::string::npos;
           pos = text.find("\"file\":\"", pos + 1)) {
        pos += 8; // skip "file":"
        std::string file;
        for (std::size_t i = pos; i < text.size() && text[i] != '"'; ++i) {
          if (text[i] == '\\' && i + 1 < text.size()) {
            ++i; // unescape \" and \\ (the manifest's only escapes)
          }
          file += text[i];
        }
        std::int64_t imageBytes = 0;
        if (file.empty() || !fileExists(file, &imageBytes) ||
            imageBytes <= 0) {
          report.problems.push_back("Manifest image missing: " + file);
        } else {
          ++images;
        }
      }
      if (images == 0) {
        report.problems.push_back("Manifest references no images.");
      }
    }
    report.ok = report.problems.empty();
    return report;
  }

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
