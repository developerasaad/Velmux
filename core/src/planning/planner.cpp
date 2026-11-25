#include "planning/planner.h"

#include "compression/compression.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace velmux::core {

namespace {

// Static priors used before telemetry exists (frames/sec for 1080p-class
// content on mid-range hardware; the cost model only compares relatively).
struct BackendPrior {
  BackendId backend;
  double softwareFps; // full software transcode
  double hardwareFps; // hardware-assisted path
  double setupMs;
  double transferMsPerFrame; // CPU<->GPU bridging cost
};

const BackendPrior kPriors[] = {
    {BackendId::Android, 45.0, 110.0, 120.0, 0.9},
    {BackendId::Apple, 60.0, 140.0, 90.0, 0.5},
    {BackendId::Ffmpeg, 80.0, 80.0, 25.0, 0.0},
};

const BackendPrior &priorFor(BackendId backend) {
  for (const auto &p : kPriors) {
    if (p.backend == backend) {
      return p;
    }
  }
  static const BackendPrior kFallback{BackendId::Ffmpeg, 50.0, 50.0, 100.0, 0.0};
  return kFallback;
}

bool backendServesWholePipeline(const CapabilityRegistry &caps,
                                BackendId backend, const Pipeline &pipeline) {
  for (const auto &assignment : pipeline.stages) {
    if (assignment.backend != backend) {
      continue;
    }
    switch (assignment.stage) {
      case StageId::Demux:
      case StageId::Mux:
        if (caps.capabilitiesOf(backend) == nullptr) {
          return false;
        }
        break;
      case StageId::Decode:
      case StageId::Encode:
      case StageId::Bitstream:
        break; // codec-specific checks happen for decode/encode below
      case StageId::VideoProcessing:
      case StageId::AudioProcessing:
      case StageId::SubtitleProcessing:
        if (const BackendCapabilities *bc = caps.capabilitiesOf(backend)) {
          if (assignment.stage == StageId::VideoProcessing && !bc->videoScaling) {
            return false;
          }
          if (assignment.stage == StageId::AudioProcessing &&
              (!bc->audioResampling && !bc->audioDsp)) {
            return false;
          }
        } else {
          return false;
        }
        break;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Normalized conversion shape: every typed payload reduces to this view the
// capability gates and cost model consume (spec §2 mapping).
// ---------------------------------------------------------------------------

struct Shape {
  MediaSource source;
  Container outputContainer = Container::Mp4;
  bool wantsVideo = true;
  bool wantsAudio = true;
  // nullopt = Automatic: preserve the compatible source codec.
  std::optional<VideoCodec> videoCodec;
  std::optional<AudioCodec> audioCodec;
  std::optional<std::int32_t> resolutionHeight;
  std::optional<double> frameRate;
  CropIntent crop;
  bool hasCrop = false;
  std::int32_t rotationDegrees = 0;
  bool flipHorizontal = false;
  bool flipVertical = false;
  std::optional<double> playbackSpeed;
  bool reverse = false;
  bool hdrToSdr = false;
  // Retained timeline intervals (empty = the whole timeline).
  std::vector<TimeRange> trimRanges;
  // Audio lane management (mandate §23).
  bool allSourceAudioTracks = false;  // keep every source lane
  std::int32_t audioTrackIndex = -1;  // explicit single-track selection
  std::optional<std::string> replaceAudioUri;
  bool hasExternalAudio = false;      // external addition lanes exist
  std::optional<std::string> coverArtUri;
  bool preserveCoverArt = true;
  // Audio DSP (audio-only operations, mandate §32).
  std::optional<double> gainDb;
  std::optional<std::int32_t> fadeInMs;
  std::optional<std::int32_t> fadeOutMs;
  bool normalize = false;
  bool removeSilence = false;
  std::optional<double> audioSpeed;
  // Subtitle lanes.
  bool subtitleExtract = false;  // the ExtractSubtitles operation
  bool burnIn = false;           // a burn-in rule exists
  bool keepSubtitles = false;    // a preserved/embedded source track remains
  bool stripMetadata = false;    // the all-Clear intention
  CompressionIntent compression;
  BudgetInputs budget;
};

// True when the metadata intention clears every familiar field (the moral
// equivalent of the retired stripMetadata control).
bool isFullStrip(const MetadataIntention &metadata) {
  const auto cleared = [](MetadataFieldAction action) {
    return action == MetadataFieldAction::Clear;
  };
  return cleared(metadata.titleAction) && cleared(metadata.artistAction) &&
         cleared(metadata.albumAction) && cleared(metadata.genreAction) &&
         cleared(metadata.dateAction) && cleared(metadata.descriptionAction) &&
         cleared(metadata.locationAction) && cleared(metadata.artworkAction);
}

void fillAudioSettingsShape(const AudioSettings &settings, Shape &shape) {
  shape.audioCodec = settings.codec;
  shape.budget.audio = settings;
}

void fillCompressionShape(const CompressionIntent &compression,
                          const MetadataIntention &metadata, Shape &shape) {
  shape.compression = compression;
  shape.stripMetadata = isFullStrip(metadata);
}

// VideoConvert (mandate §19/§23/§24/§25): field mapping into the shape.
Error shapeForVideoConvert(const job::VideoConvert &request,
                           const std::vector<MediaInfo> &sources,
                           const MediaInfo &source, Shape &shape) {
  shape.source = request.source;
  shape.outputContainer =
      request.output.container.value_or(source.container);
  shape.videoCodec = request.video.codec;
  shape.resolutionHeight = request.video.resolutionHeight.has_value()
                               ? request.video.resolutionHeight
                               : request.video.customHeight;
  shape.frameRate = request.video.frameRate.has_value()
                        ? std::optional<double>(
                              request.video.frameRate->toDouble())
                        : std::nullopt;
  // Track management (mandate §23): empty rules = preserve all.
  const std::int32_t audioTrackCount = std::int32_t(source.audio.size());
  std::vector<std::int32_t> keptTracks;
  for (const AudioTrackRule &rule : request.audioTracks) {
    if (rule.sourceTrackIndex >= audioTrackCount ||
        rule.sourceTrackIndex < -1) {
      return Error::make(ErrorCode::InvalidTrackIdentity,
                         "audio track index " +
                             std::to_string(rule.sourceTrackIndex) +
                             " is outside this source (0.." +
                             std::to_string(audioTrackCount - 1) + ").");
    }
    if (rule.externalUri.has_value() && !rule.externalUri->empty()) {
      if (rule.replace) {
        if (rule.sourceTrackIndex < 0 && audioTrackCount > 0) {
          return Error::make(
              ErrorCode::InvalidArgument,
              "replacing audio requires an explicit source track index.");
        }
        shape.replaceAudioUri = rule.externalUri;
      } else {
        shape.hasExternalAudio = true; // external addition lane
      }
      continue;
    }
    if (!rule.include) {
      continue; // removal rule: the track does not survive
    }
    if (rule.sourceTrackIndex < 0) {
      shape.allSourceAudioTracks = true; // default track = all-preserve
      continue;
    }
    keptTracks.push_back(rule.sourceTrackIndex);
  }
  if (request.audioTracks.empty()) {
    shape.allSourceAudioTracks = true;
  } else if (!shape.replaceAudioUri.has_value()) {
    // Replacement (v1) replaces the whole audio lane; otherwise the kept
    // set decides. A multi-keep selects every kept lane (the executor
    // filters the exact set from the intent's rules).
    if (shape.allSourceAudioTracks) {
      // keep all
    } else if (keptTracks.empty()) {
      shape.wantsAudio = shape.hasExternalAudio; // everything removed
    } else if (keptTracks.size() == 1) {
      shape.audioTrackIndex = keptTracks.front();
    } else {
      shape.allSourceAudioTracks = true;
    }
  }
  // Subtitle lanes (mandate §24): empty = preserve all.
  const std::int32_t subTrackCount = std::int32_t(source.subtitles.size());
  for (const SubtitleTrackRule &rule : request.subtitleTracks) {
    if (rule.sourceTrackIndex >= subTrackCount ||
        rule.sourceTrackIndex < -1) {
      return Error::make(ErrorCode::InvalidTrackIdentity,
                         "subtitle track index " +
                             std::to_string(rule.sourceTrackIndex) +
                             " is outside this source (0.." +
                             std::to_string(subTrackCount - 1) + ").");
    }
    switch (rule.policy) {
      case SubtitleTrackPolicy::Preserve:
      case SubtitleTrackPolicy::Embed:
        shape.keepSubtitles = true;
        break;
      case SubtitleTrackPolicy::Remove:
        break;
      case SubtitleTrackPolicy::BurnIn:
        shape.burnIn = true;
        break;
    }
  }
  if (request.subtitleTracks.empty() && source.hasSubtitles()) {
    shape.keepSubtitles = true;
  }
  if (shape.burnIn && !request.subtitleTracks.empty() &&
      std::none_of(request.subtitleTracks.begin(),
                   request.subtitleTracks.end(),
                   [](const SubtitleTrackRule &rule) {
                     return rule.policy == SubtitleTrackPolicy::BurnIn &&
                            (rule.externalUri.has_value() ||
                             rule.sourceTrackIndex >= 0);
                   })) {
    return Error::make(ErrorCode::InvalidArgument,
                       "Burn-in requested but no subtitle track or external "
                       "subtitle file was provided.");
  }
  // Adjustments (mandate §25).
  shape.rotationDegrees = request.adjustments.rotationDegrees;
  shape.flipHorizontal = request.adjustments.flipHorizontal;
  shape.flipVertical = request.adjustments.flipVertical;
  shape.playbackSpeed = request.adjustments.playbackSpeed;
  shape.reverse = request.adjustments.reverse;
  shape.hdrToSdr = request.adjustments.hdrToSdr;
  fillAudioSettingsShape(request.audioSettings, shape);
  fillCompressionShape(request.compression, request.metadata, shape);
  shape.coverArtUri = request.metadata.artworkAction ==
                              MetadataFieldAction::Set
                          ? std::optional<std::string>(
                                request.metadata.artworkUri)
                          : std::nullopt;
  (void)sources;
  return {};
}

// AudioConvert (mandate §20/§32): audio-only pipeline.
Error shapeForAudioConvert(const job::AudioConvert &request,
                           const MediaInfo &source, Shape &shape) {
  shape.source = request.source;
  shape.outputContainer =
      request.output.container.value_or(source.container);
  shape.wantsVideo = false;
  shape.wantsAudio = true;
  shape.gainDb = request.adjustments.gainDb;
  shape.fadeInMs = request.adjustments.fadeInMs;
  shape.fadeOutMs = request.adjustments.fadeOutMs;
  shape.normalize = request.adjustments.normalize;
  shape.removeSilence = request.adjustments.removeSilence;
  shape.audioSpeed = request.adjustments.playbackSpeed;
  fillAudioSettingsShape(request.audio, shape);
  fillCompressionShape(request.compression, request.metadata, shape);
  shape.coverArtUri = request.metadata.artworkAction == MetadataFieldAction::Set
                          ? std::optional<std::string>(
                                request.metadata.artworkUri)
                          : std::nullopt;
  return {};
}

// Crop (mandate §27 spatial): transcode with crop; no format/codec change
// by default (source container; source codec when encodable, else H.264).
Error shapeForCrop(const job::Crop &request, const MediaInfo &source,
                   const FeatureRegistry &features, Shape &shape) {
  shape.source = request.source;
  shape.outputContainer =
      request.output.container.value_or(source.container);
  shape.hasCrop = true;
  shape.crop = request.crop;
  // Full-frame unchanged crop = pure remux (no pixel work at all).
  const VideoStream *video =
      source.hasVideo() ? &source.video.front() : nullptr;
  const bool fullFrame =
      video != nullptr && request.crop.x == 0 && request.crop.y == 0 &&
      request.crop.width >= video->width &&
      request.crop.height >= video->height;
  if (!fullFrame) {
    shape.crop = request.crop;
  } else {
    shape.hasCrop = false;
  }
  if (!source.hasVideo()) {
    return Error::make(ErrorCode::InvalidArgument,
                       "crop needs a video source.");
  }
  // Validate the rectangle against the source display dims (post-rotation
  // display space handled at execution; the planner sees the stored dims).
  if (request.crop.x < 0 || request.crop.y < 0 ||
      request.crop.x + request.crop.width > video->width ||
      request.crop.y + request.crop.height > video->height) {
    return Error::make(ErrorCode::InvalidCrop,
                       "crop rectangle does not fit inside the source "
                       "frame.");
  }
  // Source codec when encodable into the target container, else H.264.
  if (video != nullptr && features.knowsVideoCodec(video->codec) &&
      features.videoSupport(video->codec).encode &&
      features.containerAccepts(shape.outputContainer, video->codec)) {
    shape.videoCodec = video->codec;
  } else {
    shape.videoCodec = VideoCodec::H264;
  }
  shape.wantsAudio = true;  // audio/subtitles preserved (stream copy)
  shape.keepSubtitles = source.hasSubtitles();
  return {};
}

// Trim (mandate §27.1): the retained window drives the existing trim logic.
Error shapeForTrim(const job::Trim &request, const MediaInfo &source,
                   Shape &shape) {
  shape.source = request.source;
  shape.outputContainer =
      request.output.container.value_or(source.container);
  if (!request.range.valid()) {
    return Error::make(ErrorCode::InvalidTimeRange,
                       "Trim window is empty: end must come after start.");
  }
  shape.trimRanges.push_back(request.range);
  // Trim preserves the source representation: the codec resolution below
  // keeps the source codec (nullopt = preserve).
  return {};
}

// ExtractAudio (mandate §23/§28): audio-only extraction with track
// selection and an optional range.
Error shapeForExtractAudio(const job::ExtractAudio &request,
                           const MediaInfo &source, Shape &shape) {
  shape.source = request.source;
  shape.outputContainer =
      request.output.container.value_or(source.container);
  shape.wantsVideo = false;
  shape.wantsAudio = true;
  const std::int32_t trackCount = std::int32_t(source.audio.size());
  for (std::int32_t index : request.trackIndices) {
    if (index < 0 || index >= trackCount) {
      return Error::make(
          ErrorCode::InvalidTrackIdentity,
          "audio track index " + std::to_string(index) +
              " is outside this source (0.." +
              std::to_string(trackCount - 1) + ").");
    }
  }
  if (request.trackIndices.empty()) {
    shape.audioTrackIndex = -1; // default track
  } else if (request.trackIndices.size() == 1) {
    shape.audioTrackIndex = request.trackIndices.front();
  } else if (request.trackIndices.size() ==
             static_cast<std::size_t>(trackCount)) {
    // Selecting every track = the all-lanes shape.
    std::vector<std::int32_t> sorted = request.trackIndices;
    std::sort(sorted.begin(), sorted.end());
    bool every = true;
    for (std::int32_t i = 0; i < trackCount; ++i) {
      if (sorted[std::size_t(i)] != i) {
        every = false;
        break;
      }
    }
    if (!every) {
      return Error::make(ErrorCode::InvalidArgument,
                         "selecting a subset of audio tracks is not yet "
                         "implemented in this build.");
    }
    shape.allSourceAudioTracks = true;
    shape.audioTrackIndex = -1;
  } else {
    return Error::make(ErrorCode::InvalidArgument,
                       "selecting a subset of audio tracks is not yet "
                       "implemented in this build.");
  }
  if (request.range.has_value()) {
    if (!request.range->valid()) {
      return Error::make(ErrorCode::InvalidTimeRange,
                         "Extraction range is empty: end must come after "
                         "start.");
    }
    shape.trimRanges.push_back(*request.range);
  }
  fillAudioSettingsShape(request.audio, shape);
  fillCompressionShape(request.compression, request.metadata, shape);
  shape.coverArtUri = request.metadata.artworkAction == MetadataFieldAction::Set
                          ? std::optional<std::string>(
                                request.metadata.artworkUri)
                          : std::nullopt;
  return {};
}

} // namespace

FailedConfigKey failedConfigKeyFor(BackendId backend, const MediaInfo &source,
                                   const JobRequest &request,
                                   VideoCodec outVideoCodec) {
  FailedConfigKey key;
  key.backend = backend;
  key.codec = outVideoCodec;
  if (!source.video.empty()) {
    // Output geometry through the executor's transform order
    // (crop -> rotation -> scale); mirrors what validateOutput expects.
    std::int32_t w = source.video.front().width;
    std::int32_t h = source.video.front().height;
    std::optional<CropIntent> crop;
    std::int32_t rotation = 0;
    std::optional<std::int32_t> resolutionHeight;
    if (request.operation == Operation::VideoCrop) {
      if (const auto *payload = std::get_if<job::Crop>(&request.payload)) {
        crop = payload->crop;
      }
    } else if (const auto *payload =
                   std::get_if<job::VideoConvert>(&request.payload)) {
      rotation = payload->adjustments.rotationDegrees;
      resolutionHeight = payload->video.resolutionHeight.has_value()
                              ? payload->video.resolutionHeight
                              : payload->video.customHeight;
    }
    if (crop.has_value() && crop->valid()) {
      w = std::clamp(crop->width, 2, std::max(0, w - crop->x));
      h = std::clamp(crop->height, 2, std::max(0, h - crop->y));
    }
    if (rotation == 90 || rotation == 270) {
      std::swap(w, h);
    }
    if (resolutionHeight.has_value() && h > 0) {
      const std::int32_t targetH = *resolutionHeight;
      w = std::max<std::int32_t>(
          2, std::int32_t(std::lround(double(w) * double(targetH) / double(h))) & ~1);
      h = targetH;
    }
    key.width = w;
    key.height = h;
  }
  return key;
}

Planner::Planner(const FeatureRegistry &features,
                 const CapabilityRegistry &caps, const ITelemetryQuery *telemetry)
    : features_(features), caps_(caps), telemetry_(telemetry) {}

Result<PlanResult> Planner::plan(const JobRequest &request,
                                 const std::vector<MediaInfo> &sources) {
  const auto startTime = std::chrono::steady_clock::now();
  PlanResult result;

  if (sources.empty()) {
    return Result<PlanResult>(Error::make(
        ErrorCode::InvalidArgument,
        "planning needs at least one inspected source"));
  }
  const MediaInfo &source = sources.front();

  // ----- Normalize the typed payload into the conversion shape ----------
  Shape shape;
  bool deferred = false;
  switch (request.payload.index()) {
    case 0: { // VideoConvert
      if (request.operation != Operation::VideoConvert) {
        return Result<PlanResult>(Error::make(
            ErrorCode::InvalidArgument,
            "request operation does not match its payload"));
      }
      const Error error = shapeForVideoConvert(
          std::get<job::VideoConvert>(request.payload), sources, source,
          shape);
      if (!error.ok()) {
        result.feasible = false;
        result.message = error.message;
        result.errorCode = error.code;
        return Result<PlanResult>(std::move(result));
      }
      break;
    }
    case 1: { // AudioConvert
      const Error error = shapeForAudioConvert(
          std::get<job::AudioConvert>(request.payload), source, shape);
      if (!error.ok()) {
        result.feasible = false;
        result.message = error.message;
        result.errorCode = error.code;
        return Result<PlanResult>(std::move(result));
      }
      break;
    }
    case 2: { // Crop
      const Error error =
          shapeForCrop(std::get<job::Crop>(request.payload), source,
                       features_, shape);
      if (!error.ok()) {
        result.feasible = false;
        result.message = error.message;
        result.errorCode = error.code;
        return Result<PlanResult>(std::move(result));
      }
      break;
    }
    case 3: { // Trim
      const Error error = shapeForTrim(std::get<job::Trim>(request.payload),
                                       source, shape);
      if (!error.ok()) {
        result.feasible = false;
        result.message = error.message;
        result.errorCode = error.code;
        return Result<PlanResult>(std::move(result));
      }
      break;
    }
    case 4: { // Cut: retained = complement of removed.
      const auto &cut = std::get<job::Cut>(request.payload);
      shape.source = cut.source;
      shape.outputContainer = cut.output.container.value_or(source.container);
      if (cut.removed.empty()) {
        // Removing nothing = the whole timeline: a pure remux.
        break;
      }
      // Validate the removal intervals; execution of multi-range cuts
      // lands with the timeline executor phase.
      for (const TimeRange &range : cut.removed) {
        if (!range.valid()) {
          result.feasible = false;
          result.message = "Removed interval is empty: end must come after "
                           "start.";
          result.errorCode = ErrorCode::InvalidTimeRange;
          return Result<PlanResult>(std::move(result));
        }
      }
      deferred = true;
      break;
    }
    case 5:   // Split
    case 6: { // Merge
      // Planning-level validation only in this phase; execution of the
      // multi-output (Split) and N-source (Merge) shapes lands with the
      // timeline executor phase.
      if (request.payload.index() == 5) {
        const auto &split = std::get<job::Split>(request.payload);
        shape.source = split.source;
        shape.outputContainer =
            split.output.container.value_or(source.container);
        // Resolvability check (spec §2): unresolvable boundary strategies
        // are infeasible with a specific message, before the deferral.
        if (split.rules.mode == SplitMode::MaxSize &&
            source.overallBitRate() <= 0.0 && source.sizeBytes <= 0) {
          result.feasible = false;
          result.message =
              "Cannot estimate part sizes: the source bitrate is unknown. "
              "Choose a different split strategy.";
          return Result<PlanResult>(std::move(result));
        }
      } else {
        const auto &merge = std::get<job::Merge>(request.payload);
        shape.source =
            merge.sources.empty() ? MediaSource{} : merge.sources.front();
        shape.outputContainer =
            merge.output.container.value_or(
                sources.empty() ? Container::Mp4 : sources.front().container);
        if (merge.sources.size() < 2) {
          result.feasible = false;
          result.message = "Merge needs at least two sources.";
          result.errorCode = ErrorCode::InvalidArgument;
          return Result<PlanResult>(std::move(result));
        }
        if (merge.rules.layout != MergeLayout::Sequential &&
            merge.sources.size() != 2) {
          result.feasible = false;
          result.message = "Spatial merge requires exactly two sources.";
          result.errorCode = ErrorCode::InvalidArgument;
          return Result<PlanResult>(std::move(result));
        }
        for (const MediaInfo &info : sources) {
          if (!info.hasVideo() && merge.rules.layout != MergeLayout::Sequential) {
            result.feasible = false;
            result.message = "Spatial merge requires decodable video in "
                             "every source.";
            result.errorCode = ErrorCode::InvalidArgument;
            return Result<PlanResult>(std::move(result));
          }
        }
      }
      deferred = true;
      break;
    }
    case 7: { // ExtractAudio
      const Error error =
          shapeForExtractAudio(std::get<job::ExtractAudio>(request.payload),
                               source, shape);
      if (!error.ok()) {
        result.feasible = false;
        result.message = error.message;
        result.errorCode = error.code;
        return Result<PlanResult>(std::move(result));
      }
      break;
    }
    case 8: { // ExtractFrames: gate on a decoder for the source video codec.
      const auto &frames = std::get<job::ExtractFrames>(request.payload);
      shape.source = frames.source;
      if (!source.hasVideo()) {
        result.feasible = false;
        result.message = "Frame extraction needs a video source.";
        result.errorCode = ErrorCode::InvalidArgument;
        return Result<PlanResult>(std::move(result));
      }
      const VideoCodec codec = source.video.front().codec;
      if (caps_.backendsFor(codec, CodecOp::Decode).empty()) {
        result.feasible = false;
        result.message = "No backend can decode this video for frame "
                         "extraction.";
        result.errorCode = ErrorCode::DecoderUnavailable;
        return Result<PlanResult>(std::move(result));
      }
      deferred = true;
      break;
    }
    case 9: { // ExtractSubtitles: standalone sidecar extraction.
      const auto &extract = std::get<job::ExtractSubtitles>(request.payload);
      shape.source = extract.source;
      shape.subtitleExtract = true;
      shape.wantsVideo = false;
      shape.wantsAudio = false;
      if (extract.trackIndices.size() > 1) {
        result.feasible = false;
        result.message =
            "Multi-track subtitle extraction (multiple sidecars) is not "
            "yet implemented in this build; select one track.";
        result.errorCode = ErrorCode::UnsupportedOperation;
        return Result<PlanResult>(std::move(result));
      }
      if (!extract.trackIndices.empty() &&
          (extract.trackIndices.front() < 0 ||
           extract.trackIndices.front() >=
               std::int32_t(source.subtitles.size()))) {
        result.feasible = false;
        result.message = "Subtitle track index is outside this source.";
        result.errorCode = ErrorCode::InvalidTrackIdentity;
        return Result<PlanResult>(std::move(result));
      }
      if (!source.hasSubtitles()) {
        result.feasible = false;
        result.message = "This source has no subtitle track to extract.";
        return Result<PlanResult>(std::move(result));
      }
      const std::size_t track = extract.trackIndices.empty()
                                    ? 0
                                    : std::size_t(extract.trackIndices.front());
      const SubtitleFormat format = source.subtitles[track].format;
      const bool textExtractable =
          format == SubtitleFormat::Srt || format == SubtitleFormat::WebVtt ||
          format == SubtitleFormat::Ass || format == SubtitleFormat::Ssa ||
          format == SubtitleFormat::MovText || format == SubtitleFormat::Ttml;
      if (!textExtractable) {
        result.feasible = false;
        result.message =
            std::string("The subtitle track (") + subtitleFormatName(format) +
            ") cannot be extracted: bitmap and CEA formats need OCR, which "
            "is outside V1.";
        return Result<PlanResult>(std::move(result));
      }
      break;
    }
    default:
      return Result<PlanResult>(Error::make(ErrorCode::InvalidArgument,
                                            "unknown request payload"));
  }

  // ----- Deferred operations: honest refusal ------------------------------
  // Split, Merge, multi-range Cut and ExtractFrames are not executable in
  // this build; the planner says so instead of planning a route that would
  // fail mid-execution.
  if (deferred) {
    result.feasible = false;
    result.errorCode = ErrorCode::UnsupportedOperation;
    result.message = "operation not yet implemented in this build";
    return Result<PlanResult>(std::move(result));
  }

  // ----- Resolve requested output shape -------------------------------------
  const Container outContainer = shape.outputContainer;
  if (!features_.knowsContainer(outContainer)) {
    result.feasible = false;
    result.message = "Output container is not a supported Velmux format.";
    return Result<PlanResult>(std::move(result));
  }

  const bool wantsVideo = shape.wantsVideo && source.hasVideo();
  const bool wantsAudio = shape.wantsAudio && source.hasAudio();

  VideoCodec outVideoCodec = VideoCodec::H264;
  if (wantsVideo) {
    outVideoCodec = shape.videoCodec.value_or(source.video.front().codec);
    if (!features_.knowsVideoCodec(outVideoCodec)) {
      result.feasible = false;
      result.message = "Video codec is not in the Velmux catalog.";
      return Result<PlanResult>(std::move(result));
    }
    if (!features_.containerAccepts(outContainer, outVideoCodec)) {
      result.feasible = false;
      result.message =
          std::string("Container ") + containerName(outContainer) +
          " does not accept video codec " + codecName(outVideoCodec) + ".";
      return Result<PlanResult>(std::move(result));
    }
  }

  AudioCodec outAudioCodec = AudioCodec::Aac;
  if (wantsAudio) {
    outAudioCodec = shape.audioCodec.value_or(source.audio.front().codec);
    if (!features_.knowsAudioCodec(outAudioCodec)) {
      result.feasible = false;
      result.message = "Audio codec is not in the Velmux catalog.";
      return Result<PlanResult>(std::move(result));
    }
    if (!features_.containerAccepts(outContainer, outAudioCodec)) {
      result.feasible = false;
      result.message =
          std::string("Container ") + containerName(outContainer) +
          " does not accept audio codec " + codecName(outAudioCodec) + ".";
      return Result<PlanResult>(std::move(result));
    }
  }

  // Trim window sanity (spec §15/§16). Executors clamp against the real
  // duration; an inverted or empty window is a request error.
  for (const TimeRange &range : shape.trimRanges) {
    if (!range.valid()) {
      result.feasible = false;
      result.message = "Trim window is empty: end must come after start.";
      result.errorCode = ErrorCode::InvalidTimeRange;
      return Result<PlanResult>(std::move(result));
    }
    if (range.startUs < 0) {
      result.feasible = false;
      result.message = "Trim start must not be negative.";
      result.errorCode = ErrorCode::InvalidTimeRange;
      return Result<PlanResult>(std::move(result));
    }
  }

  // Preserved subtitle tracks must survive the container (never silently
  // dropped: removal must be explicit).
  if (shape.keepSubtitles && source.hasSubtitles()) {
    for (const auto &sub : source.subtitles) {
      if (!features_.containerAccepts(outContainer, sub.format)) {
        // Preserving this track in this container is impossible; surface it
        // rather than silently dropping.
        result.feasible = false;
        result.message =
            std::string("Subtitle track (") + subtitleFormatName(sub.format) +
            ") cannot be preserved in " + containerName(outContainer) +
            ". Choose another container, or remove/extract the track.";
        return Result<PlanResult>(std::move(result));
      }
    }
  }

  // ----- Candidate generation -----------------------------------------------
  // v1 execution model: a single backend serves all stages of a candidate.
  // Candidates are generated stage-wise so the structure stays correct when
  // hybrid composition lands (Milestone 4).
  struct Candidate {
    Pipeline pipeline;
    double throughput = 0.0;
    double setupMs = 0.0;
    double transferMs = 0.0;
    bool usesHardware = false;
    double preferenceScore = 0.0;
  };
  std::vector<Candidate> candidates;
  // Set when a viable candidate was dropped solely because its exact
  // configuration has a live failure record; the infeasibility message
  // must then explain that (it is not a capability gap).
  bool blockedByFailureRecord = false;

  // Extraction candidates: demux + subtitle processing on a backend that
  // extracts. The artifact is a text sidecar; the extension comes from the
  // track's format (native text copies as-is, container-internal text
  // decodes to SRT).
  if (shape.subtitleExtract) {
    const auto &extractPayload =
        std::get<job::ExtractSubtitles>(request.payload);
    const std::size_t trackIndex =
        extractPayload.trackIndices.empty()
            ? 0
            : std::size_t(extractPayload.trackIndices.front());
    // Per-track target (mandate §30): empty/Original keeps the native
    // sidecar extension (or the SRT decode fallback for container-internal
    // text); an explicit Srt/Vtt/Ass target picks its own extension — the
    // executor writes exactly that representation.
    const SubtitleTextTarget target =
        extractPayload.targets.empty() ? SubtitleTextTarget::Original
                                       : extractPayload.targets.front();
    const SubtitleFormat first = source.subtitles[trackIndex].format;
    for (BackendId backend : caps_.registeredBackends()) {
      const auto *caps = caps_.capabilitiesOf(backend);
      if (caps == nullptr || !caps->subtitleExtract) {
        continue;
      }
      if (std::find(caps->readableContainers.begin(),
                    caps->readableContainers.end(),
                    source.container) ==
          caps->readableContainers.end()) {
        continue;
      }
      const char *ext = "srt";
      switch (target) {
        case SubtitleTextTarget::Original: {
          switch (first) {
            case SubtitleFormat::WebVtt:
              ext = "vtt";
              break;
            case SubtitleFormat::Ass:
            case SubtitleFormat::Ssa:
              ext = "ass";
              break;
            default:
              break; // Srt/MovText/Ttml -> SRT (copy or decode)
          }
          break;
        }
        case SubtitleTextTarget::Srt:
          ext = "srt";
          break;
        case SubtitleTextTarget::Vtt:
          ext = "vtt";
          break;
        case SubtitleTextTarget::Ass:
          ext = "ass";
          break;
      }
      Candidate candidate;
      Pipeline &pipeline = candidate.pipeline;
      pipeline.stages.push_back({StageId::Demux, backend});
      pipeline.stages.push_back({StageId::SubtitleProcessing, backend});
      pipeline.container = source.container; // artifact is the sidecar
      pipeline.sidecarExtension = ext;
      pipeline.subtitleStreamCopy = false;
      const BackendPrior &prior = priorFor(backend);
      candidate.throughput = prior.softwareFps * 20.0; // demux-level work
      candidate.setupMs = prior.setupMs;
      candidate.transferMs = 0.0;
      candidate.usesHardware = false;
      candidate.preferenceScore = 0.0;
      pipeline.summary = std::string("Extract subtitles (") +
                         (pipeline.sidecarExtension == "ass" ? "ASS" : "SRT") +
                         ") to a sidecar file";
      if (source.subtitles.size() > 1) {
        pipeline.summary += " (track " + std::to_string(trackIndex + 1) +
                            " of " + std::to_string(source.subtitles.size()) +
                            ")";
      }
      candidates.push_back(std::move(candidate));
    }
    if (candidates.empty()) {
      result.feasible = false;
      result.message =
          "No registered backend can extract subtitles on this device.";
      return Result<PlanResult>(std::move(result));
    }
  }

  const bool canStreamCopyVideo =
      wantsVideo && source.video.front().codec == outVideoCodec &&
      features_.videoSupport(outVideoCodec).streamCopy &&
      // No transformation requested: stream copy is only valid when the
      // output streams are identical to the input.
      !shape.resolutionHeight.has_value() && !shape.frameRate.has_value() &&
      // Geometry transforms rewrite pixels (crop/rotate/flip): never a
      // stream copy, exactly like burn-in.
      !shape.hasCrop && shape.rotationDegrees == 0 && !shape.flipHorizontal &&
      !shape.flipVertical &&
      // Timing transforms rewrite the presentation timeline.
      !shape.playbackSpeed.has_value() && !shape.reverse &&
      // HDR tone mapping rewrites pixels.
      !shape.hdrToSdr &&
      // Burn-in rewrites the video pixels: it can never stream-copy.
      !shape.burnIn;
  const bool canStreamCopyAudio =
      wantsAudio &&
      // Replacement audio is a different stream: never a copy of the
      // source's audio.
      !shape.replaceAudioUri.has_value() &&
      // With every source lane kept, each must be copyable into the target
      // container as-is; a mixed copy/encode result is not "stream copy".
      (!shape.allSourceAudioTracks ||
       std::all_of(source.audio.begin(), source.audio.end(), [&](
                       const AudioStream &track) {
         return track.codec == outAudioCodec &&
                features_.audioSupport(track.codec).streamCopy &&
                features_.containerAccepts(outContainer, track.codec);
       })) &&
      (shape.allSourceAudioTracks ||
       shape.audioTrackIndex < 0 ||
       (source.audio[std::size_t(shape.audioTrackIndex)].codec ==
            outAudioCodec &&
        features_.audioSupport(outAudioCodec).streamCopy &&
        features_.containerAccepts(
            outContainer,
            source.audio[std::size_t(shape.audioTrackIndex)].codec))) &&
      !shape.budget.audio.bitRate.has_value() &&
      !shape.budget.audio.sampleRate.has_value() &&
      !shape.budget.audio.channels.has_value() &&
      // DSP rewrites audio samples: gain/fades/normalization/speed/silence
      // removal all require re-encoding.
      !shape.gainDb.has_value() && !shape.fadeInMs.has_value() &&
      !shape.fadeOutMs.has_value() && !shape.normalize &&
      !shape.audioSpeed.has_value() && !shape.removeSilence;
  // Goal sanity: remuxing cannot shrink anything. Stream copy satisfies a
  // size objective only when the source already fits under the target.
  const bool sizeGoalAllowsCopy =
      !shape.compression.enabled ||
      shape.compression.preset == CompressionPreset::HighQuality ||
      shape.compression.preset == CompressionPreset::Balanced ||
      shape.compression.preset == CompressionPreset::Custom ||
      (shape.compression.preset == CompressionPreset::TargetSize &&
       shape.compression.targetSizeBytes > 0 && source.sizeBytes > 0 &&
       source.sizeBytes <= shape.compression.targetSizeBytes);
  const bool streamCopyIsValid =
      sizeGoalAllowsCopy && canStreamCopyVideo && canStreamCopyAudio;
  const bool sameContainer = source.container == outContainer;

  const auto addCandidate = [&](BackendId backend, bool streamCopy) {
    // Capability checks: the backend must actually support every stage it
    // is assigned for the involved codecs.
    const auto *caps = caps_.capabilitiesOf(backend);
    if (caps == nullptr) {
      return;
    }
    if (std::find(caps->writableContainers.begin(),
                  caps->writableContainers.end(),
                  outContainer) == caps->writableContainers.end()) {
      return;
    }
    if (std::find(caps->readableContainers.begin(),
                  caps->readableContainers.end(),
                  source.container) == caps->readableContainers.end()) {
      return;
    }
    if (wantsVideo) {
      if (streamCopy) {
        const auto backends = caps_.backendsFor(outVideoCodec, CodecOp::StreamCopy);
        if (std::find(backends.begin(), backends.end(), backend) == backends.end()) {
          return;
        }
      } else {
        const auto decodeBackends =
            caps_.backendsFor(source.video.front().codec, CodecOp::Decode);
        if (std::find(decodeBackends.begin(), decodeBackends.end(), backend) ==
            decodeBackends.end()) {
          return;
        }
        const auto encodeBackends =
            caps_.backendsFor(outVideoCodec, CodecOp::Encode);
        if (std::find(encodeBackends.begin(), encodeBackends.end(), backend) ==
            encodeBackends.end()) {
          return;
        }
      }
    }
    if (wantsAudio && !streamCopy) {
      const auto encodeBackends = caps_.backendsFor(outAudioCodec, CodecOp::Encode);
      if (std::find(encodeBackends.begin(), encodeBackends.end(), backend) ==
          encodeBackends.end()) {
        return;
      }
    }
    // Runtime failure feedback (spec §15): a configuration that already
    // failed execution on this backend is avoided while the record is
    // live (the registry's cooldown re-admits it later). This is narrow —
    // a different codec/geometry on the same backend still plans.
    if (caps_.hasFailed(
            failedConfigKeyFor(backend, source, request, outVideoCodec))) {
      blockedByFailureRecord = true;
      return;
    }
    // Processing capabilities: geometry / rate / audio-format changes and
    // subtitle work only route to backends that actually implement them
    // (reported via BackendCapabilities; executors double-check).
    const bool wantsTrim = !shape.trimRanges.empty();
    if (wantsTrim && !caps->timeTrim) {
      // An executor that cannot trim must never receive a trimmed request:
      // it would silently produce the full-length output.
      return;
    }
    // Geometry transforms (spec §9) run through the video-processing
    // filters: only backends reporting videoScaling execute them (the
    // capability's documented meaning covers scale/rotate/crop).
    if (wantsVideo &&
        (shape.hasCrop || shape.rotationDegrees != 0 || shape.flipHorizontal ||
         shape.flipVertical) &&
        !caps->videoScaling) {
      return;
    }
    // Timing transforms (playback speed / reverse) rewrite presentation
    // timing: they need the temporal-processing surface (videoScaling
    // covers it in this build) and, with audio present, audioDsp (the
    // audio timeline must follow atempo/areverse).
    if (wantsVideo &&
        (shape.playbackSpeed.has_value() || shape.reverse) &&
        !caps->videoScaling) {
      return;
    }
    if (wantsAudio &&
        (shape.playbackSpeed.has_value() || shape.reverse) && !caps->audioDsp) {
      return;
    }
    // Audio DSP chain (spec §10): only backends reporting audioDsp.
    if (wantsAudio &&
        (shape.gainDb.has_value() || shape.fadeInMs.has_value() ||
         shape.fadeOutMs.has_value() || shape.normalize ||
         shape.audioSpeed.has_value() || shape.removeSilence) &&
        !caps->audioDsp) {
      return;
    }
    // HDR -> SDR conversion (spec §9) needs a tone-mapping backend; only
    // meaningful when the source is actually HDR.
    if (wantsVideo && !streamCopy && shape.hdrToSdr && source.hasVideo() &&
        source.video.front().hdr && !caps->hdrToneMapping) {
      return;
    }
    // Any transcode of HDR content on a tone-map-less backend would encode
    // PQ/HLG pixels through the default 8-bit SDR pipeline and silently
    // produce washed-out output. Stream copy keeps the bitstream intact
    // and is never gated here.
    if (wantsVideo && !streamCopy && source.hasVideo() &&
        source.video.front().hdr && !caps->hdrToneMapping) {
      return;
    }
    // Multi-track audio and explicit track selection (spec §9).
    if (wantsAudio &&
        ((shape.allSourceAudioTracks && source.audio.size() > 1) ||
         shape.audioTrackIndex >= 0 || shape.hasExternalAudio) &&
        !caps->multiTrackAudio) {
      return;
    }
    // Audio replacement and cover art (spec §9/§10).
    if (shape.replaceAudioUri.has_value() && !caps->audioReplacement) {
      return;
    }
    if (shape.coverArtUri.has_value() && !caps->coverArt) {
      return;
    }
    if (!streamCopy) {
      const bool geometryChange =
          wantsVideo && shape.resolutionHeight.has_value() &&
          *shape.resolutionHeight !=
              (source.hasVideo() ? source.video.front().height : 0);
      const bool rateChange =
          wantsVideo && shape.frameRate.has_value() && source.hasVideo() &&
          source.video.front().frameRate > 0.0 &&
          std::abs(*shape.frameRate -
                   source.video.front().frameRate) > 0.01;
      if ((geometryChange || rateChange) && !caps->videoScaling) {
        return;
      }
      const bool sampleRateChange =
          wantsAudio && shape.budget.audio.sampleRate.has_value() &&
          source.hasAudio() && source.audio.front().sampleRate > 0 &&
          *shape.budget.audio.sampleRate != source.audio.front().sampleRate;
      const bool channelChange =
          wantsAudio && shape.budget.audio.channels.has_value() &&
          source.hasAudio() && source.audio.front().channels > 0 &&
          *shape.budget.audio.channels != source.audio.front().channels;
      if ((sampleRateChange || channelChange) && !caps->audioResampling) {
        return;
      }
    }
    // Subtitle work: passthrough needs subtitlePassthrough, burn-in needs
    // subtitleBurnIn. Sources with subtitles under a Preserve policy must
    // not lose them by routing to a backend that silently drops text
    // tracks.
    const bool burnIn = shape.burnIn;
    const bool preserveWithSubtitles = shape.keepSubtitles &&
                                       source.hasSubtitles();
    if (burnIn && !caps->subtitleBurnIn) {
      return;
    }
    if (preserveWithSubtitles && !caps->subtitlePassthrough) {
      return;
    }

    Candidate candidate;
    Pipeline &pipeline = candidate.pipeline;
    const auto assign = [&](StageId stage) {
      pipeline.stages.push_back({stage, backend});
    };
    assign(StageId::Demux);
    if (wantsVideo) {
      assign(StageId::Decode);
      assign(StageId::VideoProcessing);
    }
    if (wantsAudio) {
      assign(StageId::AudioProcessing);
    }
    if ((source.hasSubtitles() || shape.burnIn) && shape.burnIn) {
      assign(StageId::SubtitleProcessing);
    }
    if (!streamCopy) {
      assign(StageId::Encode);
    }
    assign(StageId::Mux);
    assign(StageId::Bitstream);
    pipeline.videoCodec = outVideoCodec;
    pipeline.audioCodec = outAudioCodec;
    pipeline.container = outContainer;
    pipeline.sourceFrameRate =
        source.hasVideo() && source.video.front().frameRate > 0.0
            ? source.video.front().frameRate
            : 0.0;
    pipeline.videoStreamCopy = streamCopy && canStreamCopyVideo;
    pipeline.audioStreamCopy = streamCopy && canStreamCopyAudio;
    pipeline.subtitleStreamCopy = shape.keepSubtitles && !shape.burnIn;

    // Cost inputs: prefer telemetry observations over static priors.
    double throughput = 0.0, setupMs = 0.0, transferMs = 0.0;
    if (telemetry_ != nullptr) {
      if (auto sample = telemetry_->throughputFor(
              backend, wantsVideo ? std::optional<VideoCodec>(outVideoCodec)
                                  : std::nullopt,
              wantsAudio ? std::optional<AudioCodec>(outAudioCodec)
                         : std::nullopt)) {
        throughput = sample->framesPerSecond;
        setupMs = sample->setupMs;
        transferMs = sample->transferMsPerFrame;
      }
    }
    const BackendPrior &prior = priorFor(backend);
    if (throughput <= 0.0) {
      throughput = prior.softwareFps;
    }
    if (setupMs <= 0.0) {
      setupMs = prior.setupMs;
    }
    if (transferMs <= 0.0) {
      transferMs = prior.transferMsPerFrame;
    }
    const bool hw = caps_.videoCapability(backend, outVideoCodec)
                        .value_or(VideoCapability{})
                        .hardware;
    candidate.usesHardware = hw;
    if (hw && throughput < prior.hardwareFps) {
      throughput = prior.hardwareFps;
    }
    candidate.throughput = streamCopy ? throughput * 20.0 : throughput;
    candidate.setupMs = setupMs;
    candidate.transferMs = transferMs;
    candidate.preferenceScore = 0.0; // automatic routing (mode retired)

    std::string summary;
    if (pipeline.videoStreamCopy && pipeline.audioStreamCopy &&
        sameContainer) {
      summary = "Fast remux (stream copy, no re-encoding)";
      if (wantsTrim) {
        summary += " with keyframe-snapped trim";
      }
    } else if (pipeline.videoStreamCopy) {
      summary = std::string("Remux to ") + containerName(outContainer) +
                " (video stream copy)";
      if (wantsTrim) {
        summary += " with keyframe-snapped trim";
      }
    } else {
      summary = std::string("Transcode ") + codecName(outVideoCodec) + " via " +
                backendName(backend) + (hw ? " (hardware)" : " (software)") +
                " -> " + containerName(outContainer);
      if (pipeline.audioStreamCopy) {
        summary += " (audio copy)";
      }
      if (shape.hasCrop) {
        summary += " (crop)";
      }
      if (shape.rotationDegrees != 0) {
        summary += " (rotate " + std::to_string(shape.rotationDegrees) + ")";
      }
      if (shape.playbackSpeed.has_value()) {
        summary += " (speed " +
                   std::to_string(int(*shape.playbackSpeed * 100.0) / 100.0) +
                   "x)";
      }
      if (shape.reverse) {
        summary += " (reverse)";
      }
      if (shape.hdrToSdr) {
        summary += " (HDR -> SDR)";
      }
    }
    pipeline.summary = std::move(summary);
    candidates.push_back(std::move(candidate));
  };

  // Stream-copy first (never re-encode unnecessarily), then transcodes.
  // Extraction jobs already have their candidates; generating transcode
  // candidates for a subtitle-only request would be meaningless.
  if (!shape.subtitleExtract) {
    if (streamCopyIsValid &&
        (sameContainer ||
         features_.containerAccepts(outContainer, outVideoCodec))) {
      for (BackendId backend : caps_.registeredBackends()) {
        addCandidate(backend, true);
      }
    }
    for (BackendId backend : caps_.registeredBackends()) {
      addCandidate(backend, false);
    }
  }

  // ----- Hybrid candidates (Milestone 4) -----------------------------------
  // Only when no single-backend transcode candidate exists: hybrid
  // execution exists for pipelines that would otherwise have NO valid
  // route (e.g. a source codec only backend A can decode, an output codec
  // only backend B can encode). Single-backend pipelines keep winning
  // whenever they exist — cross-backend CPU-frame copies cost real time.
  const bool hasTranscodeCandidate = std::any_of(
      candidates.begin(), candidates.end(), [](const Candidate &c) {
        return !c.pipeline.videoStreamCopy;
      });
  // Hybrid stage workers do not trim (v1): a trimmed request must not
  // compose a hybrid route that would silently ignore the window. The same
  // holds for geometry transforms and the audio DSP chain: the v1 stage
  // contracts carry plain Yuv420P frames and stream-copied audio only.
  const bool wantsAnyTrim = !shape.trimRanges.empty();
  const bool wantsTransform =
      shape.hasCrop || shape.rotationDegrees != 0 || shape.flipHorizontal ||
      shape.flipVertical || shape.playbackSpeed.has_value() || shape.reverse ||
      shape.hdrToSdr;
  const bool wantsAudioDsp =
      shape.gainDb.has_value() || shape.fadeInMs.has_value() ||
      shape.fadeOutMs.has_value() || shape.normalize ||
      shape.audioSpeed.has_value() || shape.removeSilence;
  // v1 stage contracts carry no multi-track / replacement / cover-art
  // audio handling: such requests stay on whole-pipeline backends.
  const bool wantsAdvancedAudio =
      (shape.allSourceAudioTracks && source.audio.size() > 1) ||
      shape.audioTrackIndex >= 0 || shape.replaceAudioUri.has_value() ||
      shape.coverArtUri.has_value() || shape.hasExternalAudio;
  // Geometry/rate changes mirror HybridExecutor::canExecute (v1 hybrid
  // carries source geometry and rate): planning a hybrid route the
  // executor must then refuse would surface as a mid-job execution error
  // instead of an honest infeasible plan.
  const bool wantsGeometryChange =
      shape.resolutionHeight.has_value() || shape.frameRate.has_value();
  // HDR sources never ride the hybrid interchange: the CPU-frame pipeline
  // re-encodes PQ/HLG pixels through the default 8-bit SDR path — the same
  // washed-out output the single-backend HDR gate refuses. Only a
  // tone-mapping stage-decode backend (none in the pinned v1 set) could
  // compose this honestly.
  const bool hdrSource =
      wantsVideo && source.hasVideo() && source.video.front().hdr;
  // registeredBackends() returns BY VALUE: materialize one vector —
  // calling it twice for begin()/end() would iterate two different
  // temporaries (a heap-buffer-overflow caught by ASan on the first full
  // sanitizer run of this code).
  const std::vector<BackendId> registeredBackends = caps_.registeredBackends();
  const bool anyStageDecoderToneMaps = std::any_of(
      registeredBackends.begin(), registeredBackends.end(),
      [this](BackendId id) {
        const auto *caps = caps_.capabilitiesOf(id);
        return caps != nullptr && caps->stageDecodeToCpuFrames &&
               caps->hdrToneMapping;
      });
  if (wantsVideo && !hasTranscodeCandidate && !wantsAnyTrim &&
      !wantsTransform && !wantsAudioDsp && !wantsAdvancedAudio &&
      !wantsGeometryChange && !(hdrSource && !anyStageDecoderToneMaps)) {
    const bool audioCopyable =
        !wantsAudio ||
        (source.hasAudio() &&
         source.audio.front().codec == outAudioCodec &&
         features_.audioSupport(source.audio.front().codec).streamCopy &&
         features_.containerAccepts(outContainer, source.audio.front().codec));
    for (BackendId decodeBackend : caps_.registeredBackends()) {
      const auto *decodeCaps = caps_.capabilitiesOf(decodeBackend);
      if (decodeCaps == nullptr || !decodeCaps->stageDecodeToCpuFrames) {
        continue;
      }
      if (std::find(decodeCaps->readableContainers.begin(),
                    decodeCaps->readableContainers.end(),
                    source.container) ==
          decodeCaps->readableContainers.end()) {
        continue;
      }
      const auto decoders = caps_.backendsFor(source.video.front().codec,
                                              CodecOp::Decode);
      if (std::find(decoders.begin(), decoders.end(), decodeBackend) ==
          decoders.end()) {
        continue;
      }
      // Same runtime-failure avoidance as single-backend candidates: a
      // decode side with a live failure record for this configuration
      // must not be composed into the only remaining route.
      if (caps_.hasFailed(failedConfigKeyFor(decodeBackend, source, request,
                                             outVideoCodec))) {
        blockedByFailureRecord = true;
        continue;
      }
      for (BackendId muxBackend : caps_.registeredBackends()) {
        const auto *muxCaps = caps_.capabilitiesOf(muxBackend);
        if (muxCaps == nullptr || !muxCaps->stageEncodeFromCpuFrames) {
          continue;
        }
        if (std::find(muxCaps->writableContainers.begin(),
                      muxCaps->writableContainers.end(),
                      outContainer) == muxCaps->writableContainers.end()) {
          continue;
        }
        // The mux side opens the same file for the audio copy.
        if (std::find(muxCaps->readableContainers.begin(),
                      muxCaps->readableContainers.end(),
                      source.container) ==
            muxCaps->readableContainers.end()) {
          continue;
        }
        const auto encoders =
            caps_.backendsFor(outVideoCodec, CodecOp::Encode);
        if (std::find(encoders.begin(), encoders.end(), muxBackend) ==
            encoders.end()) {
          continue;
        }
        if (caps_.hasFailed(failedConfigKeyFor(muxBackend, source, request,
                                               outVideoCodec))) {
          blockedByFailureRecord = true;
          continue;
        }
        if (!audioCopyable) {
          continue; // v1 hybrid carries only stream-copyable audio
        }
        if (!features_.containerAccepts(outContainer, outVideoCodec)) {
          continue;
        }

        Candidate candidate;
        Pipeline &pipeline = candidate.pipeline;
        pipeline.stages.push_back({StageId::Demux, decodeBackend});
        pipeline.stages.push_back({StageId::Decode, decodeBackend});
        pipeline.stages.push_back({StageId::VideoProcessing, decodeBackend});
        if (wantsAudio) {
          pipeline.stages.push_back({StageId::AudioProcessing, muxBackend});
        }
        pipeline.stages.push_back({StageId::Encode, muxBackend});
        pipeline.stages.push_back({StageId::Mux, muxBackend});
        pipeline.stages.push_back({StageId::Bitstream, muxBackend});
        pipeline.videoCodec = outVideoCodec;
        pipeline.audioCodec = outAudioCodec;
        pipeline.container = outContainer;
        pipeline.sourceFrameRate =
            source.hasVideo() && source.video.front().frameRate > 0.0
                ? source.video.front().frameRate
                : 0.0;
        pipeline.videoStreamCopy = false;
        pipeline.audioStreamCopy = wantsAudio;
        pipeline.subtitleStreamCopy = false;

        // Cost: the slower side bounds throughput; CPU-frame copies add
        // transfer on every frame.
        const BackendPrior &decPrior = priorFor(decodeBackend);
        const BackendPrior &muxPrior = priorFor(muxBackend);
        candidate.throughput = std::min(decPrior.softwareFps,
                                        muxPrior.softwareFps) *
                               0.7;
        candidate.setupMs = decPrior.setupMs + muxPrior.setupMs;
        candidate.transferMs = decPrior.transferMsPerFrame +
                               muxPrior.transferMsPerFrame;
        candidate.usesHardware =
            caps_.videoCapability(muxBackend, outVideoCodec)
                .value_or(VideoCapability{})
                .hardware;
        candidate.preferenceScore = 0.0;
        pipeline.summary =
            std::string("Hybrid: ") + backendName(decodeBackend) +
            " decode -> " + backendName(muxBackend) + " encode -> " +
            containerName(outContainer);
        candidates.push_back(std::move(candidate));
      }
    }
  }

  if (candidates.empty()) {
    result.feasible = false;
    if (blockedByFailureRecord) {
      result.message =
          "Every backend route for this exact configuration failed "
          "recently; the failure record cools down automatically. Try "
          "again later or change the output settings.";
    } else {
      result.message =
          wantsVideo
              ? "No registered backend can decode this source and encode the "
                "requested output on this device."
              : "No registered backend can produce the requested output.";
    }
    return Result<PlanResult>(std::move(result));
  }

  // ----- Cost model + ranking ----------------------------------------------
  const auto scoreCandidate = [&](const Candidate &c) {
    double cost = 0.0;
    const double durationSec =
        source.durationSec > 0.0 ? source.durationSec : 1.0;
    const double frames =
        durationSec * (source.hasVideo() && source.video.front().frameRate > 0.0
                           ? source.video.front().frameRate
                           : 30.0);
    // Time-bound estimate: slower pipelines cost more.
    cost += frames / std::max(c.throughput, 1.0) * weights_.throughput;
    cost += c.setupMs * weights_.setupLatency;
    cost += frames * c.transferMs * weights_.transferCost;
    if (c.usesHardware) {
      cost += weights_.powerPenalty;
    }
    if (!c.pipeline.videoStreamCopy) {
      cost += weights_.recoveryOverhead; // checkpoints cost more on transcode
    }
    cost -= c.preferenceScore * weights_.preferenceBonus;
    return std::max(cost, 0.0);
  };

  std::sort(candidates.begin(), candidates.end(),
            [&](const Candidate &a, const Candidate &b) {
              return scoreCandidate(a) < scoreCandidate(b);
            });

  // The rate policy + budget for size estimation.
  shape.budget.outputContainer = outContainer;
  shape.budget.audioEnabled = wantsAudio;
  shape.budget.videoEnabled = wantsVideo;
  shape.budget.videoCodec = outVideoCodec;
  shape.budget.resolutionHeight = shape.resolutionHeight;
  shape.budget.frameRate = shape.frameRate;
  shape.budget.playbackSpeed = shape.playbackSpeed;
  const ResolvedRatePolicy rate =
      resolveRatePolicy(source, shape.compression, shape.budget);

  for (auto &candidate : candidates) {
    PlannedPipeline planned;
    planned.pipeline = std::move(candidate.pipeline);
    planned.cost = scoreCandidate(candidate);
    planned.expectedThroughput = candidate.throughput;
    planned.setupLatencyMs = candidate.setupMs;
    planned.transferCostMs = candidate.transferMs;
    if (shape.subtitleExtract) {
      planned.estimatedOutputBytes = std::nullopt; // sidecar: unknown, honest
    } else {
      planned.estimatedOutputBytes =
          estimateOutputBytes(source, planned.pipeline, shape.budget, rate);
    }
    planned.rationale = planned.pipeline.summary;
    result.alternates.push_back(std::move(planned));
  }
  result.best = result.alternates.front();
  result.feasible = true;

  // Post-selection notes (advisories). They compose: closed captions,
  // budget warnings and future notes append rather than replace.
  const auto addNote = [&result](const std::string &note) {
    if (note.empty()) {
      return;
    }
    if (!result.message.empty()) {
      result.message += " ";
    }
    result.message += note;
  };

  // Closed-caption honesty (spec §11): CEA-608/708 captions ride the
  // video elementary stream. Transcoding rewrites it and drops them;
  // stream copy keeps them. Say so instead of losing them silently.
  if (source.hasVideo() && source.video.front().hasClosedCaptions) {
    if (result.best->pipeline.videoStreamCopy) {
      addNote(
          "Closed captions (CEA-608/708) detected; stream copy preserves "
          "them in the video bitstream.");
    } else {
      addNote(
          "Closed captions (CEA-608/708) detected; this transcode drops "
          "them because they ride the video bitstream. Use stream copy "
          "(same codec) to keep them.");
    }
  }

  // Target-size feasibility cross-check with real budget math.
  if (shape.compression.enabled &&
      shape.compression.preset == CompressionPreset::TargetSize &&
      shape.compression.targetSizeBytes > 0) {
    const BitrateBudget budget = computeBitrateBudget(
        source, shape.budget, shape.compression.targetSizeBytes);
    if (!budget.feasible) {
      addNote(budget.message);
    } else if (result.best->estimatedOutputBytes.value_or(0) >
               std::int64_t(shape.compression.targetSizeBytes * 1.10)) {
      addNote(
          "Estimated output exceeds the target size; consider a lower "
          "resolution or bitrate for a safer margin.");
    }
  }

  const auto elapsed = std::chrono::steady_clock::now() - startTime;
  result.planningMs =
      std::chrono::duration<double, std::milli>(elapsed).count();
  return Result<PlanResult>(std::move(result));
}

} // namespace velmux::core
