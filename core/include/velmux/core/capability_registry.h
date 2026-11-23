// Velmux core — capability registry.
//
// Describes which backend can execute a Velmux-supported operation on the
// CURRENT device/build. Runtime discovery exists to choose execution paths,
// not to generate product features: only codecs that the feature registry
// intentionally supports are ever queried or exposed.
//
// Failure records are narrow: a failed codec configuration is recorded as
// backend + codec + profile + resolution + pixel format, never as a
// whole-codec ban.
#pragma once

#include "velmux/core/types.h"

#include <optional>
#include <vector>

namespace velmux::core {

struct VideoCapability {
  VideoCodec codec;
  CodecSupport support;
  // True when the implementation runs on dedicated hardware.
  bool hardware = false;
  // Ceiling for this implementation (0 = no special limit known).
  std::int32_t maxWidth = 0;
  std::int32_t maxHeight = 0;
};

struct AudioCapability {
  AudioCodec codec;
  CodecSupport support;
};

struct BackendCapabilities {
  BackendId backend;
  std::vector<VideoCapability> video;
  std::vector<AudioCapability> audio;
  std::vector<Container> readableContainers;
  std::vector<Container> writableContainers;
  // True when the backend can inspect media (demux metadata without
  // decoding).
  bool canInspect = false;
  // Can the backend honor a trim window (frame-exact transcode trims and
  // keyframe-snapped stream-copy trims)? When false, the planner must not
  // route trimmed requests to this backend — an executor that silently
  // ignores trim would present an untrimmed output as trimmed.
  bool timeTrim = false;
  // Can the backend change video geometry/frame-rate during a transcode
  // (scale/rotate/crop/temporal resample)? When false, the planner only
  // routes 1:1-geometry native pipelines to it.
  bool videoScaling = false;
  // Can the backend resample audio (sample rate / channel changes)?
  bool audioResampling = false;
  // Can the backend apply the audio DSP chain (gain, fades, loudness
  // normalization, pitch-preserving speed, silence removal) before the
  // output encoder? When false, the planner routes such requests elsewhere.
  bool audioDsp = false;
  // Can the backend carry subtitle tracks through without decoding them?
  bool subtitlePassthrough = false;
  // Can the backend render (burn in) subtitles onto the video?
  bool subtitleBurnIn = false;
  // Can the backend extract subtitle tracks into text sidecar files
  // (stream copy for native text formats, decode+re-encode to SRT for
  // container-internal text like MP4 mov_text)? Bitmap formats (PGS,
  // VobSub, DVB) need OCR and stay out of scope (spec §14).
  bool subtitleExtract = false;
  // Milestone 4 stage-composition participation: can this backend decode
  // video into CPU frames (Yuv420P interchange) for a hybrid pipeline?
  bool stageDecodeToCpuFrames = false;
  // Can this backend encode video from CPU frames and mux the result
  // (with stream-copied audio) for a hybrid pipeline?
  bool stageEncodeFromCpuFrames = false;
  // Can the backend honor advanced encoder tuning (profile, level, pixel
  // format, GOP size, B-frame count)? When false, the planner routes
  // requests setting any of those elsewhere (refusing when nowhere can).
  bool videoEncoderTuning = false;
  // Can the backend process every source audio track (multi-track output
  // with language metadata) and select among them?
  bool multiTrackAudio = false;
  // Can the backend replace the source audio with an external file?
  bool audioReplacement = false;
  // Can the backend embed external cover art and carry attached pictures?
  bool coverArt = false;
  // Can the backend tone-map HDR sources to SDR during a transcode?
  bool hdrToneMapping = false;
  // Free-form implementation label, e.g. "ffmpeg 7.1.1 (static, feature
  // selected)" or "MediaCodec (OMX.qcom.video.encoder.avc)".
  std::string implementation;
};

// Narrow failure key: identifies an unsupported configuration precisely.
struct FailedConfigKey {
  BackendId backend;
  VideoCodec codec;
  std::string profile;
  std::int32_t width = 0;
  std::int32_t height = 0;
  std::string pixelFormat;

  bool operator==(const FailedConfigKey &other) const;
};

class CapabilityRegistry {
public:
  void registerBackend(BackendCapabilities capabilities);

  // Remove everything a backend reported (used when a backend shuts down or
  // its host invalidates caches, e.g. after an OS upgrade).
  void invalidate(BackendId backend);

  // Best-effort query: which backends can serve this op?
  std::vector<BackendId> backendsFor(VideoCodec codec, CodecOp op) const;
  std::vector<BackendId> backendsFor(AudioCodec codec, CodecOp op) const;

  std::vector<BackendId> backendsThatRead(Container container) const;
  std::vector<BackendId> backendsThatWrite(Container container) const;

  std::optional<VideoCapability> videoCapability(BackendId backend,
                                                 VideoCodec codec) const;
  std::optional<AudioCapability> audioCapability(BackendId backend,
                                                 AudioCodec codec) const;

  // Narrow failure records. Recording a failure never bans the codec: only
  // this exact configuration on this backend.
  void recordFailure(const FailedConfigKey &key);
  bool hasFailed(const FailedConfigKey &key) const;
  void clearFailures(BackendId backend);
  std::size_t failureCount() const;

  std::vector<BackendId> registeredBackends() const;
  const BackendCapabilities *capabilitiesOf(BackendId backend) const;

private:
  std::vector<BackendCapabilities> backends_;
  std::vector<FailedConfigKey> failures_;
};

} // namespace velmux::core
