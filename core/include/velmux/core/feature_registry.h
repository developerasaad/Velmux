// Velmux core — feature registry.
//
// Defines what Velmux INTENTIONALLY supports (product-controlled). This is
// deliberately separate from the capability registry, which describes what
// the current device/build can execute. An OS adding a random new codec must
// not make a new format appear in the product.
//
// The registry is compiled, static data that mirrors docs/FEATURE_MATRIX.md.
#pragma once

#include "velmux/core/types.h"

#include <vector>

namespace velmux::core {

enum class Operation : std::uint16_t {
  // Video
  VideoCodecConversion,
  BitrateControl,
  QualityCompression,
  TargetSizeCompression,
  ResolutionConversion,
  FrameRateConversion,
  Crop,
  Rotation,
  Flip,
  Trim,
  RemoveAudio,
  ReplaceAudio,
  ExtractAudio,
  MultipleAudioTracks,
  FastRemux,
  StreamCopy,
  BatchProcessing,
  MetadataPreservation,
  HdrPreservation,
  HdrToSdr,
  SubtitlePreservation,
  SubtitleBurnIn,
  MultipleLanguageTracks,
  OutputValidation,
  // Audio
  AudioCodecConversion,
  AudioBitrateControl,
  SampleRateConversion,
  ChannelLayout,
  PcmBitDepth,
  Gain,
  LoudnessNormalization,
  FadeInOut,
  PlaybackSpeed,
  TimeStretch,
  SilenceRemoval,
  CoverArt,
  // Subtitles
  SubtitleExtract,
  SubtitleAddExternal,
  SubtitleTimingAdjust,
  SubtitleTextConversion,
};

struct CodecSupport {
  bool decode = false;
  bool encode = false;
  bool streamCopy = false;
};

struct VideoCodecFeature {
  VideoCodec codec;
  CodecSupport support;
  // Containers this codec can be legitimately muxed into by Velmux.
  std::vector<Container> containers;
  // Notes surfaced to planners/diagnostics (e.g. "decode/copy only in V1").
  const char *notes;
};

struct AudioCodecFeature {
  AudioCodec codec;
  CodecSupport support;
  std::vector<Container> containers;
  const char *notes;
};

struct SubtitleFormatFeature {
  SubtitleFormat format;
  // Text formats can be converted between each other without OCR.
  bool isText = false;
  // Bitmap formats can be burned into video (via libass / compositing).
  bool burnable = false;
  // Codec/container aware: mp4 timed text only in ISO-BMFF containers etc.
  std::vector<Container> containers;
};

class FeatureRegistry {
public:
  static const FeatureRegistry &instance();

  const std::vector<VideoCodecFeature> &videoCodecs() const;
  const std::vector<AudioCodecFeature> &audioCodecs() const;
  const std::vector<SubtitleFormatFeature> &subtitleFormats() const;

  // Is the codec part of the intentional catalog at all?
  bool knowsVideoCodec(VideoCodec codec) const;
  bool knowsAudioCodec(AudioCodec codec) const;

  // Is a specific operation supported by this codec (ignoring runtime)?
  CodecSupport videoSupport(VideoCodec codec) const;
  CodecSupport audioSupport(AudioCodec codec) const;

  // Container compatibility for a codec (explicit; never inferred).
  bool containerAccepts(Container container, VideoCodec codec) const;
  bool containerAccepts(Container container, AudioCodec codec) const;
  bool containerAccepts(Container container, SubtitleFormat format) const;

  // Whether a container is an intentional Velmux container at all.
  bool knowsContainer(Container container) const;

  // Containers valid for a video+audio pair (used by UI option lists).
  std::vector<Container>
  containersFor(const VideoCodec *video, const AudioCodec *audio) const;

private:
  FeatureRegistry();
  std::vector<VideoCodecFeature> video_;
  std::vector<AudioCodecFeature> audio_;
  std::vector<SubtitleFormatFeature> subtitles_;
  std::vector<Container> containers_;
};

} // namespace velmux::core
