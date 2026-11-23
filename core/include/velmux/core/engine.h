// Velmux core — engine facade.
//
// VelmuxEngine is the single entry point every frontend uses (React Native
// bridge, future Kotlin/Swift/Flutter/CLI hosts). It owns the registries,
// the job manager, the scheduler and the event bus, and wires the crash
// recovery sequence at startup.
#pragma once

#include "velmux/core/capability_registry.h"
#include "velmux/core/events.h"
#include "velmux/core/feature_registry.h"
#include "velmux/core/job.h"
#include "velmux/core/job_manager.h"
#include "velmux/core/scheduler.h"
#include "velmux/core/types.h"

#include <memory>
#include <string>
#include <vector>

namespace velmux::backend {
class IBackend;
}

namespace velmux::core {

class Planner;
class TelemetryStore;
struct PlanResult;

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
  Result<PlanResult> plan(const MediaSource &source,
                          const ConversionRequest &request);
  // Sample generation (spec §20): short preview of the requested settings
  // over the densest section of the source. Synchronous (hosts call off
  // the UI thread); output lands under <dataDir>/samples.
  Result<SampleResult> generateSample(const ConversionRequest &request,
                                       std::int32_t sampleSeconds);
  Result<std::string> startJob(const ConversionRequest &request);
  Result<bool> cancelJob(const std::string &jobId);
  Result<bool> pauseJob(const std::string &jobId);
  Result<bool> resumeJob(const std::string &jobId);
  // Re-queue a failed/canceled/interrupted durable job.
  Result<bool> retryJob(const std::string &jobId);
  // Delete a terminal job's durable record (outputs are not touched).
  Result<bool> deleteJob(const std::string &jobId);
  std::optional<JobSnapshot> getJob(const std::string &jobId);
  std::vector<JobSnapshot> listJobs();
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
