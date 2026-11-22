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
  if (request.subtitles.policy == SubtitlePolicy::Extract) {
    // Extraction to sidecar files is not implemented yet. Reject honestly
    // instead of silently dropping the tracks (D-013).
    result.feasible = false;
    result.message =
        "Subtitle extraction is not implemented in this build yet; tracks "
        "can be preserved, removed, or burned in.";
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

  const bool canStreamCopyVideo =
      wantsVideo && source.video.front().codec == outVideoCodec &&
      features_.videoSupport(outVideoCodec).streamCopy &&
      // No transformation requested: stream copy is only valid when the
      // output streams are identical to the input.
      !request.video.resolutionHeight.has_value() &&
      !request.video.frameRate.has_value() &&
      // Burn-in rewrites the video pixels: it can never stream-copy.
      request.subtitles.policy != SubtitlePolicy::BurnIn;
  const bool canStreamCopyAudio =
      wantsAudio && source.audio.front().codec == outAudioCodec &&
      features_.audioSupport(outAudioCodec).streamCopy &&
      !request.audio.sampleRate.has_value() &&
      !request.audio.channels.has_value() &&
      !request.audio.bitRate.has_value();
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
    // Processing capabilities: geometry / rate / audio-format changes and
    // subtitle work only route to backends that actually implement them
    // (reported via BackendCapabilities; executors double-check).
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
  if (streamCopyIsValid &&
      (sameContainer || features_.containerAccepts(outContainer, outVideoCodec))) {
    for (BackendId backend : caps_.registeredBackends()) {
      addCandidate(backend, true);
    }
  }
  for (BackendId backend : caps_.registeredBackends()) {
    addCandidate(backend, false);
  }

  if (candidates.empty()) {
    result.feasible = false;
    result.message =
        wantsVideo
            ? "No registered backend can decode this source and encode the "
              "requested output on this device."
            : "No registered backend can produce the requested output.";
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

  // Fit-under-size feasibility cross-check with real budget math.
  if (request.goal.kind == GoalKind::FitUnderSize &&
      request.goal.targetSizeBytes > 0) {
    const BitrateBudget budget = computeBitrateBudget(
        source, request, request.goal.targetSizeBytes);
    if (!budget.feasible) {
      result.message = budget.message;
    } else if (result.best->estimatedOutputBytes.value_or(0) >
               std::int64_t(request.goal.targetSizeBytes * 1.10)) {
      result.message =
          "Estimated output exceeds the target size; consider a lower "
          "resolution or bitrate for a safer margin.";
    }
  }

  const auto elapsed = std::chrono::steady_clock::now() - startTime;
  result.planningMs =
      std::chrono::duration<double, std::milli>(elapsed).count();
  return result;
}

} // namespace velmux::core
