// FFmpeg backend — stage workers for hybrid (stage-composed) pipelines
// (Milestone 4). These are ADDITIVE to the whole-pipeline executor: the
// whole-pipeline path remains the default for single-backend candidates
// (including all segmented-recovery behavior); stage workers exist so the
// core-owned hybrid executor can compose FFmpeg stages with other
// backends' stages through the CPU-frame interchange (spec §10).
#pragma once

#include "velmux/backend/stage.h"

#include <memory>

namespace velmux::ffmpeg {

// Decodes the first video stream of a source into the negotiated
// interchange format (Yuv420P through swscale).
class FfmpegStageDecoder final
    : public velmux::backend::IVideoStageDecoder {
public:
  FfmpegStageDecoder();
  ~FfmpegStageDecoder() override;
  velmux::core::Error open(const std::string &sourcePath,
                           velmux::backend::InterchangePixel pixel) override;
  velmux::backend::InterchangeFormat outputFormat() const override;
  std::int64_t durationUs() const override;
  velmux::core::Error next(velmux::backend::CpuFramePtr &frame) override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Encodes CPU frames with the pinned encoders (libx264 / libsvtav1 / ...).
class FfmpegStageEncoder final
    : public velmux::backend::IVideoStageEncoder {
public:
  FfmpegStageEncoder();
  ~FfmpegStageEncoder() override;
  velmux::core::Error open(const velmux::backend::StageEncoderConfig &config,
                           velmux::backend::InterchangePixel pixel) override;
  velmux::core::Error push(const velmux::backend::ICpuFrame &frame) override;
  velmux::core::Error pull(velmux::backend::PacketView &packet) override;
  velmux::core::Error flush() override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Writes the output container. Video packets come from the same backend's
// encoder; audio is stream-copied from the source when requested
// (compressed data never crosses backends — spec §31 interop cannot
// arise).
class FfmpegMuxStage final : public velmux::backend::IMuxStage {
public:
  FfmpegMuxStage();
  ~FfmpegMuxStage() override;
  velmux::core::Error
  open(const std::string &outputPath,
       const velmux::backend::IMuxStage::TrackOptions &tracks) override;
  bool wantsGlobalHeader() const override;
  velmux::core::Error
  writeVideoPacket(const velmux::backend::PacketView &packet) override;
  velmux::core::Error finalize() override;

private:
  // Copies source audio packets with pts <= boundaryPtsUs (all when
  // negative) into the output, holding one lookahead packet back.
  velmux::core::Error drainAudioUpTo(std::int64_t boundaryPtsUs);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// IStageBackend implementation: the FFmpeg backend exposes every stage.
class FfmpegStageBackend final : public velmux::backend::IStageBackend {
public:
  velmux::backend::IVideoStageDecoder *createVideoDecoder() override;
  velmux::backend::IVideoStageEncoder *createVideoEncoder() override;
  velmux::backend::IMuxStage *createMux() override;
};

} // namespace velmux::ffmpeg
