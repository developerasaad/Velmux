#include "jobs/job_manager.h"

#include "processing/contracts/contracts.h"
#include "compression/compression.h"
#include "processing/hybrid_executor.h"
#include "planning/planner.h"
#include "recovery/recovery.h"
#include "persistence/telemetry.h"
#include "validation/validation.h"

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
                      (c >= '0' && c <= '9') || c == '-' || c == '_';
    out.push_back(safe ? c : '_');
  }
  // No path traversal: strip separators entirely (already excluded above)
  // and leading dots.
  while (!out.empty() && out.front() == '.') {
    out.erase(out.begin());
  }
  return out;
}

// Which backend a pipeline's execution is attributed to: the single
// backend that served every stage, or — for hybrid pipelines — the
// encode/mux side that finalizes the output (the same attribution the
// telemetry PerfRecord uses).
BackendId attributedBackendFor(const Pipeline &pipeline,
                               backend::IBackend *backend, bool hybridPath) {
  if (!hybridPath) {
    return backend != nullptr ? backend->id() : BackendId::Ffmpeg;
  }
  if (pipeline.assigns(StageId::Mux, BackendId::Android)) {
    return BackendId::Android;
  }
  if (pipeline.assigns(StageId::Mux, BackendId::Apple)) {
    return BackendId::Apple;
  }
  return BackendId::Ffmpeg;
}

// Only these error codes mean "the backend attempted execution and
// errored": they are the runtime feedback the capability registry wants.
// Cancels, contract-level rejections (invalid arguments), environmental
// failures (storage full, source gone) and validation failures are not the
// backend's execution fault and must not steer planning.
bool isExecutionFailure(ErrorCode code) {
  return code == ErrorCode::ExecutionFailed || code == ErrorCode::BackendFailure;
}

JobSnapshot snapshotOf(const JobRecord &record) {
  JobSnapshot snapshot;
  snapshot.id = record.id;
  snapshot.submissionId = record.submissionId;
  snapshot.operation = record.request.operation;
  snapshot.state = record.state;
  snapshot.progress = record.progress;
  const MediaSource *source = std::visit(
      [](const auto &payload) -> const MediaSource * {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, job::Merge>) {
          return payload.sources.empty() ? nullptr : &payload.sources.front();
        } else {
          return &payload.source;
        }
      },
      record.request.payload);
  snapshot.sourceName = source != nullptr ? source->displayName : "";
  snapshot.outputName = std::visit(
      [](const auto &payload) {
        return payload.output.name.value_or("");
      },
      record.request.payload);
  snapshot.outputPath = record.outputPath;
  snapshot.outputs = record.outputs;
  snapshot.error = record.error;
  snapshot.errorCode = record.errorCode;
  snapshot.outputSizeBytes = record.outputSizeBytes;
  snapshot.inputSizeBytes = record.inputSizeBytes;
  snapshot.pipelineSummary = record.pipeline.summary;
  snapshot.createdAtMs = record.createdAtMs;
  snapshot.finishedAtMs = record.finishedAtMs;
  return snapshot;
}

// Every source a request references (Merge carries several).
std::vector<MediaSource> sourcesOf(const JobRequest &request) {
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

// True when the request wants a video lane (video-capable operations with
// a video-bearing source).
bool wantsVideoOf(const JobRequest &request, const MediaInfo &info) {
  switch (request.payload.index()) {
    case 0: // VideoConvert
    case 2: // Crop
    case 3: // Trim
      return info.hasVideo();
    default:
      return false;
  }
}

// The compression intent of the payload (nullopt for operations without
// compression).
const CompressionIntent *compressionOf(const JobRequest &request) {
  return std::visit(
      [](const auto &payload) -> const CompressionIntent * {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, job::VideoConvert> ||
                      std::is_same_v<Payload, job::AudioConvert> ||
                      std::is_same_v<Payload, job::ExtractAudio>) {
          return &payload.compression;
        }
        return nullptr;
      },
      request.payload);
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

Result<PlanResult> JobManager::plan(const JobRequest &request) {
  std::vector<MediaInfo> infos;
  for (const MediaSource &source : sourcesOf(request)) {
    const Result<MediaInfo> inspected = inspect(source);
    if (!inspected.ok()) {
      return Result<PlanResult>(inspected.error());
    }
    infos.push_back(inspected.value());
  }
  Result<PlanResult> result = planner_.plan(request, infos);
  if (result.ok()) {
    (void)telemetry_.recordPlanningLatency(result.value().planningMs);
  }
  return result;
}

Result<SampleResult> JobManager::generateSample(
    const JobRequest &request, std::int32_t sampleSeconds,
    const std::string &outputPath) {
  if (sampleSeconds <= 0) {
    return Result<SampleResult>(Error::make(ErrorCode::InvalidArgument,
                                            "sample duration must be positive",
                                            "core"));
  }
  // Only conversion operations preview meaningfully through a short
  // window (mandate §22); other operations refuse honestly.
  const bool conversionShape = request.payload.index() == 0 ||
                               request.payload.index() == 1;
  if (!conversionShape) {
    return Result<SampleResult>(
        Error::make(ErrorCode::UnsupportedOperation,
                    "sample generation is only available for conversion "
                    "operations in this build",
                    "core"));
  }
  // Copy, never reference: sourcesOf returns a temporary vector whose
  // elements die at the end of the full expression (a reference here
  // dangled and crashed sample generation).
  const MediaSource source = sourcesOf(request).front();
  // Duration drives the window; a representative sample needs one.
  const Result<MediaInfo> inspected = inspect(source);
  if (!inspected.ok()) {
    return Result<SampleResult>(inspected.error());
  }
  const MediaInfo &info = inspected.value();
  if (info.durationSec <= 0.0) {
    return Result<SampleResult>(Error::make(
        ErrorCode::InspectionFailed,
        "cannot determine the source duration for sampling", "core"));
  }
  const std::int64_t durationUs = std::int64_t(info.durationSec * 1000000.0);
  const std::int64_t windowLenUs =
      std::min<std::int64_t>(std::int64_t(sampleSeconds) * 1000000,
                             durationUs);

  // Representative window (spec §20): the densest section per a backend's
  // demux-level scan. Mid-file fallback when no backend can pick (avoids
  // the abrupt intro/outro regions by starting a quarter in).
  std::int64_t startUs = std::max<std::int64_t>((durationUs - windowLenUs) / 4, 0);
  std::int64_t endUs = startUs + windowLenUs;
  const std::int32_t windowSeconds =
      std::max<std::int32_t>(std::int32_t(windowLenUs / 1000000), 1);
  for (backend::IBackend *backend : backends_) {
    if (backend->inspector() != nullptr &&
        backend->inspector()->representativeWindow(
            source, windowSeconds, startUs, endUs)) {
      break;
    }
  }

  // Plan against the FULL source so the sample uses exactly the settings
  // the real job would (preset-derived quality, budget, codec routing),
  // then execute only the chosen window through the same dispatch as jobs.
  const Result<PlanResult> planned = planner_.plan(request, {info});
  if (!planned.ok()) {
    return Result<SampleResult>(planned.error());
  }
  if (!planned.value().feasible || !planned.value().best.has_value()) {
    return Result<SampleResult>(Error::make(
        ErrorCode::UnsupportedOperation,
        planned.value().message.empty()
            ? "no compatible pipeline for the requested settings"
            : planned.value().message,
        "core"));
  }
  const Pipeline pipeline = planned.value().best->pipeline;

  backend::IBackend *backend = backendFor(pipeline);
  const bool hybridPath = backend == nullptr && hybrid_ != nullptr &&
                          hybrid_->canExecute(pipeline, request);
  if ((backend == nullptr || backend->executor() == nullptr) && !hybridPath) {
    return Result<SampleResult>(
        Error::make(ErrorCode::UnsupportedOperation,
                    "no backend could execute the planned pipeline", "core"));
  }

  backend::ExecuteParams params;
  params.pipeline = pipeline;
  params.request = request;
  params.outputPath = outputPath;
  // The sample window replaces the request's timeline selection in the
  // executor's internal execution view (samples preview a slice).
  params.sampleWindow = TimeRange{startUs, endUs};
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
    ValidationExpectation expect =
        expectationFor(request, {info}, pipeline);
    expect.expectedDurationSec = 0.0; // a window, not the whole timeline
    if (request.payload.index() == 1) {
      // AudioConvert payload: audio-only sample.
      expect.expectVideo = false;
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

Result<std::string> JobManager::submit(const JobRequest &request,
                                       const std::vector<MediaInfo> &sourceInfos,
                                       const std::string &submissionId) {
  // Durable idempotency (mandate §14.2): a resubmission with the same
  // submission id returns the existing job's id without creating a
  // duplicate.
  if (!submissionId.empty()) {
    Error error;
    if (auto existing = store_.findJobBySubmissionId(submissionId, error)) {
      return Result<std::string>(existing->id);
    }
    if (!error.ok()) {
      return Result<std::string>(error);
    }
  }

  // Validate source accessibility first: never enqueue phantom jobs.
  std::vector<MediaInfo> infos = sourceInfos;
  if (infos.empty()) {
    for (const MediaSource &source : sourcesOf(request)) {
      const Result<MediaInfo> inspected = inspect(source);
      if (!inspected.ok()) {
        return Result<std::string>(inspected.error());
      }
      infos.push_back(inspected.value());
    }
  }

  JobRecord record;
  record.id = generateJobId();
  record.submissionId = submissionId;
  record.state = JobState::Queued;
  record.request = request;
  record.sourceInfo = infos;
  record.inputSizeBytes =
      infos.empty() ? -1 : std::int64_t(infos.front().sizeBytes);
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
      record->state == JobState::TargetNotMet ||
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
  if (record->state != JobState::Processing && record->state != JobState::Queued &&
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
  record->errorCode.clear();
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
      record->state != JobState::TargetNotMet &&
      record->state != JobState::Canceled &&
      record->state != JobState::Interrupted) {
    return Result<void>(Error::make(ErrorCode::InvalidArgument,
                                    "job is not retryable"));
  }
  record->state = JobState::Queued;
  record->error.clear();
  record->errorCode.clear();
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
      record->state != JobState::TargetNotMet &&
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
  RecoveryPlanner recoveryPlanner;
  std::vector<RecoveryDecision> decisions =
      recoveryPlanner.reconcile(resumable, checkpoints_, config_.dataDir);
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
        // the job re-enters the queue honestly. Submission ids survive the
        // round-trip: recovery re-submits cannot duplicate.
        record->state = JobState::Recovering;
        persistAndEmit(*record, "recovery");
        record->state = JobState::Queued;
        record->error.clear();
        record->errorCode.clear();
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
           durable->state == JobState::TargetNotMet ||
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
  event.errorCode = record.errorCode;
  events_.emit(event);
}

void JobManager::setDispatchHooks(
    std::function<bool(const std::string &jobId, const Pipeline &pipeline)>
        gate,
    std::function<void(const std::string &jobId)> onHardwareStarted) {
  dispatchGate_ = std::move(gate);
  hardwareStartedCb_ = std::move(onHardwareStarted);
}

void JobManager::setFailureRecorder(
    std::function<void(const FailedConfigKey &)> recorder) {
  failureRecorder_ = std::move(recorder);
}

JobManager::ExecuteOutcome JobManager::executeJob(const std::string &jobId) {
  Error loadError;
  auto record = store_.loadJob(jobId, loadError);
  if (!record.has_value()) {
    return ExecuteOutcome::Finished;
  }
  if (record->state == JobState::Canceled ||
      record->state == JobState::Completed ||
      record->state == JobState::TargetNotMet ||
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
  MediaInfo info;
  if (!record->sourceInfo.empty()) {
    info = record->sourceInfo.front();
  }
  if (info.durationSec <= 0.0 || (info.hasVideo() && info.video.empty())) {
    const std::vector<MediaSource> sources = sourcesOf(record->request);
    std::vector<MediaInfo> inspected;
    if (!sources.empty()) {
      const Result<MediaInfo> result = inspect(sources.front());
      if (!result.ok()) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!runStillCurrent(jobId, generation)) {
          return ExecuteOutcome::Finished; // superseded by a newer run
        }
        auto &running = running_[jobId];
        running.record.state = JobState::Failed;
        running.record.errorCode = errorCodeName(result.error().code);
        running.record.error =
            "Source became unavailable: " + result.error().message;
        finishFailure(running.record);
        return ExecuteOutcome::Finished;
      }
      inspected.push_back(result.value());
    }
    if (inspected.empty()) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!runStillCurrent(jobId, generation)) {
        return ExecuteOutcome::Finished;
      }
      auto &running = running_[jobId];
      running.record.state = JobState::Failed;
      running.record.errorCode = errorCodeName(ErrorCode::InvalidArgument);
      running.record.error = "the request carries no source";
      finishFailure(running.record);
      return ExecuteOutcome::Finished;
    }
    // Validation compares the output against the inspection the planner
    // used; keep the record consistent so the duration/geometry checks
    // are live after the store round-trip.
    record->sourceInfo = inspected;
    info = inspected.front();
  }

  Result<PlanResult> planned = planner_.plan(record->request, record->sourceInfo);
  if (!planned.ok() || !planned.value().feasible ||
      !planned.value().best.has_value()) {
    std::string message = !planned.ok()
                              ? planned.error().message
                              : planned.value().message;
    const ErrorCode code = !planned.ok()
                               ? planned.error().code
                               : planned.value().errorCode;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!runStillCurrent(jobId, generation)) {
      return ExecuteOutcome::Finished; // superseded by a newer run
    }
    auto &running = running_[jobId];
    running.record.state = JobState::Failed;
    running.record.errorCode = errorCodeName(code);
    running.record.error =
        message.empty() ? "no compatible pipeline" : message;
    finishFailure(running.record);
    return ExecuteOutcome::Finished;
  }
  record->pipeline.planned = true;

  const Pipeline pipeline = planned.value().best->pipeline;
  record->pipeline.summary = pipeline.summary;
  record->pipeline.usesHardware = pipeline.usesHardware;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!runStillCurrent(jobId, generation)) {
      return ExecuteOutcome::Finished; // superseded by a newer run
    }
    running_[jobId].record.pipeline.summary = pipeline.summary;
    running_[jobId].record.pipeline.usesHardware = pipeline.usesHardware;
    running_[jobId].pipeline = pipeline;
  }

  backend::IBackend *backend = backendFor(pipeline);
  const bool hybridPath = backend == nullptr && hybrid_ != nullptr &&
                          hybrid_->canExecute(pipeline, record->request);
  if ((backend == nullptr || backend->executor() == nullptr) && !hybridPath) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!runStillCurrent(jobId, generation)) {
      return ExecuteOutcome::Finished; // superseded by a newer run
    }
    auto &running = running_[jobId];
    running.record.state = JobState::Failed;
    running.record.errorCode = errorCodeName(ErrorCode::NoCompatiblePipeline);
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
  record->outputPath = outputNameFor(*record, pipeline);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!runStillCurrent(jobId, generation)) {
      return ExecuteOutcome::Finished; // superseded by a newer run
    }
    running_[jobId].record.outputPath = record->outputPath;
    running_[jobId].record.state = JobState::Processing;
  }
  persistAndEmit(*record, "processing");

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
  // target-size corrective retry.
  const auto dispatchExecution =
      [&](const JobRequest &req) -> backend::ExecuteResult {
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

  backend::ExecuteResult execResult = dispatchExecution(record->request);

  // Target-size corrective retry (mandate §21): encoders cannot guarantee
  // exact output sizes; when the first attempt lands over the target, ONE
  // re-encode at a proportionally reduced bitrate runs in-place. Bounded
  // to a single retry; a still-missed target completes honestly with
  // TargetNotMet (valid output, honest state) — never a fake guarantee.
  // The retry re-encodes the whole file: the first attempt's checkpoints
  // describe different encoding decisions.
  bool fitRetryRan = false;
  bool fitTargetMissed = false;
  const CompressionIntent *compression = compressionOf(record->request);
  if (execResult.success && compression != nullptr &&
      compression->enabled &&
      compression->preset == CompressionPreset::TargetSize &&
      compression->targetSizeBytes > 0 && !cancel->isCancelled()) {
    const std::int64_t target = compression->targetSizeBytes;
    std::int64_t actual = 0;
    if (fileExistsWithSize(record->outputPath, actual) && actual > target) {
      // Base bitrate: what the first attempt actually used (the resolved
      // policy from the same compression intent the executor applies).
      BudgetInputs budget;
      budget.outputContainer = pipeline.container;
      budget.audioEnabled = !pipeline.audioStreamCopy;
      budget.videoEnabled = wantsVideoOf(record->request, info);
      const ResolvedRatePolicy base =
          resolveRatePolicy(info, *compression, budget);
      std::int64_t baseBitrate = base.mode == ResolvedRatePolicy::Mode::Bitrate
                                     ? base.bitRate
                                     : 0;
      if (baseBitrate <= 0) {
        const BitrateBudget budgetPlan =
            computeBitrateBudget(info, budget, target);
        baseBitrate = budgetPlan.videoBitrate;
      }
      if (baseBitrate > 0) {
        // 10% safety margin under the proportional scale.
        const double scale = (double(target) / double(actual)) * 0.90;
        const std::int64_t newBitrate =
            std::max<std::int64_t>(std::int64_t(double(baseBitrate) * scale),
                                   10000);
        if (newBitrate < baseBitrate) {
          // The retry request: the accepted payload with an explicit
          // Custom bitrate (the only path the executor honors verbatim).
          JobRequest retryRequest = record->request;
          CompressionIntent *retryCompression =
              const_cast<CompressionIntent *>(compressionOf(retryRequest));
          retryCompression->preset = CompressionPreset::Custom;
          retryCompression->videoBitrate = newBitrate;
          retryCompression->quality.reset();
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
    perf.backend = attributedBackendFor(pipeline, backend, hybridPath);
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
    perf.setupMs = planned.value().best->setupLatencyMs;
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
    if (running.record.state == JobState::Processing ||
        running.record.state == JobState::Preparing ||
        running.record.state == JobState::Validating ||
        running.record.state == JobState::Finalizing) {
      running.record.state = JobState::Canceled;
      running.record.errorCode = errorCodeName(ErrorCode::Cancelled);
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
    // Runtime failure feedback (spec §15): an execution-attributable
    // backend error marks the exact failed configuration so the next plan
    // avoids that route. Computed from the same request the plan used
    // (the corrective retry changes bitrates only, which are not part
    // of the narrow key).
    std::optional<FailedConfigKey> failedKey;
    if (isExecutionFailure(execResult.error.code)) {
      failedKey = failedConfigKeyFor(
          attributedBackendFor(pipeline, backend, hybridPath), info,
          record->request, pipeline.videoCodec);
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!runStillCurrent(jobId, generation)) {
        return ExecuteOutcome::Finished; // superseded by a newer run
      }
      auto &running = running_[jobId];
      running.record.state = JobState::Failed;
      running.record.errorCode = errorCodeName(execResult.error.code);
      running.record.error = execResult.error.message.empty()
                                 ? "execution failed"
                                 : execResult.error.message;
      finishFailure(running.record);
    }
    // Leaf call into the capability registry (its own lock), made outside
    // the manager lock so recording can never deadlock against it.
    if (failedKey.has_value() && failureRecorder_) {
      failureRecorder_(*failedKey);
    }
    return ExecuteOutcome::Finished;
  }

  // Multi-output operations (Split / ExtractFrames): the primary artifact
  // is the executor's first manifest entry (the JobSnapshot contract:
  // "primary output (first manifest entry)") — the planned path only
  // served as those artifacts' naming stem. Point outputPath at the real
  // primary BEFORE validation so the validator reads the finished
  // artifact, not the never-written planned name.
  if (!execResult.outputs.empty()) {
    record->outputPath = execResult.outputs.front().path;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!runStillCurrent(jobId, generation)) {
        return ExecuteOutcome::Finished; // superseded; the newer run owns
                                         // finalization
      }
      running_[jobId].record.outputPath = record->outputPath;
    }
  }

  // Execution finished: the artifacts are finalized (segment scratch
  // removed by the executor) before output validation reads them.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!runStillCurrent(jobId, generation)) {
      return ExecuteOutcome::Finished; // superseded by a newer run
    }
    running_[jobId].record.state = JobState::Finalizing;
  }
  persistAndEmit(*record, "finalizing");

  // Validation before completion (never trust pipeline success alone).
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!runStillCurrent(jobId, generation)) {
      return ExecuteOutcome::Finished; // superseded by a newer run
    }
    running_[jobId].record.state = JobState::Validating;
  }
  persistAndEmit(*record, "validating");
  if (!validateOutput(*record, pipeline, generation)) {
    return ExecuteOutcome::Finished; // finishFailure already ran (or the run
                                     // was superseded)
  }

  // Outputs manifest: the executor's multi-output manifest when filled
  // (Split / ExtractFrames / batch extraction, later phases); conversion
  // executors leave it empty and the primary output entry comes from the
  // output path.
  std::int64_t outputBytes = -1;
  fileExistsWithSize(record->outputPath, outputBytes);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!runStillCurrent(jobId, generation)) {
      return ExecuteOutcome::Finished; // superseded by a newer run
    }
    auto &running = running_[jobId];
    // The executor confirms the actually-taken strategy in its runtime
    // summary (keyframe-safe copy vs normalized transcode, actual part
    // count, dropped lanes...); the planned summary was only a prediction.
    if (!execResult.pipelineSummary.empty()) {
      running.record.pipeline.summary = execResult.pipelineSummary;
    }
    if (execResult.outputs.empty()) {
      if (!record->outputPath.empty()) {
        JobOutput primary;
        primary.path = record->outputPath;
        primary.sizeBytes = outputBytes;
        primary.kind = pipeline.sidecarExtension.empty() ? "media" : "subtitle";
        running.record.outputs = {std::move(primary)};
      }
    } else {
      running.record.outputs = execResult.outputs;
    }
    if (fitRetryRan) {
      // Honest target-size outcome in the pipeline summary (shown in
      // history and job details): a retry happened, and whether it landed
      // under the target. Never a fake guarantee.
      running.record.pipeline.summary +=
          fitTargetMissed
              ? " [target-size: target not reached after retry]"
              : " [target-size: retried at a lower bitrate]";
    }
    // TargetNotMet (mandate §21/§37): validation succeeded and the output
    // is GOOD, but an accepted target-size objective was still exceeded
    // after the corrective retry. Honest completion, never fake success.
    const bool targetMissed =
        compression != nullptr && compression->enabled &&
        compression->preset == CompressionPreset::TargetSize &&
        compression->targetSizeBytes > 0 && outputBytes > 0 &&
        outputBytes > compression->targetSizeBytes;
    running.record.state =
        targetMissed ? JobState::TargetNotMet : JobState::Completed;
    if (targetMissed) {
      running.record.errorCode = errorCodeName(ErrorCode::TargetNotMet);
      running.record.error =
          "valid output, but the target size was not reached: " +
          std::to_string(outputBytes) + " bytes > " +
          std::to_string(compression->targetSizeBytes) + " bytes";
    }
    running.record.progress = 1.0;
    running.record.outputSizeBytes = outputBytes;
    if (!running.record.outputs.empty() &&
        running.record.outputs.front().path == record->outputPath) {
      running.record.outputs.front().sizeBytes = outputBytes;
    }
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
  if (record.errorCode.empty()) {
    record.errorCode = errorCodeName(ErrorCode::ExecutionFailed);
  }
  JobRecord final = record;
  const std::string jobId = record.id;
  running_.erase(jobId);
  persistAndEmit(final, "failed");
}

bool JobManager::validateOutput(JobRecord &record, const Pipeline &pipeline,
                                std::uint64_t generation) {
  if (validator_ == nullptr) {
    // No validator wired: honest failure rather than fake success.
    std::lock_guard<std::mutex> lock(mutex_);
    if (!runStillCurrent(record.id, generation)) {
      return false; // superseded; the newer run owns finalization
    }
    auto &running = running_[record.id];
    running.record.state = JobState::Failed;
    running.record.errorCode = errorCodeName(ErrorCode::InternalError);
    running.record.error = "output validator unavailable";
    finishFailure(running.record);
    return false;
  }
  const ValidationExpectation expect =
      expectationFor(record.request, record.sourceInfo, pipeline);
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
  running.record.errorCode = errorCodeName(ErrorCode::OutputValidationFailed);
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

std::string JobManager::outputNameFor(const JobRecord &record,
                                      const Pipeline &pipeline) const {
  const std::string sourceName = std::visit(
      [](const auto &payload) {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, job::Merge>) {
          return payload.sources.empty() ? std::string()
                                         : payload.sources.front().displayName;
        } else {
          return payload.source.displayName;
        }
      },
      record.request.payload);
  std::string stem = sanitizeFileName(sourceName);
  const std::size_t dot = stem.rfind('.');
  if (dot != std::string::npos && dot > 0) {
    stem = stem.substr(0, dot);
  }
  // Extraction jobs produce a text sidecar whose extension comes from
  // the track's format (planner-decided), not a media container.
  const std::string ext = !pipeline.sidecarExtension.empty()
                              ? pipeline.sidecarExtension
                              : containerName(pipeline.container);
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
