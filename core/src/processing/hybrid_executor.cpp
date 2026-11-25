// Core-owned hybrid (stage-composed) executor — implementation.
// See hybrid_executor.h for the v1 scope contract.
#include "processing/hybrid_executor.h"

#include <velmux/types.h>

#include <chrono>

namespace velmux::core {

using backend::ExecuteParams;
using backend::ExecuteResult;
using backend::IBackend;
using backend::IMuxStage;
using backend::IVideoStageDecoder;
using backend::IVideoStageEncoder;

HybridExecutor::HybridExecutor(std::vector<IBackend *> backends)
    : backends_(std::move(backends)) {}

IBackend *HybridExecutor::decodeBackendFor(const Pipeline &pipeline) const {
  for (IBackend *backend : backends_) {
    if (pipeline.assigns(StageId::Decode, backend->id())) {
      return backend->stages() != nullptr ? backend : nullptr;
    }
  }
  return nullptr;
}

IBackend *HybridExecutor::muxBackendFor(const Pipeline &pipeline) const {
  for (IBackend *backend : backends_) {
    if (pipeline.assigns(StageId::Mux, backend->id())) {
      return backend->stages() != nullptr ? backend : nullptr;
    }
  }
  return nullptr;
}

bool HybridExecutor::canExecute(const Pipeline &pipeline,
                                const JobRequest &request) const {
  // Only the plain VideoConvert shape rides the v1 interchange: every
  // other operation's payload carries work the stage contracts cannot
  // express (trim windows, crop rects, timeline selections, extraction).
  const auto *payload = std::get_if<job::VideoConvert>(&request.payload);
  if (payload == nullptr) {
    return false;
  }
  if (pipeline.videoStreamCopy) {
    return false; // remux/stream-copy stays whole-pipeline
  }
  // v1 hybrid carries source geometry and rate.
  if (payload->video.resolutionHeight.has_value() ||
      payload->video.customWidth.has_value() ||
      payload->video.customHeight.has_value() ||
      payload->video.frameRate.has_value()) {
    return false;
  }
  // The plain-frame interchange has no transform/timing filters.
  if (payload->adjustments.rotationDegrees != 0 ||
      payload->adjustments.flipHorizontal ||
      payload->adjustments.flipVertical ||
      payload->adjustments.playbackSpeed.has_value() ||
      payload->adjustments.reverse || payload->adjustments.hdrToSdr) {
    return false;
  }
  // Audio: stream-copied by the mux side only. Removal rules are fine
  // (nothing to carry); selection/external/replacement lanes are
  // whole-pipeline work.
  if (!payload->audioTracks.empty()) {
    for (const AudioTrackRule &rule : payload->audioTracks) {
      if (rule.externalUri.has_value() || rule.replace) {
        return false;
      }
    }
  }
  // Subtitles: Remove only (Preserve/Embed/BurnIn need whole-pipeline
  // backends — refusing here is what keeps "never silently dropped"
  // true; an empty rule list means preserve-all and is refused too).
  if (payload->subtitleTracks.empty()) {
    return false;
  }
  for (const SubtitleTrackRule &rule : payload->subtitleTracks) {
    if (rule.policy != SubtitleTrackPolicy::Remove) {
      return false;
    }
  }
  if (!pipeline.audioStreamCopy) {
    return false; // audio transcode stays whole-pipeline
  }
  // Stage shape: the decode side owns demux + video processing; the mux
  // side owns encode + mux + bitstream.
  IBackend *decodeSide = decodeBackendFor(pipeline);
  IBackend *muxSide = muxBackendFor(pipeline);
  if (decodeSide == nullptr || muxSide == nullptr) {
    return false;
  }
  if (decodeSide->stages() == nullptr || muxSide->stages() == nullptr) {
    return false;
  }
  const BackendId decodeId = decodeSide->id();
  if (!pipeline.assigns(StageId::Demux, decodeId) ||
      !pipeline.assigns(StageId::VideoProcessing, decodeId)) {
    return false;
  }
  const BackendId muxId = muxSide->id();
  if (!pipeline.assigns(StageId::Encode, muxId) ||
      !pipeline.assigns(StageId::Mux, muxId)) {
    return false;
  }
  return true;
}

ExecuteResult HybridExecutor::execute(const ExecuteParams &params) {
  ExecuteResult result;
  const auto started = std::chrono::steady_clock::now();
  const Pipeline &pipeline = params.pipeline;
  const JobRequest &request = params.request;

  if (!canExecute(pipeline, request)) {
    result.error = Error::make(ErrorCode::UnsupportedOperation,
                               "the hybrid executor cannot serve this "
                               "pipeline shape",
                               "hybrid");
    return result;
  }
  const auto &payload = std::get<job::VideoConvert>(request.payload);
  IBackend *decodeSide = decodeBackendFor(pipeline);
  IBackend *muxSide = muxBackendFor(pipeline);

  // Source path: hybrid execution needs a local file both sides can open.
  const std::string sourceUri = payload.source.uri;
  const std::string prefix = "file://";
  std::string sourcePath = sourceUri;
  if (sourcePath.rfind(prefix, 0) == 0) {
    sourcePath = sourcePath.substr(prefix.size());
  }
  if (sourcePath.rfind("content://", 0) == 0 ||
      sourcePath.rfind("http://", 0) == 0 ||
      sourcePath.rfind("https://", 0) == 0 || sourcePath.empty() ||
      sourcePath.front() != '/') {
    result.error = Error::make(ErrorCode::SourceUnavailable,
                               "the hybrid path needs a local file", "hybrid");
    return result;
  }

  const std::unique_ptr<IVideoStageDecoder> decoder(
      decodeSide->stages()->createVideoDecoder());
  if (decoder == nullptr) {
    result.error = Error::make(ErrorCode::UnsupportedOperation,
                               "the decode backend cannot create a stage "
                               "decoder",
                               "hybrid");
    return result;
  }
  Error err = decoder->open(sourcePath, backend::InterchangePixel::Yuv420P);
  if (!err.ok()) {
    result.error = err;
    return result;
  }

  // Audio copy decision: removal rules drop the lane; everything else
  // keeps the source audio (the mux side stream-copies it).
  const bool copyAudio = [&payload]() {
    if (payload.audioTracks.empty()) {
      return true;
    }
    return !std::all_of(payload.audioTracks.begin(), payload.audioTracks.end(),
                        [](const AudioTrackRule &rule) {
                          return !rule.include;
                        });
  }();

  std::unique_ptr<IMuxStage> mux(muxSide->stages()->createMux());
  if (mux == nullptr) {
    result.error = Error::make(ErrorCode::UnsupportedOperation,
                               "the mux backend cannot create a stage muxer",
                               "hybrid");
    return result;
  }
  const backend::InterchangeFormat fmt = decoder->outputFormat();
  IMuxStage::TrackOptions tracks;
  tracks.videoCodec = pipeline.videoCodec;
  tracks.container = pipeline.container;
  tracks.width = fmt.width;
  tracks.height = fmt.height;
  tracks.frameRate = pipeline.sourceFrameRate;
  tracks.copyAudio = copyAudio;
  tracks.sourcePath = sourcePath;
  err = mux->open(params.outputPath, tracks);
  if (!err.ok()) {
    result.error = err;
    return result;
  }

  // Rate policy from the compression intent (mandate §20): presets map to
  // the quality scale; Custom honors explicit values; TargetSize cannot
  // budget honestly on this path (no inspection data) and falls back to
  // the balanced scale — the planner keeps size-targeted jobs on
  // whole-pipeline routes whenever one exists.
  const CompressionIntent &compression = payload.compression;
  std::int32_t quality = 65; // disabled: high-fidelity default (CRF ~18)
  std::int64_t bitRate = 0;
  if (compression.enabled) {
    switch (compression.preset) {
      case CompressionPreset::HighQuality:
        quality = 85;
        break;
      case CompressionPreset::Balanced:
      case CompressionPreset::TargetSize:
        quality = 60;
        break;
      case CompressionPreset::SmallerFile:
        quality = 35;
        break;
      case CompressionPreset::Custom:
        if (compression.videoBitrate.has_value() &&
            *compression.videoBitrate > 0) {
          bitRate = *compression.videoBitrate;
        } else {
          quality = std::clamp(compression.quality.value_or(60), 1, 100);
        }
        break;
    }
  }

  backend::StageEncoderConfig encConfig;
  encConfig.codec = pipeline.videoCodec;
  encConfig.width = fmt.width;
  encConfig.height = fmt.height;
  encConfig.frameRate = pipeline.sourceFrameRate;
  if (bitRate > 0) {
    encConfig.bitRate = bitRate;
  } else {
    encConfig.quality = quality;
  }
  encConfig.globalHeader = mux->wantsGlobalHeader();
  std::unique_ptr<IVideoStageEncoder> encoder(
      muxSide->stages()->createVideoEncoder());
  if (encoder == nullptr) {
    result.error = Error::make(ErrorCode::UnsupportedOperation,
                               "the encode backend cannot create a stage "
                               "encoder",
                               "hybrid");
    return result;
  }
  err = encoder->open(encConfig, backend::InterchangePixel::Yuv420P);
  if (!err.ok()) {
    result.error = err;
    return result;
  }

  const std::int64_t totalUs = decoder->durationUs();
  const auto reportProgress = [&](std::int64_t ptsUs) {
    if (params.onProgress && totalUs > 0) {
      backend::ProgressReport report;
      report.processedDurationUs = ptsUs;
      report.totalDurationUs = totalUs;
      report.fraction =
          std::min(1.0, double(std::max<std::int64_t>(ptsUs, 0)) /
                            double(totalUs));
      params.onProgress(report);
    }
  };

  bool cancelled = false;
  std::int64_t frames = 0;
  for (;;) {
    if (params.cancel && params.cancel->isCancelled()) {
      cancelled = true;
      break;
    }
    backend::CpuFramePtr frame;
    err = decoder->next(frame);
    if (!err.ok()) {
      result.error = err;
      return result;
    }
    if (frame == nullptr) {
      break; // end of stream
    }
    err = encoder->push(*frame);
    if (!err.ok()) {
      result.error = err;
      return result;
    }
    ++frames;
    reportProgress(frame->ptsUs());
    backend::PacketView packet;
    for (;;) {
      err = encoder->pull(packet);
      if (!err.ok()) {
        result.error = err;
        return result;
      }
      if (packet.size == 0) {
        break;
      }
      err = mux->writeVideoPacket(packet);
      if (!err.ok()) {
        result.error = err;
        return result;
      }
    }
  }

  if (!cancelled) {
    err = encoder->flush();
    if (!err.ok()) {
      result.error = err;
      return result;
    }
    backend::PacketView packet;
    for (;;) {
      err = encoder->pull(packet);
      if (!err.ok()) {
        result.error = err;
        return result;
      }
      if (packet.size == 0) {
        break;
      }
      err = mux->writeVideoPacket(packet);
      if (!err.ok()) {
        result.error = err;
        return result;
      }
    }
    err = mux->finalize();
    if (!err.ok()) {
      result.error = err;
      return result;
    }
  }

  if (cancelled) {
    result.error = Error::make(ErrorCode::Cancelled, "hybrid run cancelled",
                               "hybrid");
    return result;
  }
  result.success = true;
  result.framesProcessed = frames;
  result.wallClockSec =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
          .count();
  return result;
}

} // namespace velmux::core
