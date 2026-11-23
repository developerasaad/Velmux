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
  std::lock_guard<std::mutex> lock(mutex_);
  resources_ = snapshot;
  policy_ = policyFor(snapshot);
  pump();
}

void Scheduler::submit(const std::string &jobId, std::int32_t priority) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto existing = std::find_if(queue_.begin(), queue_.end(),
                                     [&](const QueueEntry &e) {
                                       return e.jobId == jobId;
                                     });
  if (existing != queue_.end() || isSuspended(jobId)) {
    return; // idempotent
  }
  if (running_.count(jobId) != 0) {
    return;
  }
  queue_.push_back(QueueEntry{jobId, priority, std::chrono::steady_clock::now(),
                              ++nextGeneration_});
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

void Scheduler::eraseRunningLocked(const std::string &jobId) {
  const auto it = running_.find(jobId);
  if (it == running_.end()) {
    return;
  }
  if (it->second.hardware && hardwareRunningCount_ > 0) {
    --hardwareRunningCount_;
  }
  running_.erase(it);
}

void Scheduler::finish(const std::string &jobId) {
  std::lock_guard<std::mutex> lock(mutex_);
  eraseRunningLocked(jobId);
  queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                              [&](const QueueEntry &e) {
                                return e.jobId == jobId;
                              }),
               queue_.end());
  suspended_.erase(std::remove(suspended_.begin(), suspended_.end(), jobId),
                   suspended_.end());
  hardwareIntent_.erase(jobId);
  pump();
}

void Scheduler::suspend(const std::string &jobId) {
  std::lock_guard<std::mutex> lock(mutex_);
  // The slot frees immediately (the control path flipped the durable state
  // to Paused); the still-unwinding worker's runFinished() becomes a no-op
  // generation mismatch.
  eraseRunningLocked(jobId);
  if (std::find(suspended_.begin(), suspended_.end(), jobId) ==
      suspended_.end()) {
    suspended_.push_back(jobId);
  }
  pump();
}

void Scheduler::resume(const std::string &jobId) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    suspended_.erase(std::remove(suspended_.begin(), suspended_.end(), jobId),
                     suspended_.end());
  }
  // Requeue at high priority so the user's explicit resume wins. Called
  // outside the lock: submit() takes it.
  submit(jobId, 9);
}

void Scheduler::runFinished(const std::string &jobId,
                            std::uint64_t generation, bool deferred) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = running_.find(jobId);
  if (it != running_.end() && it->second.generation == generation) {
    eraseRunningLocked(jobId);
    if (!deferred) {
      // The job reached a terminal state; its hardware intent is obsolete.
      // Deferred jobs keep it: that is exactly what stops a saturated
      // hardware cap from busy-spinning the re-queued job.
      hardwareIntent_.erase(jobId);
    }
  }
  pump();
}

void Scheduler::noteHardwareIntent(const std::string &jobId) {
  std::lock_guard<std::mutex> lock(mutex_);
  hardwareIntent_.insert(jobId);
}

bool Scheduler::hardwareSlotAvailable() const {
  std::lock_guard<std::mutex> lock(mutex_);
  // A cap of 0 (no hardware codec reported in the resource snapshot) must
  // not starve the queue forever: degrade to 1 and let an oversubscribed
  // codec fail honestly at execution time.
  const std::size_t cap = std::max<std::int32_t>(
      policy_.maxConcurrentHardwareJobs, 1);
  return hardwareRunningCount_ < cap;
}

void Scheduler::jobHardwareStarted(const std::string &jobId) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = running_.find(jobId);
  if (it == running_.end() || it->second.hardware) {
    return; // superseded run, or already counted
  }
  it->second.hardware = true;
  ++hardwareRunningCount_;
  hardwareIntent_.insert(jobId);
}

void Scheduler::pump() {
  // Callers hold mutex_.
  while (running_.size() < std::size_t(policy_.maxConcurrentJobs) &&
         !queue_.empty()) {
    const std::size_t hwCap = std::max<std::int32_t>(
        policy_.maxConcurrentHardwareJobs, 1);
    // Find the first startable entry: not suspended, and hardware pipelines
    // only while hardware capacity allows (spec §21: scarcity accounted).
    auto next = queue_.begin();
    while (next != queue_.end() &&
           (isSuspended(next->jobId) ||
            (hardwareIntent_.count(next->jobId) != 0 &&
             hardwareRunningCount_ >= hwCap))) {
      ++next;
    }
    if (next == queue_.end()) {
      return;
    }
    QueueEntry entry = std::move(*next);
    queue_.erase(next);
    const std::string jobId = entry.jobId;
    running_[jobId] = RunningEntry{entry.generation, false};
    if (runJob_) {
      runJob_(jobId, entry.generation);
    }
  }
}

std::size_t Scheduler::queuedCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size();
}

std::size_t Scheduler::runningCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return running_.size();
}

std::vector<std::string> Scheduler::queuedJobIds() const {
  std::lock_guard<std::mutex> lock(mutex_);
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
