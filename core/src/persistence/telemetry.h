// Velmux core — telemetry (performance learning).
//
// Device-local performance records: decoder/encoder throughput, setup time,
// transfer cost, failure patterns, thermal/concurrency observations. Used
// for future planning. Never benchmarked at user request time.
#pragma once

#include "persistence/telemetry_fwd.h"
#include <velmux/types.h>

#include <string>
#include <vector>

namespace velmux::core {

class SqliteStore;

struct PerfRecord {
  std::int64_t id = 0;
  BackendId backend = BackendId::Ffmpeg;
  std::optional<VideoCodec> videoCodec;
  std::optional<AudioCodec> audioCodec;
  double framesPerSecond = 0.0;
  double setupMs = 0.0;
  double transferMsPerFrame = 0.0;
  bool success = true;
  std::int64_t recordedAtMs = 0;
  std::string note;
};

class TelemetryStore : public ITelemetryQuery {
public:
  explicit TelemetryStore(SqliteStore &db) : db_(db) {}

  Error record(const PerfRecord &record);
  Error recordPlanningLatency(double planningMs);

  // ITelemetryQuery
  std::optional<ThroughputSample>
  throughputFor(BackendId backend, const std::optional<VideoCodec> &video,
                const std::optional<AudioCodec> &audio) const override;
  double planningLatencyP50Ms() const override;
  std::int32_t planningSampleCount() const override;

private:
  SqliteStore &db_;
};

} // namespace velmux::core
