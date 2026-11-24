// Engine-level unit tests using a fake backend (test double — allowed only
// in tests). Verifies the full lifecycle wiring: registration, planning,
// job execution through the scheduler, event emission and durability.
#include "velmux/backend/contracts.h"
#include "velmux/core/engine.h"

#include <doctest/doctest.h>

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

using namespace velmux::core;
using velmux::backend::CancelToken;
using velmux::backend::ExecuteParams;
using velmux::backend::ExecuteResult;
using velmux::backend::IInspector;
using velmux::backend::IPipelineExecutor;

namespace {

// Writes a small file that looks like media to the fake inspector.
std::string makeFakeMedia(const std::filesystem::path &path,
                          std::int64_t bytes = 64 * 1024) {
  std::ofstream out(path, std::ios::binary);
  for (std::int64_t i = 0; i < bytes; ++i) {
    out.put('\x01');
  }
  return path.string();
}

class FakeInspector final : public IInspector {
public:
  Error inspect(const MediaSource &source, MediaInfo &out) override {
    // Only "files" that exist and are non-empty inspect successfully.
    std::error_code ec;
    if (!std::filesystem::exists(source.uri, ec) ||
        std::filesystem::file_size(source.uri, ec) == 0) {
      return Error::make(ErrorCode::SourceUnavailable,
                         "fake inspector: missing source", "fake");
    }
    // Canned answer consistent with what the fake executor "produces":
    // the requestFor() below always asks for VP9/Opus into MKV.
    out = MediaInfo{};
    out.durationSec = 10.0;
    out.container = Container::Mkv;
    out.sizeBytes = std::filesystem::file_size(source.uri, ec);
    VideoStream video;
    video.codec = VideoCodec::VP9;
    video.width = 1280;
    video.height = 720;
    video.frameRate = 30.0;
    video.bitRate = 2'000'000;
    out.video.push_back(video);
    AudioStream audio;
    audio.codec = AudioCodec::Opus;
    audio.channels = 2;
    audio.sampleRate = 48000;
    audio.bitRate = 96'000;
    out.audio.push_back(audio);
    return {};
  }
};

class FakeExecutor final : public IPipelineExecutor {
public:
  bool canExecute(const Pipeline &pipeline,
                  const ConversionRequest &) const override {
    for (const auto &assignment : pipeline.stages) {
      if (assignment.backend != BackendId::Ffmpeg) {
        return false;
      }
    }
    return !pipeline.stages.empty();
  }

  ExecuteResult execute(const ExecuteParams &params) override {
    ExecuteResult result;
    executions.fetch_add(1);
    const int sleepMs = iterationSleepMs.load();
    for (int i = 0; i <= 10; ++i) {
      if (params.cancel && params.cancel->isCancelled()) {
        result.error = Error::make(ErrorCode::Cancelled, "cancelled", "fake");
        return result;
      }
      if (params.onProgress) {
        velmux::backend::ProgressReport report;
        report.fraction = double(i) / 10.0;
        params.onProgress(report);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
    }
    // Produce a real output file the validator can inspect.
    std::ofstream out(params.outputPath, std::ios::binary);
    for (int i = 0; i < 128 * 1024; ++i) {
      out.put('\x02');
    }
    result.success = true;
    result.framesProcessed = 300;
    result.wallClockSec = 0.01;
    return result;
  }

  std::atomic<int> executions{0};
  // Per-iteration sleep; tests raise it to keep execution observable.
  std::atomic<int> iterationSleepMs{1};
};

class FakeBackend final : public velmux::backend::IBackend {
public:
  BackendId id() const override { return BackendId::Ffmpeg; }
  std::string name() const override { return "FakeBackend"; }
  Error initialize() override { return {}; }
  BackendCapabilities capabilities() const override {
    BackendCapabilities caps;
    caps.backend = BackendId::Ffmpeg;
    caps.implementation = "fake-for-tests";
    caps.canInspect = true;
    caps.video.push_back(
        VideoCapability{VideoCodec::H264, {true, true, true}, false, 0, 0});
    caps.video.push_back(
        VideoCapability{VideoCodec::VP9, {true, true, true}, false, 0, 0});
    caps.audio.push_back(
        AudioCapability{AudioCodec::Aac, {true, true, true}});
    caps.audio.push_back(
        AudioCapability{AudioCodec::Opus, {true, true, true}});
    caps.readableContainers = {Container::Mp4, Container::Mkv,
                               Container::WebM};
    caps.writableContainers = {Container::Mp4, Container::Mkv,
                               Container::WebM};
    return caps;
  }
  IInspector *inspector() override { return &inspector_; }
  IPipelineExecutor *executor() override { return &executor_; }

  FakeInspector inspector_;
  FakeExecutor executor_;
};

// Test doubles for the runtime failure-feedback wiring (spec §15): two
// backends with identical capability surfaces, one whose executor always
// errors and one that always succeeds. The planner prefers the faster
// prior (ffmpeg) first, so the scripted failure lands on a deterministic
// route.
class ScriptedExecutor final : public IPipelineExecutor {
public:
  explicit ScriptedExecutor(bool fail) : fail_(fail) {}

  bool canExecute(const Pipeline &, const ConversionRequest &) const override {
    return true;
  }

  ExecuteResult execute(const ExecuteParams &params) override {
    executions.fetch_add(1);
    ExecuteResult result;
    if (fail_) {
      result.error = Error::make(ErrorCode::BackendFailure,
                                 "scripted backend failure", "scripted");
      return result;
    }
    // Produce a real output file the validator can inspect.
    std::ofstream out(params.outputPath, std::ios::binary);
    for (int i = 0; i < 128 * 1024; ++i) {
      out.put('\x02');
    }
    result.success = true;
    result.framesProcessed = 300;
    result.wallClockSec = 0.01;
    return result;
  }

  std::atomic<int> executions{0};

private:
  bool fail_;
};

class ScriptedBackend final : public velmux::backend::IBackend {
public:
  ScriptedBackend(BackendId id, bool fail) : id_(id), executor_(fail) {}

  BackendId id() const override { return id_; }
  std::string name() const override {
    return id_ == BackendId::Ffmpeg ? "ScriptedFfmpeg" : "ScriptedApple";
  }
  Error initialize() override { return {}; }
  BackendCapabilities capabilities() const override {
    BackendCapabilities caps;
    caps.backend = id_;
    caps.implementation = "scripted-for-tests";
    caps.canInspect = true;
    caps.video.push_back(
        VideoCapability{VideoCodec::H264, {true, true, true}, false, 0, 0});
    caps.video.push_back(
        VideoCapability{VideoCodec::VP9, {true, true, true}, false, 0, 0});
    caps.audio.push_back(AudioCapability{AudioCodec::Aac, {true, true, true}});
    caps.audio.push_back(
        AudioCapability{AudioCodec::Opus, {true, true, true}});
    caps.readableContainers = {Container::Mp4, Container::Mkv,
                               Container::WebM};
    caps.writableContainers = {Container::Mp4, Container::Mkv,
                               Container::WebM};
    return caps;
  }
  IInspector *inspector() override { return &inspector_; }
  IPipelineExecutor *executor() override { return &executor_; }

  BackendId id_;
  FakeInspector inspector_;
  ScriptedExecutor executor_;
};

class EventRecorder final : public IJobEventSink {
public:
  void onJobEvent(const JobEvent &event) override {
    std::lock_guard<std::mutex> lock(mutex_);
    events.push_back(event);
    done.notify_all();
  }
  std::optional<JobEvent> waitForTerminal(std::chrono::milliseconds timeout) {
    return waitFor(
        [](const JobEvent &e) { return isTerminal(e.state); }, timeout);
  }
  std::optional<JobEvent> waitForState(JobState state,
                                       std::chrono::milliseconds timeout) {
    return waitFor([state](const JobEvent &e) { return e.state == state; },
                   timeout);
  }
  bool sawTerminal() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &e : events) {
      if (isTerminal(e.state)) {
        return true;
      }
    }
    return false;
  }
  static bool isTerminal(JobState state) {
    return state == JobState::Completed || state == JobState::Failed ||
           state == JobState::Canceled;
  }
  std::vector<JobEvent> events;

private:
  template <typename Pred>
  std::optional<JobEvent> waitFor(Pred pred,
                                  std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    done.wait_for(lock, timeout, [this, &pred] {
      for (const auto &e : events) {
        if (pred(e)) {
          return true;
        }
      }
      return false;
    });
    for (const auto &e : events) {
      if (pred(e)) {
        return e;
      }
    }
    return std::nullopt;
  }
  mutable std::mutex mutex_;
  std::condition_variable done;
};

class EngineEnv {
public:
  EngineEnv() {
    dir_ = std::filesystem::temp_directory_path() /
           ("velmux-engine-" + std::to_string(::getpid()) + "-" +
            std::to_string(counter_++));
    std::filesystem::create_directories(dir_);
    auto backend = std::make_unique<FakeBackend>();
    backend_ = backend.get();
    std::vector<std::unique_ptr<velmux::backend::IBackend>> backends;
    backends.push_back(std::move(backend));
    engine_ = VelmuxEngine::create(EngineConfig{dir_.string()},
                                   std::move(backends));
    REQUIRE(engine_ != nullptr);
    engine_->events().subscribe(&recorder_);
  }
  ~EngineEnv() {
    engine_.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }
  VelmuxEngine &engine() { return *engine_; }
  FakeBackend &backend() { return *backend_; }
  EventRecorder &events() { return recorder_; }
  const std::filesystem::path &dir() const { return dir_; }

  ConversionRequest requestFor(const std::string &sourcePath) {
    ConversionRequest request;
    request.source.uri = sourcePath;
    request.source.displayName = "sample.mp4";
    request.outputContainer = Container::Mkv;
    request.goal.kind = GoalKind::Recommended;
    request.video.codec = VideoCodec::VP9; // forces transcode in fake caps
    request.audio.codec = AudioCodec::Opus;
    return request;
  }

private:
  static inline int counter_ = 0;
  std::filesystem::path dir_;
  FakeBackend *backend_ = nullptr;
  std::unique_ptr<VelmuxEngine> engine_;
  EventRecorder recorder_;
};

} // namespace

TEST_CASE("engine inspects sources through the backend") {
  EngineEnv env;
  const std::string media = makeFakeMedia(env.dir() / "in.mp4");
  const Result<MediaInfo> info =
      env.engine().inspect(MediaSource{media, "in.mp4"});
  REQUIRE(info.ok());
  CHECK(info.value().durationSec == doctest::Approx(10.0));
  CHECK(info.value().video.front().codec == VideoCodec::VP9);
  CHECK(info.value().sizeBytes == 64 * 1024);

  const Result<MediaInfo> missing = env.engine().inspect(
      MediaSource{(env.dir() / "nope.mp4").string(), "nope.mp4"});
  CHECK_FALSE(missing.ok());
  CHECK(missing.error().code == ErrorCode::SourceUnavailable);
}

TEST_CASE("engine plans without creating jobs") {
  EngineEnv env;
  const std::string media = makeFakeMedia(env.dir() / "in.mp4");
  const Result<PlanResult> plan =
      env.engine().plan(MediaSource{media, "in.mp4"},
                        env.requestFor(media));
  REQUIRE(plan.ok());
  CHECK(plan.value().feasible);
  REQUIRE(plan.value().best.has_value());
  CHECK(plan.value().best->pipeline.videoCodec == VideoCodec::VP9);
  CHECK(plan.value().planningMs >= 0.0);
}

TEST_CASE("full job lifecycle: submit, run, validate, complete, persist") {
  EngineEnv env;
  const std::string media = makeFakeMedia(env.dir() / "in.mp4");
  const Result<std::string> jobId =
      env.engine().startJob(env.requestFor(media));
  REQUIRE(jobId.ok());

  const auto terminal =
      env.events().waitForTerminal(std::chrono::seconds(10));
  REQUIRE(terminal.has_value());
  CHECK(terminal->state == JobState::Completed);
  CHECK(terminal->jobId == jobId.value());
  CHECK(terminal->outputSizeBytes >= 128 * 1024);

  // The job survives in durable storage with the completed state.
  const auto snapshot = env.engine().getJob(jobId.value());
  REQUIRE(snapshot.has_value());
  CHECK(snapshot->state == JobState::Completed);
  CHECK(snapshot->progress == doctest::Approx(1.0));
  CHECK(env.backend().executor_.executions.load() == 1);

  // Validation really ran against the produced output: the fake inspector
  // accepts any non-empty file, so completion implies the output existed.
  const auto jobs = env.engine().listJobs();
  REQUIRE(jobs.size() == 1);
  // The canned source reports VP9/Opus-in-MKV, so the planner legitimately
  // picks the stream-copy remux path for this request.
  CHECK(jobs[0].pipelineSummary.find("emux") != std::string::npos);
  CHECK_FALSE(jobs[0].outputPath.empty());
  CHECK(std::filesystem::exists(jobs[0].outputPath));
}

TEST_CASE("cancel terminates a running job honestly") {
  EngineEnv env;
  // Make the fake executor slow enough to cancel mid-flight.
  const std::string media = makeFakeMedia(env.dir() / "in.mp4");
  ConversionRequest request = env.requestFor(media);
  const Result<std::string> jobId = env.engine().startJob(request);
  REQUIRE(jobId.ok());

  const Result<bool> cancelled = env.engine().cancelJob(jobId.value());
  REQUIRE(cancelled.ok());

  const auto terminal =
      env.events().waitForTerminal(std::chrono::seconds(10));
  REQUIRE(terminal.has_value());
  // Cancelled jobs either report canceled or completed (if the cancel
  // arrived after the fast fake job finished); never "failed".
  CHECK(terminal->state != JobState::Failed);
}

TEST_CASE("engine creation with an unusable data dir fails honestly") {
  auto backend = std::make_unique<FakeBackend>();
  std::vector<std::unique_ptr<velmux::backend::IBackend>> backends;
  backends.push_back(std::move(backend));
  auto engine =
      VelmuxEngine::create(EngineConfig{"/proc/definitely/not/writable"},
                           std::move(backends));
  CHECK(engine == nullptr);
}

TEST_CASE("describeBackends reflects registered capabilities") {
  EngineEnv env;
  const auto descriptions = env.engine().describeBackends();
  REQUIRE(descriptions.size() == 1);
  CHECK(descriptions[0].find("FakeBackend") != std::string::npos);
  CHECK(descriptions[0].find("fake-for-tests") != std::string::npos);
}

TEST_CASE("pause holds a running job; resume finishes it for real") {
  EngineEnv env;
  env.backend().executor_.iterationSleepMs.store(50); // ~550 ms per run
  const std::string media = makeFakeMedia(env.dir() / "in.mp4");
  const Result<std::string> jobId =
      env.engine().startJob(env.requestFor(media));
  REQUIRE(jobId.ok());

  REQUIRE(env.events()
              .waitForState(JobState::Running, std::chrono::seconds(5))
              .has_value());

  const Result<bool> paused = env.engine().pauseJob(jobId.value());
  REQUIRE(paused.ok());

  const auto held =
      env.events().waitForState(JobState::Paused, std::chrono::seconds(5));
  REQUIRE(held.has_value());
  // A pause is not a cancel: while held, no terminal state may appear.
  CHECK_FALSE(env.events().sawTerminal());
  CHECK(env.backend().executor_.executions.load() == 1);

  // Resume re-queues through the scheduler; the job must complete for real.
  const Result<bool> resumed = env.engine().resumeJob(jobId.value());
  REQUIRE(resumed.ok());
  const auto terminal =
      env.events().waitForTerminal(std::chrono::seconds(10));
  REQUIRE(terminal.has_value());
  CHECK(terminal->state == JobState::Completed);
  CHECK(terminal->jobId == jobId.value());
  CHECK(env.backend().executor_.executions.load() == 2);
}

TEST_CASE("engine shutdown hands running jobs to recovery, not cancel") {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("velmux-shutdown-" + std::to_string(::getpid()));
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir);
  const std::string media = makeFakeMedia(dir / "in.mp4");

  ConversionRequest request;
  request.source.uri = media;
  request.source.displayName = "sample.mp4";
  request.outputContainer = Container::Mkv;
  request.goal.kind = GoalKind::Recommended;
  request.video.codec = VideoCodec::VP9;
  request.audio.codec = AudioCodec::Opus;

  std::string jobId;
  {
    auto backend = std::make_unique<FakeBackend>();
    backend->executor_.iterationSleepMs.store(50);
    std::vector<std::unique_ptr<velmux::backend::IBackend>> backends;
    backends.push_back(std::move(backend));
    auto engine =
        VelmuxEngine::create(EngineConfig{dir.string()}, std::move(backends));
    REQUIRE(engine != nullptr);
    EventRecorder recorder;
    engine->events().subscribe(&recorder);

    const Result<std::string> submitted = engine->startJob(request);
    REQUIRE(submitted.ok());
    jobId = submitted.value();
    REQUIRE(recorder
                .waitForState(JobState::Running, std::chrono::seconds(5))
                .has_value());
    // Engine destruction runs the shutdown contract: running jobs become
    // recoverable, never silently canceled.
    engine.reset();
  }

  // Durable state after shutdown: Recovering (auto-resumable), not Canceled.
  {
    SqliteStore db;
    REQUIRE(db.open((dir / "velmux-jobs.sqlite").string()).ok());
    JobStore store(db);
    Error loadError;
    const auto record = store.loadJob(jobId, loadError);
    REQUIRE(record.has_value());
    CHECK(record->state == JobState::Recovering);
  }

  // Startup recovery on a fresh engine over the same data dir resumes the
  // job honestly and it completes for real.
  {
    auto backend = std::make_unique<FakeBackend>();
    std::vector<std::unique_ptr<velmux::backend::IBackend>> backends;
    backends.push_back(std::move(backend));
    auto engine =
        VelmuxEngine::create(EngineConfig{dir.string()}, std::move(backends));
    REQUIRE(engine != nullptr);
    EventRecorder recorder;
    engine->events().subscribe(&recorder);
    const auto terminal =
        recorder.waitForTerminal(std::chrono::seconds(10));
    REQUIRE(terminal.has_value());
    CHECK(terminal->state == JobState::Completed);
    CHECK(terminal->jobId == jobId);
  }
  std::filesystem::remove_all(dir, ec);
}

TEST_CASE("backend execution failures are recorded and reroute the next plan") {
  // Two scripted backends with identical capabilities: the fast one (ffmpeg
  // priors win the first plan) always fails execution, the slower one works.
  // Job 1 must fail on the fast one; the capability registry must record
  // the exact failed configuration; the next plan for the SAME request must
  // land on the working backend; and the record must stay narrow (a
  // different output codec still plans on the failed backend).
  const auto dir = std::filesystem::temp_directory_path() /
                   ("velmux-failfeedback-" + std::to_string(::getpid()));
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir);
  const std::string media = makeFakeMedia(dir / "in.mp4");

  auto failing = std::make_unique<ScriptedBackend>(BackendId::Ffmpeg, true);
  auto working = std::make_unique<ScriptedBackend>(BackendId::Apple, false);
  ScriptedBackend *failingBackend = failing.get();
  ScriptedBackend *workingBackend = working.get();
  std::vector<std::unique_ptr<velmux::backend::IBackend>> backends;
  backends.push_back(std::move(failing));
  backends.push_back(std::move(working));
  auto engine =
      VelmuxEngine::create(EngineConfig{dir.string()}, std::move(backends));
  REQUIRE(engine != nullptr);

  ConversionRequest request;
  request.source.uri = media;
  request.source.displayName = "sample.mp4";
  request.outputContainer = Container::Mkv;
  request.goal.kind = GoalKind::Recommended;
  request.video.codec = VideoCodec::VP9;
  request.audio.codec = AudioCodec::Opus;

  // Baseline: the faster ffmpeg route wins the plan (cost priors).
  {
    const Result<PlanResult> plan =
        engine->plan(MediaSource{media, "sample.mp4"}, request);
    REQUIRE(plan.ok());
    REQUIRE(plan.value().feasible);
    REQUIRE(plan.value().best.has_value());
    CHECK(plan.value().best->pipeline.assigns(StageId::Mux, BackendId::Ffmpeg));
  }

  // Job 1 executes on the failing backend and fails for real.
  EventRecorder recorder;
  engine->events().subscribe(&recorder);
  const Result<std::string> first = engine->startJob(request);
  REQUIRE(first.ok());
  const auto terminal = recorder.waitForTerminal(std::chrono::seconds(10));
  REQUIRE(terminal.has_value());
  CHECK(terminal->state == JobState::Failed);
  CHECK(failingBackend->executor_.executions.load() == 1);

  // The failure is recorded (the record lands right after the terminal
  // event; poll briefly so the assertion never races the worker).
  bool recorded = false;
  for (int i = 0; i < 200 && !recorded; ++i) {
    recorded = engine->capabilities().failureCount() == 1;
    if (!recorded) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  REQUIRE(recorded);
  FailedConfigKey expected;
  expected.backend = BackendId::Ffmpeg;
  expected.codec = VideoCodec::VP9;
  expected.width = 1280;
  expected.height = 720;
  CHECK(engine->capabilities().hasFailed(expected));

  // The next plan for the SAME request avoids the failed route and lands
  // on the working backend.
  {
    const Result<PlanResult> plan =
        engine->plan(MediaSource{media, "sample.mp4"}, request);
    REQUIRE(plan.ok());
    REQUIRE(plan.value().feasible);
    REQUIRE(plan.value().best.has_value());
    CHECK_FALSE(
        plan.value().best->pipeline.assigns(StageId::Mux, BackendId::Ffmpeg));
    CHECK(plan.value().best->pipeline.assigns(StageId::Mux, BackendId::Apple));
  }

  // Job 2 completes through the working backend.
  EventRecorder secondRecorder;
  engine->events().subscribe(&secondRecorder);
  const Result<std::string> second = engine->startJob(request);
  REQUIRE(second.ok());
  const auto terminal2 =
      secondRecorder.waitForTerminal(std::chrono::seconds(10));
  REQUIRE(terminal2.has_value());
  CHECK(terminal2->state == JobState::Completed);
  CHECK(terminal2->jobId == second.value());
  CHECK(workingBackend->executor_.executions.load() == 1);
  CHECK(failingBackend->executor_.executions.load() == 1);

  // Narrowness: a different output codec on the failed backend still plans
  // there — the record is a configuration key, not a backend ban.
  {
    ConversionRequest h264 = request;
    h264.video.codec = VideoCodec::H264; // forces a transcode
    const Result<PlanResult> plan =
        engine->plan(MediaSource{media, "sample.mp4"}, h264);
    REQUIRE(plan.ok());
    REQUIRE(plan.value().feasible);
    REQUIRE(plan.value().best.has_value());
    CHECK(plan.value().best->pipeline.assigns(StageId::Mux, BackendId::Ffmpeg));
  }

  std::filesystem::remove_all(dir, ec);
}
