#include "velmux/core/recovery.h"

#include <sqlite3.h>

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>

namespace velmux::core {

RecoveryStrategy recoveryStrategyFor(StageId stage) {
  switch (stage) {
    case StageId::Demux:
      return RecoveryStrategy::PacketBoundary;
    case StageId::Decode:
      return RecoveryStrategy::FinalizedSegments;
    case StageId::VideoProcessing:
      return RecoveryStrategy::DependencyBoundary;
    case StageId::AudioProcessing:
      return RecoveryStrategy::SampleSegment;
    case StageId::SubtitleProcessing:
      return RecoveryStrategy::CueTimeline;
    case StageId::Encode:
      return RecoveryStrategy::FinalizedSegments;
    case StageId::Mux:
      return RecoveryStrategy::PacketBoundary;
    case StageId::Bitstream:
      return RecoveryStrategy::PacketBoundary;
  }
  return RecoveryStrategy::PacketBoundary;
}

const char *recoveryStrategyName(RecoveryStrategy strategy) {
  switch (strategy) {
    case RecoveryStrategy::ByteOffset:
      return "byte-offset";
    case RecoveryStrategy::PacketBoundary:
      return "packet-boundary";
    case RecoveryStrategy::SampleSegment:
      return "sample-segment";
    case RecoveryStrategy::FinalizedSegments:
      return "finalized-segments";
    case RecoveryStrategy::CueTimeline:
      return "cue-timeline";
    case RecoveryStrategy::DependencyBoundary:
      return "dependency-boundary";
    case RecoveryStrategy::AssembleFromPieces:
      return "assemble-from-pieces";
  }
  return "unknown";
}

Error CheckpointStore::insertCheckpoint(CheckpointRecord &record) {
  Error error;
  const std::string sql =
      "INSERT INTO checkpoints (job_id, sequence, stage, position, "
      "duration_us, artifact_path, artifact_bytes, committed_at) "
      "VALUES (?,?,?,?,?,?,?,?)";
  sqlite3_stmt *stmt = db_.prepare(sql, error);
  if (stmt == nullptr) {
    return error;
  }
  sqlite3_bind_text(stmt, 1, record.jobId.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 2, record.sequence);
  sqlite3_bind_text(stmt, 3, stageName(record.stage), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 4, record.position);
  sqlite3_bind_int64(stmt, 5, record.durationUs);
  sqlite3_bind_text(stmt, 6, record.artifactPath.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 7, record.artifactBytes);
  sqlite3_bind_int64(stmt, 8, record.committedAtMs);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    return Error::make(ErrorCode::InternalError, "insertCheckpoint failed");
  }
  record.id = db_.lastInsertRowId();
  return {};
}

std::vector<CheckpointRecord>
CheckpointStore::loadCheckpoints(const std::string &jobId, Error &error) {
  std::vector<CheckpointRecord> records;
  const std::string sql =
      "SELECT id, job_id, sequence, stage, position, duration_us, "
      "artifact_path, artifact_bytes, committed_at FROM checkpoints "
      "WHERE job_id = ? ORDER BY sequence ASC";
  sqlite3_stmt *stmt = db_.prepare(sql, error);
  if (stmt == nullptr) {
    return records;
  }
  sqlite3_bind_text(stmt, 1, jobId.c_str(), -1, SQLITE_TRANSIENT);
  for (;;) {
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      CheckpointRecord rec;
      rec.id = sqlite3_column_int64(stmt, 0);
      rec.jobId = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
      rec.sequence = sqlite3_column_int(stmt, 2);
      const std::string stage = reinterpret_cast<const char *>(
          sqlite3_column_text(stmt, 3));
      rec.position = sqlite3_column_int64(stmt, 4);
      rec.durationUs = sqlite3_column_int64(stmt, 5);
      const unsigned char *path = sqlite3_column_text(stmt, 6);
      if (path != nullptr) {
        rec.artifactPath = reinterpret_cast<const char *>(path);
      }
      rec.artifactBytes = sqlite3_column_int64(stmt, 7);
      rec.committedAtMs = sqlite3_column_int64(stmt, 8);
      // Stage name -> enum.
      for (std::uint8_t i = 0;
           i <= std::uint8_t(StageId::Bitstream); ++i) {
        if (stage == stageName(StageId(i))) {
          rec.stage = StageId(i);
          break;
        }
      }
      records.push_back(std::move(rec));
      continue;
    }
    if (rc != SQLITE_DONE) {
      error = Error::make(ErrorCode::InternalError, "loadCheckpoints failed");
    }
    break;
  }
  sqlite3_finalize(stmt);
  return records;
}

Error CheckpointStore::deleteCheckpoints(const std::string &jobId) {
  Error error;
  if (!db_.exec("DELETE FROM checkpoints WHERE job_id = '" + jobId + "'",
                error)) {
    return error;
  }
  return {};
}

std::string CheckpointStore::artifactsDirFor(const std::string &jobsRoot,
                                             const std::string &jobId) {
  return jobsRoot + "/artifacts/" + jobId;
}

bool RecoveryPlanner::artifactLooksValid(const CheckpointRecord &record) {
  if (record.artifactPath.empty()) {
    // Position-only checkpoints are valid by definition (no artifact to
    // verify); the executor will re-validate state when resuming.
    return true;
  }
  struct stat st{};
  if (::stat(record.artifactPath.c_str(), &st) != 0) {
    return false;
  }
  if (!S_ISREG(st.st_mode)) {
    return false;
  }
  // Artifact must be non-trivial in size OR explicitly recorded as zero
  // (e.g. a header-only segment start).
  if (record.artifactBytes <= 0) {
    return st.st_size == 0;
  }
  return st.st_size >= record.artifactBytes / 2;
}

std::vector<RecoveryDecision>
RecoveryPlanner::reconcile(const std::vector<JobRecord> &jobs,
                           CheckpointStore &checkpoints,
                           const std::string &jobsRoot) {
  std::vector<RecoveryDecision> decisions;
  for (const JobRecord &job : jobs) {
    RecoveryDecision decision;
    decision.jobId = job.id;

    switch (job.state) {
      case JobState::Completed:
      case JobState::Failed:
      case JobState::Canceled:
        decision.action = RecoveryAction::NothingToDo;
        decisions.push_back(std::move(decision));
        continue;
      case JobState::Queued:
      case JobState::Preparing:
        // Never started executing: run from the beginning.
        decision.action = RecoveryAction::RestartJob;
        decision.message = "Job had not started; queued for a fresh run.";
        decisions.push_back(std::move(decision));
        continue;
      default:
        break;
    }

    // Running/checkpointing/paused/recovering/interrupted: look for
    // committed checkpoints and validate the latest one against disk.
    Error loadError;
    std::vector<CheckpointRecord> records =
        checkpoints.loadCheckpoints(job.id, loadError);
    if (!loadError.ok() || records.empty()) {
      decision.action = RecoveryAction::RestartJob;
      decision.message =
          "No committed checkpoint survived; the job restarts from the "
          "beginning.";
      decisions.push_back(std::move(decision));
      continue;
    }

    // Walk backwards to the newest checkpoint whose artifact validates.
    std::optional<CheckpointRecord> resume;
    for (auto it = records.rbegin(); it != records.rend(); ++it) {
      if (artifactLooksValid(*it)) {
        resume = *it;
        break;
      }
    }
    if (!resume.has_value()) {
      decision.action = RecoveryAction::RestartJob;
      decision.message =
          "Checkpoints were recorded but their artifacts are gone or "
          "partial; the job restarts from the beginning.";
      decisions.push_back(std::move(decision));
      continue;
    }

    decision.action = RecoveryAction::ResumeFromCheckpoint;
    decision.resumeFrom = resume;
    decision.message =
        "Committed work found; resuming from checkpoint #" +
        std::to_string(resume->sequence) + " (" +
        recoveryStrategyName(recoveryStrategyFor(resume->stage)) + ").";
    decisions.push_back(std::move(decision));
  }
  return decisions;
}

} // namespace velmux::core
