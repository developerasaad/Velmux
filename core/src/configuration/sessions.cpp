// Velmux core — configuration session manager implementation (mandate §14).
//
// Sessions are the authoritative configuration surface: the UI renders
// session snapshots, queries supported options and submits updates; the
// core validates everything and owns the accepted state. Updates are
// transactional with revision control and provenance tracking.
#include "configuration/sessions.h"

#include "capabilities/capabilities.h"
#include "catalog/features.h"
#include "serialization/json.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>
#include <sstream>

namespace velmux::core {

using json::JsonValue;
using json::JsonReader;

namespace {

// Standard frame rates offered as options (mandate §19.4: rational
// representation; 23.976 = 24000/1001 etc.).
struct RateOption {
  const char *id;
  Rational rate;
};
constexpr RateOption kRateOptions[] = {
    {"24000/1001", {24000, 1001}}, {"24/1", {24, 1}},     {"25/1", {25, 1}},
    {"30000/1001", {30000, 1001}}, {"30/1", {30, 1}},     {"48/1", {48, 1}},
    {"50/1", {50, 1}},             {"60000/1001", {60000, 1001}},
    {"60/1", {60, 1}},
};

// Standard audio sample rates (mandate §31.2).
constexpr std::int32_t kSampleRates[] = {
    8000, 16000, 22050, 24000, 32000, 44100, 48000, 88200, 96000,
};

// Resolution ladder bounds (mandate §19.3).
struct ResolutionOption {
  const char *id;
  std::int32_t landscapeWidth;
  std::int32_t landscapeHeight;
};
constexpr ResolutionOption kResolutions[] = {
    {"2160p", 3840, 2160}, {"1440p", 2560, 1440}, {"1080p", 1920, 1080},
    {"720p", 1280, 720},   {"480p", 854, 480},    {"360p", 640, 360},
};

constexpr std::int64_t kMinBitrate = 1000;             // 1 kbps
constexpr std::int64_t kMaxVideoBitrate = 200'000'000; // 200 Mbps
constexpr std::int64_t kMaxAudioBitrate = 2'000'000;   // 2 Mbps

std::string numberId(std::int64_t value) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
  return buf;
}

std::string rationalId(const Rational &rate) {
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%lld/%lld",
                static_cast<long long>(rate.num),
                static_cast<long long>(rate.den));
  return buf;
}

std::optional<Rational> rationalFromId(const std::string &id) {
  const auto slash = id.find('/');
  if (slash == std::string::npos) {
    char *end = nullptr;
    errno = 0;
    const long long whole = std::strtoll(id.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0' || whole <= 0) {
      return std::nullopt;
    }
    return Rational{whole, 1};
  }
  char *end = nullptr;
  errno = 0;
  const long long num = std::strtoll(id.c_str(), &end, 10);
  if (errno != 0 || end == nullptr || *end != '/' || num <= 0) {
    return std::nullopt;
  }
  errno = 0;
  const long long den = std::strtoll(id.c_str() + slash + 1, &end, 10);
  if (errno != 0 || end == nullptr || *end != '\0' || den <= 0 ||
      num > 1000000LL * den) {
    return std::nullopt;
  }
  return Rational{num, den};
}

std::int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

} // namespace

// ---------------------------------------------------------------------------
// Session storage
// ---------------------------------------------------------------------------

struct SessionManager::Session {
  std::string id;
  Operation operation = Operation::VideoConvert;
  SessionState state = SessionState::Created;
  std::vector<MediaSource> sources;
  std::vector<MediaInfo> infos;
  std::uint64_t revision = 0;
  JobRequest accepted;
  std::map<FieldId, ValueOrigin> origins;
  std::optional<RelatedChange> relatedChange;
  std::string lastSubmissionId;
  std::string lastSubmittedJobId;
  bool configurationValid = false;
  std::string validationMessage;
  OutputEstimate estimate;
  std::string pipelineSummary;
  std::optional<std::string> samplePath;
  std::uint64_t sampleRevision = 0;
  std::mutex mutex;
};

SessionManager::SessionManager(const FeatureRegistry &features,
                               const CapabilityRegistry &capabilities,
                               SessionHooks hooks)
    : features_(features), capabilities_(capabilities),
      hooks_(std::move(hooks)) {}

SessionManager::~SessionManager() = default;

std::string SessionManager::generateSessionId() {
  static std::atomic<std::uint64_t> counter{0};
  char buf[64];
  std::snprintf(buf, sizeof(buf), "s-%lld-%llu",
                static_cast<long long>(nowMs()),
                static_cast<unsigned long long>(++counter));
  return buf;
}

Result<SessionManager::SessionPtr>
SessionManager::lockedSession(const std::string &sessionId,
                              std::unique_lock<std::mutex> &lock) {
  std::lock_guard<std::mutex> mapLock(mutex_);
  auto it = sessions_.find(sessionId);
  if (it == sessions_.end()) {
    return Error::make(ErrorCode::InvalidArgument, "unknown session");
  }
  lock = std::unique_lock<std::mutex>(it->second->mutex);
  return it->second;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

Result<std::string>
SessionManager::createSession(Operation operation,
                              std::vector<MediaSource> sources) {
  const bool multiSource =
      operation == Operation::VideoCutMerge || operation == Operation::AudioCutMerge;
  if (sources.empty() ||
      (multiSource ? (sources.size() < 1 || sources.size() > 16)
                   : sources.size() != 1)) {
    return Error::make(ErrorCode::InvalidArgument,
                       "invalid source count for operation");
  }
  for (const auto &source : sources) {
    if (source.uri.empty()) {
      return Error::make(ErrorCode::InvalidArgument, "empty source uri");
    }
  }
  auto session = std::make_shared<Session>();
  session->id = generateSessionId();
  session->operation = operation;
  session->sources = std::move(sources);
  session->accepted.operation = operation;
  session->state = SessionState::Created;
  std::lock_guard<std::mutex> mapLock(mutex_);
  sessions_[session->id] = session;
  return session->id;
}

Result<SessionSnapshot>
SessionManager::prepareSession(const std::string &sessionId, bool force) {
  std::unique_lock<std::mutex> lock;
  auto sessionResult = lockedSession(sessionId, lock);
  if (!sessionResult.ok()) {
    return sessionResult.error();
  }
  SessionPtr session = sessionResult.take();
  if (session->state == SessionState::Closed) {
    return Error::make(ErrorCode::InvalidArgument, "session is closed");
  }
  if (session->state == SessionState::Ready && !force) {
    return snapshotOf(*session);
  }
  session->state = SessionState::Preparing;
  session->infos.clear();
  session->relatedChange.reset();

  // Inspection happens WITHOUT holding the session lock: it performs real
  // file I/O and must not block other session interactions (mandate §43.1).
  std::vector<MediaInfo> infos;
  lock.unlock();
  for (const auto &source : session->sources) {
    if (!hooks_.inspect) {
      lock.lock();
      session->state = SessionState::Failed;
      session->validationMessage = "no inspector available";
      return snapshotOf(*session);
    }
    auto info = hooks_.inspect(source);
    lock.lock();
    if (!info.ok()) {
      session->state = SessionState::Failed;
      session->validationMessage = info.error().message;
      return snapshotOf(*session);
    }
    infos.push_back(info.value());
    lock.unlock();
  }
  lock.lock();

  session->infos = std::move(infos);
  session->accepted = initialRequest(session->operation, session->sources,
                                     session->infos);
  session->revision = 1;
  session->origins.clear();
  session->samplePath.reset();

  // Assess the initial configuration (feasibility + estimate).
  if (hooks_.assess) {
    auto assessment = hooks_.assess(session->accepted, session->infos);
    if (assessment.ok()) {
      session->configurationValid = assessment.value().feasible;
      session->validationMessage = assessment.value().message;
      session->estimate = assessment.value().estimate;
      session->pipelineSummary = assessment.value().pipelineSummary;
    } else {
      session->configurationValid = false;
      session->validationMessage = assessment.error().message;
      session->estimate = OutputEstimate{};
    }
  }
  session->state = SessionState::Ready;
  return snapshotOf(*session);
}

Result<SessionSnapshot>
SessionManager::getSnapshot(const std::string &sessionId) {
  std::unique_lock<std::mutex> lock;
  auto sessionResult = lockedSession(sessionId, lock);
  if (!sessionResult.ok()) {
    return sessionResult.error();
  }
  return snapshotOf(*sessionResult.value());
}

SessionSnapshot SessionManager::snapshotOf(const Session &session) const {
  SessionSnapshot snapshot;
  snapshot.sessionId = session.id;
  snapshot.operation = session.operation;
  snapshot.state = session.state;
  snapshot.revision = session.revision;
  snapshot.sources = session.infos;
  snapshot.configurationValid = session.configurationValid;
  snapshot.validationMessage = session.validationMessage;
  snapshot.estimate = session.estimate;
  snapshot.relatedChange = session.relatedChange;
  snapshot.samplePath = session.samplePath;
  // Accepted values keyed by field id.
  for (const auto &[field, origin] : session.origins) {
    AcceptedValue value;
    value.field = field;
    value.origin = origin;
    snapshot.values.push_back(std::move(value));
  }
  return snapshot;
}

Result<FieldOptions> SessionManager::getOptions(const std::string &sessionId,
                                                FieldId field,
                                                std::uint64_t revision) {
  std::unique_lock<std::mutex> lock;
  auto sessionResult = lockedSession(sessionId, lock);
  if (!sessionResult.ok()) {
    return sessionResult.error();
  }
  SessionPtr session = sessionResult.value();
  if (session->state != SessionState::Ready) {
    return Error::make(ErrorCode::InvalidArgument,
                       "session is not prepared");
  }
  if (revision != 0 && revision != session->revision) {
    return Error::make(ErrorCode::StaleRevision, "stale revision");
  }
  return deriveOptions(*session, field);
}

// ---------------------------------------------------------------------------
// Updates
// ---------------------------------------------------------------------------

Result<SessionSnapshot> SessionManager::updateSession(
    const std::string &sessionId, std::uint64_t expectedRevision,
    const std::vector<FieldChange> &changes, bool acceptRelatedChange) {
  std::unique_lock<std::mutex> lock;
  auto sessionResult = lockedSession(sessionId, lock);
  if (!sessionResult.ok()) {
    return sessionResult.error();
  }
  SessionPtr session = sessionResult.value();
  if (session->state != SessionState::Ready) {
    return Error::make(ErrorCode::InvalidArgument, "session is not prepared");
  }
  if (changes.empty()) {
    return Error::make(ErrorCode::InvalidArgument, "empty change set");
  }
  // Stale revisions cannot overwrite newer accepted state (mandate §14.2).
  if (expectedRevision != session->revision) {
    return Error::make(ErrorCode::StaleRevision, "stale revision");
  }

  // Construct the candidate from the accepted configuration.
  JobRequest candidate = session->accepted;
  std::optional<RelatedChange> related;
  for (const auto &change : changes) {
    Error error =
        applyChange(*session, change, candidate, acceptRelatedChange, related);
    if (!error.ok()) {
      // Rejected: the accepted configuration is unchanged.
      return error;
    }
  }
  resolveAutomatic(*session, candidate, related);
  if (related.has_value() && !acceptRelatedChange) {
    // Propose the related change without applying the update.
    session->relatedChange = related;
    return snapshotOf(*session);
  }
  session->relatedChange.reset();

  // Validate the complete candidate through the planner hook.
  if (hooks_.assess) {
    auto assessment = hooks_.assess(candidate, session->infos);
    if (assessment.ok() && !assessment.value().feasible) {
      return Error::make(ErrorCode::NoCompatiblePipeline,
                         assessment.value().message);
    }
    if (assessment.ok()) {
      session->estimate = assessment.value().estimate;
      session->pipelineSummary = assessment.value().pipelineSummary;
      session->validationMessage = assessment.value().message;
    } else if (assessment.error().code != ErrorCode::StaleRevision) {
      return assessment.error();
    }
  }

  // Commit.
  session->accepted = std::move(candidate);
  ++session->revision;
  session->configurationValid = true;
  if (session->samplePath.has_value() &&
      session->sampleRevision != session->revision) {
    session->samplePath.reset(); // invalidated by the change
  }
  return snapshotOf(*session);
}

Error SessionManager::applyChange(Session &session, const FieldChange &change,
                                  JobRequest &candidate,
                                  bool acceptRelatedChange,
                                  std::optional<RelatedChange> &outRelated) const {
  const Operation op = session.operation;
  auto markUser = [&](FieldId field) { session.origins[field] = ValueOrigin::User; };

  // Helper accessors for payload parsing.
  JsonValue payload;
  if (!change.payloadJson.empty()) {
    JsonReader reader(change.payloadJson);
    std::string parseError = reader.parse(payload);
    if (!parseError.empty()) {
      return Error::make(ErrorCode::InvalidArgument,
                         "malformed payload for field " +
                             std::string(fieldIdName(change.field)));
    }
  }

  auto videoRequest = [&]() -> VideoConvertRequest & {
    return std::get<job::VideoConvert>(candidate.payload);
  };
  auto audioRequest = [&]() -> AudioConvertRequest & {
    return std::get<job::AudioConvert>(candidate.payload);
  };
  auto outputOf = [&]() -> OutputSpec & {
    switch (candidate.payload.index()) {
    case 0: return std::get<job::VideoConvert>(candidate.payload).output;
    case 1: return std::get<job::AudioConvert>(candidate.payload).output;
    case 2: return std::get<job::Crop>(candidate.payload).output;
    case 3: return std::get<job::Trim>(candidate.payload).output;
    case 4: return std::get<job::Cut>(candidate.payload).output;
    case 5: return std::get<job::Split>(candidate.payload).output;
    case 6: return std::get<job::Merge>(candidate.payload).output;
    case 7: return std::get<job::ExtractAudio>(candidate.payload).output;
    case 8: return std::get<job::ExtractFrames>(candidate.payload).output;
    case 9: return std::get<job::ExtractSubtitles>(candidate.payload).output;
    }
    __builtin_unreachable();
  };

  switch (change.field) {
  case FieldId::Format: {
    Container target{};
    if (change.valueId == "original") {
      if (session.infos.empty() ||
          !containerWritable(session.infos.front().container)) {
        return Error::make(ErrorCode::IncompatibleFormat,
                           "source container is not writable by Velmux");
      }
      target = session.infos.front().container;
    } else {
      auto parsed = containerFromName(change.valueId);
      if (!parsed.has_value() || !features_.knowsContainer(*parsed)) {
        return Error::make(ErrorCode::IncompatibleFormat,
                           "unknown output container: " + change.valueId);
      }
      if (!containerWritable(*parsed)) {
        return Error::make(ErrorCode::IncompatibleFormat,
                           "container not writable on this build: " +
                               change.valueId);
      }
      target = *parsed;
    }
    // Validate codec compatibility; explicit conflicts become related
    // changes rather than silent rewrites (mandate §14.1).
    if (candidate.payload.index() == 0) {
      auto &request = videoRequest();
      if (request.video.codec.has_value() &&
          !features_.containerAccepts(target, *request.video.codec)) {
        if (!acceptRelatedChange) {
          outRelated = RelatedChange{FieldId::Codec,
                                     "the selected container cannot hold the "
                                     "explicitly selected video codec",
                                     codecName(*request.video.codec), "auto"};
          return Error::make(ErrorCode::RelatedChangeRequired,
                             "related change required");
        }
        request.video.codec.reset();
        session.origins[FieldId::Codec] = ValueOrigin::Automatic;
      }
      request.output.container = target;
    } else if (candidate.payload.index() == 1 ||
               candidate.payload.index() == 7) {
      AudioSettings &settings =
          candidate.payload.index() == 1
              ? std::get<job::AudioConvert>(candidate.payload).audio
              : std::get<job::ExtractAudio>(candidate.payload).audio;
      const AudioCodec *explicitCodec = settings.codec
                                            ? &*settings.codec
                                            : nullptr;
      if (explicitCodec &&
          !features_.containerAccepts(target, *explicitCodec)) {
        if (!acceptRelatedChange) {
          outRelated = RelatedChange{
              FieldId::AudioCodec,
              "the selected container cannot hold the explicitly selected "
              "audio codec",
              codecName(*explicitCodec), "auto"};
          return Error::make(ErrorCode::RelatedChangeRequired,
                             "related change required");
        }
        settings.codec.reset();
        session.origins[FieldId::AudioCodec] = ValueOrigin::Automatic;
      }
      if (candidate.payload.index() == 1) {
        std::get<job::AudioConvert>(candidate.payload).output.container =
            target;
      } else {
        std::get<job::ExtractAudio>(candidate.payload).output.container =
            target;
      }
    } else {
      return Error::make(ErrorCode::UnsupportedOperation,
                         "format is not configurable for this operation");
    }
    markUser(FieldId::Format);
    return {};
  }
  case FieldId::Codec: {
    if (candidate.payload.index() != 0) {
      return Error::make(ErrorCode::UnsupportedOperation,
                         "codec field does not belong to this operation");
    }
    auto &request = videoRequest();
    const Container target = request.output.container.value_or(
        session.infos.empty() ? Container::Mp4
                              : session.infos.front().container);
    if (change.valueId == "auto") {
      request.video.codec.reset();
    } else {
      auto parsed = videoCodecFromName(change.valueId);
      if (!parsed.has_value() || !features_.knowsVideoCodec(*parsed)) {
        return Error::make(ErrorCode::IncompatibleCodec,
                           "unknown video codec: " + change.valueId);
      }
      if (!videoEncodeSupported(*parsed, target)) {
        return Error::make(
            ErrorCode::EncoderUnavailable,
            std::string("video codec cannot be encoded into ") +
                containerName(target) + ": " + change.valueId);
      }
      request.video.codec = *parsed;
    }
    markUser(FieldId::Codec);
    return {};
  }
  case FieldId::Resolution: {
    if (candidate.payload.index() != 0) {
      return Error::make(ErrorCode::UnsupportedOperation,
                         "resolution does not belong to this operation");
    }
    auto &request = videoRequest();
    if (change.valueId == "original") {
      request.video.resolutionHeight.reset();
      request.video.customWidth.reset();
      request.video.customHeight.reset();
    } else if (change.valueId == "custom") {
      std::int32_t width = 0;
      std::int32_t height = 0;
      if (const JsonValue *w = payload.find("width")) {
        width = std::int32_t(w->asInt());
      }
      if (const JsonValue *h = payload.find("height")) {
        height = std::int32_t(h->asInt());
      }
      if (width < 16 || height < 16 || width > 8192 || height > 8192 ||
          (width % 2) != 0 || (height % 2) != 0) {
        return Error::make(ErrorCode::InvalidResolution,
                           "invalid custom resolution");
      }
      request.video.resolutionHeight.reset();
      request.video.customWidth = width;
      request.video.customHeight = height;
    } else {
      std::int32_t height = 0;
      for (const auto &option : kResolutions) {
        if (change.valueId == option.id) {
          height = option.landscapeHeight;
          break;
        }
      }
      if (height == 0) {
        return Error::make(ErrorCode::InvalidResolution,
                           "unknown resolution preset: " + change.valueId);
      }
      request.video.customWidth.reset();
      request.video.customHeight.reset();
      request.video.resolutionHeight = height;
    }
    markUser(FieldId::Resolution);
    return {};
  }
  case FieldId::FrameRate: {
    if (candidate.payload.index() != 0) {
      return Error::make(ErrorCode::UnsupportedOperation,
                         "frame rate does not belong to this operation");
    }
    auto &request = videoRequest();
    if (change.valueId == "original") {
      request.video.frameRate.reset();
    } else {
      auto rate = rationalFromId(change.valueId);
      if (!rate.has_value()) {
        return Error::make(ErrorCode::InvalidFrameRate,
                           "invalid frame rate: " + change.valueId);
      }
      request.video.frameRate = *rate;
    }
    markUser(FieldId::FrameRate);
    return {};
  }
  case FieldId::CompressionEnabled: {
    CompressionIntent *compression = nullptr;
    if (candidate.payload.index() == 0) {
      compression = &videoRequest().compression;
    } else if (candidate.payload.index() == 1) {
      compression = &audioRequest().compression;
    } else if (candidate.payload.index() == 7) {
      compression = &std::get<job::ExtractAudio>(candidate.payload).compression;
    } else {
      return Error::make(ErrorCode::UnsupportedOperation,
                         "compression does not belong to this operation");
    }
    compression->enabled = change.valueId == "true";
    markUser(FieldId::CompressionEnabled);
    return {};
  }
  case FieldId::CompressionPreset: {
    CompressionIntent *compression = nullptr;
    if (candidate.payload.index() == 0) {
      compression = &videoRequest().compression;
    } else if (candidate.payload.index() == 1) {
      compression = &audioRequest().compression;
    } else if (candidate.payload.index() == 7) {
      compression = &std::get<job::ExtractAudio>(candidate.payload).compression;
    } else {
      return Error::make(ErrorCode::UnsupportedOperation,
                         "compression does not belong to this operation");
    }
    auto preset = compressionPresetFromName(change.valueId);
    if (!preset.has_value()) {
      return Error::make(ErrorCode::InvalidArgument,
                         "unknown compression preset: " + change.valueId);
    }
    compression->preset = *preset;
    if (*preset != CompressionPreset::Custom) {
      // Preset-derived values clear custom user requirements.
      compression->quality.reset();
      compression->videoBitrate.reset();
      compression->audioBitrate.reset();
    }
    markUser(FieldId::CompressionPreset);
    return {};
  }
  case FieldId::CompressionQuality: {
    CompressionIntent *compression = nullptr;
    if (candidate.payload.index() == 0) {
      compression = &videoRequest().compression;
    } else if (candidate.payload.index() == 1) {
      compression = &audioRequest().compression;
    } else if (candidate.payload.index() == 7) {
      compression = &std::get<job::ExtractAudio>(candidate.payload).compression;
    } else {
      return Error::make(ErrorCode::UnsupportedOperation,
                         "compression does not belong to this operation");
    }
    char *end = nullptr;
    errno = 0;
    const long long quality = std::strtoll(change.valueId.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0' || quality < 1 ||
        quality > 100) {
      return Error::make(ErrorCode::InvalidArgument, "invalid quality value");
    }
    compression->preset = CompressionPreset::Custom;
    compression->quality = std::int32_t(quality);
    markUser(FieldId::CompressionQuality);
    return {};
  }
  case FieldId::CompressionVideoBitrate:
  case FieldId::CompressionAudioBitrate: {
    const bool video = change.field == FieldId::CompressionVideoBitrate;
    CompressionIntent *compression = nullptr;
    if (candidate.payload.index() == 0) {
      compression = &videoRequest().compression;
    } else if (candidate.payload.index() == 1) {
      compression = &audioRequest().compression;
    } else if (candidate.payload.index() == 7) {
      compression = &std::get<job::ExtractAudio>(candidate.payload).compression;
    } else {
      return Error::make(ErrorCode::UnsupportedOperation,
                         "compression does not belong to this operation");
    }
    char *end = nullptr;
    errno = 0;
    const long long bitrate = std::strtoll(change.valueId.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0' ||
        bitrate < kMinBitrate ||
        bitrate > (video ? kMaxVideoBitrate : kMaxAudioBitrate)) {
      return Error::make(ErrorCode::InvalidArgument, "invalid bitrate value");
    }
    compression->preset = CompressionPreset::Custom;
    if (video) {
      compression->videoBitrate = bitrate;
    } else {
      compression->audioBitrate = bitrate;
    }
    markUser(change.field);
    return {};
  }
  case FieldId::CompressionTargetSize: {
    CompressionIntent *compression = nullptr;
    if (candidate.payload.index() == 0) {
      compression = &videoRequest().compression;
    } else if (candidate.payload.index() == 1) {
      compression = &audioRequest().compression;
    } else if (candidate.payload.index() == 7) {
      compression = &std::get<job::ExtractAudio>(candidate.payload).compression;
    } else {
      return Error::make(ErrorCode::UnsupportedOperation,
                         "compression does not belong to this operation");
    }
    char *end = nullptr;
    errno = 0;
    const long long target = std::strtoll(change.valueId.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0' || target < 1024 ||
        target > 1LL << 40) {
      return Error::make(ErrorCode::InfeasibleTargetSize,
                         "invalid target size");
    }
    compression->preset = CompressionPreset::TargetSize;
    compression->targetSizeBytes = target;
    markUser(FieldId::CompressionTargetSize);
    return {};
  }
  case FieldId::AudioCodec: {
    AudioSettings *settings = nullptr;
    Container target{};
    if (candidate.payload.index() == 0) {
      settings = &videoRequest().audioSettings;
      target = videoRequest().output.container.value_or(
          session.infos.empty() ? Container::Mp4
                                : session.infos.front().container);
    } else if (candidate.payload.index() == 1) {
      settings = &audioRequest().audio;
      target = audioRequest().output.container.value_or(
          session.infos.empty() ? Container::M4a
                                : session.infos.front().container);
    } else if (candidate.payload.index() == 7) {
      auto &request = std::get<job::ExtractAudio>(candidate.payload);
      settings = &request.audio;
      target = request.output.container.value_or(
          session.infos.empty() ? Container::M4a
                                : session.infos.front().container);
    } else {
      return Error::make(ErrorCode::UnsupportedOperation,
                         "audio codec does not belong to this operation");
    }
    if (change.valueId == "auto") {
      settings->codec.reset();
    } else {
      auto parsed = audioCodecFromName(change.valueId);
      if (!parsed.has_value() || !features_.knowsAudioCodec(*parsed)) {
        return Error::make(ErrorCode::IncompatibleCodec,
                           "unknown audio codec: " + change.valueId);
      }
      if (!audioEncodeSupported(*parsed, target)) {
        return Error::make(
            ErrorCode::EncoderUnavailable,
            std::string("audio codec cannot be encoded into ") +
                containerName(target) + ": " + change.valueId);
      }
      settings->codec = *parsed;
    }
    markUser(FieldId::AudioCodec);
    return {};
  }
  case FieldId::AudioBitrate: {
    AudioSettings *settings = nullptr;
    if (candidate.payload.index() == 0) {
      settings = &videoRequest().audioSettings;
    } else if (candidate.payload.index() == 1) {
      settings = &audioRequest().audio;
    } else if (candidate.payload.index() == 7) {
      settings = &std::get<job::ExtractAudio>(candidate.payload).audio;
    } else {
      return Error::make(ErrorCode::UnsupportedOperation,
                         "audio bitrate does not belong to this operation");
    }
    if (change.valueId == "auto") {
      settings->bitRate.reset();
    } else {
      char *end = nullptr;
      errno = 0;
      const long long bitrate = std::strtoll(change.valueId.c_str(), &end, 10);
      if (errno != 0 || end == nullptr || *end != '\0' ||
          bitrate < kMinBitrate || bitrate > kMaxAudioBitrate) {
        return Error::make(ErrorCode::InvalidArgument, "invalid audio bitrate");
      }
      settings->bitRate = bitrate;
    }
    markUser(FieldId::AudioBitrate);
    return {};
  }
  case FieldId::AudioSampleRate: {
    AudioSettings *settings = nullptr;
    if (candidate.payload.index() == 0) {
      settings = &videoRequest().audioSettings;
    } else if (candidate.payload.index() == 1) {
      settings = &audioRequest().audio;
    } else if (candidate.payload.index() == 7) {
      settings = &std::get<job::ExtractAudio>(candidate.payload).audio;
    } else {
      return Error::make(ErrorCode::UnsupportedOperation,
                         "sample rate does not belong to this operation");
    }
    if (change.valueId == "original") {
      settings->sampleRate.reset();
    } else {
      char *end = nullptr;
      errno = 0;
      const long long rate = std::strtoll(change.valueId.c_str(), &end, 10);
      if (errno != 0 || end == nullptr || *end != '\0' || rate < 4000 ||
          rate > 192000) {
        return Error::make(ErrorCode::InvalidSampleRate,
                           "unsupported sample rate: " + change.valueId);
      }
      settings->sampleRate = std::int32_t(rate);
    }
    markUser(FieldId::AudioSampleRate);
    return {};
  }
  case FieldId::AudioChannels: {
    AudioSettings *settings = nullptr;
    if (candidate.payload.index() == 0) {
      settings = &videoRequest().audioSettings;
    } else if (candidate.payload.index() == 1) {
      settings = &audioRequest().audio;
    } else if (candidate.payload.index() == 7) {
      settings = &std::get<job::ExtractAudio>(candidate.payload).audio;
    } else {
      return Error::make(ErrorCode::UnsupportedOperation,
                         "channels do not belong to this operation");
    }
    if (change.valueId == "original") {
      settings->channels.reset();
    } else if (change.valueId == "1" || change.valueId == "2" ||
               change.valueId == "6") {
      settings->channels = std::int32_t(std::stoi(change.valueId));
    } else {
      return Error::make(ErrorCode::InvalidChannelLayout,
                         "unsupported channel layout: " + change.valueId);
    }
    markUser(FieldId::AudioChannels);
    return {};
  }
  case FieldId::AudioTracks: {
    if (candidate.payload.index() != 0) {
      return Error::make(ErrorCode::UnsupportedOperation,
                         "audio tracks do not belong to this operation");
    }
    auto &request = videoRequest();
    if (payload.kind != JsonValue::Kind::Array) {
      return Error::make(ErrorCode::InvalidArgument,
                         "audio tracks payload must be an array");
    }
    const std::int32_t trackCount = std::int32_t(
        session.infos.empty() ? 0 : session.infos.front().audio.size());
    std::vector<AudioTrackRule> rules;
    for (const auto &item : payload.items) {
      AudioTrackRule rule;
      if (const JsonValue *index = item.find("index")) {
        rule.sourceTrackIndex = std::int32_t(index->asInt());
      }
      if (rule.sourceTrackIndex < -1 ||
          rule.sourceTrackIndex >= trackCount) {
        return Error::make(ErrorCode::InvalidTrackIdentity,
                           "invalid audio track index");
      }
      if (const JsonValue *include = item.find("include")) {
        rule.include = include->kind == JsonValue::Kind::Bool
                           ? include->boolValue
                           : include->asInt() != 0;
      }
      if (const JsonValue *external = item.find("external")) {
        if (external->kind == JsonValue::Kind::String &&
            !external->text.empty()) {
          rule.externalUri = external->text;
        }
      }
      if (const JsonValue *replace = item.find("replace")) {
        rule.replace = replace->kind == JsonValue::Kind::Bool
                           ? replace->boolValue
                           : replace->asInt() != 0;
      }
      rules.push_back(std::move(rule));
    }
    request.audioTracks = std::move(rules);
    markUser(FieldId::AudioTracks);
    return {};
  }
  case FieldId::Subtitles:
  case FieldId::SubtitleTracks: {
    if (change.field == FieldId::Subtitles && op != Operation::VideoConvert) {
      return Error::make(ErrorCode::UnsupportedOperation,
                         "subtitles do not belong to this operation");
    }
    if (change.field == FieldId::SubtitleTracks &&
        op != Operation::ExtractSubtitles) {
      return Error::make(ErrorCode::UnsupportedOperation,
                         "subtitle track selection does not belong to this "
                         "operation");
    }
    if (payload.kind != JsonValue::Kind::Array) {
      return Error::make(ErrorCode::InvalidArgument,
                         "subtitle payload must be an array");
    }
    if (change.field == FieldId::Subtitles) {
      auto &request = videoRequest();
      const std::int32_t trackCount =
          std::int32_t(session.infos.empty()
                           ? 0
                           : session.infos.front().subtitles.size());
      std::vector<SubtitleTrackRule> rules;
      for (const auto &item : payload.items) {
        SubtitleTrackRule rule;
        if (const JsonValue *index = item.find("index")) {
          rule.sourceTrackIndex = std::int32_t(index->asInt());
        }
        if (rule.sourceTrackIndex < -1 ||
            rule.sourceTrackIndex >= trackCount) {
          return Error::make(ErrorCode::InvalidTrackIdentity,
                             "invalid subtitle track index");
        }
        if (const JsonValue *policy = item.find("policy")) {
          if (policy->text == "preserve") {
            rule.policy = SubtitleTrackPolicy::Preserve;
          } else if (policy->text == "remove") {
            rule.policy = SubtitleTrackPolicy::Remove;
          } else if (policy->text == "embed") {
            rule.policy = SubtitleTrackPolicy::Embed;
          } else if (policy->text == "burnIn") {
            rule.policy = SubtitleTrackPolicy::BurnIn;
          } else {
            return Error::make(ErrorCode::InvalidArgument,
                               "invalid subtitle policy");
          }
        }
        if (const JsonValue *external = item.find("external")) {
          if (external->kind == JsonValue::Kind::String &&
              !external->text.empty()) {
            rule.externalUri = external->text;
          }
        }
        if (const JsonValue *fontSize = item.find("fontSize")) {
          const auto size = fontSize->asInt();
          if (size < 8 || size > 144) {
            return Error::make(ErrorCode::InvalidArgument,
                               "invalid subtitle font size");
          }
          rule.fontSize = std::int32_t(size);
        }
        if (const JsonValue *color = item.find("color")) {
          rule.color = color->text;
        }
        if (const JsonValue *position = item.find("position")) {
          const auto pos = position->asInt();
          if (pos < 0 || pos > 100) {
            return Error::make(ErrorCode::InvalidArgument,
                               "invalid subtitle position");
          }
          rule.position = std::int32_t(pos);
        }
        rules.push_back(std::move(rule));
      }
      request.subtitleTracks = std::move(rules);
    } else {
      auto &request = std::get<job::ExtractSubtitles>(candidate.payload);
      std::vector<std::int32_t> tracks;
      for (const auto &item : payload.items) {
        const auto index = std::int32_t(item.asInt());
        const std::int32_t trackCount =
            std::int32_t(session.infos.empty()
                             ? 0
                             : session.infos.front().subtitles.size());
        if (index < 0 || index >= trackCount) {
          return Error::make(ErrorCode::InvalidTrackIdentity,
                             "invalid subtitle track index");
        }
        tracks.push_back(index);
      }
      request.trackIndices = std::move(tracks);
    }
    markUser(change.field);
    return {};
  }
  case FieldId::SubtitleTarget: {
    if (op != Operation::ExtractSubtitles) {
      return Error::make(ErrorCode::UnsupportedOperation,
                         "subtitle target does not belong to this operation");
    }
    if (payload.kind != JsonValue::Kind::Array) {
      return Error::make(ErrorCode::InvalidArgument,
                         "subtitle target payload must be an array");
    }
    auto &request = std::get<job::ExtractSubtitles>(candidate.payload);
    std::vector<SubtitleTextTarget> targets;
    for (const auto &item : payload.items) {
      if (item.text == "original") {
        targets.push_back(SubtitleTextTarget::Original);
      } else if (item.text == "srt") {
        targets.push_back(SubtitleTextTarget::Srt);
      } else if (item.text == "vtt") {
        targets.push_back(SubtitleTextTarget::Vtt);
      } else if (item.text == "ass") {
        targets.push_back(SubtitleTextTarget::Ass);
      } else {
        return Error::make(ErrorCode::InvalidArgument,
                           "unknown subtitle target: " + item.text);
      }
    }
    request.targets = std::move(targets);
    markUser(FieldId::SubtitleTarget);
    return {};
  }
  case FieldId::Adjustments: {
    if (payload.kind != JsonValue::Kind::Object) {
      return Error::make(ErrorCode::InvalidArgument,
                         "adjustments payload must be an object");
    }
    if (op == Operation::VideoConvert) {
      auto &request = videoRequest();
      VideoAdjustments &adj = request.adjustments;
      if (const JsonValue *rotation = payload.find("rotation")) {
        const auto value = rotation->asInt();
        if (value != 0 && value != 90 && value != 180 && value != 270) {
          return Error::make(ErrorCode::InvalidArgument,
                             "invalid rotation");
        }
        adj.rotationDegrees = std::int32_t(value);
      }
      if (const JsonValue *flipH = payload.find("flipH")) {
        adj.flipHorizontal = flipH->boolValue;
      }
      if (const JsonValue *flipV = payload.find("flipV")) {
        adj.flipVertical = flipV->boolValue;
      }
      if (const JsonValue *speed = payload.find("speed")) {
        if (speed->kind == JsonValue::Kind::Null) {
          adj.playbackSpeed.reset();
        } else {
          const double value = speed->asDouble();
          if (value < 0.5 || value > 2.0) {
            return Error::make(ErrorCode::InvalidArgument,
                               "playback speed out of range (0.5..2.0)");
          }
          adj.playbackSpeed = value;
        }
      }
      if (const JsonValue *reverse = payload.find("reverse")) {
        adj.reverse = reverse->boolValue;
      }
      if (const JsonValue *hdr = payload.find("hdrToSdr")) {
        adj.hdrToSdr = hdr->boolValue;
      }
    } else if (op == Operation::AudioConvert) {
      auto &request = audioRequest();
      AudioAdjustments &adj = request.adjustments;
      if (const JsonValue *volume = payload.find("volume")) {
        if (volume->kind == JsonValue::Kind::Null) {
          adj.gainDb.reset();
        } else {
          const double value = volume->asDouble();
          if (value < -60.0 || value > 60.0) {
            return Error::make(ErrorCode::InvalidArgument,
                               "volume out of range (-60..60 dB)");
          }
          adj.gainDb = value;
        }
      }
      if (const JsonValue *normalize = payload.find("normalize")) {
        adj.normalize = normalize->boolValue;
      }
      if (const JsonValue *fadeIn = payload.find("fadeIn")) {
        const auto value = fadeIn->asInt();
        if (value < 0 || value > 60000) {
          return Error::make(ErrorCode::InvalidArgument, "invalid fade");
        }
        adj.fadeInMs = std::int32_t(value);
      }
      if (const JsonValue *fadeOut = payload.find("fadeOut")) {
        const auto value = fadeOut->asInt();
        if (value < 0 || value > 60000) {
          return Error::make(ErrorCode::InvalidArgument, "invalid fade");
        }
        adj.fadeOutMs = std::int32_t(value);
      }
      if (const JsonValue *speed = payload.find("speed")) {
        if (speed->kind == JsonValue::Kind::Null) {
          adj.playbackSpeed.reset();
        } else {
          const double value = speed->asDouble();
          if (value < 0.5 || value > 2.0) {
            return Error::make(ErrorCode::InvalidArgument,
                               "playback speed out of range (0.5..2.0)");
          }
          adj.playbackSpeed = value;
        }
      }
      if (const JsonValue *pitch = payload.find("preservePitch")) {
        adj.preservePitch = pitch->boolValue;
      }
      if (const JsonValue *reverse = payload.find("reverse")) {
        adj.reverse = reverse->boolValue;
      }
      if (const JsonValue *silence = payload.find("removeSilence")) {
        adj.removeSilence = silence->boolValue;
      }
    } else {
      return Error::make(ErrorCode::UnsupportedOperation,
                         "adjustments do not belong to this operation");
    }
    markUser(FieldId::Adjustments);
    return {};
  }
  case FieldId::Metadata: {
    MetadataIntention *metadata = nullptr;
    if (candidate.payload.index() == 0) {
      metadata = &videoRequest().metadata;
    } else if (candidate.payload.index() == 1) {
      metadata = &audioRequest().metadata;
    } else if (candidate.payload.index() == 7) {
      metadata = &std::get<job::ExtractAudio>(candidate.payload).metadata;
    } else {
      return Error::make(ErrorCode::UnsupportedOperation,
                         "metadata does not belong to this operation");
    }
    if (payload.kind != JsonValue::Kind::Object) {
      return Error::make(ErrorCode::InvalidArgument,
                         "metadata payload must be an object");
    }
    // Keys present with a string value = Set; explicit null = Clear;
    // absent = Unchanged (mandate §33.1).
    auto applyText = [&](const char *key, MetadataFieldAction &action,
                         std::string &value) {
      if (const JsonValue *field = payload.find(key)) {
        if (field->kind == JsonValue::Kind::Null) {
          action = MetadataFieldAction::Clear;
          value.clear();
        } else if (field->kind == JsonValue::Kind::String) {
          action = MetadataFieldAction::Set;
          value = field->text;
        }
      }
    };
    applyText("title", metadata->titleAction, metadata->title);
    applyText("artist", metadata->artistAction, metadata->artist);
    applyText("album", metadata->albumAction, metadata->album);
    applyText("genre", metadata->genreAction, metadata->genre);
    applyText("date", metadata->dateAction, metadata->date);
    applyText("description", metadata->descriptionAction,
              metadata->description);
    applyText("location", metadata->locationAction, metadata->location);
    if (const JsonValue *artwork = payload.find("artwork")) {
      if (artwork->kind == JsonValue::Kind::Null) {
        metadata->artworkAction = MetadataFieldAction::Clear;
        metadata->artworkUri.clear();
      } else if (artwork->kind == JsonValue::Kind::String) {
        metadata->artworkAction = MetadataFieldAction::Set;
        metadata->artworkUri = artwork->text;
      }
    }
    markUser(FieldId::Metadata);
    return {};
  }
  case FieldId::OutputName: {
    OutputSpec &output = outputOf();
    if (change.payloadJson.size() > 255) {
      return Error::make(ErrorCode::InvalidArgument, "output name too long");
    }
    // Sanitize: no path separators, no traversal (defense in depth; the
    // output manager sanitizes again at execution time).
    std::string name = change.payloadJson;
    for (char &c : name) {
      if (c == '/' || c == '\\') {
        c = '_';
      }
    }
    if (name == "." || name == "..") {
      return Error::make(ErrorCode::InvalidArgument, "invalid output name");
    }
    output.name = name;
    markUser(FieldId::OutputName);
    return {};
  }
  case FieldId::CropRect: {
    if (op != Operation::VideoCrop) {
      return Error::make(ErrorCode::UnsupportedOperation,
                         "crop does not belong to this operation");
    }
    auto &request = std::get<job::Crop>(candidate.payload);
    if (const JsonValue *x = payload.find("x")) {
      request.crop.x = std::int32_t(x->asInt());
    }
    if (const JsonValue *y = payload.find("y")) {
      request.crop.y = std::int32_t(y->asInt());
    }
    if (const JsonValue *width = payload.find("width")) {
      request.crop.width = std::int32_t(width->asInt());
    }
    if (const JsonValue *height = payload.find("height")) {
      request.crop.height = std::int32_t(height->asInt());
    }
    if (!request.crop.valid()) {
      return Error::make(ErrorCode::InvalidCrop, "crop dimensions must be "
                                                 "positive");
    }
    markUser(FieldId::CropRect);
    return {};
  }
  case FieldId::TimelineMode: {
    if (op != Operation::VideoCutMerge && op != Operation::AudioCutMerge) {
      return Error::make(ErrorCode::UnsupportedOperation,
                         "timeline mode does not belong to this operation");
    }
    auto mode = timelineModeFromName(change.valueId);
    if (!mode.has_value()) {
      return Error::make(ErrorCode::InvalidArgument,
                         "unknown timeline mode: " + change.valueId);
    }
    // Switching modes replaces the payload with mode defaults while
    // preserving sources and the output name.
    OutputSpec output = outputOf();
    const MediaSource source = session.sources.front();
    switch (*mode) {
    case TimelineMode::Trim:
      candidate.payload = job::Trim{source, output, TimeRange{}};
      break;
    case TimelineMode::Cut:
      candidate.payload = job::Cut{source, output, {}};
      break;
    case TimelineMode::Split:
      candidate.payload = job::Split{source, output, SplitRules{}};
      break;
    case TimelineMode::Merge:
      candidate.payload =
          job::Merge{session.sources, output, MergeRules{}};
      break;
    }
    markUser(FieldId::TimelineMode);
    return {};
  }
  case FieldId::TimelineRanges: {
    if (payload.kind != JsonValue::Kind::Object) {
      return Error::make(ErrorCode::InvalidArgument,
                         "timeline payload must be an object");
    }
    const std::int64_t durationUs = std::int64_t(
        (session.infos.empty() ? 0.0
                               : session.infos.front().durationSec) *
        1e6);
    if (candidate.payload.index() == 3) { // Trim
      auto &request = std::get<job::Trim>(candidate.payload);
      if (const JsonValue *start = payload.find("startUs")) {
        request.range.startUs = start->asInt();
      }
      if (const JsonValue *end = payload.find("endUs")) {
        request.range.endUs = end->asInt();
      }
      if (!request.range.valid() ||
          (durationUs > 0 && request.range.startUs >= durationUs) ||
          (request.range.endUs >= 0 && durationUs > 0 &&
           request.range.endUs > durationUs)) {
        return Error::make(ErrorCode::InvalidTimeRange,
                           "invalid trim range");
      }
    } else if (candidate.payload.index() == 4) { // Cut
      auto &request = std::get<job::Cut>(candidate.payload);
      if (const JsonValue *removed = payload.find("removed")) {
        if (removed->kind != JsonValue::Kind::Array) {
          return Error::make(ErrorCode::InvalidArgument,
                             "removed must be an array");
        }
        std::vector<TimeRange> ranges;
        for (const auto &item : removed->items) {
          TimeRange range;
          if (const JsonValue *start = item.find("startUs")) {
            range.startUs = start->asInt();
          }
          if (const JsonValue *end = item.find("endUs")) {
            range.endUs = end->asInt();
          }
          if (!range.valid()) {
            return Error::make(ErrorCode::InvalidTimeRange,
                               "invalid removal interval");
          }
          ranges.push_back(range);
        }
        std::sort(ranges.begin(), ranges.end(),
                  [](const TimeRange &a, const TimeRange &b) {
                    return a.startUs < b.startUs;
                  });
        // Normalize: merge overlapping/adjacent intervals (mandate §27.2).
        std::vector<TimeRange> normalized;
        for (const auto &range : ranges) {
          if (!normalized.empty()) {
            TimeRange &last = normalized.back();
            const std::int64_t lastEnd =
                last.endUs < 0 ? durationUs : last.endUs;
            const std::int64_t rangeEnd =
                range.endUs < 0 ? durationUs : range.endUs;
            if (range.startUs <= lastEnd) {
              last.endUs = std::max(lastEnd, rangeEnd);
              continue;
            }
          }
          normalized.push_back(range);
        }
        // Reject removing the complete source (mandate §27.2).
        if (!normalized.empty() && durationUs > 0) {
          const TimeRange &first = normalized.front();
          const TimeRange &last = normalized.back();
          const bool fullStart = first.startUs <= 0;
          const bool fullEnd =
              last.endUs < 0 || last.endUs >= durationUs;
          bool contiguous = normalized.size() == 1;
          if (normalized.size() > 1) {
            contiguous = true;
            for (std::size_t i = 1; i < normalized.size(); ++i) {
              const std::int64_t prevEnd = normalized[i - 1].endUs < 0
                                               ? durationUs
                                               : normalized[i - 1].endUs;
              if (normalized[i].startUs > prevEnd) {
                contiguous = false;
                break;
              }
            }
          }
          if (fullStart && fullEnd && contiguous) {
            return Error::make(ErrorCode::InvalidTimeRange,
                               "cannot remove the complete source");
          }
        }
        request.removed = std::move(normalized);
      }
    } else {
      return Error::make(ErrorCode::UnsupportedOperation,
                         "timeline ranges do not belong to this operation");
    }
    markUser(FieldId::TimelineRanges);
    return {};
  }
  case FieldId::SplitRules: {
    if (candidate.payload.index() != 5) {
      return Error::make(ErrorCode::UnsupportedOperation,
                         "split rules do not belong to this operation");
    }
    auto &request = std::get<job::Split>(candidate.payload);
    std::string mode = "equal-parts";
    if (const JsonValue *modeValue = payload.find("mode")) {
      mode = modeValue->text;
    }
    if (mode == "equal-parts") {
      request.rules.mode = SplitMode::EqualParts;
    } else if (mode == "duration") {
      request.rules.mode = SplitMode::Duration;
    } else if (mode == "max-size") {
      request.rules.mode = SplitMode::MaxSize;
    } else if (mode == "custom") {
      request.rules.mode = SplitMode::CustomTimestamps;
    } else if (mode == "chapters") {
      request.rules.mode = SplitMode::Chapters;
    } else if (mode == "scenes") {
      request.rules.mode = SplitMode::Scenes;
    } else {
      return Error::make(ErrorCode::InvalidArgument, "unknown split mode");
    }
    if (const JsonValue *count = payload.find("partCount")) {
      const auto value = count->asInt();
      if (value < 2 || value > 100) {
        return Error::make(ErrorCode::InvalidArgument,
                           "part count out of range (2..100)");
      }
      request.rules.partCount = std::int32_t(value);
    }
    if (const JsonValue *duration = payload.find("partDurationUs")) {
      const auto value = duration->asInt();
      if (value <= 0 || value > 24LL * 3600 * 1000000) {
        return Error::make(ErrorCode::InvalidArgument, "invalid part duration");
      }
      request.rules.partDurationUs = value;
    }
    if (const JsonValue *maxSize = payload.find("maxSizeBytes")) {
      const auto value = maxSize->asInt();
      if (value < 1024 || value > 1LL << 42) {
        return Error::make(ErrorCode::InvalidArgument,
                           "invalid maximum part size");
      }
      request.rules.maxSizeBytes = value;
    }
    if (const JsonValue *boundaries = payload.find("boundaries")) {
      if (boundaries->kind != JsonValue::Kind::Array) {
        return Error::make(ErrorCode::InvalidArgument,
                           "boundaries must be an array");
      }
      std::vector<std::int64_t> stamps;
      for (const auto &item : boundaries->items) {
        const auto value = item.asInt();
        if (value <= 0 || value >= 24LL * 3600 * 1000000) {
          return Error::make(ErrorCode::InvalidTimeRange,
                             "invalid split boundary");
        }
        stamps.push_back(value);
      }
      std::sort(stamps.begin(), stamps.end());
      stamps.erase(std::unique(stamps.begin(), stamps.end()), stamps.end());
      request.rules.boundaryUs = std::move(stamps);
    }
    markUser(FieldId::SplitRules);
    return {};
  }
  case FieldId::MergeLayout: {
    if (candidate.payload.index() != 6) {
      return Error::make(ErrorCode::UnsupportedOperation,
                         "merge layout does not belong to this operation");
    }
    auto &request = std::get<job::Merge>(candidate.payload);
    if (change.valueId == "sequential") {
      request.rules.layout = MergeLayout::Sequential;
    } else if (change.valueId == "side-by-side") {
      request.rules.layout = MergeLayout::SideBySide;
    } else if (change.valueId == "top-bottom") {
      request.rules.layout = MergeLayout::TopBottom;
    } else {
      return Error::make(ErrorCode::InvalidArgument,
                         "unknown merge layout: " + change.valueId);
    }
    if (request.rules.layout != MergeLayout::Sequential &&
        request.sources.size() != 2) {
      return Error::make(ErrorCode::InvalidArgument,
                         "spatial merge requires exactly two sources");
    }
    markUser(FieldId::MergeLayout);
    return {};
  }
  case FieldId::MergeAudio: {
    if (candidate.payload.index() != 6) {
      return Error::make(ErrorCode::UnsupportedOperation,
                         "merge audio does not belong to this operation");
    }
    auto &request = std::get<job::Merge>(candidate.payload);
    if (change.valueId == "first") {
      request.rules.audio = MergeAudioSelection::First;
    } else if (change.valueId == "second") {
      request.rules.audio = MergeAudioSelection::Second;
    } else if (change.valueId == "both") {
      request.rules.audio = MergeAudioSelection::Both;
    } else if (change.valueId == "none") {
      request.rules.audio = MergeAudioSelection::None;
    } else {
      return Error::make(ErrorCode::InvalidArgument,
                         "unknown merge audio selection");
    }
    if (const JsonValue *longer = payload.find("longerDuration")) {
      request.rules.longerDuration = longer->boolValue;
    }
    markUser(FieldId::MergeAudio);
    return {};
  }
  case FieldId::FrameSelection: {
    if (op != Operation::ExtractFrames) {
      return Error::make(ErrorCode::UnsupportedOperation,
                         "frame selection does not belong to this operation");
    }
    auto &request = std::get<job::ExtractFrames>(candidate.payload);
    std::string mode = "all";
    if (const JsonValue *modeValue = payload.find("mode")) {
      mode = modeValue->text;
    }
    if (mode == "current") {
      request.selection.mode = FrameSelectionMode::Current;
    } else if (mode == "selected") {
      request.selection.mode = FrameSelectionMode::Selected;
    } else if (mode == "interval") {
      request.selection.mode = FrameSelectionMode::Interval;
    } else if (mode == "frame-rate") {
      request.selection.mode = FrameSelectionMode::FrameRate;
    } else if (mode == "count") {
      request.selection.mode = FrameSelectionMode::Count;
    } else if (mode == "all") {
      request.selection.mode = FrameSelectionMode::AllInRange;
    } else if (mode == "keyframes") {
      request.selection.mode = FrameSelectionMode::Keyframes;
    } else if (mode == "scenes") {
      request.selection.mode = FrameSelectionMode::SceneChanges;
    } else {
      return Error::make(ErrorCode::InvalidArgument,
                         "unknown frame selection mode");
    }
    if (const JsonValue *interval = payload.find("intervalUs")) {
      const auto value = interval->asInt();
      if (value <= 0) {
        return Error::make(ErrorCode::InvalidArgument, "invalid interval");
      }
      request.selection.intervalUs = value;
    }
    if (const JsonValue *rate = payload.find("frameRate")) {
      auto parsed = rationalFromId(rate->text);
      if (!parsed.has_value()) {
        return Error::make(ErrorCode::InvalidFrameRate,
                           "invalid sampling rate");
      }
      request.selection.frameRate = *parsed;
    }
    if (const JsonValue *count = payload.find("count")) {
      const auto value = count->asInt();
      if (value < 1 || value > 100000) {
        return Error::make(ErrorCode::InvalidArgument,
                           "invalid frame count");
      }
      request.selection.count = std::int32_t(value);
    }
    if (const JsonValue *timestamps = payload.find("timestampsUs")) {
      if (timestamps->kind != JsonValue::Kind::Array) {
        return Error::make(ErrorCode::InvalidArgument,
                           "timestamps must be an array");
      }
      std::vector<std::int64_t> stamps;
      for (const auto &item : timestamps->items) {
        if (item.asInt() < 0) {
          return Error::make(ErrorCode::InvalidTimeRange,
                             "invalid frame timestamp");
        }
        stamps.push_back(item.asInt());
      }
      request.selection.timestampsUs = std::move(stamps);
    }
    if (const JsonValue *range = payload.find("range")) {
      TimeRange selectionRange;
      if (const JsonValue *start = range->find("startUs")) {
        selectionRange.startUs = start->asInt();
      }
      if (const JsonValue *end = range->find("endUs")) {
        selectionRange.endUs = end->asInt();
      }
      if (!selectionRange.valid()) {
        return Error::make(ErrorCode::InvalidTimeRange,
                           "invalid frame selection range");
      }
      request.selection.range = selectionRange;
    }
    markUser(FieldId::FrameSelection);
    return {};
  }
  case FieldId::ImageFormat: {
    if (op != Operation::ExtractFrames) {
      return Error::make(ErrorCode::UnsupportedOperation,
                         "image format does not belong to this operation");
    }
    auto &request = std::get<job::ExtractFrames>(candidate.payload);
    if (change.valueId == "jpeg") {
      request.image.format = ImageOutputRules::Format::Jpeg;
    } else if (change.valueId == "png") {
      request.image.format = ImageOutputRules::Format::Png;
    } else if (change.valueId == "webp") {
      request.image.format = ImageOutputRules::Format::WebP;
    } else {
      return Error::make(ErrorCode::InvalidArgument,
                         "unknown image format: " + change.valueId);
    }
    markUser(FieldId::ImageFormat);
    return {};
  }
  case FieldId::ImageQuality: {
    if (op != Operation::ExtractFrames) {
      return Error::make(ErrorCode::UnsupportedOperation,
                         "image quality does not belong to this operation");
    }
    auto &request = std::get<job::ExtractFrames>(candidate.payload);
    char *end = nullptr;
    errno = 0;
    const long long quality = std::strtoll(change.valueId.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0' || quality < 1 ||
        quality > 100) {
      return Error::make(ErrorCode::InvalidArgument, "invalid image quality");
    }
    request.image.quality = std::int32_t(quality);
    markUser(FieldId::ImageQuality);
    return {};
  }
  case FieldId::ImageDimensions: {
    if (op != Operation::ExtractFrames) {
      return Error::make(ErrorCode::UnsupportedOperation,
                         "image dimensions do not belong to this operation");
    }
    auto &request = std::get<job::ExtractFrames>(candidate.payload);
    if (const JsonValue *width = payload.find("maxWidth")) {
      const auto value = width->asInt();
      if (value != 0 && (value < 16 || value > 8192)) {
        return Error::make(ErrorCode::InvalidResolution,
                           "invalid image width");
      }
      request.image.maxWidth = value ? std::optional<std::int32_t>(
                                           std::int32_t(value))
                                     : std::nullopt;
    }
    if (const JsonValue *height = payload.find("maxHeight")) {
      const auto value = height->asInt();
      if (value != 0 && (value < 16 || value > 8192)) {
        return Error::make(ErrorCode::InvalidResolution,
                           "invalid image height");
      }
      request.image.maxHeight = value ? std::optional<std::int32_t>(
                                            std::int32_t(value))
                                      : std::nullopt;
    }
    markUser(FieldId::ImageDimensions);
    return {};
  }
  case FieldId::ExtractAudioTracks: {
    if (op != Operation::ExtractAudio) {
      return Error::make(ErrorCode::UnsupportedOperation,
                         "track selection does not belong to this operation");
    }
    auto &request = std::get<job::ExtractAudio>(candidate.payload);
    if (payload.kind != JsonValue::Kind::Array) {
      return Error::make(ErrorCode::InvalidArgument,
                         "track payload must be an array");
    }
    const std::int32_t trackCount =
        std::int32_t(session.infos.empty()
                         ? 0
                         : session.infos.front().audio.size());
    std::vector<std::int32_t> tracks;
    for (const auto &item : payload.items) {
      const auto index = std::int32_t(item.asInt());
      if (index < 0 || index >= trackCount) {
        return Error::make(ErrorCode::InvalidTrackIdentity,
                           "invalid audio track index");
      }
      tracks.push_back(index);
    }
    request.trackIndices = std::move(tracks);
    markUser(FieldId::ExtractAudioTracks);
    return {};
  }
  case FieldId::ExtractAudioRange: {
    if (op != Operation::ExtractAudio) {
      return Error::make(ErrorCode::UnsupportedOperation,
                         "range does not belong to this operation");
    }
    auto &request = std::get<job::ExtractAudio>(candidate.payload);
    TimeRange range;
    if (const JsonValue *start = payload.find("startUs")) {
      range.startUs = start->asInt();
    }
    if (const JsonValue *end = payload.find("endUs")) {
      range.endUs = end->asInt();
    }
    if (!range.valid()) {
      return Error::make(ErrorCode::InvalidTimeRange, "invalid range");
    }
    request.range = range;
    markUser(FieldId::ExtractAudioRange);
    return {};
  }
  }
  return Error::make(ErrorCode::UnsupportedOperation, "unhandled field");
}

void SessionManager::resolveAutomatic(Session &session, JobRequest &candidate,
                                      std::optional<RelatedChange> &) const {
  // Automatic resolution: fields whose accepted value is Automatic/Default
  // resolve from the current candidate dependencies. Explicit selections
  // are never silently overwritten (applyChange already handles conflicts
  // through related changes).
  if (candidate.payload.index() == 0) {
    auto &request = std::get<job::VideoConvert>(candidate.payload);
    const Container container = request.output.container.value_or(
        session.infos.empty() ? Container::Mp4
                              : session.infos.front().container);
    // Automatic video codec under a format that cannot hold the source
    // codec: resolve the container's preferred codec (mandate fallback
    // table). Automatic = unset.
    if (!request.video.codec.has_value()) {
      // nothing to do: the planner resolves "auto" per-execution and the
      // payload stays unset.
      (void)container;
    }
  }
}

// ---------------------------------------------------------------------------
// Supported-option derivation (mandate §13)
// ---------------------------------------------------------------------------

FieldOptions SessionManager::deriveOptions(const Session &session,
                                           FieldId field) const {
  FieldOptions options;
  options.field = field;
  const Operation op = session.operation;
  const MediaInfo *info =
      session.infos.empty() ? nullptr : &session.infos.front();
  const bool isAudioOp = op == Operation::AudioConvert ||
                         op == Operation::AudioCutMerge ||
                         op == Operation::ExtractAudio;

  auto acceptedContainer = [&]() -> Container {
    const JobRequest &request = session.accepted;
    switch (request.payload.index()) {
    case 0: {
      const auto &r = std::get<job::VideoConvert>(request.payload);
      return r.output.container.value_or(info ? info->container
                                              : Container::Mp4);
    }
    case 1: {
      const auto &r = std::get<job::AudioConvert>(request.payload);
      return r.output.container.value_or(info ? info->container
                                              : Container::M4a);
    }
    case 7: {
      const auto &r = std::get<job::ExtractAudio>(request.payload);
      return r.output.container.value_or(info ? info->container
                                              : Container::M4a);
    }
    default:
      return info ? info->container : Container::Mp4;
    }
  };

  switch (field) {
  case FieldId::Format: {
    if (op == Operation::VideoConvert || op == Operation::AudioConvert ||
        op == Operation::ExtractAudio) {
      for (Container container : writableContainersFor(op)) {
        OptionValue value;
        value.id = containerName(container);
        value.attributes["writable"] = "1";
        options.values.push_back(std::move(value));
      }
      // "original" is offered first when the source container is writable.
      if (info && containerWritable(info->container)) {
        OptionValue original;
        original.id = "original";
        options.values.insert(options.values.begin(),
                              std::move(original));
      }
    }
    break;
  }
  case FieldId::Codec: {
    if (op == Operation::VideoConvert) {
      OptionValue automatic;
      automatic.id = "auto";
      options.values.push_back(std::move(automatic));
      for (VideoCodec codec : encodableVideoCodecsFor(acceptedContainer())) {
        OptionValue value;
        value.id = codecName(codec);
        options.values.push_back(std::move(value));
      }
    }
    break;
  }
  case FieldId::Resolution: {
    if (op == Operation::VideoConvert) {
      OptionValue original;
      original.id = "original";
      options.values.push_back(std::move(original));
      for (const auto &resolution : kResolutions) {
        OptionValue value;
        value.id = resolution.id;
        value.attributes["width"] = numberId(resolution.landscapeWidth);
        value.attributes["height"] = numberId(resolution.landscapeHeight);
        options.values.push_back(std::move(value));
      }
      OptionValue custom;
      custom.id = "custom";
      options.values.push_back(std::move(custom));
    }
    break;
  }
  case FieldId::FrameRate: {
    if (op == Operation::VideoConvert) {
      OptionValue original;
      original.id = "original";
      options.values.push_back(std::move(original));
      for (const auto &rate : kRateOptions) {
        OptionValue value;
        value.id = rate.id;
        options.values.push_back(std::move(value));
      }
    }
    break;
  }
  case FieldId::CompressionEnabled: {
    if (op == Operation::VideoConvert || op == Operation::AudioConvert ||
        op == Operation::ExtractAudio) {
      options.values.push_back({"false", {}});
      options.values.push_back({"true", {}});
    }
    break;
  }
  case FieldId::CompressionPreset: {
    if (op == Operation::VideoConvert || op == Operation::AudioConvert ||
        op == Operation::ExtractAudio) {
      for (const auto preset : {CompressionPreset::HighQuality,
                                CompressionPreset::Balanced,
                                CompressionPreset::SmallerFile,
                                CompressionPreset::TargetSize,
                                CompressionPreset::Custom}) {
        OptionValue value;
        value.id = compressionPresetName(preset);
        options.values.push_back(std::move(value));
      }
    }
    break;
  }
  case FieldId::CompressionQuality:
  case FieldId::CompressionVideoBitrate:
  case FieldId::CompressionAudioBitrate:
  case FieldId::CompressionTargetSize: {
    // Free numeric input; no enumerated options. The UI uses the field's
    // attributes (ranges) rather than a value list.
    break;
  }
  case FieldId::AudioCodec: {
    if (op == Operation::VideoConvert || op == Operation::AudioConvert ||
        op == Operation::ExtractAudio) {
      OptionValue automatic;
      automatic.id = "auto";
      options.values.push_back(std::move(automatic));
      for (AudioCodec codec : isAudioOp
                                   ? extractableAudioCodecsFor(
                                         acceptedContainer())
                                   : encodableAudioCodecsFor(
                                         acceptedContainer())) {
        OptionValue value;
        value.id = codecName(codec);
        options.values.push_back(std::move(value));
      }
    }
    break;
  }
  case FieldId::AudioBitrate: {
    if (op == Operation::VideoConvert || op == Operation::AudioConvert ||
        op == Operation::ExtractAudio) {
      OptionValue automatic;
      automatic.id = "auto";
      options.values.push_back(std::move(automatic));
      for (std::int64_t bitrate : {64000, 96000, 128000, 160000, 192000,
                                   256000, 320000}) {
        OptionValue value;
        value.id = numberId(bitrate);
        options.values.push_back(std::move(value));
      }
    }
    break;
  }
  case FieldId::AudioSampleRate: {
    if (op == Operation::VideoConvert || op == Operation::AudioConvert ||
        op == Operation::ExtractAudio) {
      OptionValue original;
      original.id = "original";
      options.values.push_back(std::move(original));
      for (std::int32_t rate : kSampleRates) {
        OptionValue value;
        value.id = numberId(rate);
        options.values.push_back(std::move(value));
      }
    }
    break;
  }
  case FieldId::AudioChannels: {
    if (op == Operation::VideoConvert || op == Operation::AudioConvert ||
        op == Operation::ExtractAudio) {
      OptionValue original;
      original.id = "original";
      options.values.push_back(std::move(original));
      options.values.push_back({"1", {{"layout", "mono"}}});
      options.values.push_back({"2", {{"layout", "stereo"}}});
      options.values.push_back({"6", {{"layout", "5.1"}}});
    }
    break;
  }
  case FieldId::TimelineMode: {
    if (op == Operation::VideoCutMerge || op == Operation::AudioCutMerge) {
      for (const auto mode : {TimelineMode::Trim, TimelineMode::Cut,
                              TimelineMode::Split, TimelineMode::Merge}) {
        OptionValue value;
        value.id = timelineModeName(mode);
        options.values.push_back(std::move(value));
      }
    }
    break;
  }
  case FieldId::MergeLayout: {
    if (op == Operation::VideoCutMerge) {
      options.values.push_back({"sequential", {}});
      options.values.push_back({"side-by-side", {}});
      options.values.push_back({"top-bottom", {}});
    }
    break;
  }
  case FieldId::MergeAudio: {
    if (op == Operation::VideoCutMerge) {
      options.values.push_back({"first", {}});
      options.values.push_back({"second", {}});
      options.values.push_back({"both", {}});
      options.values.push_back({"none", {}});
    }
    break;
  }
  case FieldId::ImageFormat: {
    if (op == Operation::ExtractFrames) {
      options.values.push_back({"jpeg", {}});
      options.values.push_back({"png", {}});
      // Still WebP requires the libwebp encoder (verified through the
      // capability registry by the engine wiring; the catalog marks it
      // available when an encoder-capable backend exists).
      if (features_.knowsVideoCodec(VideoCodec::Mjpeg) &&
          capabilities_.registeredBackends().size() > 0) {
        options.values.push_back({"webp", {}});
      }
    }
    break;
  }
  case FieldId::SubtitleTarget: {
    if (op == Operation::ExtractSubtitles) {
      options.values.push_back({"original", {}});
      options.values.push_back({"srt", {}});
      options.values.push_back({"vtt", {}});
      options.values.push_back({"ass", {}});
    }
    break;
  }
  case FieldId::AudioTracks:
  case FieldId::Subtitles:
  case FieldId::SubtitleTracks:
  case FieldId::Adjustments:
  case FieldId::Metadata:
  case FieldId::OutputName:
  case FieldId::CropRect:
  case FieldId::TimelineRanges:
  case FieldId::SplitRules:
  case FieldId::FrameSelection:
  case FieldId::ImageQuality:
  case FieldId::ImageDimensions:
  case FieldId::ExtractAudioTracks:
  case FieldId::ExtractAudioRange:
    // Structured payload fields: no enumerated values (the payload schema
    // itself is the contract; the UI edits structured data).
    break;
  }
  return options;
}

// ---------------------------------------------------------------------------
// Samples + submission
// ---------------------------------------------------------------------------

Result<SampleResult>
SessionManager::generateSample(const std::string &sessionId,
                               std::uint64_t expectedRevision) {
  std::unique_lock<std::mutex> lock;
  auto sessionResult = lockedSession(sessionId, lock);
  if (!sessionResult.ok()) {
    return sessionResult.error();
  }
  SessionPtr session = sessionResult.value();
  if (session->state != SessionState::Ready) {
    return Error::make(ErrorCode::InvalidArgument, "session is not prepared");
  }
  if (expectedRevision != session->revision) {
    return Error::make(ErrorCode::StaleRevision, "stale revision");
  }
  if (!hooks_.generateSample) {
    return Error::make(ErrorCode::UnsupportedOperation,
                       "sample generation unavailable");
  }
  // Sample generation performs real work; run it without holding the
  // session lock (the mandate: no engine-wide locks across I/O).
  JobRequest request = session->accepted;
  lock.unlock();
  auto sample = hooks_.generateSample(request);
  lock.lock();
  if (sample.ok()) {
    session->samplePath = sample.value().outputPath;
    session->sampleRevision = session->revision;
  }
  return sample;
}

Result<std::string>
SessionManager::submitSession(const std::string &sessionId,
                              std::uint64_t expectedRevision,
                              const std::string &submissionId) {
  std::unique_lock<std::mutex> lock;
  auto sessionResult = lockedSession(sessionId, lock);
  if (!sessionResult.ok()) {
    return sessionResult.error();
  }
  SessionPtr session = sessionResult.value();
  if (session->state != SessionState::Ready) {
    return Error::make(ErrorCode::InvalidArgument, "session is not prepared");
  }
  if (submissionId.empty()) {
    return Error::make(ErrorCode::InvalidArgument, "empty submission id");
  }
  // Idempotency: the same submissionId returns the same job (mandate
  // §14.2), both in-session and through the durable store.
  if (session->lastSubmissionId == submissionId &&
      !session->lastSubmittedJobId.empty()) {
    return session->lastSubmittedJobId;
  }
  if (expectedRevision != session->revision) {
    return Error::make(ErrorCode::StaleRevision, "stale revision");
  }
  if (!session->configurationValid) {
    return Error::make(ErrorCode::NoCompatiblePipeline,
                       session->validationMessage.empty()
                           ? "configuration is not valid"
                           : session->validationMessage);
  }
  if (!hooks_.submit) {
    return Error::make(ErrorCode::InternalError, "no submit hook wired");
  }
  JobRequest request = session->accepted;
  std::vector<MediaInfo> infos = session->infos;
  lock.unlock();
  auto jobId = hooks_.submit(request, infos, submissionId);
  lock.lock();
  if (jobId.ok()) {
    session->lastSubmissionId = submissionId;
    session->lastSubmittedJobId = jobId.value();
  }
  return jobId;
}

Result<void> SessionManager::closeSession(const std::string &sessionId) {
  std::lock_guard<std::mutex> mapLock(mutex_);
  auto it = sessions_.find(sessionId);
  if (it == sessions_.end()) {
    return Error::make(ErrorCode::InvalidArgument, "unknown session");
  }
  {
    std::lock_guard<std::mutex> sessionLock(it->second->mutex);
    it->second->state = SessionState::Closed;
  }
  sessions_.erase(it);
  return {};
}

Result<SessionSnapshot> SessionManager::seedAccepted(const std::string &sessionId,
                                                    const JobRequest &request) {
  std::unique_lock<std::mutex> lock;
  auto sessionResult = lockedSession(sessionId, lock);
  if (!sessionResult.ok()) {
    return sessionResult.error();
  }
  SessionPtr session = sessionResult.value();
  if (session->state != SessionState::Ready) {
    return Error::make(ErrorCode::InvalidArgument, "session is not prepared");
  }
  if (request.operation != session->operation) {
    return Error::make(ErrorCode::InvalidArgument,
                       "seeded request does not match the session operation");
  }
  // The stored job's accepted intent becomes this session's accepted
  // configuration verbatim (a NEW editable configuration; the original
  // job stays immutable). Provenance is Automatic: the values came from
  // resolved state, not from this session's user.
  session->accepted = request;
  session->origins.clear();
  session->relatedChange.reset();
  session->samplePath.reset();
  ++session->revision;
  if (hooks_.assess) {
    auto assessment = hooks_.assess(session->accepted, session->infos);
    if (assessment.ok()) {
      session->configurationValid = assessment.value().feasible;
      session->validationMessage = assessment.value().message;
      session->estimate = assessment.value().estimate;
      session->pipelineSummary = assessment.value().pipelineSummary;
    } else {
      session->configurationValid = false;
      session->validationMessage = assessment.error().message;
      session->estimate = OutputEstimate{};
    }
  }
  return snapshotOf(*session);
}

void SessionManager::closeAll() {
  std::lock_guard<std::mutex> mapLock(mutex_);
  sessions_.clear();
}

// ---------------------------------------------------------------------------
// Capability helpers
// ---------------------------------------------------------------------------

bool SessionManager::containerWritable(Container container) const {
  return !capabilities_.backendsThatWrite(container).empty();
}

bool SessionManager::containerReadable(Container container) const {
  return !capabilities_.backendsThatRead(container).empty();
}

bool SessionManager::videoEncodeSupported(VideoCodec codec,
                                          Container container) const {
  if (!features_.videoSupport(codec).encode ||
      !features_.containerAccepts(container, codec)) {
    return false;
  }
  return !capabilities_.backendsFor(codec, CodecOp::Encode).empty();
}

bool SessionManager::audioEncodeSupported(AudioCodec codec,
                                          Container container) const {
  if (!features_.audioSupport(codec).encode ||
      !features_.containerAccepts(container, codec)) {
    return false;
  }
  return !capabilities_.backendsFor(codec, CodecOp::Encode).empty();
}

bool SessionManager::audioDecodeSupported(AudioCodec codec) const {
  if (!features_.audioSupport(codec).decode) {
    return false;
  }
  return !capabilities_.backendsFor(codec, CodecOp::Decode).empty();
}

bool SessionManager::videoDecodeSupported(VideoCodec codec) const {
  if (!features_.videoSupport(codec).decode) {
    return false;
  }
  return !capabilities_.backendsFor(codec, CodecOp::Decode).empty();
}

std::vector<Container>
SessionManager::writableContainersFor(Operation op) const {
  // The intentionally supported output containers per operation domain
  // (mandate §19.2 video coverage; §31.1 audio coverage), intersected
  // with what a backend on this build can actually write.
  static const std::vector<Container> kVideoContainers = {
      Container::Mp4, Container::M4v, Container::Mov, Container::Mkv,
      Container::WebM, Container::Avi, Container::MpegTs, Container::Flv,
      Container::ThreeGp, Container::Asf,
  };
  static const std::vector<Container> kAudioContainers = {
      Container::M4a, Container::Mp3, Container::Wav, Container::Flac,
      Container::Ogg, Container::Asf,
  };
  const std::vector<Container> &domain =
      (op == Operation::AudioConvert || op == Operation::AudioCutMerge ||
       op == Operation::ExtractAudio)
          ? kAudioContainers
          : kVideoContainers;
  std::vector<Container> result;
  for (Container container : domain) {
    if (features_.knowsContainer(container) && containerWritable(container)) {
      result.push_back(container);
    }
  }
  return result;
}

std::vector<VideoCodec>
SessionManager::encodableVideoCodecsFor(Container container) const {
  std::vector<VideoCodec> result;
  for (const auto &feature : features_.videoCodecs()) {
    if (feature.support.encode &&
        features_.containerAccepts(container, feature.codec) &&
        !capabilities_.backendsFor(feature.codec, CodecOp::Encode).empty()) {
      result.push_back(feature.codec);
    }
  }
  return result;
}

std::vector<AudioCodec>
SessionManager::encodableAudioCodecsFor(Container container) const {
  std::vector<AudioCodec> result;
  for (const auto &feature : features_.audioCodecs()) {
    if (feature.support.encode &&
        features_.containerAccepts(container, feature.codec) &&
        !capabilities_.backendsFor(feature.codec, CodecOp::Encode).empty()) {
      result.push_back(feature.codec);
    }
  }
  return result;
}

std::vector<AudioCodec>
SessionManager::extractableAudioCodecsFor(Container container) const {
  // For standalone audio output, codecs that can be encoded OR whose
  // source representation can be copied into this container.
  std::vector<AudioCodec> result = encodableAudioCodecsFor(container);
  return result;
}

JobRequest SessionManager::initialRequest(
    Operation operation, const std::vector<MediaSource> &sources,
    const std::vector<MediaInfo> &infos) const {
  JobRequest request;
  request.operation = operation;
  const MediaInfo *info = infos.empty() ? nullptr : &infos.front();
  const auto defaultContainer = [&](Container fallback) -> Container {
    if (info && containerWritable(info->container)) {
      return info->container;
    }
    return fallback;
  };

  switch (operation) {
  case Operation::VideoConvert: {
    job::VideoConvert payload;
    payload.source = sources.front();
    payload.output.container = defaultContainer(Container::Mp4);
    request.payload = std::move(payload);
    break;
  }
  case Operation::AudioConvert: {
    job::AudioConvert payload;
    payload.source = sources.front();
    payload.output.container = defaultContainer(Container::M4a);
    request.payload = std::move(payload);
    break;
  }
  case Operation::VideoCrop: {
    job::Crop payload;
    payload.source = sources.front();
    if (info && !info->video.empty()) {
      // Default crop = full display frame.
      payload.crop.width = info->video.front().width;
      payload.crop.height = info->video.front().height;
    }
    request.payload = std::move(payload);
    break;
  }
  case Operation::VideoCutMerge:
  case Operation::AudioCutMerge: {
    job::Trim payload;
    payload.source = sources.front();
    request.payload = std::move(payload);
    break;
  }
  case Operation::ExtractAudio: {
    job::ExtractAudio payload;
    payload.source = sources.front();
    payload.output.container = defaultContainer(Container::M4a);
    request.payload = std::move(payload);
    break;
  }
  case Operation::ExtractFrames: {
    job::ExtractFrames payload;
    payload.source = sources.front();
    payload.selection.mode = FrameSelectionMode::AllInRange;
    request.payload = std::move(payload);
    break;
  }
  case Operation::ExtractSubtitles: {
    job::ExtractSubtitles payload;
    payload.source = sources.front();
    request.payload = std::move(payload);
    break;
  }
  }
  return request;
}

} // namespace velmux::core
