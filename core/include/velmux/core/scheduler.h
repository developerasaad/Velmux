// Velmux core — resource-aware scheduler.
//
// Bounds concurrent jobs by real resource considerations: CPU, memory,
// thermal state, power source, hardware codec availability and measured
// concurrency behavior (spec §21). Jobs are NOT all run simultaneously;
// one broken file must not fail the batch — failures are isolated per job.
#pragma once

#include "velmux/core/job.h"

#include <chrono>
#include <deque>
#include <functional>
#include <optional>
#include <string>

namespace velmux::core {

struct ResourceSnapshot {
  std::int32_t cpuCores = 1;
  std::int64_t memoryAvailableMb = 0;
  // 0..1, higher = more thermally constrained (0 = unknown/cool).
  double thermalPressure = 0.0;
  bool onBattery = false;
  // Hardware encoder currently free (unknown backends report true).
  bool hardwareCodecAvailable = true;
};

// Policy inputs the scheduler uses. Defaults are conservative and derived
// from the resource snapshot; tests can pin them.
struct SchedulerPolicy {
  std::int32_t maxConcurrentJobs = 1;
  std::int32_t maxConcurrentHardwareJobs = 1;
  // Priority ordering weight: higher priority jobs run first; equal
  // priorities run FIFO by creation time.
  bool honorPriority = true;
};

class Scheduler {
public:
  using RunJobFn = std::function<void(const std::string &jobId)>;

  Scheduler(RunJobFn runJob) : runJob_(std::move(runJob)) {}

  // Feed current resource state; the scheduler re-evaluates concurrency.
  void updateResources(const ResourceSnapshot &snapshot);

  // Enqueue a job. idempotent per jobId.
  void submit(const std::string &jobId, std::int32_t priority = 5);

  // Remove a job (completed / cancelled / failed). Kicks off the next one.
  void finish(const std::string &jobId);

  // A running job entered a non-running state (paused); frees its slot.
  void suspend(const std::string &jobId);
  // A suspended job is ready to run again.
  void resume(const std::string &jobId);

  // Re-check queue and start jobs while capacity allows.
  void pump();

  std::size_t queuedCount() const { return queue_.size(); }
  std::size_t runningCount() const { return running_.size(); }
  // Snapshot of queued job ids in execution order.
  std::vector<std::string> queuedJobIds() const;

  const SchedulerPolicy &policy() const { return policy_; }
  void setPolicy(const SchedulerPolicy &p) { policy_ = p; }

  // Derive a concurrency policy from a resource snapshot (pure; tested).
  static SchedulerPolicy policyFor(const ResourceSnapshot &snapshot);

private:
  struct QueueEntry {
    std::string jobId;
    std::int32_t priority = 5;
    std::chrono::steady_clock::time_point enqueuedAt;
  };

  RunJobFn runJob_;
  SchedulerPolicy policy_{};
  ResourceSnapshot resources_{};
  std::deque<QueueEntry> queue_;
  std::vector<std::string> running_;
  // Suspended jobs wait for resume(); they keep their queue position in
  // `queue_` but are not started by pump() until resumed.
  std::vector<std::string> suspended_;
  bool isSuspended(const std::string &jobId) const;
};

} // namespace velmux::core
