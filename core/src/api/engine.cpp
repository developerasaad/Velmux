#include <filesystem>
#include <velmux/engine.h>

#include "processing/contracts/contracts.h"
#include "compression/compression.h"
#include "configuration/sessions.h"
#include "planning/planner.h"
#include "recovery/recovery.h"
#include "persistence/telemetry.h"
#include "validation/validation.h"

#include <sys/stat.h>
#include <thread>
#include <vector>

namespace velmux::core {

namespace {

bool ensureDir(const std::string &path) {
  return ::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

// Every source a request references (Merge carries several).
std::vector<MediaSource> sourcesOfRequest(const JobRequest &request) {
  std::vector<MediaSource> sources;
  std::visit(
      [&](const auto &payload) {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, job::Merge>) {
          sources = payload.sources;
        } else {
          sources.push_back(payload.source);
        }
      },
      request.payload);
  return sources;
}

// The output container a request names (nullopt = Automatic).
std::optional<Container> outputContainerOf(const JobRequest &request) {
  return std::visit(
      [](const auto &payload) -> std::optional<Container> {
        return payload.output.container;
      },
      request.payload);
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
  if (sessions_ != nullptr) {
    sessions_->closeAll();
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

  // Backend failure feedback: execution-attributable failures record a
  // narrow FailedConfigKey in the capability registry so the next plan
  // avoids the failed route (10-minute cooldown; never a codec-wide ban).
  // The recorder runs on worker threads, which are joined before teardown.
  engine->jobs_->setFailureRecorder(
      [registry = &engine->capabilities_](const FailedConfigKey &key) {
        registry->recordFailure(key);
      });

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

  // Configuration sessions (mandate §14): the SessionManager owns the
  // accepted configuration; the engine wires its hooks to the real
  // inspector, the real planner, the durable job machinery and the
  // scheduler (constructed above).
  {
    SessionHooks hooks;
    hooks.inspect = [jobs = engine->jobs_.get()](const MediaSource &source) {
      return jobs->inspect(source);
    };
    hooks.assess = [planner = engine->planner_.get()](
                       const JobRequest &request,
                       const std::vector<MediaInfo> &sources)
        -> Result<ConfigurationAssessment> {
      const Result<PlanResult> plan = planner->plan(request, sources);
      if (!plan.ok()) {
        return plan.error();
      }
      ConfigurationAssessment assessment;
      assessment.feasible = plan.value().feasible;
      assessment.message = plan.value().message;
      if (plan.value().feasible && plan.value().best.has_value()) {
        const PlannedPipeline &best = *plan.value().best;
        assessment.pipelineSummary = best.pipeline.summary;
        if (best.estimatedOutputBytes.has_value()) {
          assessment.estimate.known = true;
          assessment.estimate.estimatedBytes = *best.estimatedOutputBytes;
          assessment.estimate.lowerBoundBytes =
              std::int64_t(*best.estimatedOutputBytes * 0.85);
          assessment.estimate.upperBoundBytes =
              std::int64_t(*best.estimatedOutputBytes * 1.15);
        }
      }
      return assessment;
    };
    hooks.submit = [jobs = engine->jobs_.get(),
                    scheduler = engine->scheduler_.get()](
                       const JobRequest &request,
                       const std::vector<MediaInfo> &sourceInfos,
                       const std::string &submissionId)
        -> Result<std::string> {
      const Result<std::string> submitted =
          jobs->submit(request, sourceInfos, submissionId);
      if (!submitted.ok()) {
        return submitted;
      }
      scheduler->submit(submitted.value());
      return submitted;
    };
    hooks.generateSample = [jobs = engine->jobs_.get(),
                            dataDir = config.dataDir](
                               const JobRequest &request)
        -> Result<SampleResult> {
      // The sample path (mandate §22): a ~5s preview of the densest
      // window through the same execution dispatch as real jobs. Other
      // operations refuse honestly inside the manager.
      std::filesystem::path dir = std::filesystem::path(dataDir) / "samples";
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      std::string stem;
      for (const MediaSource &source : sourcesOfRequest(request)) {
        stem = source.displayName;
        break;
      }
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
      const std::string outPath = (dir / (safe + "-sample.mp4")).string();
      return jobs->generateSample(request, 5, outPath);
    };
    engine->sessions_ = std::make_unique<SessionManager>(
        engine->features_, engine->capabilities_, std::move(hooks));
  }

  // Crash recovery: reconcile DB + artifacts before serving traffic. Jobs
  // that recovery re-queued are handed to the scheduler so they actually
  // resume (a durable Queued record alone would never execute). Submission
  // ids survive the round-trip, so recovery cannot duplicate work.
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

Result<PlanResult> VelmuxEngine::plan(const JobRequest &request) {
  return jobs_->plan(request);
}

Result<SampleResult> VelmuxEngine::generateSample(
    const JobRequest &request, std::int32_t sampleSeconds) {
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
  std::string stem;
  for (const MediaSource &source : sourcesOfRequest(request)) {
    stem = source.displayName;
    break;
  }
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
  const std::optional<Container> container = outputContainerOf(request);
  const Container effective =
      container.value_or(Container::Mp4);
  switch (effective) {
    case Container::Mkv:
    case Container::WebM:
      ext = "mkv";
      if (effective == Container::WebM) {
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
  const std::string outPath = (dir / (safe + "-sample." + ext)).string();
  return jobs_->generateSample(request, sampleSeconds, outPath);
}

Result<std::string> VelmuxEngine::submitJob(
    const JobRequest &request, const std::vector<MediaInfo> &sourceInfos,
    const std::string &submissionId, std::int32_t priority) {
  const Result<std::string> submitted =
      jobs_->submit(request, sourceInfos, submissionId);
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

Result<std::string> VelmuxEngine::reuseJobSettings(const std::string &jobId) {
  const auto record = jobs_->record(jobId);
  if (!record.has_value()) {
    return Result<std::string>(
        Error::make(ErrorCode::InvalidArgument, "unknown job: " + jobId));
  }
  if (!isTerminalJobState(record->state)) {
    return Result<std::string>(Error::make(
        ErrorCode::InvalidArgument,
        "job is still running; wait for a terminal state before reusing "
        "its settings"));
  }
  // A NEW editable configuration seeded with the accepted intent: the
  // sources are prepared (re-inspected; the originals may have changed),
  // then the payload is copied verbatim.
  const std::vector<MediaSource> sources = sourcesOfRequest(record->request);
  if (sources.empty()) {
    return Result<std::string>(Error::make(ErrorCode::InvalidArgument,
                                           "stored job has no source"));
  }
  const Result<std::string> session =
      sessions_->createSession(record->request.operation, sources);
  if (!session.ok()) {
    return session;
  }
  const Result<SessionSnapshot> prepared =
      sessions_->prepareSession(session.value());
  if (!prepared.ok()) {
    (void)sessions_->closeSession(session.value());
    return Result<std::string>(prepared.error());
  }
  const Result<SessionSnapshot> seeded =
      sessions_->seedAccepted(session.value(), record->request);
  if (!seeded.ok()) {
    (void)sessions_->closeSession(session.value());
    return Result<std::string>(seeded.error());
  }
  return session;
}

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------

Result<std::string> VelmuxEngine::createSession(
    Operation operation, std::vector<MediaSource> sources) {
  return sessions_->createSession(operation, std::move(sources));
}

Result<SessionSnapshot>
VelmuxEngine::prepareSession(const std::string &sessionId) {
  return sessions_->prepareSession(sessionId);
}

Result<SessionSnapshot>
VelmuxEngine::getSessionSnapshot(const std::string &sessionId) {
  return sessions_->getSnapshot(sessionId);
}

Result<FieldOptions> VelmuxEngine::getSessionOptions(
    const std::string &sessionId, FieldId field, std::uint64_t revision) {
  return sessions_->getOptions(sessionId, field, revision);
}

Result<SessionSnapshot> VelmuxEngine::updateSession(
    const std::string &sessionId, std::uint64_t expectedRevision,
    const std::vector<FieldChange> &changes, bool acceptRelatedChange) {
  return sessions_->updateSession(sessionId, expectedRevision, changes,
                                  acceptRelatedChange);
}

Result<SampleResult> VelmuxEngine::generateSessionSample(
    const std::string &sessionId, std::uint64_t expectedRevision) {
  return sessions_->generateSample(sessionId, expectedRevision);
}

Result<std::string> VelmuxEngine::submitSession(
    const std::string &sessionId, std::uint64_t expectedRevision,
    const std::string &submissionId) {
  const Result<std::string> submitted =
      sessions_->submitSession(sessionId, expectedRevision, submissionId);
  if (!submitted.ok()) {
    return submitted;
  }
  scheduler_->submit(submitted.value());
  return submitted;
}

Result<void> VelmuxEngine::closeSession(const std::string &sessionId) {
  return sessions_->closeSession(sessionId);
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
