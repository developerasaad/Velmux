// Velmux core — normalized media model.
//
// The inspection result is the single source of truth about a media file,
// produced by any backend's inspector and consumed by the planner, job
// manager, recovery and validation subsystems. It intentionally mirrors the
// React Native bridge contract (see apps/mobile/src/native/NativeVelmux.ts).
#pragma once

#include "velmux/core/types.h"

#include <vector>

namespace velmux::core {

struct MediaSource {
  // Opaque, durable source reference. On Android this may be a SAF content
  // URI; on iOS a file URL or security-scoped bookmark reference; on host a
  // filesystem path. Backends resolve it through their platform integration.
  std::string uri;
  std::string displayName;
};

struct VideoStream {
  VideoCodec codec = VideoCodec::H264;
  std::int32_t width = 0;
  std::int32_t height = 0;
  double frameRate = 0.0; // 0 when unknown
  std::int64_t bitRate = 0; // bits per second, 0 when unknown
  std::string pixelFormat; // backend-reported name, e.g. "yuv420p"
  bool hdr = false;
  // CEA-608/708 captions detected in the video elementary stream (A53 side
  // data at inspection). They ride the video bitstream: stream copy keeps
  // them, transcoding drops them (surfaced honestly, spec §11).
  bool hasClosedCaptions = false;
  // Track labels when the container carries more than one track.
  std::string language; // BCP-47-ish tag as reported, may be empty
  bool isDefault = false;
};

struct AudioStream {
  AudioCodec codec = AudioCodec::Aac;
  std::int32_t channels = 0;
  std::int32_t sampleRate = 0;
  std::int64_t bitRate = 0;
  std::string language;
  bool isDefault = false;
};

struct SubtitleStream {
  SubtitleFormat format = SubtitleFormat::Srt;
  std::string language;
  bool isDefault = false;
  bool isForced = false;
};

struct MediaInfo {
  double durationSec = 0.0;
  Container container = Container::Mp4;
  std::int64_t sizeBytes = 0;
  std::vector<VideoStream> video;
  std::vector<AudioStream> audio;
  std::vector<SubtitleStream> subtitles;

  bool hasVideo() const { return !video.empty(); }
  bool hasAudio() const { return !audio.empty(); }
  bool hasSubtitles() const { return !subtitles.empty(); }

  // Total bitrate estimate in bits per second derived from size and duration.
  double overallBitRate() const {
    return durationSec > 0.0 ? double(sizeBytes) * 8.0 / durationSec : 0.0;
  }
};

} // namespace velmux::core
