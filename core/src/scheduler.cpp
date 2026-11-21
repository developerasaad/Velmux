#include "velmux/core/scheduler.h"

#include <algorithm>

namespace velmux::core {

SchedulerPolicy Scheduler::policyFor(const ResourceSnapshot &snapshot) {
  SchedulerPolicy policy;
  // CPU-bound software transcoding: one heavy job per 2 cores, at least one.
  const std::int32_t cores = std::max<std::int32_t>(snapshot.cpuCores, 1);
  policy.maxConcurrentJobs = std::clamp<std::int32_t>(cores / 2, 1, 4);

  // Hardware codecs are typically single-instance on mobile; allow two only
  // when thermals are cool and we are not on battery.
  const bool constrained =
      snapshot.thermalPressure > 0.5 || snapshot.onBattery;
  policy.maxConcurrentHardwareJobs = constrained ? 1 : 2;

  // Under heavy thermal pressure, serialize everything.
  if (snapshot.thermalPressure > 0.8) {
    policy.maxConcurrentJobs = 1;
    policy.maxConcurrentHardwareJobs = 1;
  }
  // Memory pressure: below 512 MB free, run one job.
  if (snapshot.memoryAvailableMb > 0 && snapshot.memoryAvailableMb < 512) {
    policy.maxConcurrentJobs = 1;
  }
  if (!snapshot.hardwareCodecAvailable) {
    policy.maxConcurrentHardwareJobs = 0;
  }
  return policy;
}

void Scheduler::updateResources(const ResourceSnapshot &snapshot) {
  resources_ = snapshot;
  policy_ = policyFor(snapshot);
  pump();
}

void Scheduler::submit(const std::string &jobId, std::int32_t priority) {
  const auto existing = std::find_if(queue_.begin(), queue_.end(),
                                     [&](const QueueEntry &e) {
                                       return e.jobId == jobId;
                                     });
  if (existing != queue_.end() || isSuspended(jobId)) {
    return; // idempotent
  }
  for (const auto &running : running_) {
    if (running == jobId) {
      return;
    }
  }
  queue_.push_back(
      QueueEntry{jobId, priority, std::chrono::steady_clock::now()});
  // Stable ordering: priority desc, then FIFO.
  std::stable_sort(queue_.begin(), queue_.end(),
                   [](const QueueEntry &a, const QueueEntry &b) {
                     if (a.priority != b.priority) {
                       return a.priority > b.priority;
                     }
                     return a.enqueuedAt < b.enqueuedAt;
                   });
  pump();
}

void Scheduler::finish(const std::string &jobId) {
  running_.erase(std::remove(running_.begin(), running_.end(), jobId),
                 running_.end());
  queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                              [&](const QueueEntry &e) {
                                return e.jobId == jobId;
                              }),
               queue_.end());
  suspended_.erase(std::remove(suspended_.begin(), suspended_.end(), jobId),
                   suspended_.end());
  pump();
}

void Scheduler::suspend(const std::string &jobId) {
  running_.erase(std::remove(running_.begin(), running_.end(), jobId),
                 running_.end());
  if (std::find(suspended_.begin(), suspended_.end(), jobId) ==
      suspended_.end()) {
    suspended_.push_back(jobId);
  }
  pump();
}

void Scheduler::resume(const std::string &jobId) {
  suspended_.erase(std::remove(suspended_.begin(), suspended_.end(), jobId),
                   suspended_.end());
  // Requeue at high priority so the user's explicit resume wins.
  submit(jobId, 9);
}

void Scheduler::pump() {
  while (running_.size() < std::size_t(policy_.maxConcurrentJobs) &&
         !queue_.empty()) {
    // Find the first non-suspended entry.
    auto next = queue_.begin();
    while (next != queue_.end() && isSuspended(next->jobId)) {
      ++next;
    }
    if (next == queue_.end()) {
      return;
    }
    const std::string jobId = next->jobId;
    queue_.erase(next);
    running_.push_back(jobId);
    if (runJob_) {
      runJob_(jobId);
    }
  }
}

std::vector<std::string> Scheduler::queuedJobIds() const {
  std::vector<std::string> ids;
  ids.reserve(queue_.size());
  for (const auto &entry : queue_) {
    ids.push_back(entry.jobId);
  }
  return ids;
}

bool Scheduler::isSuspended(const std::string &jobId) const {
  return std::find(suspended_.begin(), suspended_.end(), jobId) !=
         suspended_.end();
}

} // namespace velmux::core
