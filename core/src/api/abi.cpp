// Velmux — public C ABI v2 implementation (mandate §35).
//
// Opaque engine handle, explicit string ownership, structured error JSON
// {"error":{"code":"<errorCodeName>","message":".."}}, bounded event tap
// with drop-oldest, backend registration for the backends compiled into
// this engine, sessions + typed submit + reuse settings + job control.
// Payload codecs live in core/src/api/serialization.{h,cpp} — one codec
// shared with the durable job store; the ABI never re-implements parsing.
#define VELMUX_BRIDGE_CXX_HELPERS 1
#include <velmux/abi.h>
#include <velmux/engine.h>
#include "serialization/json.h"
#include "api/serialization.h"

#if defined(VELMUX_WITH_FFMPEG_BACKEND)
#include "processing/ffmpeg/ffmpeg_backend.h"
#endif

#if defined(VELMUX_WITH_ANDROID_BACKEND)
#include "android_backend.h"
#endif
#if defined(VELMUX_WITH_APPLE_BACKEND)
#include "apple_backend.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <vector>

namespace velmux::bridge {

using namespace velmux::core;

std::string jsonEscape(const std::string &text) {
  return ::velmux::core::json::escape(text);
}

std::string quoted(const std::string &s) {
  return "\"" + jsonEscape(s) + "\"";
}

// The shared JSON reader (defensive; host input is untrusted).
using JsonValue = ::velmux::core::json::JsonValue;
using JsonReader = ::velmux::core::json::JsonReader;

// Parses a JSON object from a C string; returns nullptr and fills `error`
// on malformed input (never trusts the host).
const JsonValue *parseObject(const char *text, JsonValue &storage,
                             std::string &error) {
  if (text == nullptr) {
    error = "payload is null";
    return nullptr;
  }
  const std::string jsonText(text);
  JsonReader reader(jsonText);
  const std::string parseError = reader.parse(storage);
  if (!parseError.empty()) {
    error = "invalid JSON: " + parseError;
    return nullptr;
  }
  if (storage.kind != JsonValue::Kind::Object) {
    error = "payload must be a JSON object";
    return nullptr;
  }
  return &storage;
}

std::string optStringField(const JsonValue &parent, const char *key) {
  if (const JsonValue *field = parent.find(key);
      field != nullptr && field->kind == JsonValue::Kind::String) {
    return field->text;
  }
  return std::string();
}

std::int64_t optIntField(const JsonValue &parent, const char *key) {
  if (const JsonValue *field = parent.find(key);
      field != nullptr && field->isNumberLike()) {
    return field->asInt();
  }
  return 0;
}

} // namespace velmux::bridge

// ---------------------------------------------------------------------------
// C facade
// ---------------------------------------------------------------------------

struct VelmuxEngineHandle {
  std::unique_ptr<velmux::core::VelmuxEngine> engine;

  // Event tap: queues engine events for host polling (bounded).
  static constexpr std::size_t kMaxQueuedEvents = 1024;
  std::mutex eventsMutex;
  std::deque<velmux::core::JobEvent> eventQueue;

  struct EventTap final : public velmux::core::IJobEventSink {
    VelmuxEngineHandle *owner;
    void onJobEvent(const velmux::core::JobEvent &event) override {
      std::lock_guard<std::mutex> lock(owner->eventsMutex);
      if (owner->eventQueue.size() >= kMaxQueuedEvents) {
        owner->eventQueue.pop_front(); // drop oldest (progress spam)
      }
      owner->eventQueue.push_back(event);
    }
  };
  std::unique_ptr<EventTap> tap;
};

using namespace velmux::core;
namespace bridgeNS = velmux::bridge;
using bridgeNS::JsonValue;
using bridgeNS::JsonReader;

extern "C" {

int velmux_abi_version(void) { return VELMUX_ABI_VERSION; }

const char *velmux_engine_version(void) { return "1.0.0"; }

VelmuxEngineHandle *velmux_engine_create(const char *dataDir) {
  if (dataDir == nullptr || dataDir[0] == '\0') {
    return nullptr;
  }
  EngineConfig config;
  config.dataDir = dataDir;
  std::vector<std::unique_ptr<velmux::backend::IBackend>> backends;
  // Backends compiled into this engine register themselves; the engine
  // runs with whatever honestly initialized (failure isolation).
#if defined(VELMUX_WITH_FFMPEG_BACKEND)
  backends.push_back(std::make_unique<velmux::ffmpeg::FfmpegBackend>());
#endif
#if defined(VELMUX_WITH_ANDROID_BACKEND)
  backends.push_back(std::make_unique<velmux::android::AndroidBackend>());
#endif
#if defined(VELMUX_WITH_APPLE_BACKEND)
  backends.push_back(std::make_unique<velmux::apple::AppleBackend>());
#endif
  if (backends.empty()) {
    // No execution backend at all: refuse to create an engine that could
    // only ever fail. Hosts render an honest setup error.
    return nullptr;
  }
  auto engine = VelmuxEngine::create(config, std::move(backends));
  if (engine == nullptr) {
    return nullptr;
  }
  auto *handle = new (std::nothrow) VelmuxEngineHandle();
  if (handle == nullptr) {
    return nullptr;
  }
  handle->engine = std::move(engine);
  handle->tap = std::make_unique<VelmuxEngineHandle::EventTap>();
  handle->tap->owner = handle;
  handle->engine->events().subscribe(handle->tap.get());
  return handle;
}

void velmux_engine_destroy(VelmuxEngineHandle *handle) {
  if (handle != nullptr) {
    if (handle->engine != nullptr && handle->tap != nullptr) {
      handle->engine->events().unsubscribe(handle->tap.get());
    }
    delete handle;
  }
}

static char *allocString(const std::string &s) {
  char *buf = static_cast<char *>(std::malloc(s.size() + 1));
  if (buf != nullptr) {
    std::memcpy(buf, s.c_str(), s.size() + 1);
  }
  return buf;
}

static std::string errorJson(ErrorCode code, const std::string &message) {
  return "{\"error\":{\"code\":" + bridgeNS::quoted(errorCodeName(code)) +
         ",\"message\":" + bridgeNS::quoted(message) + "}}";
}

static std::string errorOf(const Error &error) {
  return errorJson(error.code, error.message.empty()
                                   ? std::string("operation failed")
                                   : error.message);
}

// ---------------------------------------------------------------------------
// Sessions (mandate §14)
// ---------------------------------------------------------------------------

char *velmux_create_session(VelmuxEngineHandle *handle,
                            const char *operationJson) {
  if (handle == nullptr || operationJson == nullptr) {
    return allocString(
        errorJson(ErrorCode::InvalidArgument, "engine or payload is null"));
  }
  JsonValue root;
  std::string parseError;
  if (bridgeNS::parseObject(operationJson, root, parseError) == nullptr) {
    return allocString(
        errorJson(ErrorCode::InvalidArgument, parseError));
  }
  const std::string operationName =
      bridgeNS::optStringField(root, "operation");
  const std::optional<Operation> operation =
      operationFromName(operationName);
  if (!operation.has_value()) {
    return allocString(errorJson(ErrorCode::InvalidArgument,
                                 "unknown operation: " + operationName));
  }
  std::vector<MediaSource> sources;
  if (const JsonValue *sourcesValue = root.find("sources");
      sourcesValue != nullptr &&
      sourcesValue->kind == JsonValue::Kind::Array) {
    for (const JsonValue &entry : sourcesValue->items) {
      if (entry.kind != JsonValue::Kind::Object) {
        continue;
      }
      MediaSource source;
      source.uri = bridgeNS::optStringField(entry, "uri");
      source.displayName = bridgeNS::optStringField(entry, "name");
      if (source.uri.empty()) {
        return allocString(
            errorJson(ErrorCode::InvalidArgument, "source uri is empty"));
      }
      if (source.displayName.empty()) {
        source.displayName = "Untitled media";
      }
      sources.push_back(std::move(source));
    }
  }
  const Result<std::string> sessionId =
      handle->engine->createSession(*operation, std::move(sources));
  if (!sessionId.ok()) {
    return allocString(errorOf(sessionId.error()));
  }
  return allocString("{\"sessionId\":" + bridgeNS::quoted(sessionId.value()) +
                     "}");
}

char *velmux_prepare_session(VelmuxEngineHandle *handle,
                             const char *sessionId) {
  if (handle == nullptr || sessionId == nullptr) {
    return allocString(
        errorJson(ErrorCode::InvalidArgument, "engine or sessionId is null"));
  }
  const Result<SessionSnapshot> snapshot =
      handle->engine->prepareSession(sessionId);
  if (!snapshot.ok()) {
    return allocString(errorOf(snapshot.error()));
  }
  return allocString(sessionSnapshotToJson(snapshot.value()));
}

char *velmux_get_session_snapshot(VelmuxEngineHandle *handle,
                                  const char *sessionId) {
  if (handle == nullptr || sessionId == nullptr) {
    return allocString(
        errorJson(ErrorCode::InvalidArgument, "engine or sessionId is null"));
  }
  const Result<SessionSnapshot> snapshot =
      handle->engine->getSessionSnapshot(sessionId);
  if (!snapshot.ok()) {
    return allocString(errorOf(snapshot.error()));
  }
  return allocString(sessionSnapshotToJson(snapshot.value()));
}

char *velmux_get_session_options(VelmuxEngineHandle *handle,
                                 const char *sessionId,
                                 const char *fieldJson) {
  if (handle == nullptr || sessionId == nullptr || fieldJson == nullptr) {
    return allocString(
        errorJson(ErrorCode::InvalidArgument, "engine or payload is null"));
  }
  JsonValue root;
  std::string parseError;
  if (bridgeNS::parseObject(fieldJson, root, parseError) == nullptr) {
    return allocString(errorJson(ErrorCode::InvalidArgument, parseError));
  }
  const std::string fieldName = bridgeNS::optStringField(root, "field");
  const std::optional<FieldId> field = fieldIdFromName(fieldName);
  if (!field.has_value()) {
    return allocString(errorJson(ErrorCode::InvalidArgument,
                                 "unknown field: " + fieldName));
  }
  const std::int64_t revision = bridgeNS::optIntField(root, "revision");
  const Result<FieldOptions> options = handle->engine->getSessionOptions(
      sessionId, *field, std::uint64_t(revision));
  if (!options.ok()) {
    return allocString(errorOf(options.error()));
  }
  return allocString(fieldOptionsToJson(options.value()));
}

char *velmux_update_session(VelmuxEngineHandle *handle,
                            const char *sessionId, const char *updateJson) {
  if (handle == nullptr || sessionId == nullptr || updateJson == nullptr) {
    return allocString(
        errorJson(ErrorCode::InvalidArgument, "engine or payload is null"));
  }
  std::string parseError;
  const std::optional<SessionUpdateParse> parsed =
      sessionUpdateFromJson(updateJson, parseError);
  if (!parsed.has_value()) {
    return allocString(errorJson(ErrorCode::InvalidArgument, parseError));
  }
  const Result<SessionSnapshot> snapshot = handle->engine->updateSession(
      sessionId, parsed->revision, parsed->changes,
      parsed->acceptRelatedChange);
  if (!snapshot.ok()) {
    return allocString(errorOf(snapshot.error()));
  }
  return allocString(sessionSnapshotToJson(snapshot.value()));
}

char *velmux_generate_session_sample(VelmuxEngineHandle *handle,
                                     const char *sessionId,
                                     const char *sampleJson) {
  if (handle == nullptr || sessionId == nullptr || sampleJson == nullptr) {
    return allocString(
        errorJson(ErrorCode::InvalidArgument, "engine or payload is null"));
  }
  JsonValue root;
  std::string parseError;
  if (bridgeNS::parseObject(sampleJson, root, parseError) == nullptr) {
    return allocString(errorJson(ErrorCode::InvalidArgument, parseError));
  }
  const std::int64_t revision = bridgeNS::optIntField(root, "revision");
  const Result<SampleResult> sample =
      handle->engine->generateSessionSample(sessionId,
                                            std::uint64_t(revision));
  if (!sample.ok()) {
    return allocString(errorOf(sample.error()));
  }
  return allocString(sampleResultToJson(sample.value()));
}

char *velmux_submit_session(VelmuxEngineHandle *handle,
                            const char *sessionId, const char *submitJson) {
  if (handle == nullptr || sessionId == nullptr || submitJson == nullptr) {
    return allocString(
        errorJson(ErrorCode::InvalidArgument, "engine or payload is null"));
  }
  JsonValue root;
  std::string parseError;
  if (bridgeNS::parseObject(submitJson, root, parseError) == nullptr) {
    return allocString(errorJson(ErrorCode::InvalidArgument, parseError));
  }
  const std::int64_t revision = bridgeNS::optIntField(root, "revision");
  const std::string submissionId =
      bridgeNS::optStringField(root, "submissionId");
  const Result<std::string> jobId = handle->engine->submitSession(
      sessionId, std::uint64_t(revision), submissionId);
  if (!jobId.ok()) {
    return allocString(errorOf(jobId.error()));
  }
  return allocString("{\"jobId\":" + bridgeNS::quoted(jobId.value()) + "}");
}

int velmux_close_session(VelmuxEngineHandle *handle, const char *sessionId) {
  if (handle == nullptr || sessionId == nullptr) {
    return -1;
  }
  const Result<void> closed = handle->engine->closeSession(sessionId);
  return closed.ok() ? 0 : -1;
}

// ---------------------------------------------------------------------------
// Direct submission (typed request JSON)
// ---------------------------------------------------------------------------

char *velmux_submit_job(VelmuxEngineHandle *handle, const char *requestJson) {
  if (handle == nullptr || requestJson == nullptr) {
    return allocString(
        errorJson(ErrorCode::InvalidArgument, "engine or request is null"));
  }
  std::string parseError;
  const std::optional<DecodedRequest> request =
      requestFromJson(requestJson, parseError);
  if (!request.has_value()) {
    return allocString(errorJson(ErrorCode::InvalidArgument, parseError));
  }
  const Result<std::string> jobId = handle->engine->submitJob(
      request->request, std::vector<MediaInfo>{}, request->submissionId);
  if (!jobId.ok()) {
    return allocString(errorOf(jobId.error()));
  }
  return allocString("{\"jobId\":" + bridgeNS::quoted(jobId.value()) + "}");
}

// ---------------------------------------------------------------------------
// Jobs
// ---------------------------------------------------------------------------

char *velmux_list_jobs(VelmuxEngineHandle *handle) {
  if (handle == nullptr) {
    return allocString(
        errorJson(ErrorCode::InvalidArgument, "engine is null"));
  }
  std::string out = "[";
  const auto jobs = handle->engine->listJobs();
  for (std::size_t i = 0; i < jobs.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    out += jobSnapshotToJson(jobs[i]);
  }
  out += "]";
  return allocString(out);
}

char *velmux_get_job(VelmuxEngineHandle *handle, const char *jobId) {
  if (handle == nullptr || jobId == nullptr) {
    return allocString(
        errorJson(ErrorCode::InvalidArgument, "engine or jobId is null"));
  }
  const auto snapshot = handle->engine->getJob(jobId);
  if (!snapshot.has_value()) {
    return allocString("null");
  }
  return allocString(jobSnapshotToJson(*snapshot));
}

int velmux_is_job_active(VelmuxEngineHandle *handle, const char *jobId) {
  if (handle == nullptr || jobId == nullptr) {
    return -1;
  }
  return handle->engine->isJobActive(jobId) ? 1 : 0;
}

int velmux_cancel_job(VelmuxEngineHandle *handle, const char *jobId) {
  if (handle == nullptr || jobId == nullptr) {
    return -1;
  }
  const Result<bool> result = handle->engine->cancelJob(jobId);
  return result.ok() ? (result.value() ? 1 : 0) : -1;
}

int velmux_pause_job(VelmuxEngineHandle *handle, const char *jobId) {
  if (handle == nullptr || jobId == nullptr) {
    return -1;
  }
  const Result<bool> result = handle->engine->pauseJob(jobId);
  return result.ok() ? (result.value() ? 1 : 0) : -1;
}

int velmux_resume_job(VelmuxEngineHandle *handle, const char *jobId) {
  if (handle == nullptr || jobId == nullptr) {
    return -1;
  }
  const Result<bool> result = handle->engine->resumeJob(jobId);
  return result.ok() ? (result.value() ? 1 : 0) : -1;
}

int velmux_retry_job(VelmuxEngineHandle *handle, const char *jobId) {
  if (handle == nullptr || jobId == nullptr) {
    return -1;
  }
  const Result<bool> result = handle->engine->retryJob(jobId);
  return result.ok() ? (result.value() ? 1 : 0) : -1;
}

int velmux_delete_job(VelmuxEngineHandle *handle, const char *jobId) {
  if (handle == nullptr || jobId == nullptr) {
    return -1;
  }
  const Result<bool> result = handle->engine->deleteJob(jobId);
  return result.ok() ? (result.value() ? 1 : 0) : -1;
}

char *velmux_reuse_job_settings(VelmuxEngineHandle *handle,
                                const char *jobId) {
  if (handle == nullptr || jobId == nullptr) {
    return allocString(
        errorJson(ErrorCode::InvalidArgument, "engine or jobId is null"));
  }
  const Result<std::string> sessionId =
      handle->engine->reuseJobSettings(jobId);
  if (!sessionId.ok()) {
    return allocString(errorOf(sessionId.error()));
  }
  return allocString("{\"sessionId\":" + bridgeNS::quoted(sessionId.value()) +
                     "}");
}

// ---------------------------------------------------------------------------
// Inspection + diagnostics + platform integration
// ---------------------------------------------------------------------------

char *velmux_inspect(VelmuxEngineHandle *handle, const char *uri,
                     const char *displayName) {
  if (handle == nullptr || uri == nullptr) {
    return allocString(
        errorJson(ErrorCode::InvalidArgument, "engine or uri is null"));
  }
  MediaSource source{uri, displayName != nullptr ? displayName : ""};
  const Result<MediaInfo> info = handle->engine->inspect(source);
  if (!info.ok()) {
    return allocString(errorOf(info.error()));
  }
  return allocString(mediaInfoToJson(info.value()));
}

char *velmux_describe_backends(VelmuxEngineHandle *handle) {
  if (handle == nullptr) {
    return allocString(
        errorJson(ErrorCode::InvalidArgument, "engine is null"));
  }
  std::string out = "[";
  const auto descriptions = handle->engine->describeBackends();
  for (std::size_t i = 0; i < descriptions.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    out += bridgeNS::quoted(descriptions[i]);
  }
  out += "]";
  return allocString(out);
}

int velmux_update_resources(VelmuxEngineHandle *handle, const char *json) {
  if (handle == nullptr || json == nullptr) {
    return -1;
  }
  bridgeNS::JsonValue root;
  // The reader stores a const reference: materialize the std::string so it
  // outlives the reader (binding directly to `json` would dangle on the
  // implicit temporary and read freed memory for non-SSO payloads).
  std::string parseError;
  if (bridgeNS::parseObject(json, root, parseError) == nullptr) {
    return -1;
  }
  ResourceSnapshot snapshot;
  if (const bridgeNS::JsonValue *cores = root.find("cpuCores");
      cores != nullptr && cores->isNumberLike()) {
    snapshot.cpuCores = std::int32_t(cores->asInt());
  }
  if (const bridgeNS::JsonValue *memory = root.find("memoryAvailableMb");
      memory != nullptr && memory->isNumberLike()) {
    snapshot.memoryAvailableMb = memory->asInt();
  }
  if (const bridgeNS::JsonValue *thermal = root.find("thermalPressure");
      thermal != nullptr && thermal->isNumberLike()) {
    snapshot.thermalPressure = thermal->asDouble();
  }
  if (const bridgeNS::JsonValue *battery = root.find("onBattery");
      battery != nullptr && battery->kind == bridgeNS::JsonValue::Kind::Bool) {
    snapshot.onBattery = battery->boolValue;
  }
  if (const bridgeNS::JsonValue *hardware = root.find("hardwareCodecAvailable");
      hardware != nullptr &&
      hardware->kind == bridgeNS::JsonValue::Kind::Bool) {
    snapshot.hardwareCodecAvailable = hardware->boolValue;
  }
  handle->engine->updateResources(snapshot);
  return 0;
}

char *velmux_poll_events(VelmuxEngineHandle *handle) {
  if (handle == nullptr) {
    return allocString(
        errorJson(ErrorCode::InvalidArgument, "engine is null"));
  }
  std::deque<JobEvent> drained;
  {
    std::lock_guard<std::mutex> lock(handle->eventsMutex);
    drained.swap(handle->eventQueue);
  }
  std::string out = "[";
  for (std::size_t i = 0; i < drained.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    out += jobEventToJson(drained[i]);
  }
  out += "]";
  return allocString(out);
}

void velmux_free_string(char *str) { std::free(str); }

} // extern "C"
