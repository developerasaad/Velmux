#include "velmux/core/job_manager.h"

#include "velmux/backend/contracts.h"
#include "velmux/core/compression.h"
#include "velmux/core/planner.h"
#include "velmux/core/recovery.h"
#include "velmux/core/telemetry.h"
#include "velmux/core/validation.h"

#include <atomic>
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
      : store_(store), jobId_(std::move(jobId)), cancel_(std::move(cancel)) {}

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

JobManager::JobManager(EngineConfig config, EventBus &events, JobStore &store,
                       CheckpointStore &checkpoints, Planner &planner,
                       TelemetryStore &telemetry)
    : config_(std::move(config)), events_(events), store_(store),
      checkpoints_(checkpoints), planner_(planner), telemetry_(telemetry) {}

JobManager::~JobManager() = default;

void JobManager::setBackends(std::vector<backend::IBackend *> backends) {
  backends_ = std::move(backends);
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

Result<std::string> JobManager::submit(const ConversionRequest &request,
                                       std::int32_t priority) {
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
  if (running != running_.end() && running->second.cancel != nullptr) {
    running->second.cancel->cancel();
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
  if (running != running_.end() && running->second.cancel != nullptr) {
    // Cooperative pause = cancel the current execution; the durable record
    // keeps committed checkpoints, and resume restarts from the safest
    // checkpoint (spec: no fake in-place encoder suspension).
    running->second.cancel->cancel();
    record->state = JobState::Paused;
  } else {
    record->state = JobState::Paused;
  }
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
                                std::int64_t outputBytes) {
  std::lock_guard<std::mutex> lock(mutex_);
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

void JobManager::executeJob(const std::string &jobId) {
  Error loadError;
  auto record = store_.loadJob(jobId, loadError);
  if (!record.has_value()) {
    return;
  }
  if (record->state == JobState::Canceled ||
      record->state == JobState::Completed) {
    return;
  }

  record->state = JobState::Preparing;
  record->startedAtMs = nowMs();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    RunningJob running;
    running.record = *record;
    running.cancel = std::make_shared<backend::CancelToken>();
    running_[jobId] = std::move(running);
  }
  persistAndEmit(*record, "preparing");

  // Inspect (or reuse submitted inspection) then plan.
  MediaInfo info = record->sourceInfo;
  if (info.durationSec <= 0.0 || (info.hasVideo() && info.video.empty())) {
    const Result<MediaInfo> inspected = inspect(record->request.source);
    if (!inspected.ok()) {
      std::lock_guard<std::mutex> lock(mutex_);
      auto &running = running_[jobId];
      running.record.state = JobState::Failed;
      running.record.error =
          "Source became unavailable: " + inspected.error().message;
      finishFailure(running.record);
      return;
    }
    info = inspected.value();
  }

  PlanResult plan = planner_.plan(info, record->request);
  if (!plan.feasible || !plan.best.has_value()) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto &running = running_[jobId];
    running.record.state = JobState::Failed;
    running.record.error =
        plan.message.empty() ? "no compatible pipeline" : plan.message;
    finishFailure(running.record);
    return;
  }

  const Pipeline pipeline = plan.best->pipeline;
  record->pipeline = pipeline;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    running_[jobId].record.pipeline = pipeline;
  }

  backend::IBackend *backend = backendFor(pipeline);
  if (backend == nullptr || backend->executor() == nullptr) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto &running = running_[jobId];
    running.record.state = JobState::Failed;
    running.record.error = "no backend could execute the planned pipeline";
    finishFailure(running.record);
    return;
  }

  // Output path: sanitized, never overwrites the original.
  record->outputPath = outputNameFor(*record);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    running_[jobId].record.outputPath = record->outputPath;
    running_[jobId].record.state = JobState::Running;
  }
  persistAndEmit(*record, "running");

  backend::ExecuteParams params;
  params.pipeline = pipeline;
  params.request = record->request;
  params.outputPath = record->outputPath;
  std::shared_ptr<backend::CancelToken> cancel;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    cancel = running_[jobId].cancel;
  }
  params.cancel = cancel;
  params.onProgress = [this, jobId](const backend::ProgressReport &report) {
    reportProgress(jobId, report.fraction, -1);
  };
  CheckpointCoordinator coordinator(checkpoints_, jobId, cancel);
  params.checkpoints = &coordinator;

  const backend::ExecuteResult execResult = backend->executor()->execute(params);

  // Feed real observations into telemetry (performance learning).
  if (execResult.wallClockSec > 0.0) {
    PerfRecord perf;
    perf.backend = backend->id();
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
    auto &running = running_[jobId];
    // Pause keeps checkpoints for resume; explicit cancel is terminal.
    running.record.state = running.record.state == JobState::Paused
                               ? JobState::Paused
                               : JobState::Canceled;
    if (running.record.state == JobState::Canceled) {
      running.record.finishedAtMs = nowMs();
    }
    JobRecord final = running.record;
    running_.erase(jobId);
    persistAndEmit(final, "canceled");
    return;
  }

  if (!execResult.success) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto &running = running_[jobId];
    running.record.state = JobState::Failed;
    running.record.error = execResult.error.message.empty()
                               ? "execution failed"
                               : execResult.error.message;
    finishFailure(running.record);
    return;
  }

  // Validation before completion (never trust pipeline success alone).
  {
    std::lock_guard<std::mutex> lock(mutex_);
    running_[jobId].record.state = JobState::Validating;
  }
  persistAndEmit(*record, "validating");
  if (!validateOutput(*record)) {
    return; // finishFailure already ran
  }

  std::int64_t outputBytes = -1;
  fileExistsWithSize(record->outputPath, outputBytes);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto &running = running_[jobId];
    running.record.state = JobState::Completed;
    running.record.progress = 1.0;
    running.record.outputSizeBytes = outputBytes;
    running.record.finishedAtMs = nowMs();
    JobRecord final = running.record;
    running_.erase(jobId);
    persistAndEmit(final, "completed");
  }
}

void JobManager::finishFailure(JobRecord &record) {
  record.finishedAtMs = nowMs();
  JobRecord final = record;
  const std::string jobId = record.id;
  running_.erase(jobId);
  persistAndEmit(final, "failed");
}

bool JobManager::validateOutput(JobRecord &record) {
  if (validator_ == nullptr) {
    // No validator wired: honest failure rather than fake success.
    std::lock_guard<std::mutex> lock(mutex_);
    auto &running = running_[record.id];
    running.record.state = JobState::Failed;
    running.record.error = "output validator unavailable";
    finishFailure(running.record);
    return false;
  }
  ValidationExpectation expect;
  expect.container = record.pipeline.container;
  if (record.request.video.enabled) {
    expect.videoCodec = record.pipeline.videoCodec;
    if (!record.sourceInfo.video.empty()) {
      expect.expectedHeight = record.request.video.resolutionHeight.value_or(
          record.sourceInfo.video.front().height);
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
  expect.expectedDurationSec = record.sourceInfo.durationSec;

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
  const std::string ext =
      containerName(record.pipeline.container);
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
