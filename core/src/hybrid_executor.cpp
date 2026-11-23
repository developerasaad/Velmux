// Core-owned hybrid (stage-composed) executor — implementation.
// See hybrid_executor.h for the v1 scope contract.
#include "velmux/core/hybrid_executor.h"

#include "velmux/core/types.h"

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
                                const ConversionRequest &request) const {
  if (!request.video.enabled || pipeline.videoStreamCopy) {
    return false; // remux/stream-copy stays whole-pipeline
  }
  if (request.video.resolutionHeight.has_value() ||
      request.video.frameRate.has_value()) {
    return false; // v1 hybrid carries source geometry and rate
  }
  if (request.trim.startUs != 0 || request.trim.endUs != -1) {
    return false; // v1 hybrid does not trim: no executor may silently
                  // present an untrimmed output as trimmed
  }
  if ((request.video.crop.has_value() && request.video.crop->active()) ||
      request.video.rotationDegrees != 0 || request.video.flipHorizontal ||
      request.video.flipVertical) {
    return false; // v1 stage contracts carry plain frames: no transform
                  // filters on the interchange path
  }
  if (request.audio.gainDb.has_value() || request.audio.fadeInMs.has_value() ||
      request.audio.fadeOutMs.has_value() || request.audio.normalizeLoudness ||
      request.audio.playbackSpeed.has_value() || request.audio.removeSilence) {
    return false; // v1 hybrid stream-copies audio: no DSP chain
  }
  if (request.audio.enabled && !pipeline.audioStreamCopy) {
    return false; // audio transcode stays whole-pipeline
  }
  if (request.subtitles.policy == SubtitlePolicy::Preserve ||
      request.subtitles.policy == SubtitlePolicy::Extract ||
      request.subtitles.policy == SubtitlePolicy::BurnIn) {
    return false; // Preserve/BurnIn/Extract need whole-pipeline backends
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
  const ConversionRequest &request = params.request;

  if (!canExecute(pipeline, request)) {
    result.error = Error::make(ErrorCode::UnsupportedFeature,
                               "the hybrid executor cannot serve this "
                               "pipeline shape",
                               "hybrid");
    return result;
  }
  IBackend *decodeSide = decodeBackendFor(pipeline);
  IBackend *muxSide = muxBackendFor(pipeline);

  // Source path: hybrid execution needs a local file both sides can open.
  const std::string sourceUri = request.source.uri;
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
    result.error = Error::make(ErrorCode::UnsupportedFeature,
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

  // Audio copy decision: only when the source actually has audio and the
  // request keeps it.
  const bool copyAudio = request.audio.enabled;

  std::unique_ptr<IMuxStage> mux(muxSide->stages()->createMux());
  if (mux == nullptr) {
    result.error = Error::make(ErrorCode::UnsupportedFeature,
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

  backend::StageEncoderConfig encConfig;
  encConfig.codec = pipeline.videoCodec;
  encConfig.width = fmt.width;
  encConfig.height = fmt.height;
  encConfig.frameRate = pipeline.sourceFrameRate;
  if (request.video.rateControl == RateControl::Bitrate &&
      request.video.bitRate.has_value()) {
    encConfig.bitRate = *request.video.bitRate;
  } else {
    encConfig.quality = request.video.quality.value_or(60);
  }
  encConfig.globalHeader = mux->wantsGlobalHeader();
  std::unique_ptr<IVideoStageEncoder> encoder(
      muxSide->stages()->createVideoEncoder());
  if (encoder == nullptr) {
    result.error = Error::make(ErrorCode::UnsupportedFeature,
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
