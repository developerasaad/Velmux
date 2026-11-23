// Velmux core — resource-aware scheduler.
//
// Bounds concurrent jobs by real resource considerations: CPU, memory,
// thermal state, power source, hardware codec availability and measured
// concurrency behavior (spec §21). Jobs are NOT all run simultaneously;
// one broken file must not fail the batch — failures are isolated per job.
//
// Slot accounting uses per-submission generations: a worker finishing a
// superseded run (the job was cancelled, retried or paused and re-queued in
// the meantime) must not free or consume the newer submission's slot. Only
// runFinished() with the exact generation that started the run releases it.
#pragma once

#include "velmux/core/job.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
  // The engine's worker receives the generation its run was started with;
  // it MUST pass that exact generation back to runFinished().
  using RunJobFn =
      std::function<void(const std::string &jobId, std::uint64_t generation)>;

  explicit Scheduler(RunJobFn runJob) : runJob_(std::move(runJob)) {}

  // Feed current resource state; the scheduler re-evaluates concurrency.
  void updateResources(const ResourceSnapshot &snapshot);

  // Enqueue a job. idempotent per jobId. Each submission carries a fresh
  // generation; the previous submission's worker becomes a no-op observer.
  void submit(const std::string &jobId, std::int32_t priority = 5);

  // Remove a job entirely (completed / cancelled / failed / deleted).
  // Kicks off the next one.
  void finish(const std::string &jobId);

  // A running job entered a non-running state (paused); frees its slot.
  void suspend(const std::string &jobId);
  // A suspended job is ready to run again.
  void resume(const std::string &jobId);

  // A worker finished the run it was started with. Releases the slot only
  // when the generation still matches (no newer submission superseded it).
  // `deferred` keeps the job's hardware intent so a hardware-blocked job
  // re-queued by the engine does not busy-spin against a saturated cap.
  void runFinished(const std::string &jobId, std::uint64_t generation,
                   bool deferred = false);

  // Hardware codec accounting (spec §21): mobile hardware codecs are
  // typically single-instance. The engine calls these from the dispatch
  // path — noteHardwareIntent when a hardware pipeline must wait,
  // jobHardwareStarted when one actually begins executing.
  void noteHardwareIntent(const std::string &jobId);
  // True when a hardware pipeline may start now. A policy cap of 0 (no
  // hardware codec reported) degrades to 1: the queue must never starve —
  // an oversubscribed codec fails honestly at execution instead.
  bool hardwareSlotAvailable() const;
  void jobHardwareStarted(const std::string &jobId);

  // Re-check queue and start jobs while capacity allows.
  void pump();

  std::size_t queuedCount() const;
  std::size_t runningCount() const;
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
    std::uint64_t generation = 0;
  };
  struct RunningEntry {
    std::uint64_t generation = 0;
    bool hardware = false;
  };

  void eraseRunningLocked(const std::string &jobId);
  bool isSuspended(const std::string &jobId) const;

  RunJobFn runJob_;
  SchedulerPolicy policy_{};
  ResourceSnapshot resources_{};
  std::deque<QueueEntry> queue_;
  std::unordered_map<std::string, RunningEntry> running_;
  // Suspended jobs wait for resume(); they keep their queue position in
  // `queue_` but are not started by pump() until resumed.
  std::vector<std::string> suspended_;
  // Jobs whose current pipeline wants a hardware codec (queued or running).
  std::unordered_set<std::string> hardwareIntent_;
  std::size_t hardwareRunningCount_ = 0;
  std::uint64_t nextGeneration_ = 1;
  mutable std::mutex mutex_;
};

} // namespace velmux::core
