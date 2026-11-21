// Forward declarations for the telemetry query interface used by the
// planner without pulling in the full telemetry header.
#pragma once

#include "velmux/core/types.h"

#include <optional>
#include <string>

namespace velmux::core {

struct ThroughputSample {
  BackendId backend = BackendId::Ffmpeg;
  std::optional<VideoCodec> videoCodec;
  std::optional<AudioCodec> audioCodec;
  // Observed frames per second (0 when unknown).
  double framesPerSecond = 0.0;
  // Observed setup latency in milliseconds.
  double setupMs = 0.0;
  // CPU<->hardware transfer overhead per frame in milliseconds.
  double transferMsPerFrame = 0.0;
  std::int32_t sampleCount = 0;
};

class ITelemetryQuery {
public:
  virtual ~ITelemetryQuery() = default;
  // Best known throughput sample for a backend+codec combination. Returns
  // nullopt when no observation exists (planner falls back to priors).
  virtual std::optional<ThroughputSample>
  throughputFor(BackendId backend, const std::optional<VideoCodec> &video,
                const std::optional<AudioCodec> &audio) const = 0;
  // Recorded planning latency statistics (p50 in ms, 0 when empty).
  virtual double planningLatencyP50Ms() const = 0;
  virtual std::int32_t planningSampleCount() const = 0;
};

} // namespace velmux::core
