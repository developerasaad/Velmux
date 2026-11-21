// Scheduler unit tests: resource-aware concurrency, ordering, suspend/resume.
#include "velmux/core/scheduler.h"

#include <doctest/doctest.h>

using namespace velmux::core;

namespace {

struct Recorder {
  std::vector<std::string> started;
  Scheduler::RunJobFn run = [this](const std::string &jobId) {
    started.push_back(jobId);
  };
};

ResourceSnapshot desktopSnapshot() {
  ResourceSnapshot snapshot;
  snapshot.cpuCores = 8;
  snapshot.memoryAvailableMb = 8192;
  snapshot.thermalPressure = 0.0;
  snapshot.onBattery = false;
  snapshot.hardwareCodecAvailable = true;
  return snapshot;
}

} // namespace

TEST_CASE("policy derives conservative concurrency from resources") {
  SUBCASE("healthy desktop") {
    const SchedulerPolicy policy = Scheduler::policyFor(desktopSnapshot());
    CHECK(policy.maxConcurrentJobs >= 1);
    CHECK(policy.maxConcurrentJobs <= 4);
    CHECK(policy.maxConcurrentHardwareJobs >= 1);
  }
  SUBCASE("thermally constrained") {
    ResourceSnapshot hot = desktopSnapshot();
    hot.thermalPressure = 0.9;
    const SchedulerPolicy policy = Scheduler::policyFor(hot);
    CHECK(policy.maxConcurrentJobs == 1);
    CHECK(policy.maxConcurrentHardwareJobs == 1);
  }
  SUBCASE("low memory serializes") {
    ResourceSnapshot lowMem = desktopSnapshot();
    lowMem.memoryAvailableMb = 256;
    const SchedulerPolicy policy = Scheduler::policyFor(lowMem);
    CHECK(policy.maxConcurrentJobs == 1);
  }
  SUBCASE("on battery limits hardware concurrency") {
    ResourceSnapshot battery = desktopSnapshot();
    battery.onBattery = true;
    const SchedulerPolicy policy = Scheduler::policyFor(battery);
    CHECK(policy.maxConcurrentHardwareJobs == 1);
  }
  SUBCASE("single core still runs one job") {
    ResourceSnapshot tiny = desktopSnapshot();
    tiny.cpuCores = 1;
    const SchedulerPolicy policy = Scheduler::policyFor(tiny);
    CHECK(policy.maxConcurrentJobs == 1);
  }
}

TEST_CASE("submit starts jobs while capacity allows") {
  Recorder recorder;
  Scheduler scheduler(recorder.run);
  scheduler.updateResources(desktopSnapshot());

  scheduler.submit("a");
  scheduler.submit("b");
  // Two jobs, capacity >= 2 on a healthy desktop: both run immediately.
  const auto capacity =
      std::size_t(std::max(scheduler.policy().maxConcurrentJobs, 2));
  CHECK(scheduler.runningCount() == std::min<std::size_t>(2, capacity));
  CHECK(scheduler.queuedCount() + scheduler.runningCount() == 2);
  CHECK(recorder.started.size() == scheduler.runningCount());
}

TEST_CASE("finish frees a slot and starts the next queued job") {
  Recorder recorder;
  Scheduler scheduler(recorder.run);
  ResourceSnapshot single = desktopSnapshot();
  single.cpuCores = 2; // -> 1 concurrent job
  scheduler.updateResources(single);

  scheduler.submit("a");
  scheduler.submit("b");
  REQUIRE(recorder.started.size() == 1);
  REQUIRE(recorder.started.front() == "a");

  scheduler.finish("a");
  CHECK(recorder.started.size() == 2);
  CHECK(recorder.started[1] == "b");
  CHECK(scheduler.runningCount() == 1);
}

TEST_CASE("higher priority jobs run first within a capacity batch") {
  Recorder recorder;
  Scheduler scheduler(recorder.run);
  // Queue everything while no capacity is available.
  scheduler.setPolicy(SchedulerPolicy{0, 0, true});
  scheduler.submit("low", 1);
  scheduler.submit("high", 9);
  CHECK(recorder.started.empty());

  // Grant one slot: the high-priority job starts, low stays queued.
  scheduler.setPolicy(SchedulerPolicy{1, 1, true});
  scheduler.pump();
  REQUIRE(recorder.started.size() == 1);
  CHECK(recorder.started.front() == "high");
  CHECK(scheduler.queuedJobIds().front() == "low");
}

TEST_CASE("suspend and resume keep the job alive without running it") {
  Recorder recorder;
  Scheduler scheduler(recorder.run);
  scheduler.updateResources(desktopSnapshot());
  scheduler.setPolicy(SchedulerPolicy{1, 1, true});

  scheduler.submit("a");
  REQUIRE(scheduler.runningCount() == 1);
  scheduler.suspend("a");
  CHECK(scheduler.runningCount() == 0);

  // Pump does not restart a suspended job.
  scheduler.pump();
  CHECK(scheduler.runningCount() == 0);

  scheduler.resume("a");
  CHECK(scheduler.runningCount() == 1);
}

TEST_CASE("submit is idempotent per job") {
  Recorder recorder;
  Scheduler scheduler(recorder.run);
  scheduler.updateResources(desktopSnapshot());
  scheduler.setPolicy(SchedulerPolicy{1, 1, true});

  scheduler.submit("a");
  scheduler.submit("a");
  scheduler.submit("a");
  CHECK(recorder.started.size() == 1);
  CHECK(scheduler.runningCount() + scheduler.queuedCount() == 1);
}
