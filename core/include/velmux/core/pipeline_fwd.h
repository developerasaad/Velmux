// Forward declarations for pipeline types used across backend contracts.
#pragma once

#include "velmux/core/types.h"

namespace velmux::core {

enum class JobState : std::uint8_t;

struct Pipeline {
  // Stage assignments: which backend serves which stage.
  struct Assignment {
    StageId stage;
    BackendId backend;
  };
  std::vector<Assignment> stages;
  // Resolved output configuration.
  VideoCodec videoCodec = VideoCodec::H264;
  AudioCodec audioCodec = AudioCodec::Aac;
  Container container = Container::Mp4;
  // Stream-copy flags (never re-encode unnecessarily).
  bool videoStreamCopy = false;
  bool audioStreamCopy = false;
  bool subtitleStreamCopy = false;
  bool usesHardware = false;
  std::string summary; // human-readable, shown in UI/history

  bool assigns(StageId stage, BackendId backend) const {
    for (const auto &a : stages) {
      if (a.stage == stage && a.backend == backend) {
        return true;
      }
    }
    return false;
  }
  std::vector<BackendId> involvedBackends() const;
};

struct ConversionRequest; // job.h

} // namespace velmux::core
