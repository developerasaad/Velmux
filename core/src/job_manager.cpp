#include "velmux/core/job_manager.h"

#include "velmux/backend/contracts.h"
#include "velmux/core/compression.h"
#include "velmux/core/hybrid_executor.h"
#include "velmux/core/planner.h"
#include "velmux/core/recovery.h"
#include "velmux/core/telemetry.h"
#include "velmux/core/validation.h"

#include <atomic>
#include <filesystem>
#include <chrono>
#include <cstring>
#include <random>
#include <sys/stat.h>
#include <unistd.h>

namespace velmux::core {

namespace {

std::int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string generateJobId() {
  // Stable, sortable, unique-enough job IDs without external deps.
  static std::atomic<std::uint64_t> counter{0};
  const std::uint64_t now = std::uint64_t(nowMs());
  const std::uint64_t seq = counter.fetch_add(1);
  std::random_device rd;
  const std::uint32_t rand = rd();
  char buf[64];
  std::snprintf(buf, sizeof(buf), "j-%llx-%04x-%02x",
                (unsigned long long)now, (unsigned)seq & 0xffff,
                rand & 0xff);
  return buf;
}

bool fileExistsWithSize(const std::string &path, std::int64_t &sizeOut) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
    return false;
  }
  sizeOut = st.st_size;
  return true;
}

std::string sanitizeFileName(const std::string &name) {
  std::string out;
  out.reserve(name.size());
  for (char c : name) {
    const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '.' || c == '-' ||
                      c == '_' || c == ' ';
    out.push_back(safe ? c : '_');
  }
  // No path traversal: strip separators entirely (already excluded above)
  // and leading dots.
  while (!out.empty() && out.front() == '.') {
    out.erase(out.begin());
  }
  return out;
}

JobSnapshot snapshotOf(const JobRecord &record) {
  JobSnapshot snapshot;
  snapshot.id = record.id;
  snapshot.state = record.state;
  snapshot.progress = record.progress;
  snapshot.sourceName = record.request.source.displayName;
  snapshot.outputName = record.request.outputName.value_or("");
  snapshot.outputPath = record.outputPath;
  snapshot.error = record.error;
  snapshot.outputSizeBytes = record.outputSizeBytes;
  snapshot.inputSizeBytes = record.inputSizeBytes;
  snapshot.pipelineSummary = record.pipeline.summary;
  return snapshot;
}

// Checkpoint coordinator bridging the backend contract to the durable
// checkpoint store, implementing the crash-safe sequence.
class CheckpointCoordinator final : public backend::ICheckpointCoordinator {
public:
  CheckpointCoordinator(CheckpointStore &store, std::string jobId,
                        std::shared_ptr<backend::CancelToken> cancel)
      : store_(store), jobId_(std::move(jobId)), cancel_(std::move(cancel)) {
    // (job_id, sequence) is UNIQUE and rows from earlier runs of this job
    // legitimately survive (they ARE the resume data): continue numbering
    // after the persisted maximum instead of restarting at 1.
    sequence_ = store_.maxSequence(jobId_);
  }

  bool commit(const backend::CheckpointInfo &info) override {
    if (cancel_ && cancel_->isCancelled()) {
      return false;
    }
    CheckpointRecord record;
    record.jobId = jobId_;
    record.sequence = ++sequence_;
    record.stage = info.stage;
    record.position = info.position;
    record.artifactPath = info.artifactPath;
    if (!info.artifactPath.empty()) {
      std::int64_t bytes = 0;
      if (fileExistsWithSize(info.artifactPath, bytes)) {
        record.artifactBytes = bytes;
      }
    }
    record.committedAtMs = nowMs();
    // Durable: WAL mode + synchronous=NORMAL. A commit that returns is a
    // commit that survived.
    return store_.insertCheckpoint(record).ok();
  }

private:
  CheckpointStore &store_;
  std::string jobId_;
  std::shared_ptr<backend::CancelToken> cancel_;
  std::int32_t sequence_ = 0;
};

} // namespace

namespace {

// Fit-under-size (spec §17): the computed budget DRIVES encoding. The
// goal-derived bitrate replaces the default quality mode unless the user
// set an explicit bitrate (explicit user intent is never overridden).
// Retry determinism: the budget is a pure function of source + request,
// so a reloaded job computes the same parameters.
ConversionRequest applyFitUnderSizeBudget(const MediaInfo &info,
                                           const ConversionRequest &request) {
  ConversionRequest effective = request;
  if (request.goal.kind == GoalKind::FitUnderSize &&
      request.goal.targetSizeBytes > 0 &&
      request.video.rateControl != RateControl::Bitrate &&
      !request.video.bitRate.has_value()) {
    const BitrateBudget budget = computeBitrateBudget(
        info, request, request.goal.targetSizeBytes);
    if (budget.feasible && budget.videoBitrate > 0) {
      effective.video.rateControl = RateControl::Bitrate;
      effective.video.bitRate = budget.videoBitrate;
      if (!budget.audioStreamCopy && budget.audioBitrate > 0 &&
          !effective.audio.bitRate.has_value()) {
        effective.audio.bitRate = budget.audioBitrate;
      }
    }
  }
  return effective;
}

} // namespace

JobManager::JobManager(EngineConfig config, EventBus &events, JobStore &store,
                       CheckpointStore &checkpoints, Planner &planner,
                       TelemetryStore &telemetry)
    : config_(std::move(config)), events_(events), store_(store),
      checkpoints_(checkpoints), planner_(planner), telemetry_(telemetry) {}

JobManager::~JobManager() = default;

void JobManager::setBackends(std::vector<backend::IBackend *> backends) {
  backends_ = std::move(backends);
  // The hybrid executor composes stages across the registered backends
  // (Milestone 4); it re-seeds whenever the backend set changes.
  hybrid_ = std::make_unique<HybridExecutor>(backends_);
}

void JobManager::setValidator(OutputValidator *validator) {
  validator_ = validator;
}

Result<MediaInfo> JobManager::inspect(const MediaSource &source) {
  for (backend::IBackend *backend : backends_) {
    if (backend->inspector() == nullptr) {
      continue;
    }
    MediaInfo info;
    const Error error = backend->inspector()->inspect(source, info);
    if (error.ok()) {
      return Result<MediaInfo>(std::move(info));
    }
    // Untrusted input isolation: try the next backend; if all fail, report
    // the last error.
    if (backend == backends_.back()) {
      return Result<MediaInfo>(error);
    }
  }
  return Result<MediaInfo>(Error::make(
      ErrorCode::InspectionFailed, "no backend could inspect this source"));
}

Result<PlanResult> JobManager::plan(const MediaSource &source,
                                    const ConversionRequest &request) {
  const Result<MediaInfo> inspected = inspect(source);
  if (!inspected.ok()) {
    return Result<PlanResult>(inspected.error());
  }
  PlanResult result = planner_.plan(inspected.value(), request);
  (void)telemetry_.recordPlanningLatency(result.planningMs);
  return Result<PlanResult>(std::move(result));
}

Result<SampleResult> JobManager::generateSample(
    const ConversionRequest &request, std::int32_t sampleSeconds,
    const std::string &outputPath) {
  if (sampleSeconds <= 0) {
    return Result<SampleResult>(Error::make(ErrorCode::InvalidArgument,
                                            "sample duration must be positive",
                                            "core"));
  }
  // Duration drives the window; a representative sample needs one.
  const Result<MediaInfo> inspected = inspect(request.source);
  if (!inspected.ok()) {
    return Result<SampleResult>(inspected.error());
  }
  const MediaInfo &info = inspected.value();
  if (info.durationSec <= 0.0) {
    return Result<SampleResult>(Error::make(
        ErrorCode::InspectionFailed,
        "cannot determine the source duration for sampling", "core"));
  }
  const std::int64_t durationUs =
      std::int64_t(info.durationSec * 1000000.0);
  const std::int64_t windowLenUs =
      std::min<std::int64_t>(std::int64_t(sampleSeconds) * 1000000,
                             durationUs);

  // Representative window (spec §20): the densest section per a backend's
  // demux-level scan. Mid-file fallback when no backend can pick (avoids
  // the abrupt intro/outro regions by starting a quarter in).
  std::int64_t startUs =
      std::max<std::int64_t>((durationUs - windowLenUs) / 4, 0);
  std::int64_t endUs = startUs + windowLenUs;
  const std::int32_t windowSeconds =
      std::max<std::int32_t>(std::int32_t(windowLenUs / 1000000), 1);
  for (backend::IBackend *backend : backends_) {
    if (backend->inspector() != nullptr &&
        backend->inspector()->representativeWindow(
            request.source, windowSeconds, startUs, endUs)) {
      break;
    }
  }

  // Plan against the FULL source so the sample uses exactly the settings
  // the real job would (goal-derived quality, budget, codec routing), then
  // execute only the chosen window through the same dispatch as jobs. The
  // fit-under-size budget applies here too: samples must preview the exact
  // parameters a real job would use.
  const ConversionRequest effectiveSampleRequest =
      applyFitUnderSizeBudget(info, request);
  const Result<PlanResult> planned = planner_.plan(info, effectiveSampleRequest);
  if (!planned.ok()) {
    return Result<SampleResult>(planned.error());
  }
  if (!planned.value().feasible || !planned.value().best.has_value()) {
    return Result<SampleResult>(Error::make(
        ErrorCode::UnsupportedFeature,
        planned.value().message.empty()
            ? "no compatible pipeline for the requested settings"
            : planned.value().message,
        "core"));
  }
  const Pipeline pipeline = planned.value().best->pipeline;

  backend::IBackend *backend = backendFor(pipeline);
  const bool hybridPath =
      backend == nullptr && hybrid_ != nullptr &&
      hybrid_->canExecute(pipeline, effectiveSampleRequest);
  if ((backend == nullptr || backend->executor() == nullptr) && !hybridPath) {
    return Result<SampleResult>(
        Error::make(ErrorCode::UnsupportedFeature,
                    "no backend could execute the planned pipeline", "core"));
  }

  ConversionRequest sampleRequest = effectiveSampleRequest;
  sampleRequest.trim.startUs = startUs;
  sampleRequest.trim.endUs = endUs;

  backend::ExecuteParams params;
  params.pipeline = pipeline;
  params.request = sampleRequest;
  params.outputPath = outputPath;
  // No checkpoints for samples: single bounded pass, restartable by
  // regenerating (documented limitation, not silent behavior).
  params.jobId = "";
  params.cancel = std::make_shared<backend::CancelToken>();
  const backend::ExecuteResult execResult =
      hybridPath ? hybrid_->execute(params)
                 : backend->executor()->execute(params);
  if (!execResult.success) {
    return Result<SampleResult>(execResult.error);
  }

  // Independent validation before reporting success (spec §27): codecs,
  // streams, geometry, minimum size. Duration stays unchecked — a trimmed
  // window legitimately differs from the source.
  if (validator_ != nullptr) {
    ValidationExpectation expect;
    expect.container = pipeline.container;
    if (sampleRequest.video.enabled && !info.video.empty()) {
      expect.videoCodec = pipeline.videoCodec;
      expect.expectedHeight = sampleRequest.video.resolutionHeight.value_or(
          info.video.front().height);
    } else {
      expect.expectVideo = false;
    }
    if (sampleRequest.audio.enabled && !info.audio.empty()) {
      expect.audioCodec = pipeline.audioCodec;
    } else if (!sampleRequest.audio.enabled) {
      expect.expectAudio = false;
    }
    const ValidationReport report = validator_->validate(outputPath, expect);
    if (!report.ok) {
      std::string problems;
      for (const std::string &p : report.problems) {
        problems += p + "; ";
      }
      return Result<SampleResult>(Error::make(
          ErrorCode::OutputValidationFailed,
          "sample failed output validation: " + problems, "core"));
    }
  }

  SampleResult sample;
  sample.outputPath = outputPath;
  sample.startUs = startUs;
  sample.endUs = endUs;
  std::error_code ec;
  sample.sizeBytes = std::filesystem::file_size(outputPath, ec);
  if (ec) {
    sample.sizeBytes = -1;
  }
  return Result<SampleResult>(std::move(sample));
}

Result<std::string> JobManager::submit(const ConversionRequest &request,
                                       std::int32_t priority) {
  // Priority is honored by the scheduler when the engine hands the queued
  // job over; the manager itself is priority-agnostic.
  (void)priority;
  // Validate source accessibility first: never enqueue phantom jobs.
  const Result<MediaInfo> inspected = inspect(request.source);
  if (!inspected.ok()) {
    return Result<std::string>(inspected.error());
  }

  JobRecord record;
  record.id = generateJobId();
  record.state = JobState::Queued;
  record.request = request;
  record.sourceInfo = inspected.value();
  record.inputSizeBytes = inspected.value().sizeBytes;
  record.createdAtMs = nowMs();
  record.updatedAtMs = nowMs();
  record.schemaVersion = JobStore::kSchemaVersion;

  const Error insertError = store_.insertJob(record);
  if (!insertError.ok()) {
    return Result<std::string>(insertError);
  }
  return Result<std::string>(record.id);
}

Result<void> JobManager::cancel(const std::string &jobId) {
  std::lock_guard<std::mutex> lock(mutex_);
  Error loadError;
  auto record = store_.loadJob(jobId, loadError);
  if (!record.has_value()) {
    return Result<void>(Error::make(ErrorCode::InvalidArgument,
                                    "unknown job: " + jobId));
  }
  if (record->state == JobState::Completed ||
      record->state == JobState::Failed ||
      record->state == JobState::Canceled) {
    return Result<void>(Error::make(ErrorCode::InvalidArgument,
                                    "job already finished"));
  }
  auto running = running_.find(jobId);
  if (running != running_.end()) {
    // Record the intent in the in-memory running record too: when the worker
    // observes the cancel token it finalizes exactly this state.
    running->second.record.state = JobState::Canceled;
    if (running->second.cancel != nullptr) {
      running->second.cancel->cancel();
    }
  }
  record->state = JobState::Canceled;
  record->finishedAtMs = nowMs();
  record->progress = record->progress < 0 ? 0.0 : record->progress;
  persistAndEmit(*record, "cancel");
  return Result<void>();
}

Result<void> JobManager::pause(const std::string &jobId) {
  std::lock_guard<std::mutex> lock(mutex_);
  Error loadError;
  auto record = store_.loadJob(jobId, loadError);
  if (!record.has_value()) {
    return Result<void>(Error::make(ErrorCode::InvalidArgument,
                                    "unknown job: " + jobId));
  }
  if (record->state != JobState::Running && record->state != JobState::Queued &&
      record->state != JobState::Preparing) {
    return Result<void>(Error::make(ErrorCode::InvalidArgument,
                                    "job is not pausable in state " +
                                        std::string(jobStateName(record->state))));
  }
  auto running = running_.find(jobId);
  if (running != running_.end()) {
    // Cooperative pause = cancel the current execution; the durable record
    // keeps committed checkpoints, and resume restarts from the safest
    // checkpoint (spec: no fake in-place encoder suspension). The intent is
    // recorded in the running record so the worker finalizes Paused, not
    // Canceled (they share the cooperative token).
    running->second.record.state = JobState::Paused;
    if (running->second.cancel != nullptr) {
      running->second.cancel->cancel();
    }
  }
  record->state = JobState::Paused;
  persistAndEmit(*record, "pause");
  return Result<void>();
}

Result<void> JobManager::resume(const std::string &jobId) {
  Error loadError;
  auto record = store_.loadJob(jobId, loadError);
  if (!record.has_value()) {
    return Result<void>(Error::make(ErrorCode::InvalidArgument,
                                    "unknown job: " + jobId));
  }
  if (record->state != JobState::Paused &&
      record->state != JobState::Interrupted) {
    return Result<void>(Error::make(ErrorCode::InvalidArgument,
                                    "job is not resumable in state " +
                                        std::string(jobStateName(record->state))));
  }
  record->state = JobState::Queued;
  record->error.clear();
  persistAndEmit(*record, "resume");
  return Result<void>();
}

Result<void> JobManager::retry(const std::string &jobId) {
  Error loadError;
  auto record = store_.loadJob(jobId, loadError);
  if (!record.has_value()) {
    return Result<void>(Error::make(ErrorCode::InvalidArgument,
                                    "unknown job: " + jobId));
  }
  if (record->state != JobState::Failed &&
      record->state != JobState::Canceled &&
      record->state != JobState::Interrupted) {
    return Result<void>(
        Error::make(ErrorCode::InvalidArgument, "job is not retryable"));
  }
  record->state = JobState::Queued;
  record->error.clear();
  record->progress = -1.0;
  persistAndEmit(*record, "retry");
  return Result<void>();
}

Result<void> JobManager::remove(const std::string &jobId) {
  std::lock_guard<std::mutex> lock(mutex_);
  Error loadError;
  auto record = store_.loadJob(jobId, loadError);
  if (!record.has_value()) {
    return Result<void>(Error::make(ErrorCode::InvalidArgument,
                                    "unknown job: " + jobId));
  }
  if (record->state != JobState::Completed &&
      record->state != JobState::Failed &&
      record->state != JobState::Canceled) {
    return Result<void>(Error::make(
        ErrorCode::InvalidArgument,
        "job is still active (cancel or pause it first)"));
  }
  const Error deleteError = store_.deleteJob(jobId);
  if (!deleteError.ok()) {
    return Result<void>(deleteError);
  }
  return Result<void>();
}

std::optional<JobSnapshot> JobManager::snapshot(const std::string &jobId) {
  std::lock_guard<std::mutex> lock(mutex_);
  Error loadError;
  auto record = store_.loadJob(jobId, loadError);
  if (!record.has_value()) {
    return std::nullopt;
  }
  auto running = running_.find(jobId);
  if (running != running_.end()) {
    // Blend in live progress from the in-memory running record.
    JobSnapshot snapshot = snapshotOf(running->second.record);
    return snapshot;
  }
  return snapshotOf(*record);
}

std::vector<JobSnapshot> JobManager::snapshots() {
  std::lock_guard<std::mutex> lock(mutex_);
  Error loadError;
  std::vector<JobRecord> records = store_.loadAllJobs(loadError);
  std::vector<JobSnapshot> result;
  result.reserve(records.size());
  for (const auto &record : records) {
    auto running = running_.find(record.id);
    if (running != running_.end()) {
      result.push_back(snapshotOf(running->second.record));
    } else {
      result.push_back(snapshotOf(record));
    }
  }
  return result;
}

std::optional<JobRecord> JobManager::record(const std::string &jobId) {
  Error loadError;
  return store_.loadJob(jobId, loadError);
}

bool JobManager::isActive(const std::string &jobId) {
  std::lock_guard<std::mutex> lock(mutex_);
  return running_.find(jobId) != running_.end();
}

std::vector<RecoveryDecision> JobManager::recoverInterruptedJobs() {
  Error error;
  std::vector<JobRecord> resumable = store_.loadResumableJobs(error);
  RecoveryPlanner planner;
  std::vector<RecoveryDecision> decisions =
      planner.reconcile(resumable, checkpoints_, config_.dataDir);
  for (const RecoveryDecision &decision : decisions) {
    Error loadError;
    auto record = store_.loadJob(decision.jobId, loadError);
    if (!record.has_value()) {
      continue;
    }
    switch (decision.action) {
      case RecoveryAction::RestartJob:
      case RecoveryAction::ResumeFromCheckpoint: {
        // The execution path resumes from the safest checkpoint; either way
        // the job re-enters the queue honestly.
        record->state = JobState::Recovering;
        persistAndEmit(*record, "recovery");
        record->state = JobState::Queued;
        record->error.clear();
        persistAndEmit(*record, "recovery-queued");
        break;
      }
      case RecoveryAction::MarkInterrupted:
        record->state = JobState::Interrupted;
        record->error = decision.message;
        persistAndEmit(*record, "recovery-interrupted");
        break;
      case RecoveryAction::NothingToDo:
        break;
    }
  }
  return decisions;
}

void JobManager::attachScheduler(Scheduler &scheduler) {
  // The scheduler calls back into executeJob when capacity allows. The
  // manager does not own the scheduler; the engine does.
  (void)scheduler;
}

void JobManager::reportProgress(const std::string &jobId, double fraction,
                                std::int64_t outputBytes,
                                std::uint64_t generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!runStillCurrent(jobId, generation)) {
    return; // superseded run; the newer run owns the record
  }
  auto running = running_.find(jobId);
  if (running == running_.end()) {
    return;
  }
  running->second.record.progress = fraction;
  if (outputBytes >= 0) {
    running->second.record.outputSizeBytes = outputBytes;
  }
  JobEvent event;
  event.jobId = jobId;
  event.state = running->second.record.state;
  event.progress = fraction;
  event.outputSizeBytes = running->second.record.outputSizeBytes;
  events_.emit(event);
}

bool JobManager::runStillCurrent(const std::string &jobId,
                                 std::uint64_t generation) const {
  const auto it = running_.find(jobId);
  return it != running_.end() && it->second.generation == generation;
}

void JobManager::cancelAllRunning(const char *reason) {
  const std::string why = reason != nullptr ? reason : "engine shutdown";
  std::vector<std::shared_ptr<backend::CancelToken>> tokens;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &entry : running_) {
      RunningJob &running = entry.second;
      // Respect earlier user intent: a job already canceled/paused (or
      // finished) keeps that state. Actively executing jobs move to
      // Recovering so the next startup auto-resumes them from their
      // committed checkpoints (spec §4.5/§22: a shutdown is not a user
      // cancel; committed work is preserved aggressively).
      Error loadError;
      auto durable = store_.loadJob(entry.first, loadError);
      const bool userHeld =
          durable.has_value() &&
          (durable->state == JobState::Canceled ||
           durable->state == JobState::Paused ||
           durable->state == JobState::Completed ||
           durable->state == JobState::Failed ||
           durable->state == JobState::Interrupted);
      if (!userHeld) {
        running.record.state = JobState::Recovering;
        running.record.error = why;
        persistAndEmit(running.record, "recovering");
      }
      if (running.cancel != nullptr) {
        tokens.push_back(running.cancel);
      }
    }
  }
  // Tokens fire after the Recovering records are durable, so workers
  // observe a consistent intended state when they finalize.
  for (const auto &token : tokens) {
    token->cancel();
  }
}

void JobManager::persistAndEmit(JobRecord &record, const char * /*note*/) {
  record.updatedAtMs = nowMs();
  (void)store_.updateJob(record);
  JobEvent event;
  event.jobId = record.id;
  event.state = record.state;
  event.progress = record.progress;
  event.outputSizeBytes = record.outputSizeBytes;
  event.error = record.error;
  events_.emit(event);
}

void JobManager::setDispatchHooks(
    std::function<bool(const std::string &jobId, const Pipeline &pipeline)>
        gate,
    std::function<void(const std::string &jobId)> onHardwareStarted) {
  dispatchGate_ = std::move(gate);
  hardwareStartedCb_ = std::move(onHardwareStarted);
}

JobManager::ExecuteOutcome JobManager::executeJob(const std::string &jobId) {
  Error loadError;
  auto record = store_.loadJob(jobId, loadError);
  if (!record.has_value()) {
    return ExecuteOutcome::Finished;
  }
  if (record->state == JobState::Canceled ||
      record->state == JobState::Completed ||
      record->state == JobState::Failed ||
      record->state == JobState::Paused ||
      record->state == JobState::Interrupted) {
    // Terminal or user-held states never (re)start execution from the
    // scheduler path; resume()/retry() re-queue explicitly.
    return ExecuteOutcome::Finished;
  }

  record->state = JobState::Preparing;
  record->startedAtMs = nowMs();
  std::uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    RunningJob running;
    running.record = *record;
    running.cancel = std::make_shared<backend::CancelToken>();
    running.generation = ++nextGeneration_;
    generation = running.generation;
    // Replaces any superseded run's entry: the new run owns the record from
    // here on, and the old worker will detect the generation mismatch.
    running_[jobId] = std::move(running);
  }
  persistAndEmit(*record, "preparing");

  // Inspect (or reuse submitted inspection) then plan.
  MediaInfo info = record->sourceInfo;
  if (info.durationSec <= 0.0 || (info.hasVideo() && info.video.empty())) {
    const Result<MediaInfo> inspected = inspect(record->request.source);
    if (!inspected.ok()) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!runStillCurrent(jobId, generation)) {
        return ExecuteOutcome::Finished; // superseded by a newer run
      }
      auto &running = running_[jobId];
      running.record.state = JobState::Failed;
      running.record.error =
          "Source became unavailable: " + inspected.error().message;
      finishFailure(running.record);
      return ExecuteOutcome::Finished;
    }
    info = inspected.value();
    // Validation compares the output against the inspection the planner
    // used; keep the record consistent so the duration/geometry checks
    // are live after the store round-trip (they were silently dead when
    // the reloaded sourceInfo stayed empty).
    record->sourceInfo = info;
  }

  // Fit-under-size: apply the budget before planning so the plan's size
  // estimate and feasibility message describe what will actually execute.
  const ConversionRequest effectiveRequest =
      applyFitUnderSizeBudget(info, record->request);

  PlanResult plan = planner_.plan(info, effectiveRequest);
  if (!plan.feasible || !plan.best.has_value()) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!runStillCurrent(jobId, generation)) {
      return ExecuteOutcome::Finished; // superseded by a newer run
    }
    auto &running = running_[jobId];
    running.record.state = JobState::Failed;
    running.record.error =
        plan.message.empty() ? "no compatible pipeline" : plan.message;
    finishFailure(running.record);
    return ExecuteOutcome::Finished;
  }

  const Pipeline pipeline = plan.best->pipeline;
  record->pipeline = pipeline;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!runStillCurrent(jobId, generation)) {
      return ExecuteOutcome::Finished; // superseded by a newer run
    }
    running_[jobId].record.pipeline = pipeline;
  }

  backend::IBackend *backend = backendFor(pipeline);
  const bool hybridPath =
      backend == nullptr && hybrid_ != nullptr &&
      hybrid_->canExecute(pipeline, record->request);
  if ((backend == nullptr || backend->executor() == nullptr) && !hybridPath) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!runStillCurrent(jobId, generation)) {
      return ExecuteOutcome::Finished; // superseded by a newer run
    }
    auto &running = running_[jobId];
    running.record.state = JobState::Failed;
    running.record.error = "no backend could execute the planned pipeline";
    finishFailure(running.record);
    return ExecuteOutcome::Finished;
  }

  // Hardware-codec scarcity gate (spec §21): a hardware pipeline defers
  // while the scheduler reports hardware capacity saturated. The durable
  // state returns to Queued; the engine re-queues the job, and the
  // scheduler holds it (not busy-spins) until capacity frees.
  if (pipeline.usesHardware && dispatchGate_ &&
      !dispatchGate_(jobId, pipeline)) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!runStillCurrent(jobId, generation)) {
        return ExecuteOutcome::Finished; // superseded by a newer run
      }
      running_.erase(jobId);
    }
    record->state = JobState::Queued;
    record->progress = 0.0;
    persistAndEmit(*record, "queued");
    return ExecuteOutcome::Deferred;
  }
  if (pipeline.usesHardware && hardwareStartedCb_) {
    hardwareStartedCb_(jobId);
  }

  // Output path: sanitized, never overwrites the original.
  record->outputPath = outputNameFor(*record);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!runStillCurrent(jobId, generation)) {
      return ExecuteOutcome::Finished; // superseded by a newer run
    }
    running_[jobId].record.outputPath = record->outputPath;
    running_[jobId].record.state = JobState::Running;
  }
  persistAndEmit(*record, "running");

  // The run's cancel token: shared with the control path; the executor
  // observes cooperative cancellation through it.
  std::shared_ptr<backend::CancelToken> cancel;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!runStillCurrent(jobId, generation)) {
      return ExecuteOutcome::Finished; // superseded by a newer run
    }
    cancel = running_[jobId].cancel;
  }

  // Dispatch closure: one executor run (whole-pipeline or hybrid) with
  // checkpoints wired. Used by the first attempt and, when justified, the
  // fit-under-size corrective retry.
  const auto dispatchExecution =
      [&](const ConversionRequest &req) -> backend::ExecuteResult {
        backend::ExecuteParams params;
        params.pipeline = pipeline;
        params.request = req;
        params.outputPath = record->outputPath;
        params.jobId = jobId;
        params.cancel = cancel;
        params.onProgress =
            [this, jobId, generation](const backend::ProgressReport &report) {
              reportProgress(jobId, report.fraction, -1, generation);
            };
        CheckpointCoordinator coordinator(checkpoints_, jobId, cancel);
        params.checkpoints = &coordinator;
        // Recovery input: checkpoints committed by earlier runs of this
        // job. Fresh jobs have none; paused/resumed/recovered jobs get the
        // durable set and the executor skips work covered by valid
        // artifacts.
        {
          Error checkpointError;
          for (const CheckpointRecord &cp :
               checkpoints_.loadCheckpoints(jobId, checkpointError)) {
            if (cp.artifactPath.empty()) {
              continue; // position-only marks carry nothing resumable
            }
            backend::CheckpointInfo info2;
            info2.stage = cp.stage;
            info2.position = cp.position;
            info2.artifactPath = cp.artifactPath;
            params.resumeFrom.push_back(std::move(info2));
          }
        }
        return hybridPath ? hybrid_->execute(params)
                          : backend->executor()->execute(params);
      };

  backend::ExecuteResult execResult = dispatchExecution(effectiveRequest);

  // Fit-under-size corrective retry (spec §17): encoders cannot guarantee
  // exact output sizes; when the first attempt lands over the target, ONE
  // re-encode at a proportionally reduced bitrate runs in-place. Bounded to
  // a single retry; a still-missed target completes honestly with a note —
  // never a fake guarantee. The retry re-encodes the whole file: the first
  // attempt's checkpoints describe different encoding decisions.
  bool fitRetryRan = false;
  bool fitTargetMissed = false;
  if (execResult.success &&
      record->request.goal.kind == GoalKind::FitUnderSize &&
      record->request.goal.targetSizeBytes > 0 && !cancel->isCancelled()) {
    const std::int64_t target = record->request.goal.targetSizeBytes;
    std::int64_t actual = 0;
    if (fileExistsWithSize(record->outputPath, actual) && actual > target) {
      std::int64_t baseBitrate = effectiveRequest.video.bitRate.value_or(0);
      if (baseBitrate <= 0) {
        const BitrateBudget budget = computeBitrateBudget(
            info, record->request, target);
        baseBitrate = budget.videoBitrate;
      }
      if (baseBitrate > 0) {
        // 10% safety margin under the proportional scale.
        const double scale =
            (double(target) / double(actual)) * 0.90;
        const std::int64_t newBitrate =
            std::max<std::int64_t>(
                std::int64_t(double(baseBitrate) * scale), 10000);
        if (newBitrate < baseBitrate) {
          ConversionRequest retryRequest = effectiveRequest;
          retryRequest.video.rateControl = RateControl::Bitrate;
          retryRequest.video.bitRate = newBitrate;
          (void)checkpoints_.deleteCheckpoints(jobId);
          fitRetryRan = true;
          execResult = dispatchExecution(retryRequest);
          if (execResult.success) {
            std::int64_t after = 0;
            fitTargetMissed = fileExistsWithSize(record->outputPath, after) &&
                              after > target;
          }
        }
      }
    }
  }

  // Feed real observations into telemetry (performance learning).
  if (execResult.wallClockSec > 0.0) {
    PerfRecord perf;
    perf.backend = hybridPath
                        ? pipeline.assigns(StageId::Mux, BackendId::Android)
                              ? BackendId::Android
                          : pipeline.assigns(StageId::Mux, BackendId::Apple)
                              ? BackendId::Apple
                              : BackendId::Ffmpeg
                        : backend->id();
    if (pipeline.videoStreamCopy == false && !info.video.empty()) {
      perf.videoCodec = pipeline.videoCodec;
    }
    if (!info.audio.empty()) {
      perf.audioCodec = pipeline.audioCodec;
    }
    perf.framesPerSecond =
        execResult.framesProcessed > 0
            ? double(execResult.framesProcessed) / execResult.wallClockSec
            : 0.0;
    perf.setupMs = plan.best->setupLatencyMs;
    perf.success = execResult.success;
    perf.note = pipeline.summary;
    (void)telemetry_.record(perf);
  }

  if (cancel->isCancelled()) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!runStillCurrent(jobId, generation)) {
      return ExecuteOutcome::Finished; // superseded (e.g. paused then quickly
                                       // resumed): the newer run owns the
                                       // record; this worker must not touch it.
    }
    auto &running = running_[jobId];
    // The control path (cancel / pause / engine shutdown) already recorded
    // the intended durable state on the running record; finalize exactly
    // that. An active state here means the token fired without a recorded
    // decision — the honest fallback is a terminal cancel.
    if (running.record.state == JobState::Running ||
        running.record.state == JobState::Preparing ||
        running.record.state == JobState::Validating ||
        running.record.state == JobState::Checkpointing) {
      running.record.state = JobState::Canceled;
    }
    if (running.record.state == JobState::Canceled) {
      running.record.finishedAtMs = nowMs();
    }
    JobRecord final = running.record;
    running_.erase(jobId);
    persistAndEmit(final, "final");
    return ExecuteOutcome::Finished;
  }

  if (!execResult.success) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!runStillCurrent(jobId, generation)) {
      return ExecuteOutcome::Finished; // superseded by a newer run
    }
    auto &running = running_[jobId];
    running.record.state = JobState::Failed;
    running.record.error = execResult.error.message.empty()
                               ? "execution failed"
                               : execResult.error.message;
    finishFailure(running.record);
    return ExecuteOutcome::Finished;
  }

  // Validation before completion (never trust pipeline success alone).
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!runStillCurrent(jobId, generation)) {
      return ExecuteOutcome::Finished; // superseded by a newer run
    }
    running_[jobId].record.state = JobState::Validating;
  }
  persistAndEmit(*record, "validating");
  if (!validateOutput(*record, generation)) {
    return ExecuteOutcome::Finished; // finishFailure already ran (or the run
                                     // was superseded)
  }

  std::int64_t outputBytes = -1;
  fileExistsWithSize(record->outputPath, outputBytes);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!runStillCurrent(jobId, generation)) {
      return ExecuteOutcome::Finished; // superseded by a newer run
    }
    auto &running = running_[jobId];
    if (fitRetryRan) {
      // Honest fit-under-size outcome in the pipeline summary (shown in
      // history and job details): a retry happened, and whether it landed
      // under the target. Never a fake guarantee.
      running.record.pipeline.summary +=
          fitTargetMissed
              ? " [fit-under-size: target not reached after retry]"
              : " [fit-under-size: retried at a lower bitrate]";
    }
    running.record.state = JobState::Completed;
    running.record.progress = 1.0;
    running.record.outputSizeBytes = outputBytes;
    running.record.finishedAtMs = nowMs();
    JobRecord final = running.record;
    running_.erase(jobId);
    persistAndEmit(final, "completed");
  }
  // Completed work is the output itself: the executor already removed the
  // segment scratch, so the checkpoint rows must not outlive their
  // artifacts (updateJob deliberately no longer cascades here).
  (void)checkpoints_.deleteCheckpoints(jobId);
  return ExecuteOutcome::Finished;
}

void JobManager::finishFailure(JobRecord &record) {
  record.finishedAtMs = nowMs();
  JobRecord final = record;
  const std::string jobId = record.id;
  running_.erase(jobId);
  persistAndEmit(final, "failed");
}

bool JobManager::validateOutput(JobRecord &record, std::uint64_t generation) {
  if (validator_ == nullptr) {
    // No validator wired: honest failure rather than fake success.
    std::lock_guard<std::mutex> lock(mutex_);
    if (!runStillCurrent(record.id, generation)) {
      return false; // superseded; the newer run owns finalization
    }
    auto &running = running_[record.id];
    running.record.state = JobState::Failed;
    running.record.error = "output validator unavailable";
    finishFailure(running.record);
    return false;
  }
  ValidationExpectation expect;
  expect.container = record.pipeline.container;
  if (!record.pipeline.sidecarExtension.empty()) {
    // Extraction job: the artifact is a text sidecar; validate cues.
    // The container-oriented 1 KiB floor would reject valid short
    // sidecars (a two-cue SRT is ~100 bytes); one cue line is the floor.
    expect.subtitleSidecar = true;
    expect.expectVideo = false;
    expect.expectAudio = false;
    expect.expectedDurationSec = 0.0;
    expect.minimumBytes = 32;
  } else if (record.request.video.enabled) {
    expect.videoCodec = record.pipeline.videoCodec;
    if (!record.sourceInfo.video.empty()) {
      // Transform-aware expected geometry (the executor applies
      // crop -> rotation -> scale): a correct 90-degree rotation of a
      // 1920x1080 source is 1080 high, and a cropped output is the crop
      // height — comparing against the raw source height would fail every
      // correct transformed output.
      std::int32_t baseH = record.sourceInfo.video.front().height;
      std::int32_t baseW = record.sourceInfo.video.front().width;
      if (record.request.video.crop.has_value() &&
          record.request.video.crop->active()) {
        const std::int32_t availW =
            std::max(0, baseW - record.request.video.crop->x);
        const std::int32_t availH =
            std::max(0, baseH - record.request.video.crop->y);
        baseW = std::clamp(record.request.video.crop->width, 2, availW);
        baseH = std::clamp(record.request.video.crop->height, 2, availH);
      }
      if (record.request.video.rotationDegrees == 90 ||
          record.request.video.rotationDegrees == 270) {
        std::swap(baseW, baseH);
      }
      expect.expectedHeight =
          record.request.video.resolutionHeight.value_or(baseH);
    }
  } else {
    expect.expectVideo = false;
  }
  if (record.request.audio.enabled) {
    expect.audioCodec = record.pipeline.audioCodec;
    if (record.request.audio.sampleRate.has_value()) {
      expect.expectedSampleRate = *record.request.audio.sampleRate;
    }
  } else {
    expect.expectAudio = false;
  }
  // Duration expectation: the source duration minus an active trim window
  // (a trimmed output is SHORTER by design; the validator used to compare
  // against the full source and only got away with it while the persisted
  // sourceInfo was empty and silently disabled this check).
  expect.expectedDurationSec = record.sourceInfo.durationSec;
  if (record.request.trim.startUs > 0 ||
      record.request.trim.endUs >= 0) {
    const std::int64_t trimStartUs = record.request.trim.startUs;
    const std::int64_t trimEndUs =
        record.request.trim.endUs >= 0
            ? record.request.trim.endUs
            : std::int64_t(record.sourceInfo.durationSec * 1'000'000.0);
    if (trimEndUs > trimStartUs) {
      const double trimmedSec =
          double(trimEndUs - trimStartUs) / 1'000'000.0;
      expect.expectedDurationSec = trimmedSec;
      // Stream-copy trims snap their start to the preceding keyframe;
      // transcode trims are frame-exact. A generous extra second absorbs
      // the keyframe snap without opening a fake-success window.
      expect.durationToleranceSec += 1.0;
    }
  }
  // Audio DSP timeline effects (spec §10): pitch-preserving speed stretches
  // the duration by exactly 1/speed; silence removal can only shorten.
  if (record.request.audio.playbackSpeed.has_value() &&
      *record.request.audio.playbackSpeed > 0.0) {
    expect.expectedDurationSec /= *record.request.audio.playbackSpeed;
  }
  if (record.request.audio.removeSilence) {
    // Removed silence is content-dependent: no exact expectation is
    // honest. Upper bound = unstretched duration (+tolerance); a floor of
    // 10% catches truncated output without demanding exact silence
    // detection. Encoded via the same expectedDuration path with a wide
    // tolerance: the validator's check is |actual - expected| <= tol,
    // so set expected to the midpoint of [0.1, 1.0] x duration and a
    // tolerance covering the whole band.
    const double span = expect.expectedDurationSec;
    expect.expectedDurationSec = span * 0.55;
    expect.durationToleranceSec += span * 0.45;
  }

  const ValidationReport report =
      validator_->validate(record.outputPath, expect);
  if (report.ok) {
    return true;
  }
  std::string problems;
  for (const std::string &problem : report.problems) {
    problems += problem + "; ";
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!runStillCurrent(record.id, generation)) {
    return false; // superseded; the newer run owns finalization
  }
  auto &running = running_[record.id];
  running.record.state = JobState::Failed;
  running.record.error = "output validation failed: " + problems;
  finishFailure(running.record);
  return false;
}

backend::IBackend *JobManager::backendFor(const Pipeline &pipeline) {
  // v1: a single backend must serve every stage of the pipeline.
  std::map<BackendId, std::size_t> counts;
  for (const auto &assignment : pipeline.stages) {
    counts[assignment.backend]++;
  }
  for (const auto &[backendId, count] : counts) {
    if (count == pipeline.stages.size()) {
      for (backend::IBackend *backend : backends_) {
        if (backend->id() == backendId) {
          return backend;
        }
      }
    }
  }
  return nullptr;
}

std::string JobManager::outputNameFor(const JobRecord &record) const {
  const std::string sourceName =
      sanitizeFileName(record.request.source.displayName);
  std::string stem = sourceName;
  const std::size_t dot = stem.rfind('.');
  if (dot != std::string::npos && dot > 0) {
    stem = stem.substr(0, dot);
  }
  // Extraction jobs produce a text sidecar whose extension comes from
  // the track's format (planner-decided), not a media container.
  const std::string ext =
      !record.pipeline.sidecarExtension.empty()
          ? record.pipeline.sidecarExtension
          : containerName(record.pipeline.container);
  std::string name = stem.empty() ? "velmux-output" : stem;
  name += "-velmux." + ext;
  std::string path = config_.dataDir + "/outputs/" + name;
  // Never overwrite: uniquify.
  std::int32_t suffix = 1;
  while (::access(path.c_str(), F_OK) == 0) {
    path = config_.dataDir + "/outputs/" + name + "-" + std::to_string(suffix);
    path += "." + ext;
    ++suffix;
  }
  return path;
}

} // namespace velmux::core
