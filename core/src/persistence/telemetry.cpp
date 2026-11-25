#include "persistence/telemetry.h"

#include "persistence/job_store.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <vector>

namespace velmux::core {

namespace {
std::int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
} // namespace

Error TelemetryStore::record(const PerfRecord &record) {
  Error error;
  const std::string sql =
      "INSERT INTO telemetry_perf (backend, video_codec, audio_codec, fps, "
      "setup_ms, transfer_ms_per_frame, success, recorded_at, note) "
      "VALUES (?,?,?,?,?,?,?,?,?)";
  sqlite3_stmt *stmt = db_.prepare(sql, error);
  if (stmt == nullptr) {
    return error;
  }
  sqlite3_bind_text(stmt, 1, backendName(record.backend), -1,
                    SQLITE_TRANSIENT);
  if (record.videoCodec.has_value()) {
    sqlite3_bind_text(stmt, 2, codecName(*record.videoCodec), -1,
                      SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 2);
  }
  if (record.audioCodec.has_value()) {
    sqlite3_bind_text(stmt, 3, codecName(*record.audioCodec), -1,
                      SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 3);
  }
  sqlite3_bind_double(stmt, 4, record.framesPerSecond);
  sqlite3_bind_double(stmt, 5, record.setupMs);
  sqlite3_bind_double(stmt, 6, record.transferMsPerFrame);
  sqlite3_bind_int(stmt, 7, record.success ? 1 : 0);
  sqlite3_bind_int64(stmt, 8, record.recordedAtMs > 0 ? record.recordedAtMs
                                                      : nowMs());
  sqlite3_bind_text(stmt, 9, record.note.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    return Error::make(ErrorCode::InternalError, "telemetry record failed");
  }
  return {};
}

Error TelemetryStore::recordPlanningLatency(double planningMs) {
  Error error;
  const std::string sql =
      "INSERT INTO telemetry_planning (planning_ms, recorded_at) VALUES (?,?)";
  sqlite3_stmt *stmt = db_.prepare(sql, error);
  if (stmt == nullptr) {
    return error;
  }
  sqlite3_bind_double(stmt, 1, planningMs);
  sqlite3_bind_int64(stmt, 2, nowMs());
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    return Error::make(ErrorCode::InternalError,
                       "planning latency record failed");
  }
  return {};
}

std::optional<ThroughputSample> TelemetryStore::throughputFor(
    BackendId backend, const std::optional<VideoCodec> &video,
    const std::optional<AudioCodec> &audio) const {
  // Average successful observations for this backend+codec pair.
  const std::string sql =
      std::string("SELECT COUNT(*), AVG(fps), AVG(setup_ms), ") +
      "AVG(transfer_ms_per_frame) FROM telemetry_perf WHERE backend = '" +
      backendName(backend) + "' AND success = 1" +
      (video.has_value()
           ? " AND video_codec = '" + std::string(codecName(*video)) + "'"
           : " AND video_codec IS NULL") +
      (audio.has_value()
           ? " AND audio_codec = '" + std::string(codecName(*audio)) + "'"
           : " AND audio_codec IS NULL");
  Error error;
  std::optional<ThroughputSample> sample;
  const_cast<SqliteStore &>(db_).query(
      sql,
      [&](sqlite3_stmt &stmt) {
        const std::int32_t count = sqlite3_column_int(&stmt, 0);
        if (count > 0) {
          ThroughputSample s;
          s.backend = backend;
          s.videoCodec = video;
          s.audioCodec = audio;
          s.framesPerSecond = sqlite3_column_double(&stmt, 1);
          s.setupMs = sqlite3_column_double(&stmt, 2);
          s.transferMsPerFrame = sqlite3_column_double(&stmt, 3);
          s.sampleCount = count;
          sample = s;
        }
        return false;
      },
      error);
  return sample;
}

double TelemetryStore::planningLatencyP50Ms() const {
  Error error;
  double p50 = 0.0;
  std::vector<double> values;
  const_cast<SqliteStore &>(db_).query(
      "SELECT planning_ms FROM telemetry_planning ORDER BY planning_ms",
      [&](sqlite3_stmt &stmt) {
        values.push_back(sqlite3_column_double(&stmt, 0));
        return true;
      },
      error);
  if (!values.empty()) {
    p50 = values[values.size() / 2];
  }
  return p50;
}

std::int32_t TelemetryStore::planningSampleCount() const {
  Error error;
  std::int32_t count = 0;
  const_cast<SqliteStore &>(db_).query(
      "SELECT COUNT(*) FROM telemetry_planning",
      [&](sqlite3_stmt &stmt) {
        count = sqlite3_column_int(&stmt, 0);
        return false;
      },
      error);
  return count;
}

} // namespace velmux::core
