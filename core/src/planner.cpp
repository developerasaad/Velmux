#include "velmux/core/planner.h"

#include "velmux/core/compression.h"

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
        break;
    }
  }
  return true;
}

} // namespace

FailedConfigKey failedConfigKeyFor(BackendId backend, const MediaInfo &source,
                                   const ConversionRequest &request,
                                   VideoCodec outVideoCodec) {
  FailedConfigKey key;
  key.backend = backend;
  key.codec = outVideoCodec;
  if (request.video.profile.has_value()) {
    key.profile = *request.video.profile;
  }
  if (request.video.pixelFormat.has_value()) {
    key.pixelFormat = pixelFormatName(*request.video.pixelFormat);
  }
  if (!source.video.empty()) {
    // Output geometry through the executor's transform order
    // (crop -> rotation -> scale); mirrors what validateOutput expects.
    std::int32_t w = source.video.front().width;
    std::int32_t h = source.video.front().height;
    if (request.video.crop.has_value() && request.video.crop->active()) {
      w = std::clamp(request.video.crop->width, 2,
                     std::max(0, w - request.video.crop->x));
      h = std::clamp(request.video.crop->height, 2,
                     std::max(0, h - request.video.crop->y));
    }
    if (request.video.rotationDegrees == 90 ||
        request.video.rotationDegrees == 270) {
      std::swap(w, h);
    }
    if (request.video.resolutionHeight.has_value() && h > 0) {
      const std::int32_t targetH = *request.video.resolutionHeight;
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

PlanResult Planner::plan(const MediaInfo &source,
                         const ConversionRequest &request) {
  const auto startTime = std::chrono::steady_clock::now();
  PlanResult result;

  // ----- Resolve requested output shape -------------------------------------
  const Container outContainer =
      request.outputContainer.value_or(source.container);
  if (!features_.knowsContainer(outContainer)) {
    result.feasible = false;
    result.message = "Output container is not a supported Velmux format.";
    return result;
  }

  const bool wantsVideo = request.video.enabled && source.hasVideo();
  const bool wantsAudio = request.audio.enabled && source.hasAudio();

  // Timeline-changing audio DSP (pitch-preserving speed, silence removal)
  // rewrites the audio timeline. With video present that desynchronizes
  // the streams (V1 has no video speed control to follow along), so the
  // request is honestly infeasible rather than producing drift. These
  // operations are for audio-only outputs (extraction, audio conversion).
  if (wantsVideo && wantsAudio &&
      (request.audio.playbackSpeed.has_value() ||
       request.audio.removeSilence)) {
    result.feasible = false;
    result.message =
        "Playback speed and silence removal apply to audio-only outputs; "
        "they cannot be combined with video in V1 (the streams would "
        "desynchronize). Disable video or remove the audio timing change.";
    return result;
  }

  VideoCodec outVideoCodec = VideoCodec::H264;
  if (wantsVideo) {
    outVideoCodec =
        request.video.codec.value_or(source.video.front().codec);
    if (!features_.knowsVideoCodec(outVideoCodec)) {
      result.feasible = false;
      result.message = "Video codec is not in the Velmux catalog.";
      return result;
    }
    if (!features_.containerAccepts(outContainer, outVideoCodec)) {
      result.feasible = false;
      result.message =
          std::string("Container ") + containerName(outContainer) +
          " does not accept video codec " + codecName(outVideoCodec) + ".";
      return result;
    }
  }

  AudioCodec outAudioCodec = AudioCodec::Aac;
  if (wantsAudio) {
    outAudioCodec = request.audio.codec.value_or(source.audio.front().codec);
    if (!features_.knowsAudioCodec(outAudioCodec)) {
      result.feasible = false;
      result.message = "Audio codec is not in the Velmux catalog.";
      return result;
    }
    if (!features_.containerAccepts(outContainer, outAudioCodec)) {
      result.feasible = false;
      result.message =
          std::string("Container ") + containerName(outContainer) +
          " does not accept audio codec " + codecName(outAudioCodec) + ".";
      return result;
    }
  }

  // Subtitle policy constraints (never silently discard: removal must be
  // explicit).
  bool extractJob = false;
  if (request.subtitles.policy == SubtitlePolicy::Extract) {
    // Extraction produces a text sidecar, not a media container: it is a
    // subtitle-only job. Mixed "convert + extract" would need two outputs;
    // v1 asks for two jobs (honest refusal, no silent half-work).
    extractJob = true;
    if (request.video.enabled || request.audio.enabled) {
      result.feasible = false;
      result.message =
          "Subtitle extraction is a standalone operation in this build: "
          "disable video and audio for the extract job.";
      return result;
    }
    if (!source.hasSubtitles()) {
      result.feasible = false;
      result.message = "This source has no subtitle track to extract.";
      return result;
    }
    // v1 extracts the first subtitle track. Bitmap formats need OCR
    // (outside V1, spec §14); CEA captions ride the video elementary
    // stream and have no demuxed track here.
    const SubtitleFormat first = source.subtitles.front().format;
    const bool textExtractable =
        first == SubtitleFormat::Srt || first == SubtitleFormat::WebVtt ||
        first == SubtitleFormat::Ass || first == SubtitleFormat::Ssa ||
        first == SubtitleFormat::MovText || first == SubtitleFormat::Ttml;
    if (!textExtractable) {
      result.feasible = false;
      result.message =
          std::string("The first subtitle track (") +
          subtitleFormatName(first) +
          ") cannot be extracted: bitmap and CEA formats need OCR, which "
          "is outside V1.";
      return result;
    }
  }
  // Trim window sanity (spec §15/§16). Executors clamp against the real
  // duration; an inverted or empty window is a request error.
  if (request.trim.endUs >= 0 && request.trim.endUs <= request.trim.startUs) {
    result.feasible = false;
    result.message = "Trim window is empty: end must come after start.";
    return result;
  }
  if (request.trim.startUs < 0) {
    result.feasible = false;
    result.message = "Trim start must not be negative.";
    return result;
  }
  // Advanced-control sanity (spec §18): range checks here, capability
  // routing below. Invalid values are request errors, never silently
  // clamped into a different encode.
  if (request.video.gopSize.has_value() && *request.video.gopSize < 1) {
    result.feasible = false;
    result.message = "GOP size must be at least 1 frame.";
    return result;
  }
  if (request.video.bFrames.has_value() && *request.video.bFrames < 0) {
    result.feasible = false;
    result.message = "B-frame count must not be negative.";
    return result;
  }
  // Track selection sanity: -1 = first track; otherwise a real index.
  if (request.audio.sourceTrackIndex < -1 ||
      request.audio.sourceTrackIndex >=
          static_cast<std::int32_t>(source.audio.size())) {
    result.feasible = false;
    result.message =
        std::string("Audio track index ") +
        std::to_string(request.audio.sourceTrackIndex) +
        " is outside this source (0.." +
        std::to_string(source.audio.empty() ? 0 : source.audio.size() - 1) +
        ").";
    return result;
  }
  // Replacing audio while processing all source tracks is contradictory.
  if (request.audio.replaceAudioUri.has_value() && request.audio.allTracks) {
    result.feasible = false;
    result.message =
        "Replacing audio cannot be combined with processing all source "
        "audio tracks.";
    return result;
  }
  if (request.subtitles.policy == SubtitlePolicy::BurnIn &&
      !request.subtitles.externalUri.has_value() && !source.hasSubtitles()) {
    result.feasible = false;
    result.message =
        "Burn-in requested but the source has no subtitle track and no "
        "external subtitle file was provided.";
    return result;
  }
  if (source.hasSubtitles() &&
      request.subtitles.policy == SubtitlePolicy::Preserve) {
    for (const auto &sub : source.subtitles) {
      if (!features_.containerAccepts(outContainer, sub.format)) {
        // Preserving this track in this container is impossible; surface it
        // rather than silently dropping.
        result.feasible = false;
        result.message =
            std::string("Subtitle track (") + subtitleFormatName(sub.format) +
            ") cannot be preserved in " + containerName(outContainer) +
            ". Choose another container, or remove/extract the track.";
        return result;
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
  if (extractJob) {
    const SubtitleFormat first = source.subtitles.front().format;
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
        pipeline.summary += " (track 1 of " +
                            std::to_string(source.subtitles.size()) + ")";
      }
      candidates.push_back(std::move(candidate));
    }
    if (candidates.empty()) {
      result.feasible = false;
      result.message =
          "No registered backend can extract subtitles on this device.";
      return result;
    }
  }

  const bool canStreamCopyVideo =
      wantsVideo && source.video.front().codec == outVideoCodec &&
      features_.videoSupport(outVideoCodec).streamCopy &&
      // No transformation requested: stream copy is only valid when the
      // output streams are identical to the input.
      !request.video.resolutionHeight.has_value() &&
      !request.video.frameRate.has_value() &&
      // Geometry transforms rewrite pixels (crop/rotate/flip): never a
      // stream copy, exactly like burn-in.
      !(request.video.crop.has_value() && request.video.crop->active()) &&
      request.video.rotationDegrees == 0 && !request.video.flipHorizontal &&
      !request.video.flipVertical &&
      // Burn-in rewrites the video pixels: it can never stream-copy.
      request.subtitles.policy != SubtitlePolicy::BurnIn;
  const bool canStreamCopyAudio =
      wantsAudio &&
      // Replacement audio is a different stream: never a copy of the
      // source's audio.
      !request.audio.replaceAudioUri.has_value() &&
      // With allTracks, every track must be copyable into the target
      // container as-is; a mixed copy/encode result is not "stream copy".
      (!request.audio.allTracks ||
       std::all_of(source.audio.begin(), source.audio.end(), [&](
                       const AudioStream &track) {
         return track.codec == outAudioCodec &&
                features_.audioSupport(track.codec).streamCopy &&
                features_.containerAccepts(outContainer, track.codec);
       })) &&
      (request.audio.allTracks ||
       (source.audio.front().codec == outAudioCodec &&
        features_.audioSupport(outAudioCodec).streamCopy &&
        features_.containerAccepts(outContainer, source.audio.front().codec))) &&
      !request.audio.sampleRate.has_value() &&
      !request.audio.channels.has_value() &&
      !request.audio.bitRate.has_value() &&
      // DSP rewrites audio samples: gain/fades/normalization/speed/silence
      // removal all require re-encoding.
      !request.audio.gainDb.has_value() &&
      !request.audio.fadeInMs.has_value() &&
      !request.audio.fadeOutMs.has_value() &&
      !request.audio.normalizeLoudness &&
      !request.audio.playbackSpeed.has_value() &&
      !request.audio.removeSilence;
  // Goal sanity: remuxing cannot shrink anything. Stream copy satisfies a
  // size goal only when the source already fits under the target.
  const bool sizeGoalAllowsCopy =
      (request.goal.kind != GoalKind::FitUnderSize &&
       request.goal.kind != GoalKind::SmallerFile) ||
      (request.goal.targetSizeBytes > 0 && source.sizeBytes > 0 &&
       source.sizeBytes <= request.goal.targetSizeBytes);
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
    // a different codec/profile/geometry on the same backend still plans.
    if (caps_.hasFailed(
            failedConfigKeyFor(backend, source, request, outVideoCodec))) {
      blockedByFailureRecord = true;
      return;
    }
    // Processing capabilities: geometry / rate / audio-format changes and
    // subtitle work only route to backends that actually implement them
    // (reported via BackendCapabilities; executors double-check).
    const bool wantsTrim = request.trim.startUs != 0 ||
                           request.trim.endUs != -1;
    if (wantsTrim && !caps->timeTrim) {
      // An executor that cannot trim must never receive a trimmed request:
      // it would silently produce the full-length output.
      return;
    }
    // Geometry transforms (spec §9) run through the video-processing
    // filters: only backends reporting videoScaling execute them (the
    // capability's documented meaning covers scale/rotate/crop).
    if (wantsVideo &&
        ((request.video.crop.has_value() && request.video.crop->active()) ||
         request.video.rotationDegrees != 0 || request.video.flipHorizontal ||
         request.video.flipVertical) &&
        !caps->videoScaling) {
      return;
    }
    // Audio DSP chain (spec §10): only backends reporting audioDsp.
    if (wantsAudio &&
        (request.audio.gainDb.has_value() ||
         request.audio.fadeInMs.has_value() ||
         request.audio.fadeOutMs.has_value() ||
         request.audio.normalizeLoudness ||
         request.audio.playbackSpeed.has_value() ||
         request.audio.removeSilence) &&
        !caps->audioDsp) {
      return;
    }
    // Advanced encoder tuning (spec §18): profile/level/pixel format/
    // GOP/B-frames only route to backends that implement the controls.
    const bool wantsEncoderTuning =
        request.video.profile.has_value() ||
        request.video.level.has_value() ||
        request.video.pixelFormat.has_value() ||
        request.video.gopSize.has_value() || request.video.bFrames.has_value();
    if (wantsVideo && wantsEncoderTuning && !caps->videoEncoderTuning) {
      return;
    }
    // HDR -> SDR conversion (spec §9) needs a tone-mapping backend; only
    // meaningful when the source is actually HDR.
    if (wantsVideo && !streamCopy &&
        request.video.hdrPolicy == HdrPolicy::ToneMapToSdr &&
        source.hasVideo() && source.video.front().hdr &&
        !caps->hdrToneMapping) {
      return;
    }
    // Multi-track audio and explicit track selection (spec §9).
    if (wantsAudio &&
        ((request.audio.allTracks && source.audio.size() > 1) ||
         request.audio.sourceTrackIndex >= 0) &&
        !caps->multiTrackAudio) {
      return;
    }
    // Audio replacement and cover art (spec §9/§10).
    if (request.audio.replaceAudioUri.has_value() &&
        !caps->audioReplacement) {
      return;
    }
    if (request.audio.coverArtUri.has_value() && !caps->coverArt) {
      return;
    }
    if (!streamCopy) {
      const bool geometryChange =
          wantsVideo && request.video.resolutionHeight.has_value() &&
          *request.video.resolutionHeight !=
              (source.hasVideo() ? source.video.front().height : 0);
      const bool rateChange =
          wantsVideo && request.video.frameRate.has_value() &&
          source.hasVideo() && source.video.front().frameRate > 0.0 &&
          std::abs(*request.video.frameRate -
                   source.video.front().frameRate) > 0.01;
      if ((geometryChange || rateChange) && !caps->videoScaling) {
        return;
      }
      const bool sampleRateChange =
          wantsAudio && request.audio.sampleRate.has_value() &&
          source.hasAudio() && source.audio.front().sampleRate > 0 &&
          *request.audio.sampleRate != source.audio.front().sampleRate;
      const bool channelChange =
          wantsAudio && request.audio.channels.has_value() &&
          source.hasAudio() && source.audio.front().channels > 0 &&
          *request.audio.channels != source.audio.front().channels;
      if ((sampleRateChange || channelChange) && !caps->audioResampling) {
        return;
      }
    }
    // Subtitle work: passthrough needs subtitlePassthrough, burn-in needs
    // subtitleBurnIn. Sources with subtitles under a Preserve policy must
    // not lose them by routing to a backend that silently drops text
    // tracks.
    const bool burnIn = request.subtitles.policy == SubtitlePolicy::BurnIn;
    const bool preserveWithSubtitles =
        request.subtitles.policy == SubtitlePolicy::Preserve &&
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
    if ((source.hasSubtitles() || request.subtitles.externalUri.has_value()) &&
        request.subtitles.policy == SubtitlePolicy::BurnIn) {
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
    pipeline.subtitleStreamCopy =
        request.subtitles.policy == SubtitlePolicy::Preserve;

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

    // Processing-mode alignment (user preference, soft factor only — a
    // valid pipeline always beats an invalid one).
    switch (request.processingMode) {
      case ProcessingMode::Auto:
        candidate.preferenceScore = 0.0;
        break;
      case ProcessingMode::PreferNative:
        candidate.preferenceScore =
            (backend == BackendId::Android || backend == BackendId::Apple)
                ? 1.0
                : 0.0;
        break;
      case ProcessingMode::PreferSoftware:
        candidate.preferenceScore = backend == BackendId::Ffmpeg ? 1.0 : 0.0;
        break;
      case ProcessingMode::Balanced:
        candidate.preferenceScore = hw ? 0.5 : 0.2;
        break;
    }

    std::string summary;
    if (pipeline.videoStreamCopy && pipeline.audioStreamCopy &&
        sameContainer) {
      summary = "Fast remux (stream copy, no re-encoding)";
    } else if (pipeline.videoStreamCopy) {
      summary = std::string("Remux to ") + containerName(outContainer) +
                " (video stream copy)";
    } else {
      summary = std::string("Transcode ") + codecName(outVideoCodec) + " via " +
                backendName(backend) + (hw ? " (hardware)" : " (software)") +
                " -> " + containerName(outContainer);
      if (pipeline.audioStreamCopy) {
        summary += " (audio copy)";
      }
    }
    pipeline.summary = std::move(summary);
    candidates.push_back(std::move(candidate));
  };

  // Stream-copy first (never re-encode unnecessarily), then transcodes.
  // Extraction jobs already have their candidates; generating transcode
  // candidates for a subtitle-only request would be meaningless.
  if (!extractJob) {
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
  const bool wantsAnyTrim =
      request.trim.startUs != 0 || request.trim.endUs != -1;
  const bool wantsTransform =
      (request.video.crop.has_value() && request.video.crop->active()) ||
      request.video.rotationDegrees != 0 || request.video.flipHorizontal ||
      request.video.flipVertical;
  const bool wantsAudioDsp =
      request.audio.gainDb.has_value() ||
      request.audio.fadeInMs.has_value() ||
      request.audio.fadeOutMs.has_value() || request.audio.normalizeLoudness ||
      request.audio.playbackSpeed.has_value() || request.audio.removeSilence;
  // v1 stage contracts carry no advanced encoder tuning and no multi-track
  // / replacement / cover-art audio handling: such requests stay on
  // whole-pipeline backends.
  const bool wantsAdvancedAudio =
      (request.audio.allTracks && source.audio.size() > 1) ||
      request.audio.sourceTrackIndex >= 0 ||
      request.audio.replaceAudioUri.has_value() ||
      request.audio.coverArtUri.has_value();
  const bool wantsEncoderTuning =
      request.video.profile.has_value() || request.video.level.has_value() ||
      request.video.pixelFormat.has_value() ||
      request.video.gopSize.has_value() || request.video.bFrames.has_value();
  if (wantsVideo && !hasTranscodeCandidate && !wantsAnyTrim &&
      !wantsTransform && !wantsAudioDsp && !wantsAdvancedAudio &&
      !wantsEncoderTuning) {
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
    return result;
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

  for (auto &candidate : candidates) {
    PlannedPipeline planned;
    planned.pipeline = std::move(candidate.pipeline);
    planned.cost = scoreCandidate(candidate);
    planned.expectedThroughput = candidate.throughput;
    planned.setupLatencyMs = candidate.setupMs;
    planned.transferCostMs = candidate.transferMs;
    planned.estimatedOutputBytes =
        estimateOutputBytes(source, planned.pipeline, request);
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

  // Fit-under-size feasibility cross-check with real budget math.
  if (request.goal.kind == GoalKind::FitUnderSize &&
      request.goal.targetSizeBytes > 0) {
    const BitrateBudget budget = computeBitrateBudget(
        source, request, request.goal.targetSizeBytes);
    if (!budget.feasible) {
      addNote(budget.message);
    } else if (result.best->estimatedOutputBytes.value_or(0) >
               std::int64_t(request.goal.targetSizeBytes * 1.10)) {
      addNote(
          "Estimated output exceeds the target size; consider a lower "
          "resolution or bitrate for a safer margin.");
    }
  }

  const auto elapsed = std::chrono::steady_clock::now() - startTime;
  result.planningMs =
      std::chrono::duration<double, std::milli>(elapsed).count();
  return result;
}

} // namespace velmux::core
