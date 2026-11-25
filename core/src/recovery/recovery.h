// Velmux core — recovery system.
//
// Checkpoints follow the crash-safe protocol (spec §26):
//   1. write temporary segment; 2. finalize; 3. validate; 4. flush durable
//   data; 5. promote/commit artifact; 6. record checkpoint in SQLite.
// Database and filesystem writes are not one atomic transaction. On
// recovery the planner reconciles DB state against artifacts on disk,
// detects orphaned/partial files, validates the last committed checkpoint
// and resumes from the safest valid point.
#pragma once

#include <velmux/jobs.h>
#include "persistence/job_store.h"
#include <velmux/types.h>

#include <optional>
#include <string>
#include <vector>

namespace velmux::core {

struct CheckpointRecord {
  std::int64_t id = 0;
  std::string jobId;
  std::int32_t sequence = 0;
  // Stage that produced the checkpoint.
  StageId stage = StageId::Encode;
  // Position in the stage's durable coordinate system (byte offset, packet
  // index or presentation timestamp depending on stage recovery strategy).
  std::int64_t position = 0;
  std::int64_t durationUs = 0; // media position when applicable
  // Finalized, validated artifact path (empty for position-only marks).
  std::string artifactPath;
  std::int64_t artifactBytes = 0;
  std::int64_t committedAtMs = 0;
};

class CheckpointStore {
public:
  explicit CheckpointStore(SqliteStore &db) : db_(db) {}

  Error insertCheckpoint(CheckpointRecord &record);
  std::vector<CheckpointRecord> loadCheckpoints(const std::string &jobId,
                                                Error &error);
  // Highest sequence ever committed for the job (0 when none). New runs
  // must continue AFTER it: (job_id, sequence) is UNIQUE, and rows from
  // earlier runs legitimately survive retries (they are the resume data).
  std::int32_t maxSequence(const std::string &jobId);
  Error deleteCheckpoints(const std::string &jobId);

  // Artifact directory management. Recovery segments live in durable
  // storage, not disposable cache, when losing cache would destroy
  // resumability (spec §37).
  static std::string artifactsDirFor(const std::string &jobsRoot,
                                     const std::string &jobId);

private:
  SqliteStore &db_;
};

// What recovery decided for one job after reconciliation.
enum class RecoveryAction : std::uint8_t {
  ResumeFromCheckpoint, // committed work exists: continue from it
  RestartJob,           // no valid checkpoint: run again from the start
  MarkInterrupted,      // cannot proceed now (e.g. source unavailable)
  NothingToDo,          // job was already terminal (completed/failed/...)
};

struct RecoveryDecision {
  RecoveryAction action = RecoveryAction::NothingToDo;
  std::string jobId;
  // For ResumeFromCheckpoint: the last validated checkpoint.
  std::optional<CheckpointRecord> resumeFrom;
  std::string message;
};

// Per-stage recovery strategies (spec §25). The planner consults these when
// weighing checkpoint overhead; executors implement them.
enum class RecoveryStrategy : std::uint8_t {
  ByteOffset,          // file copy
  PacketBoundary,      // stream copy / remux
  SampleSegment,       // audio processing
  FinalizedSegments,   // video transcode
  CueTimeline,         // subtitles
  DependencyBoundary,  // temporal filters
  AssembleFromPieces,  // final assembly
};

RecoveryStrategy recoveryStrategyFor(StageId stage);
const char *recoveryStrategyName(RecoveryStrategy strategy);

class RecoveryPlanner {
public:
  // Reconcile persisted DB state with artifacts on disk. For every
  // resumable job: inspect artifacts, detect orphaned/partial files,
  // validate the last committed checkpoint, decide the safest resume point.
  std::vector<RecoveryDecision>
  reconcile(const std::vector<JobRecord> &jobs, CheckpointStore &checkpoints,
            const std::string &jobsRoot);

  // Validate a checkpoint artifact (existence + non-zero + size sanity).
  // Real format-level validation runs through the output validator; this is
  // the cheap first gate during reconciliation.
  static bool artifactLooksValid(const CheckpointRecord &record);
};

} // namespace velmux::core
