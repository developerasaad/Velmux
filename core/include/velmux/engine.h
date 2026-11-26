// Velmux core — engine facade.
//
// VelmuxEngine is the single entry point every host uses (the C ABI facade
// for the Kotlin engine clients; host engine tests). It owns the registries,
// the job manager, the scheduler and the event bus, and wires the crash
// recovery sequence at startup.
#pragma once

#include "capabilities/capabilities.h"
#include <velmux/configuration.h>
#include <velmux/events.h>
#include "catalog/features.h"
#include <velmux/jobs.h>
#include "jobs/job_manager.h"
#include "jobs/scheduler.h"
#include <velmux/types.h>

#include <memory>
#include <string>
#include <vector>

namespace velmux::backend {
class IBackend;
}

namespace velmux::core {

class Planner;
class SessionManager;
class TelemetryStore;
struct PlanResult;
struct SessionSnapshot;
struct FieldOptions;
struct ConfigurationAssessment;

class VelmuxEngine {
public:
  // Creates the engine, opens the durable store, registers backends,
  // initializes them (capability reporting happens here — once, never on
  // the user-visible conversion path) and runs crash recovery.
  static std::unique_ptr<VelmuxEngine> create(
      const EngineConfig &config,
      std::vector<std::unique_ptr<backend::IBackend>> backends);

  ~VelmuxEngine();

  VelmuxEngine(const VelmuxEngine &) = delete;
  VelmuxEngine &operator=(const VelmuxEngine &) = delete;

  // Inspect (or reuse submitted inspection) then plan.
  Result<MediaInfo> inspect(const MediaSource &source);
  // Inspect the request's sources, then plan the typed request.
  Result<PlanResult> plan(const JobRequest &request);
  // Sample generation (spec §20): short preview of the requested settings
  // over the densest section of the source. Synchronous (hosts call off
  // the UI thread); output lands under <dataDir>/samples.
  Result<SampleResult> generateSample(const JobRequest &request,
                                      std::int32_t sampleSeconds);
  // Direct durable submission (bypasses sessions but passes the SAME
  // validation through planning). `sourceInfos` carries submit-time
  // inspection results; `submissionId` gives durable idempotency.
  Result<std::string> submitJob(const JobRequest &request,
                                const std::vector<MediaInfo> &sourceInfos = {},
                                const std::string &submissionId = "",
                                std::int32_t priority = 5);
  Result<bool> cancelJob(const std::string &jobId);
  Result<bool> pauseJob(const std::string &jobId);
  Result<bool> resumeJob(const std::string &jobId);
  // Re-queue a failed/canceled/interrupted durable job.
  Result<bool> retryJob(const std::string &jobId);
  // Delete a terminal job's durable record (outputs are not touched).
  Result<bool> deleteJob(const std::string &jobId);
  std::optional<JobSnapshot> getJob(const std::string &jobId);
  std::vector<JobSnapshot> listJobs();
  // Reuse settings (mandate §37): a NEW editable configuration session
  // pre-filled with a terminal job's accepted intent. The original job
  // stays immutable.
  Result<std::string> reuseJobSettings(const std::string &jobId);

  // ------------------------------------------------------------------------
  // Configuration sessions (mandate §14): the authoritative configuration
  // surface. The engine owns the SessionManager and wires its hooks to the
  // inspector, the planner and the job manager.
  // ------------------------------------------------------------------------
  Result<std::string> createSession(Operation operation,
                                    std::vector<MediaSource> sources);
  Result<SessionSnapshot> prepareSession(const std::string &sessionId);
  Result<SessionSnapshot> getSessionSnapshot(const std::string &sessionId);
  Result<FieldOptions> getSessionOptions(const std::string &sessionId,
                                         FieldId field,
                                         std::uint64_t revision = 0);
  Result<SessionSnapshot> updateSession(
      const std::string &sessionId, std::uint64_t expectedRevision,
      const std::vector<FieldChange> &changes,
      bool acceptRelatedChange = false);
  Result<SampleResult> generateSessionSample(const std::string &sessionId,
                                             std::uint64_t expectedRevision);
  Result<std::string> submitSession(const std::string &sessionId,
                                    std::uint64_t expectedRevision,
                                    const std::string &submissionId);
  Result<void> closeSession(const std::string &sessionId);
  // True while a worker is executing/finalizing the job. Durable state
  // may already show Canceled/Paused while this is still true (control
  // paths persist intent immediately); artifact inspection must wait for
  // the worker to unwind (false) first.
  bool isJobActive(const std::string &jobId);
  std::vector<std::string> describeBackends();

  // Resource updates from the platform host (thermal, power, memory).
  void updateResources(const ResourceSnapshot &snapshot);

  // Event subscription for UI/notifiers.
  EventBus &events();

  // Diagnostics.
  const CapabilityRegistry &capabilities() const { return capabilities_; }
  const FeatureRegistry &features() const { return features_; }

private:
  VelmuxEngine(EngineConfig config);

  EngineConfig config_;
  const FeatureRegistry &features_;
  CapabilityRegistry capabilities_;
  std::unique_ptr<SqliteStore> db_;
  std::unique_ptr<JobStore> jobStore_;
  std::unique_ptr<CheckpointStore> checkpointStore_;
  std::unique_ptr<TelemetryStore> telemetry_;
  std::unique_ptr<Planner> planner_;
  std::unique_ptr<SessionManager> sessions_;
  std::unique_ptr<OutputValidator> validator_;
  EventBus events_;
  std::unique_ptr<JobManager> jobs_;
  std::unique_ptr<Scheduler> scheduler_;
  std::vector<std::unique_ptr<backend::IBackend>> backends_;
  // Worker threads spawned by the scheduler callback; joined at shutdown
  // before any member tears down (spec: deterministic resource cleanup).
  std::vector<std::thread> workers_;
  std::mutex workersMutex_;
};

} // namespace velmux::core
