// Apple stage workers for hybrid (stage-composed) pipelines — Milestone 4.
//
// These are ADDITIVE to the whole-pipeline executor (apple_pipeline.mm):
// the whole-pipeline path stays the default for single-backend candidates;
// the workers below exist so the core-owned hybrid executor can compose
// AVFoundation / VideoToolbox stages with other backends' stages through
// the CPU-frame interchange (spec §10).
//
// Implementation lives in apple_stages.mm and compiles only its real body
// on Apple platforms (__APPLE__, exactly like apple_pipeline.mm). Host
// builds keep the declarations valid (apple_backend.h holds the
// IStageBackend member on every host) and get honest null factories; the
// backend never reports stage participation there.
//
// Packet representation across the encoder -> mux boundary (documented
// contract between AppleStageEncoder and AppleMuxStage; PacketView stays
// a plain memory view per the backend contract):
//
//   - PacketView.data/size: the encoded access unit exactly as
//     VideoToolbox produced it in the output CMSampleBuffer. For AVC/HEVC
//     that is the length-prefixed (AVCC/HVCC) NAL form with 4-byte NAL
//     lengths, matching the format description's nalUnitHeaderLength —
//     precisely what an MP4-family mux track expects. The buffer is owned
//     by the encoder and reused on its next pull: consumers must finish
//     with it before pulling again (same lifetime rule as the FFmpeg
//     stage encoder's packets).
//
//   - PacketView.configData: the codec configuration as an Annex-B
//     bytestream — every parameter set of the encoder's own format
//     description (SPS/PPS for AVC; VPS/SPS/PPS for HEVC), each prefixed
//     with the 00 00 00 01 start code. It becomes available with the
//     first encoded sample (VideoToolbox emits parameter sets out of band
//     through the format description) and is then attached to every
//     packet, mirroring the FFmpeg encoder's extradata view semantics.
//     The mux stage splits it back into parameter sets and rebuilds the
//     CMVideoFormatDescription with CMVideoFormatDescriptionCreateFrom
//     H264/HEVCParameterSets (nalUnitHeaderLength = 4).
//
//   - PacketView.keyframe: derived from kCMSampleAttachmentKey_NotSync on
//     the encoder's output sample (absent/false = sync sample).
//
//   - wantsGlobalHeader() is false: QuickTime/MP4 sample tables carry the
//     codec configuration in the track's format description (rebuilt from
//     configData), so there is no stream-header/global-header mode to
//     negotiate; VideoToolbox never emits parameter sets in-band.
//
// v1 limitations (honest, mirroring the stage contract): the decoder
// reports the track's coded geometry (naturalSize, no rotation metadata
// on the interchange path); the muxer does not preserve container
// metadata or track transforms — hybrid execution exists for pipelines
// that would otherwise have no valid route at all.
#pragma once

#include "velmux/backend/stage.h"

#include <memory>

namespace velmux::apple::media {

// AVURLAsset + AVAssetReader decode of the first video track into
// biplanar video-range YCbCr 4:2:0, copied out of the locked CVPixelBuffer
// planes into heap planar Yuv420P ICpuFrames (Y plane copy + chroma
// deinterleave, row strides honored).
class AppleStageDecoder final : public velmux::backend::IVideoStageDecoder {
public:
  AppleStageDecoder();
  ~AppleStageDecoder() override;
  velmux::core::Error open(const std::string &sourcePath,
                           velmux::backend::InterchangePixel pixel) override;
  velmux::backend::InterchangeFormat outputFormat() const override;
  std::int64_t durationUs() const override;
  velmux::core::Error next(velmux::backend::CpuFramePtr &frame) override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// VideoToolbox encoder (VTCompressionSession) over CPU planar Yuv420P
// frames, packed into a biplanar CVPixelBuffer per push. Encoded samples
// arrive on the session's callback queue and are retained in a bounded
// queue (mutex + condition); pull() serializes the head sample's block
// buffer into an owned buffer valid until the next pull.
class AppleStageEncoder final : public velmux::backend::IVideoStageEncoder {
public:
  AppleStageEncoder();
  ~AppleStageEncoder() override;
  velmux::core::Error
  open(const velmux::backend::StageEncoderConfig &config,
       velmux::backend::InterchangePixel pixel) override;
  velmux::core::Error push(const velmux::backend::ICpuFrame &frame) override;
  velmux::core::Error pull(velmux::backend::PacketView &packet) override;
  velmux::core::Error flush() override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// AVAssetWriter output (MP4/M4V/MOV/3GP family; everything else is
// refused honestly). The video track is created from the FIRST video
// packet's configData (Annex-B -> rebuilt CMVideoFormatDescription,
// outputSettings nil so the writer consumes the encoder's own bitstream);
// audio is stream-copied from the source by a second AVAssetReader with
// nil output settings (compressed samples) into a sourceFormatHint input,
// interleaved by pts with one lookahead sample (mirrors
// FfmpegMuxStage::drainAudioUpTo boundary semantics).
class AppleMuxStage final : public velmux::backend::IMuxStage {
public:
  AppleMuxStage();
  ~AppleMuxStage() override;
  velmux::core::Error
  open(const std::string &outputPath,
       const velmux::backend::IMuxStage::TrackOptions &tracks) override;
  bool wantsGlobalHeader() const override;
  velmux::core::Error
  writeVideoPacket(const velmux::backend::PacketView &packet) override;
  velmux::core::Error finalize() override;

private:
  // Copies source audio samples with pts <= boundaryPtsUs (all when
  // negative) into the writer, holding one lookahead sample back.
  velmux::core::Error drainAudioUpTo(std::int64_t boundaryPtsUs);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// IStageBackend implementation: on Apple platforms every stage is served;
// host builds get honest null factories (see apple_backend.cpp).
class AppleStageBackend final : public velmux::backend::IStageBackend {
public:
  velmux::backend::IVideoStageDecoder *createVideoDecoder() override;
  velmux::backend::IVideoStageEncoder *createVideoEncoder() override;
  velmux::backend::IMuxStage *createMux() override;
};

} // namespace velmux::apple::media
