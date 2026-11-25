// Android stage workers for hybrid (stage-composed) pipelines — Milestone 4.
//
// These are ADDITIVE to the whole-pipeline executor (media_transcoder.cpp):
// the whole-pipeline path stays the default for single-backend candidates;
// the workers below exist so the core-owned hybrid executor can compose
// MediaCodec stages with other backends' stages through the CPU-frame
// interchange (spec §10).
//
// Implementation lives in android_stages.cpp and compiles only its real body
// on Android (__ANDROID__, exactly like media_transcoder.cpp). Host builds
// keep the declarations valid (android_backend.h holds the IStageBackend
// member on every host) and get honest null factories; the backend never
// reports stage participation there.
//
// Packet representation across the encoder -> mux boundary (documented
// contract between AndroidStageEncoder and AndroidMuxStage; PacketView stays
// a plain memory view per the backend contract):
//
//   - PacketView.data/size: the encoded access unit exactly as MediaCodec
//     produced it in ByteBuffer mode. For AVC/HEVC encoders feeding an MP4
//     family muxer this is the length-prefixed (AVCC/HVCC) NAL form, which
//     is precisely what AMediaMuxer_writeSampleData expects.
//
//   - PacketView.configData: the codec-specific data serialized as a small
//     self-describing TLV so the mux stage can rebuild the track format
//     without guessing NAL boundaries:
//         u32 BE sectionCount
//         per section: u32 BE byteLength + raw bytes
//     Sections are the "csd-0"/"csd-1" ByteBuffers of the ENCODER'S OWN
//     output format (AMediaCodec_getOutputFormat), i.e. the exact shape the
//     platform normalized — the same buffers the whole-pipeline path hands
//     to AMediaMuxer_addTrack via the format object. The mux stage
//     re-attaches them as "csd-0"/"csd-1" on a fresh AMediaFormat.
//
//   - wantsGlobalHeader() is true for the MP4 family: the parameter sets
//     must ride the track format rather than the sample stream.
#pragma once

#include "processing/contracts/stage.h"

#include <memory>

namespace velmux::android::media {

// MediaExtractor -> first video track -> MediaCodec decode into an
// AImageReader Surface (YUV_420_888). Frames are copied out of the AImage
// planes (planar and semi-planar layouts) into heap Yuv420P ICpuFrames.
class AndroidStageDecoder final
    : public velmux::backend::IVideoStageDecoder {
public:
  AndroidStageDecoder();
  ~AndroidStageDecoder() override;
  velmux::core::Error open(const std::string &sourcePath,
                           velmux::backend::InterchangePixel pixel) override;
  velmux::backend::InterchangeFormat outputFormat() const override;
  std::int64_t durationUs() const override;
  velmux::core::Error next(velmux::backend::CpuFramePtr &frame) override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// MediaCodec ByteBuffer-mode encoder (COLOR_FormatYUV420Flexible input).
// Pushed CPU frames are packed into each input buffer according to the
// per-buffer input format (color format + stride + slice-height, API 29+).
class AndroidStageEncoder final
    : public velmux::backend::IVideoStageEncoder {
public:
  AndroidStageEncoder();
  ~AndroidStageEncoder() override;
  velmux::core::Error
  open(const velmux::backend::StageEncoderConfig &config,
       velmux::backend::InterchangePixel pixel) override;
  velmux::core::Error push(const velmux::backend::ICpuFrame &frame) override;
  velmux::core::Error pull(velmux::backend::PacketView &packet) override;
  velmux::core::Error flush() override;

private:
  struct Impl;
  // captureConfigFromFormat (android_stages.cpp) captures the encoder's
  // AMediaCodec output-format csd buffers into the TLV config blob; it
  // lives at file scope next to the other pull-path helpers and needs
  // access to Impl.
  friend void captureConfigFromFormat(Impl &);
  std::unique_ptr<Impl> impl_;
};

// AMediaMuxer output. The video track is added from the FIRST video
// packet's configData (early packets are buffered until it arrives); audio
// is stream-copied from the source by a second extractor with one lookahead
// sample (mirrors FfmpegMuxStage::drainAudioUpTo boundary semantics).
class AndroidMuxStage final : public velmux::backend::IMuxStage {
public:
  AndroidMuxStage();
  ~AndroidMuxStage() override;
  velmux::core::Error
  open(const std::string &outputPath,
       const velmux::backend::IMuxStage::TrackOptions &tracks) override;
  bool wantsGlobalHeader() const override;
  velmux::core::Error
  writeVideoPacket(const velmux::backend::PacketView &packet) override;
  velmux::core::Error finalize() override;

private:
  // Copies source audio samples with pts <= boundaryPtsUs (all when
  // negative) into the muxer, holding one lookahead sample back.
  velmux::core::Error drainAudioUpTo(std::int64_t boundaryPtsUs);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// IStageBackend implementation: on Android every stage is served; host
// builds get honest null factories (see android_stages.cpp).
class AndroidStageBackend final : public velmux::backend::IStageBackend {
public:
  velmux::backend::IVideoStageDecoder *createVideoDecoder() override;
  velmux::backend::IVideoStageEncoder *createVideoEncoder() override;
  velmux::backend::IMuxStage *createMux() override;
};

} // namespace velmux::android::media
