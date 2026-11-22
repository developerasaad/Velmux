// Velmux core — job manager.
//
// Owns the job lifecycle state machine and durable persistence. The React
// Native UI (and every other frontend) observes JobSnapshots and submits
// ConversionRequests; it never owns job state.
//
// State machine (spec §35 Queue):
//   queued -> preparing -> running -> validating -> completed
//                     |        |         |
//                     |        |         +-> failed (validation failed)
//                     |        +-> checkpointing -> running
//                     |        +-> paused -> running
//                     |        +-> failed / canceled / interrupted
//                     +-> recovering -> running (from safest checkpoint)
#pragma once

#include "velmux/backend/contracts.h"
#include "velmux/core/events.h"
#include "velmux/core/job.h"
#include "velmux/core/job_store.h"
#include "velmux/core/pipeline_fwd.h"
#include "velmux/core/planner.h"
#include "velmux/core/recovery.h"
#include "velmux/core/scheduler.h"
#include "velmux/core/types.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace velmux::backend {
class IBackend;
class ICheckpointCoordinator;
} // namespace velmux::backend

namespace velmux::core {

class Planner;
class OutputValidator;
class TelemetryStore;

struct EngineConfig {
  // Directory for the SQLite database and recovery artifacts. Must be
  // durable storage (not disposable cache) for resumability (spec §37).
  std::string dataDir;
};

class JobManager {
public:
  JobManager(EngineConfig config, EventBus &events, JobStore &store,
             CheckpointStore &checkpoints, Planner &planner,
             TelemetryStore &telemetry);
  ~JobManager();

  JobManager(const JobManager &) = delete;
  JobManager &operator=(const JobManager &) = delete;

  // Backends the manager may execute through. Registration happens once at
  // engine startup.
  void setBackends(std::vector<backend::IBackend *> backends);
  void setValidator(OutputValidator *validator);

  // Inspect through the first backend that can. Fast, no decoding.
  Result<MediaInfo> inspect(const MediaSource &source);

  // Plan without creating a job (estimate + feasibility for the UI).
  Result<PlanResult> plan(const MediaSource &source,
                          const ConversionRequest &request);

  // Create a durable job; persists before returning. The job enters the
  // queue; the scheduler starts it when resources allow.
  Result<std::string> submit(const ConversionRequest &request,
                             std::int32_t priority = 5);

  // Control.
  Result<void> cancel(const std::string &jobId);
  Result<void> pause(const std::string &jobId);
  Result<void> resume(const std::string &jobId);
  Result<void> retry(const std::string &jobId);
  // Delete a terminal job's durable record. Active jobs must be
  // canceled/paused first; output files are not touched.
  Result<void> remove(const std::string &jobId);

  // Queries.
  std::optional<JobSnapshot> snapshot(const std::string &jobId);
  std::vector<JobSnapshot> snapshots();
  std::optional<JobRecord> record(const std::string &jobId);
  // True while a worker thread is executing (or finalizing) this job.
  // Distinguishes "cancel recorded" (durable state already Canceled)
  // from "cancel completed" (worker unwound, artifacts stable): callers
  // that inspect execution artifacts must wait for this to turn false.
  bool isActive(const std::string &jobId);

  // Startup recovery: reconcile DB + artifacts, produce decisions, move
  // interrupted jobs to their recovery state. Must be called once after
  // setBackends() before the manager is considered operational.
  std::vector<RecoveryDecision> recoverInterruptedJobs();

  // Wire the scheduler's run callback: the manager executes the job.
  void attachScheduler(Scheduler &scheduler);

  // Scheduler entry point: executes one job to completion (worker thread).
  // Called by the engine's scheduler callback when capacity allows.
  void executeJob(const std::string &jobId);

  // Progress reporting from the execution path. The generation ties the
  // report to one execution run: a superseded run's late progress must not
  // regress the record owned by a newer run.
  void reportProgress(const std::string &jobId, double fraction,
                      std::int64_t outputBytes, std::uint64_t generation);

  // Cooperative cancel of every running job (engine shutdown). Does not
  // wait for the workers; the engine joins them separately.
  void cancelAllRunning(const char *reason);

private:
  struct RunningJob {
    JobRecord record;
    std::shared_ptr<backend::CancelToken> cancel;
    // Identifies one execution run of this job. A pause->resume or retry
    // starts a new run and replaces the entry; the superseded worker must
    // observe the mismatch and walk away without touching shared state.
    std::uint64_t generation = 0;
  };

  void persistAndEmit(JobRecord &record, const char *transitionNote);
  void finishFailure(JobRecord &record);
  bool validateOutput(JobRecord &record, std::uint64_t generation);
  // True (call under mutex_) when the running entry still belongs to the
  // given execution run. A pause->resume or retry replaces the entry; a late
  // wakeup from the superseded run must not clobber the new run's state.
  bool runStillCurrent(const std::string &jobId,
                       std::uint64_t generation) const;
  std::optional<Pipeline> planPipelineFor(const JobRecord &record);
  backend::IBackend *backendFor(const Pipeline &pipeline);
  std::string outputNameFor(const JobRecord &record) const;

  EngineConfig config_;
  EventBus &events_;
  JobStore &store_;
  CheckpointStore &checkpoints_;
  Planner &planner_;
  TelemetryStore &telemetry_;
  OutputValidator *validator_ = nullptr;
  std::vector<backend::IBackend *> backends_;
  std::map<std::string, RunningJob> running_;
  std::uint64_t nextGeneration_ = 0; // guarded by mutex_
  mutable std::mutex mutex_;
};

} // namespace velmux::core
