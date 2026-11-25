// FFmpeg backend — intent builder implementation. See execution_intent.h.
#include "processing/ffmpeg/execution_intent.h"

#include <algorithm>

namespace velmux::ffmpeg {

using velmux::core::BudgetInputs;
using velmux::core::CompressionIntent;
using velmux::core::CompressionPreset;
using velmux::core::ErrorCode;
using velmux::core::ResolvedRatePolicy;
using velmux::core::SubtitleTrackPolicy;

namespace {

RateControlIntent resolveRate(const MediaInfo &source,
                              const CompressionIntent &compression,
                              const Pipeline &pipeline) {
  BudgetInputs budget;
  budget.outputContainer = pipeline.container;
  budget.audioEnabled = !pipeline.audioStreamCopy;
  budget.videoEnabled = !pipeline.videoStreamCopy;
  budget.videoCodec = pipeline.videoCodec;
  const ResolvedRatePolicy policy =
      velmux::core::resolveRatePolicy(source, compression, budget);
  RateControlIntent rate;
  rate.bitrateMode = policy.mode == ResolvedRatePolicy::Mode::Bitrate;
  rate.bitRate = policy.bitRate;
  rate.quality = policy.quality;
  return rate;
}

// VideoConvert (mandate §19/§23/§24/§25).
Error buildVideoConvert(const JobRequest &request, const Pipeline &pipeline,
                        const MediaInfo &source,
                        const std::optional<TimeRange> &sampleWindow,
                        ExecutionIntent &intent) {
  const auto &payload =
      std::get<velmux::core::job::VideoConvert>(request.payload);
  intent.source = payload.source;
  intent.outputContainer = pipeline.container;
  intent.wantsVideo = source.hasVideo();
  intent.wantsAudio = source.hasAudio();

  intent.video.resolutionHeight =
      payload.video.resolutionHeight.has_value()
          ? payload.video.resolutionHeight
          : payload.video.customHeight;
  intent.video.frameRate = payload.video.frameRate.has_value()
                               ? std::optional<double>(
                                     payload.video.frameRate->toDouble())
                               : std::nullopt;
  intent.video.rotationDegrees = payload.adjustments.rotationDegrees;
  intent.video.flipHorizontal = payload.adjustments.flipHorizontal;
  intent.video.flipVertical = payload.adjustments.flipVertical;
  intent.video.playbackSpeed = payload.adjustments.playbackSpeed;
  intent.video.reverse = payload.adjustments.reverse;
  intent.video.hdrToSdr = payload.adjustments.hdrToSdr;

  // Audio lanes (mandate §23): empty rules = preserve every source track.
  const std::int32_t audioTrackCount = std::int32_t(source.audio.size());
  std::vector<std::int32_t> kept;
  for (const auto &rule : payload.audioTracks) {
    if (rule.externalUri.has_value() && !rule.externalUri->empty()) {
      if (rule.replace) {
        intent.replaceAudioUri = *rule.externalUri;
      } else {
        intent.externalAudioUris.push_back(*rule.externalUri);
      }
      continue;
    }
    if (!rule.include) {
      continue; // removal rule
    }
    if (rule.sourceTrackIndex < 0) {
      // Default rule = keep every track.
      kept.clear();
      for (std::int32_t i = 0; i < audioTrackCount; ++i) {
        kept.push_back(i);
      }
      break;
    }
    kept.push_back(rule.sourceTrackIndex);
  }
  if (payload.audioTracks.empty()) {
    for (std::int32_t i = 0; i < audioTrackCount; ++i) {
      kept.push_back(i);
    }
  }
  std::sort(kept.begin(), kept.end());
  kept.erase(std::unique(kept.begin(), kept.end()), kept.end());
  intent.audioSourceOrdinals = std::move(kept);
  if (intent.replaceAudioUri.empty() && intent.audioSourceOrdinals.empty() &&
      intent.externalAudioUris.empty()) {
    intent.wantsAudio = false; // every track removed
  }
  if (intent.replaceAudioUri.empty()) {
    // Out-of-range selections are request errors, never silent clamps.
    for (std::int32_t ordinal : intent.audioSourceOrdinals) {
      if (ordinal < 0 || ordinal >= audioTrackCount) {
        return Error::make(ErrorCode::InvalidTrackIdentity,
                           "audio track index " + std::to_string(ordinal) +
                               " is outside this source.");
      }
    }
  }
  intent.audioSettings = payload.audioSettings;

  // Subtitle lanes (mandate §24): empty = preserve all.
  const std::int32_t subTrackCount = std::int32_t(source.subtitles.size());
  bool anyKeep = false;
  bool anyBurn = false;
  for (const auto &rule : payload.subtitleTracks) {
    if (rule.sourceTrackIndex >= subTrackCount ||
        rule.sourceTrackIndex < -1) {
      return Error::make(ErrorCode::InvalidTrackIdentity,
                         "subtitle track index " +
                             std::to_string(rule.sourceTrackIndex) +
                             " is outside this source.");
    }
    switch (rule.policy) {
      case SubtitleTrackPolicy::Preserve:
      case SubtitleTrackPolicy::Embed:
        anyKeep = true;
        break;
      case SubtitleTrackPolicy::Remove:
        break;
      case SubtitleTrackPolicy::BurnIn:
        if (!anyBurn) {
          anyBurn = true;
          intent.subtitles.burnIn = true;
          intent.subtitles.burnInTrack = rule.sourceTrackIndex;
          if (rule.externalUri.has_value() && !rule.externalUri->empty()) {
            intent.subtitles.burnInExternal = *rule.externalUri;
          }
          intent.subtitles.fontSize = rule.fontSize;
          intent.subtitles.color = rule.color;
          intent.subtitles.position = rule.position;
        }
        break;
    }
  }
  intent.subtitles.keepSubtitles =
      payload.subtitleTracks.empty() ? source.hasSubtitles() : anyKeep;

  intent.metadata = payload.metadata;
  intent.coverArtUri =
      payload.metadata.artworkAction == velmux::core::MetadataFieldAction::Set
          ? std::optional<std::string>(payload.metadata.artworkUri)
          : std::nullopt;
  intent.preserveCoverArt =
      payload.metadata.artworkAction != velmux::core::MetadataFieldAction::Clear;
  intent.rate =
      resolveRate(source, payload.compression, pipeline);
  if (sampleWindow.has_value()) {
    intent.trim.startUs = sampleWindow->startUs;
    intent.trim.endUs = sampleWindow->endUs;
  }
  return {};
}

// AudioConvert (mandate §20/§32): audio-only pipeline.
Error buildAudioConvert(const JobRequest &request, const Pipeline &pipeline,
                        const MediaInfo &source,
                        const std::optional<TimeRange> &sampleWindow,
                        ExecutionIntent &intent) {
  const auto &payload =
      std::get<velmux::core::job::AudioConvert>(request.payload);
  intent.source = payload.source;
  intent.outputContainer = pipeline.container;
  intent.wantsVideo = false;
  intent.wantsAudio = source.hasAudio();
  for (std::int32_t i = 0; i < std::int32_t(source.audio.size()); ++i) {
    intent.audioSourceOrdinals.push_back(i);
  }
  intent.audioSettings = payload.audio;
  intent.audioDsp = payload.adjustments;
  intent.metadata = payload.metadata;
  intent.coverArtUri =
      payload.metadata.artworkAction == velmux::core::MetadataFieldAction::Set
          ? std::optional<std::string>(payload.metadata.artworkUri)
          : std::nullopt;
  intent.preserveCoverArt =
      payload.metadata.artworkAction != velmux::core::MetadataFieldAction::Clear;
  intent.rate =
      resolveRate(source, payload.compression, pipeline);
  if (sampleWindow.has_value()) {
    intent.trim.startUs = sampleWindow->startUs;
    intent.trim.endUs = sampleWindow->endUs;
  }
  return {};
}

// Crop (mandate §27 spatial): transcode with crop, everything else
// preserved.
Error buildCrop(const JobRequest &request, const Pipeline &pipeline,
                const MediaInfo &source, ExecutionIntent &intent) {
  const auto &payload = std::get<velmux::core::job::Crop>(request.payload);
  intent.source = payload.source;
  intent.outputContainer = pipeline.container;
  intent.wantsVideo = source.hasVideo();
  intent.wantsAudio = source.hasAudio();
  for (std::int32_t i = 0; i < std::int32_t(source.audio.size()); ++i) {
    intent.audioSourceOrdinals.push_back(i);
  }
  if (payload.crop.valid()) {
    CropRect crop;
    crop.x = payload.crop.x;
    crop.y = payload.crop.y;
    crop.width = payload.crop.width;
    crop.height = payload.crop.height;
    intent.video.crop = crop;
  }
  intent.subtitles.keepSubtitles = source.hasSubtitles();
  return {};
}

// Trim (mandate §27.1): the retained window.
Error buildTrim(const JobRequest &request, const Pipeline &pipeline,
                const MediaInfo &source, ExecutionIntent &intent) {
  const auto &payload = std::get<velmux::core::job::Trim>(request.payload);
  intent.source = payload.source;
  intent.outputContainer = pipeline.container;
  intent.wantsVideo = source.hasVideo();
  intent.wantsAudio = source.hasAudio();
  for (std::int32_t i = 0; i < std::int32_t(source.audio.size()); ++i) {
    intent.audioSourceOrdinals.push_back(i);
  }
  intent.subtitles.keepSubtitles = source.hasSubtitles();
  intent.trim.startUs = payload.range.startUs;
  intent.trim.endUs = payload.range.endUs;
  return {};
}

// ExtractAudio (mandate §28): audio-only extraction with track selection.
Error buildExtractAudio(const JobRequest &request, const Pipeline &pipeline,
                        const MediaInfo &source,
                        const std::optional<TimeRange> &sampleWindow,
                        ExecutionIntent &intent) {
  const auto &payload =
      std::get<velmux::core::job::ExtractAudio>(request.payload);
  intent.source = payload.source;
  intent.outputContainer = pipeline.container;
  intent.wantsVideo = false;
  intent.wantsAudio = source.hasAudio();
  if (payload.trackIndices.empty()) {
    intent.audioSourceOrdinals.push_back(0); // the default track
  } else {
    intent.audioSourceOrdinals = payload.trackIndices;
  }
  for (std::int32_t ordinal : intent.audioSourceOrdinals) {
    if (ordinal < 0 || ordinal >= std::int32_t(source.audio.size())) {
      return Error::make(ErrorCode::InvalidTrackIdentity,
                         "audio track index " + std::to_string(ordinal) +
                             " is outside this source.");
    }
  }
  intent.audioSettings = payload.audio;
  intent.metadata = payload.metadata;
  intent.coverArtUri =
      payload.metadata.artworkAction == velmux::core::MetadataFieldAction::Set
          ? std::optional<std::string>(payload.metadata.artworkUri)
          : std::nullopt;
  intent.preserveCoverArt =
      payload.metadata.artworkAction != velmux::core::MetadataFieldAction::Clear;
  intent.rate =
      resolveRate(source, payload.compression, pipeline);
  if (payload.range.has_value()) {
    intent.trim.startUs = payload.range->startUs;
    intent.trim.endUs = payload.range->endUs;
  }
  if (sampleWindow.has_value()) {
    intent.trim.startUs = sampleWindow->startUs;
    intent.trim.endUs = sampleWindow->endUs;
  }
  return {};
}

// ExtractSubtitles (mandate §30): sidecar extraction.
Error buildExtractSubtitles(const JobRequest &request,
                            const MediaInfo &source,
                            ExecutionIntent &intent) {
  const auto &payload =
      std::get<velmux::core::job::ExtractSubtitles>(request.payload);
  intent.source = payload.source;
  intent.wantsVideo = false;
  intent.wantsAudio = false;
  intent.subtitleExtract = true;
  if (payload.trackIndices.empty()) {
    intent.extractSubtitleTracks.push_back(0);
  } else {
    intent.extractSubtitleTracks = payload.trackIndices;
  }
  for (std::int32_t track : intent.extractSubtitleTracks) {
    if (track < 0 || track >= std::int32_t(source.subtitles.size())) {
      return Error::make(ErrorCode::InvalidTrackIdentity,
                         "subtitle track index " + std::to_string(track) +
                             " is outside this source.");
    }
  }
  return {};
}

} // namespace

Result<ExecutionIntent> buildIntent(const JobRequest &request,
                                    const Pipeline &pipeline,
                                    const MediaInfo &source,
                                    const std::optional<TimeRange> &sampleWindow) {
  ExecutionIntent intent;
  Error error;
  switch (request.payload.index()) {
    case 0: // VideoConvert
      error = buildVideoConvert(request, pipeline, source, sampleWindow,
                                intent);
      break;
    case 1: // AudioConvert
      error = buildAudioConvert(request, pipeline, source, sampleWindow,
                                intent);
      break;
    case 2: // Crop
      error = buildCrop(request, pipeline, source, intent);
      break;
    case 3: // Trim
      error = buildTrim(request, pipeline, source, intent);
      break;
    case 4: { // Cut (multi-range): deferred, carried for the future executor.
      const auto &payload = std::get<velmux::core::job::Cut>(request.payload);
      intent.source = payload.source;
      intent.outputContainer = pipeline.container;
      intent.cutRemoved = payload.removed;
      error = Error::make(ErrorCode::UnsupportedOperation,
                          "operation not yet implemented in this build");
      break;
    }
    case 5: { // Split: deferred, rules carried.
      const auto &payload =
          std::get<velmux::core::job::Split>(request.payload);
      intent.source = payload.source;
      intent.splitRules = payload.rules;
      error = Error::make(ErrorCode::UnsupportedOperation,
                          "operation not yet implemented in this build");
      break;
    }
    case 6: { // Merge: deferred, rules carried.
      const auto &payload =
          std::get<velmux::core::job::Merge>(request.payload);
      intent.source =
          payload.sources.empty() ? MediaSource{} : payload.sources.front();
      intent.mergeRules = payload.rules;
      error = Error::make(ErrorCode::UnsupportedOperation,
                          "operation not yet implemented in this build");
      break;
    }
    case 7: // ExtractAudio
      error = buildExtractAudio(request, pipeline, source, sampleWindow,
                                intent);
      break;
    case 8: { // ExtractFrames: deferred, rules carried.
      const auto &payload =
          std::get<velmux::core::job::ExtractFrames>(request.payload);
      intent.source = payload.source;
      intent.frameSelection = payload.selection;
      intent.imageRules = payload.image;
      error = Error::make(ErrorCode::UnsupportedOperation,
                          "operation not yet implemented in this build");
      break;
    }
    case 9: // ExtractSubtitles
      error = buildExtractSubtitles(request, source, intent);
      break;
    default:
      error = Error::make(ErrorCode::InvalidArgument, "unknown request payload");
      break;
  }
  if (!error.ok()) {
    return Result<ExecutionIntent>(error);
  }
  // Stream copy cannot serve additions (the added lane needs an encoder)
  // or timing transforms: refuse here instead of silently dropping work.
  if (pipeline.videoStreamCopy || pipeline.audioStreamCopy) {
    if (!intent.externalAudioUris.empty()) {
      return Result<ExecutionIntent>(Error::make(
          ErrorCode::UnsupportedOperation,
          "external audio additions require re-encoding", "ffmpeg"));
    }
    if (intent.video.reverse || intent.video.playbackSpeed.has_value()) {
      return Result<ExecutionIntent>(Error::make(
          ErrorCode::UnsupportedOperation,
          "reverse and playback speed require re-encoding", "ffmpeg"));
    }
    if (intent.video.hdrToSdr) {
      return Result<ExecutionIntent>(Error::make(
          ErrorCode::UnsupportedOperation,
          "HDR tone mapping requires re-encoding", "ffmpeg"));
    }
  }
  return Result<ExecutionIntent>(std::move(intent));
}

} // namespace velmux::ffmpeg
