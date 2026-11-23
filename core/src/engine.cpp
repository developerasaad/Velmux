#include <filesystem>
#include "velmux/core/engine.h"

#include "velmux/backend/contracts.h"
#include "velmux/core/compression.h"
#include "velmux/core/planner.h"
#include "velmux/core/recovery.h"
#include "velmux/core/telemetry.h"
#include "velmux/core/validation.h"

#include <sys/stat.h>
#include <thread>
#include <vector>

namespace velmux::core {

namespace {

bool ensureDir(const std::string &path) {
  return ::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

} // namespace

VelmuxEngine::VelmuxEngine(EngineConfig config)
    : config_(std::move(config)), features_(FeatureRegistry::instance()) {}

VelmuxEngine::~VelmuxEngine() {
  // Lifecycle contract: workers are joined before any member tears down.
  // Running jobs get a cooperative cancel so joins complete quickly; the
  // durable records keep their committed checkpoints for recovery.
  if (jobs_ != nullptr) {
    jobs_->cancelAllRunning("engine shutdown");
  }
  {
    std::lock_guard<std::mutex> lock(workersMutex_);
    for (std::thread &worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    workers_.clear();
  }
}

std::unique_ptr<VelmuxEngine> VelmuxEngine::create(
    const EngineConfig &config,
    std::vector<std::unique_ptr<backend::IBackend>> backends) {
  if (config.dataDir.empty()) {
    return nullptr;
  }
  if (!ensureDir(config.dataDir) || !ensureDir(config.dataDir + "/outputs") ||
      !ensureDir(config.dataDir + "/artifacts")) {
    return nullptr;
  }

  auto engine = std::unique_ptr<VelmuxEngine>(new VelmuxEngine(config));

  // Durable stores.
  engine->db_ = std::make_unique<SqliteStore>();
  const Error openError =
      engine->db_->open(config.dataDir + "/velmux-jobs.sqlite");
  if (!openError.ok()) {
    return nullptr;
  }
  engine->jobStore_ = std::make_unique<JobStore>(*engine->db_);
  engine->checkpointStore_ = std::make_unique<CheckpointStore>(*engine->db_);
  engine->telemetry_ = std::make_unique<TelemetryStore>(*engine->db_);

  // Planner + job machinery.
  engine->planner_ = std::make_unique<Planner>(engine->features_,
                                               engine->capabilities_,
                                               engine->telemetry_.get());
  engine->jobs_ = std::make_unique<JobManager>(
      config, engine->events_, *engine->jobStore_, *engine->checkpointStore_,
      *engine->planner_, *engine->telemetry_);

  // Backends: initialize once here (expensive capability enumeration happens
  // at startup, never on the user-visible conversion path), then register
  // their real capabilities.
  std::vector<backend::IBackend *> raw;
  for (auto &backend : backends) {
    const Error initError = backend->initialize();
    if (!initError.ok()) {
      // A backend that fails to initialize is not registered; the engine
      // continues with what works (failure isolation, spec §21/§36).
      continue;
    }
    engine->capabilities_.registerBackend(backend->capabilities());
    raw.push_back(backend.get());
  }
  engine->jobs_->setBackends(raw);
  engine->backends_ = std::move(backends);

  // Validator wired to the first backend with an inspector.
  for (backend::IBackend *backend : raw) {
    if (backend->inspector() != nullptr) {
      engine->validator_ = std::make_unique<OutputValidator>(*backend->inspector());
      break;
    }
  }
  engine->jobs_->setValidator(engine->validator_.get());

  // Scheduler: executes jobs on worker threads OWNED by this engine (so
  // shutdown can join them), honoring resource limits.
  ResourceSnapshot initial;
  initial.cpuCores = std::max<std::int32_t>(
      1, std::int32_t(std::thread::hardware_concurrency()));
  engine->scheduler_ = std::make_unique<Scheduler>(
      [engineRaw = engine.get()](const std::string &jobId,
                                 std::uint64_t generation) {
        // Each job runs on its own worker thread; the scheduler bounds
        // concurrency. UI threads are never blocked (spec §4.1), and the
        // engine joins every worker at shutdown.
        std::thread worker([engineRaw, jobId, generation] {
          const JobManager::ExecuteOutcome outcome =
              engineRaw->jobs_->executeJob(jobId);
          // Release the slot BEFORE re-queueing a deferred job: submit()
          // is a no-op while the job still counts as running.
          engineRaw->scheduler_->runFinished(
              jobId, generation,
              outcome == JobManager::ExecuteOutcome::Deferred);
          if (outcome == JobManager::ExecuteOutcome::Deferred) {
            engineRaw->scheduler_->submit(jobId);
          }
        });
        std::lock_guard<std::mutex> lock(engineRaw->workersMutex_);
        engineRaw->workers_.push_back(std::move(worker));
      });
  // Hardware-codec scarcity gate (spec §21): the scheduler defers hardware
  // pipelines while the hardware cap is saturated, and accounts running
  // hardware jobs so the cap is real (it was dead policy before).
  engine->jobs_->setDispatchHooks(
      [engineRaw = engine.get()](const std::string &jobId,
                                 const Pipeline &pipeline) {
        if (!pipeline.usesHardware) {
          return true;
        }
        if (engineRaw->scheduler_->hardwareSlotAvailable()) {
          return true;
        }
        engineRaw->scheduler_->noteHardwareIntent(jobId);
        return false;
      },
      [engineRaw = engine.get()](const std::string &jobId) {
        engineRaw->scheduler_->jobHardwareStarted(jobId);
      });
  engine->scheduler_->updateResources(initial);

  // Crash recovery: reconcile DB + artifacts before serving traffic. Jobs
  // that recovery re-queued are handed to the scheduler so they actually
  // resume (a durable Queued record alone would never execute).
  const std::vector<RecoveryDecision> decisions =
      engine->jobs_->recoverInterruptedJobs();
  for (const RecoveryDecision &decision : decisions) {
    if (decision.action == RecoveryAction::RestartJob ||
        decision.action == RecoveryAction::ResumeFromCheckpoint) {
      engine->scheduler_->submit(decision.jobId);
    }
  }

  return engine;
}

Result<MediaInfo> VelmuxEngine::inspect(const MediaSource &source) {
  return jobs_->inspect(source);
}

Result<PlanResult> VelmuxEngine::plan(const MediaSource &source,
                                      const ConversionRequest &request) {
  return jobs_->plan(source, request);
}

Result<SampleResult> VelmuxEngine::generateSample(
    const ConversionRequest &request, std::int32_t sampleSeconds) {
  if (sampleSeconds <= 0) {
    return Result<SampleResult>(Error::make(ErrorCode::InvalidArgument,
                                            "sample duration must be positive",
                                            "core"));
  }
  // Samples live under <dataDir>/samples with a sanitized, collision-safe
  // name; overwriting a previous sample of the same source is intended
  // (they are disposable previews).
  std::filesystem::path dir = std::filesystem::path(config_.dataDir) / "samples";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::string stem = request.source.displayName;
  const std::size_t dot = stem.rfind('.');
  if (dot != std::string::npos && dot > 0) {
    stem = stem.substr(0, dot);
  }
  std::string safe;
  for (char c : stem) {
    const bool okChar = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
    safe.push_back(okChar ? c : '_');
  }
  if (safe.empty()) {
    safe = "sample";
  }
  const char *ext = "mp4";
  switch (request.outputContainer.value_or(Container::Mp4)) {
    case Container::Mkv:
    case Container::WebM:
      ext = "mkv";
      if (request.outputContainer.value_or(Container::Mp4) ==
          Container::WebM) {
        ext = "webm";
      }
      break;
    case Container::Mov:
      ext = "mov";
      break;
    case Container::M4a:
      ext = "m4a";
      break;
    case Container::Mp3:
      ext = "mp3";
      break;
    case Container::Ogg:
      ext = "ogg";
      break;
    case Container::Wav:
      ext = "wav";
      break;
    case Container::Flac:
      ext = "flac";
      break;
    default:
      break;
  }
  const std::string outPath =
      (dir / (safe + "-sample." + ext)).string();
  return jobs_->generateSample(request, sampleSeconds, outPath);
}

Result<std::string> VelmuxEngine::startJob(const ConversionRequest &request,
                                        std::int32_t priority) {
  const Result<std::string> submitted = jobs_->submit(request, priority);
  if (!submitted.ok()) {
    return submitted;
  }
  scheduler_->submit(submitted.value(), priority);
  return submitted;
}

Result<bool> VelmuxEngine::cancelJob(const std::string &jobId) {
  const Result<void> result = jobs_->cancel(jobId);
  if (!result.ok()) {
    return Result<bool>(result.error());
  }
  scheduler_->finish(jobId);
  return Result<bool>(true);
}

Result<bool> VelmuxEngine::pauseJob(const std::string &jobId) {
  const Result<void> result = jobs_->pause(jobId);
  if (!result.ok()) {
    return Result<bool>(result.error());
  }
  scheduler_->suspend(jobId);
  return Result<bool>(true);
}

Result<bool> VelmuxEngine::resumeJob(const std::string &jobId) {
  const Result<void> result = jobs_->resume(jobId);
  if (!result.ok()) {
    return Result<bool>(result.error());
  }
  scheduler_->resume(jobId);
  return Result<bool>(true);
}

Result<bool> VelmuxEngine::retryJob(const std::string &jobId) {
  const Result<void> result = jobs_->retry(jobId);
  if (!result.ok()) {
    return Result<bool>(result.error());
  }
  // The durable record is Queued again; hand it to the scheduler at high
  // priority so an explicit retry wins the queue.
  scheduler_->submit(jobId, 9);
  return Result<bool>(true);
}

Result<bool> VelmuxEngine::deleteJob(const std::string &jobId) {
  const Result<void> result = jobs_->remove(jobId);
  if (!result.ok()) {
    return Result<bool>(result.error());
  }
  return Result<bool>(true);
}

std::optional<JobSnapshot> VelmuxEngine::getJob(const std::string &jobId) {
  return jobs_->snapshot(jobId);
}

std::vector<JobSnapshot> VelmuxEngine::listJobs() { return jobs_->snapshots(); }

bool VelmuxEngine::isJobActive(const std::string &jobId) {
  return jobs_->isActive(jobId);
}

std::vector<std::string> VelmuxEngine::describeBackends() {
  std::vector<std::string> descriptions;
  for (const auto &backend : backends_) {
    const BackendCapabilities *caps =
        capabilities_.capabilitiesOf(backend->id());
    if (caps == nullptr) {
      continue; // failed to initialize; not lying about availability
    }
    std::string description = backend->name() + " [" +
                              caps->implementation + "] video:" +
                              std::to_string(caps->video.size()) +
                              " audio:" + std::to_string(caps->audio.size());
    description += caps->canInspect ? " inspect:yes" : " inspect:no";
    description += "]";
    descriptions.push_back(std::move(description));
  }
  return descriptions;
}

void VelmuxEngine::updateResources(const ResourceSnapshot &snapshot) {
  scheduler_->updateResources(snapshot);
}

EventBus &VelmuxEngine::events() { return events_; }

} // namespace velmux::core
