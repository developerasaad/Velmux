// Scheduler unit tests: resource-aware concurrency, ordering, suspend/resume.
#include "velmux/core/scheduler.h"

#include <doctest/doctest.h>

using namespace velmux::core;

namespace {

struct Recorder {
  std::vector<std::string> started;
  std::vector<std::uint64_t> startedGenerations;
  Scheduler::RunJobFn run = [this](const std::string &jobId,
                                  std::uint64_t generation) {
    started.push_back(jobId);
    startedGenerations.push_back(generation);
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

TEST_CASE(
    "runFinished frees the slot on natural completion (the slot-leak fix)") {
  Recorder recorder;
  Scheduler scheduler(recorder.run);
  ResourceSnapshot single = desktopSnapshot();
  single.cpuCores = 2; // -> 1 concurrent job
  scheduler.updateResources(single);

  scheduler.submit("a");
  scheduler.submit("b");
  scheduler.submit("c");
  REQUIRE(recorder.started.size() == 1);
  REQUIRE(recorder.started.front() == "a");

  // The worker for "a" completes naturally: the slot must free without an
  // explicit finish() (which the engine only issues on cancel).
  scheduler.runFinished("a", recorder.startedGenerations[0]);
  CHECK(recorder.started.size() == 2);
  CHECK(recorder.started[1] == "b");

  scheduler.runFinished("b", recorder.startedGenerations[1]);
  CHECK(recorder.started.size() == 3);
  CHECK(recorder.started[2] == "c");
  CHECK(scheduler.queuedCount() == 0);
  // "c" was started and its (simulated) worker has not reported yet.
  CHECK(scheduler.runningCount() == 1);
  scheduler.runFinished("c", recorder.startedGenerations[2]);
  CHECK(scheduler.runningCount() == 0);
}

TEST_CASE("a stale generation must not free or consume the newer slot") {
  Recorder recorder;
  Scheduler scheduler(recorder.run);
  scheduler.updateResources(desktopSnapshot());
  scheduler.setPolicy(SchedulerPolicy{1, 1, true});

  scheduler.submit("a");
  REQUIRE(recorder.started.size() == 1);
  const std::uint64_t staleGeneration = recorder.startedGenerations[0];

  // Cancel + retry while the old worker is still unwinding.
  scheduler.finish("a");
  scheduler.submit("a", 9); // new submission, new generation, runs now
  REQUIRE(recorder.started.size() == 2);
  REQUIRE(scheduler.runningCount() == 1);

  // The old worker finally reports completion: it must not erase the newer
  // run's slot (this used to kill the re-queued job's queue entry).
  scheduler.runFinished("a", staleGeneration);
  CHECK(scheduler.runningCount() == 1);

  // The current generation frees correctly.
  scheduler.runFinished("a", recorder.startedGenerations[1]);
  CHECK(scheduler.runningCount() == 0);
}

TEST_CASE("hardware cap gates hardware pipelines without blocking software") {
  Recorder recorder;
  Scheduler scheduler(recorder.run);
  scheduler.updateResources(desktopSnapshot());
  // Plenty of general capacity, exactly one hardware slot.
  scheduler.setPolicy(SchedulerPolicy{4, 1, true});

  scheduler.submit("hw-a");
  scheduler.noteHardwareIntent("hw-a");
  scheduler.submit("hw-b");
  scheduler.noteHardwareIntent("hw-b");
  scheduler.submit("sw-a");

  REQUIRE(recorder.started.size() == 3);
  REQUIRE(scheduler.runningCount() == 3);
  // hw-a occupies the single hardware slot once dispatch reports it started.
  scheduler.jobHardwareStarted("hw-a");
  CHECK_FALSE(scheduler.hardwareSlotAvailable());

  // Capacity still allows a software job.
  scheduler.submit("sw-b");
  CHECK(recorder.started.size() == 4);

  // Another hardware job stays queued while the cap is saturated.
  scheduler.submit("hw-c");
  scheduler.noteHardwareIntent("hw-c");
  CHECK(scheduler.queuedJobIds().front() == "hw-c");
  const std::size_t startedCount = recorder.started.size();
  scheduler.pump();
  CHECK(recorder.started.size() == startedCount); // no busy-spin start

  // The running hardware job finishes: the queued hardware job may start.
  const auto it = std::find(recorder.started.begin(), recorder.started.end(),
                            "hw-a");
  REQUIRE(it != recorder.started.end());
  const std::size_t hwAIndex = std::size_t(it - recorder.started.begin());
  scheduler.runFinished("hw-a", recorder.startedGenerations[hwAIndex]);
  CHECK(scheduler.hardwareSlotAvailable());
  CHECK(recorder.started.size() == startedCount + 1);
  CHECK(recorder.started.back() == "hw-c");
}

TEST_CASE("deferred jobs keep hardware intent so re-queueing cannot spin") {
  Recorder recorder;
  Scheduler scheduler(recorder.run);
  scheduler.updateResources(desktopSnapshot());
  scheduler.setPolicy(SchedulerPolicy{4, 1, true});

  scheduler.submit("hw-run");
  scheduler.noteHardwareIntent("hw-run");
  scheduler.jobHardwareStarted("hw-run");

  // First defer cycle: the job starts (no intent known at submit time),
  // its executeJob hits the saturated hardware gate, defers, and the
  // engine notes intent + re-queues after the worker unwinds.
  scheduler.submit("hw-deferred");
  REQUIRE(recorder.started.size() == 2); // started: no intent known yet
  scheduler.noteHardwareIntent("hw-deferred");
  scheduler.runFinished("hw-deferred", recorder.startedGenerations[1],
                        /*deferred=*/true);
  scheduler.submit("hw-deferred");
  // Second cycle: intent known -> stays queued while the cap is saturated.
  CHECK(scheduler.queuedCount() == 1);
  CHECK(scheduler.runningCount() == 1);
  const std::size_t startedCount = recorder.started.size();
  scheduler.pump();
  CHECK(recorder.started.size() == startedCount); // no busy-spin

  // Hardware slot frees: the re-queued deferred job starts.
  scheduler.runFinished("hw-run", recorder.startedGenerations[0]);
  CHECK(scheduler.runningCount() == 1);
  CHECK(recorder.started.back() == "hw-deferred");
}
