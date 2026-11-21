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
      [engineRaw = engine.get()](const std::string &jobId) {
        // Each job runs on its own worker thread; the scheduler bounds
        // concurrency. UI threads are never blocked (spec §4.1), and the
        // engine joins every worker at shutdown.
        std::thread worker([engineRaw, jobId] {
          engineRaw->jobs_->executeJob(jobId);
        });
        std::lock_guard<std::mutex> lock(engineRaw->workersMutex_);
        engineRaw->workers_.push_back(std::move(worker));
      });
  engine->scheduler_->updateResources(initial);

  // Crash recovery: reconcile DB + artifacts before serving traffic.
  (void)engine->jobs_->recoverInterruptedJobs();

  return engine;
}

Result<MediaInfo> VelmuxEngine::inspect(const MediaSource &source) {
  return jobs_->inspect(source);
}

Result<PlanResult> VelmuxEngine::plan(const MediaSource &source,
                                      const ConversionRequest &request) {
  return jobs_->plan(source, request);
}

Result<std::string> VelmuxEngine::startJob(const ConversionRequest &request) {
  const Result<std::string> submitted = jobs_->submit(request);
  if (!submitted.ok()) {
    return submitted;
  }
  scheduler_->submit(submitted.value());
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

std::optional<JobSnapshot> VelmuxEngine::getJob(const std::string &jobId) {
  return jobs_->snapshot(jobId);
}

std::vector<JobSnapshot> VelmuxEngine::listJobs() { return jobs_->snapshots(); }

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
