#include "persistence/job_store.h"

#include "api/serialization.h"
#include "serialization/json.h"

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <functional>
#include <mutex>

namespace velmux::core {

namespace {

std::string columnText(sqlite3_stmt &stmt, int col) {
  const unsigned char *text = sqlite3_column_text(&stmt, col);
  if (text == nullptr) {
    return {};
  }
  return std::string(reinterpret_cast<const char *>(text));
}

} // namespace

// ---------------------------------------------------------------------------
// SqliteStore
// ---------------------------------------------------------------------------

SqliteStore::SqliteStore() = default;

SqliteStore::~SqliteStore() { close(); }

Error SqliteStore::open(const std::string &path) {
  close();
  sqlite3 *db = nullptr;
  const int rc = sqlite3_open_v2(path.c_str(), &db,
                                 SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                     SQLITE_OPEN_FULLMUTEX,
                                 nullptr);
  if (rc != SQLITE_OK) {
    const std::string message =
        db != nullptr ? sqlite3_errmsg(db) : "sqlite3_open failed";
    if (db != nullptr) {
      sqlite3_close(db);
    }
    return Error::make(ErrorCode::InternalError,
                       "cannot open database " + path + ": " + message);
  }
  db_ = db;
  sqlite3_busy_timeout(db_, 5000);
  Error error;
  if (!exec("PRAGMA journal_mode=WAL", error) ||
      !exec("PRAGMA synchronous=NORMAL", error) ||
      !exec("PRAGMA foreign_keys=ON", error)) {
    close();
    return error;
  }
  error = migrate();
  if (!error.ok()) {
    close();
    return error;
  }
  return {};
}

void SqliteStore::close() {
  if (db_ != nullptr) {
    sqlite3_close_v2(db_);
    db_ = nullptr;
  }
}

std::string SqliteStore::quote(const std::string &text) const {
  std::string result = "'";
  for (char c : text) {
    if (c == '\'') {
      result += "''";
    } else {
      result += c;
    }
  }
  result += "'";
  return result;
}

bool SqliteStore::exec(const std::string &sql, Error &error) {
  if (db_ == nullptr) {
    error = Error::make(ErrorCode::InternalError, "database not open");
    return false;
  }
  char *errMsg = nullptr;
  const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errMsg);
  if (rc != SQLITE_OK) {
    error = Error::make(
        ErrorCode::InternalError,
        "sql failed: " + std::string(errMsg ? errMsg : "unknown error") +
            " [" + sql.substr(0, 80) + "]");
    sqlite3_free(errMsg);
    return false;
  }
  return true;
}

sqlite3_stmt *SqliteStore::prepare(const std::string &sql, Error &error) {
  if (db_ == nullptr) {
    error = Error::make(ErrorCode::InternalError, "database not open");
    return nullptr;
  }
  sqlite3_stmt *stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    error = Error::make(ErrorCode::InternalError,
                        std::string("prepare failed: ") + sqlite3_errmsg(db_));
    return nullptr;
  }
  return stmt;
}

bool SqliteStore::query(const std::string &sql, const RowCallback &row,
                        Error &error) {
  sqlite3_stmt *stmt = prepare(sql, error);
  if (stmt == nullptr) {
    return false;
  }
  bool ok = true;
  for (;;) {
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      if (!row(*stmt)) {
        break;
      }
      continue;
    }
    if (rc == SQLITE_DONE) {
      break;
    }
    error = Error::make(ErrorCode::InternalError,
                        std::string("step failed: ") + sqlite3_errmsg(db_));
    ok = false;
    break;
  }
  sqlite3_finalize(stmt);
  return ok;
}

std::int64_t SqliteStore::lastInsertRowId() const {
  return db_ != nullptr ? sqlite3_last_insert_rowid(db_) : 0;
}

// ---------------------------------------------------------------------------
// Schema v4 creation
// ---------------------------------------------------------------------------

namespace {

const char *kJobsV4Schema =
    "CREATE TABLE IF NOT EXISTS jobs_v4 ("
    "  id TEXT PRIMARY KEY,"
    "  submission_id TEXT,"
    "  state TEXT,"
    "  operation TEXT,"
    "  request_json TEXT,"
    "  source_info_json TEXT,"
    "  pipeline_summary TEXT,"
    "  uses_hardware INTEGER NOT NULL DEFAULT 0,"
    "  output_path TEXT,"
    "  outputs_json TEXT,"
    "  progress REAL NOT NULL DEFAULT -1,"
    "  input_size INTEGER NOT NULL DEFAULT -1,"
    "  output_size INTEGER NOT NULL DEFAULT -1,"
    "  error TEXT,"
    "  error_code TEXT,"
    "  created_at INTEGER NOT NULL,"
    "  updated_at INTEGER NOT NULL,"
    "  started_at INTEGER,"
    "  finished_at INTEGER,"
    "  schema_version INTEGER NOT NULL DEFAULT 4"
    ")";

const char *kLegacyJobsSchema =
    "CREATE TABLE IF NOT EXISTS jobs ("
    "  id TEXT PRIMARY KEY,"
    "  state TEXT NOT NULL,"
    "  source_uri TEXT NOT NULL,"
    "  source_name TEXT NOT NULL,"
    "  output_name TEXT,"
    "  output_path TEXT,"
    "  output_container TEXT,"
    "  goal_kind TEXT NOT NULL,"
    "  goal_target_bytes INTEGER NOT NULL DEFAULT 0,"
    "  video_json TEXT NOT NULL DEFAULT '{}',"
    "  audio_json TEXT NOT NULL DEFAULT '{}',"
    "  subtitles_json TEXT NOT NULL DEFAULT '{}',"
    "  processing_mode TEXT NOT NULL DEFAULT 'auto',"
    "  pipeline_json TEXT NOT NULL DEFAULT '{}',"
    "  source_info_json TEXT NOT NULL DEFAULT '{}',"
    "  progress REAL NOT NULL DEFAULT -1,"
    "  input_size INTEGER NOT NULL DEFAULT -1,"
    "  output_size INTEGER NOT NULL DEFAULT -1,"
    "  error TEXT,"
    "  created_at INTEGER NOT NULL,"
    "  updated_at INTEGER NOT NULL,"
    "  started_at INTEGER,"
    "  finished_at INTEGER,"
    "  schema_version INTEGER NOT NULL DEFAULT 1,"
    "  trim_start_us INTEGER NOT NULL DEFAULT 0,"
    "  trim_end_us INTEGER NOT NULL DEFAULT -1,"
    "  strip_metadata INTEGER NOT NULL DEFAULT 0"
    ")";

const char *kCheckpointsSchema =
    "CREATE TABLE IF NOT EXISTS checkpoints ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  job_id TEXT NOT NULL,"
    "  sequence INTEGER NOT NULL,"
    "  stage TEXT NOT NULL,"
    "  position INTEGER NOT NULL,"
    "  duration_us INTEGER NOT NULL DEFAULT 0,"
    "  artifact_path TEXT,"
    "  artifact_bytes INTEGER NOT NULL DEFAULT 0,"
    "  committed_at INTEGER NOT NULL,"
    "  UNIQUE (job_id, sequence)"
    ")";

const char *kTelemetryPerfSchema =
    "CREATE TABLE IF NOT EXISTS telemetry_perf ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  backend TEXT NOT NULL,"
    "  video_codec TEXT,"
    "  audio_codec TEXT,"
    "  fps REAL NOT NULL DEFAULT 0,"
    "  setup_ms REAL NOT NULL DEFAULT 0,"
    "  transfer_ms_per_frame REAL NOT NULL DEFAULT 0,"
    "  success INTEGER NOT NULL DEFAULT 1,"
    "  recorded_at INTEGER NOT NULL,"
    "  note TEXT"
    ")";

const char *kTelemetryPlanningSchema =
    "CREATE TABLE IF NOT EXISTS telemetry_planning ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  planning_ms REAL NOT NULL,"
    "  recorded_at INTEGER NOT NULL"
    ")";

// ---------------------------------------------------------------------------
// v3 -> v4 migration: legacy k=v rows -> typed JSON requests, one
// transaction, every row preserved (spec §4).
// ---------------------------------------------------------------------------

void parsePairs(const std::string &data,
                const std::function<void(const std::string &,
                                         const std::string &)> &visit) {
  std::size_t pos = 0;
  while (pos < data.size()) {
    const std::size_t semi = data.find(';', pos);
    const std::string pair =
        data.substr(pos, semi == std::string::npos ? std::string::npos
                                                   : semi - pos);
    const std::size_t eq = pair.find('=');
    if (eq != std::string::npos) {
      visit(pair.substr(0, eq), pair.substr(eq + 1));
    }
    if (semi == std::string::npos) {
      break;
    }
    pos = semi + 1;
  }
}

struct LegacyRequest {
  bool videoEnabled = true;
  bool audioEnabled = true;
  std::optional<VideoCodec> videoCodec;
  std::optional<std::int32_t> resolutionHeight;
  std::optional<double> frameRate;
  std::optional<std::string> videoProfile;
  std::optional<std::string> videoLevel;
  std::optional<std::int32_t> videoQuality;
  std::optional<std::int64_t> videoBitrate;
  bool cropActive = false;
  std::int32_t cropX = 0;
  std::int32_t cropY = 0;
  std::int32_t cropW = 0;
  std::int32_t cropH = 0;
  std::int32_t rotation = 0;
  bool flipH = false;
  bool flipV = false;
  std::optional<AudioCodec> audioCodec;
  std::optional<std::int64_t> audioBitrate;
  std::optional<std::int32_t> audioSampleRate;
  std::optional<std::int32_t> audioChannels;
  std::optional<std::int32_t> pcmBitDepth;
  std::optional<double> gainDb;
  std::optional<std::int32_t> fadeInMs;
  std::optional<std::int32_t> fadeOutMs;
  bool normalize = false;
  std::optional<double> audioSpeed;
  bool removeSilence = false;
  std::int32_t sourceTrackIndex = -1;
  bool allTracks = false;
  std::optional<std::string> replaceAudioUri;
  std::optional<std::string> coverArtUri;
  bool preserveCoverArt = true;
  int subtitlePolicy = 0; // SubtitlePolicy: 0 preserve, 1 remove, 2 extract, 3 burnIn
  std::optional<std::string> subtitleExternalUri;
};

LegacyRequest parseLegacyRequest(const std::string &videoJson,
                                 const std::string &audioJson,
                                 const std::string &subsJson) {
  LegacyRequest legacy;
  parsePairs(videoJson, [&](const std::string &key, const std::string &value) {
    try {
      if (key == "enabled") {
        legacy.videoEnabled = value == "1";
      } else if (key == "codec") {
        legacy.videoCodec = videoCodecFromName(value);
      } else if (key == "height") {
        legacy.resolutionHeight = std::stoi(value);
      } else if (key == "fps") {
        legacy.frameRate = std::stod(value);
      } else if (key == "profile") {
        legacy.videoProfile = value;
      } else if (key == "level") {
        legacy.videoLevel = value;
      } else if (key == "bitrate") {
        legacy.videoBitrate = std::stoll(value);
      } else if (key == "quality") {
        legacy.videoQuality = std::stoi(value);
      } else if (key == "crop") {
        // x,y,w,h
        const auto comma1 = value.find(',');
        const auto comma2 = value.find(',', comma1 + 1);
        const auto comma3 = value.find(',', comma2 + 1);
        if (comma1 != std::string::npos && comma2 != std::string::npos &&
            comma3 != std::string::npos) {
          legacy.cropX = std::stoi(value.substr(0, comma1));
          legacy.cropY = std::stoi(value.substr(comma1 + 1, comma2 - comma1 - 1));
          legacy.cropW = std::stoi(value.substr(comma2 + 1, comma3 - comma2 - 1));
          legacy.cropH = std::stoi(value.substr(comma3 + 1));
          legacy.cropActive = legacy.cropW >= 2 && legacy.cropH >= 2;
        }
      } else if (key == "rotate") {
        legacy.rotation = std::stoi(value);
      } else if (key == "fliph") {
        legacy.flipH = value == "1";
      } else if (key == "flipv") {
        legacy.flipV = value == "1";
      }
    } catch (const std::exception &) {
      // A malformed pair must never break migration.
    }
  });
  parsePairs(audioJson, [&](const std::string &key, const std::string &value) {
    try {
      if (key == "enabled") {
        legacy.audioEnabled = value == "1";
      } else if (key == "codec") {
        legacy.audioCodec = audioCodecFromName(value);
      } else if (key == "bitrate") {
        legacy.audioBitrate = std::stoll(value);
      } else if (key == "rate") {
        legacy.audioSampleRate = std::stoi(value);
      } else if (key == "channels") {
        legacy.audioChannels = std::stoi(value);
      } else if (key == "gain") {
        legacy.gainDb = std::stod(value);
      } else if (key == "fadein") {
        legacy.fadeInMs = std::stoi(value);
      } else if (key == "fadeout") {
        legacy.fadeOutMs = std::stoi(value);
      } else if (key == "norm") {
        legacy.normalize = value == "1";
      } else if (key == "speed") {
        legacy.audioSpeed = std::stod(value);
      } else if (key == "desil") {
        legacy.removeSilence = value == "1";
      } else if (key == "depth") {
        legacy.pcmBitDepth = std::stoi(value);
      } else if (key == "srctrack") {
        legacy.sourceTrackIndex = std::stoi(value);
      } else if (key == "alltracks") {
        legacy.allTracks = value == "1";
      } else if (key == "replace") {
        legacy.replaceAudioUri = value;
      } else if (key == "cover") {
        legacy.coverArtUri = value;
      } else if (key == "keepcover") {
        legacy.preserveCoverArt = value == "1";
      }
    } catch (const std::exception &) {
    }
  });
  parsePairs(subsJson, [&](const std::string &key, const std::string &value) {
    try {
      if (key == "policy") {
        legacy.subtitlePolicy = std::stoi(value);
      } else if (key == "external") {
        legacy.subtitleExternalUri = value;
      }
    } catch (const std::exception &) {
    }
  });
  return legacy;
}

// Legacy goal -> compression intent (spec §4 mapping).
CompressionIntent compressionFromLegacy(const std::string &goalKind,
                                        std::int64_t goalTargetBytes,
                                        const LegacyRequest &legacy) {
  CompressionIntent compression;
  compression.enabled = true; // legacy rows always carried an active goal
  if (goalKind == "0" || goalKind == "recommended") {
    compression.preset = CompressionPreset::Balanced;
  } else if (goalKind == "1" || goalKind == "smallerFile") {
    compression.preset = CompressionPreset::SmallerFile;
  } else if (goalKind == "2" || goalKind == "highQuality") {
    compression.preset = CompressionPreset::HighQuality;
  } else if (goalKind == "3" || goalKind == "fitUnderSize") {
    compression.preset = CompressionPreset::TargetSize;
    compression.targetSizeBytes = goalTargetBytes;
  } else {
    // custom: honor explicit rate-control values where the new model has
    // an equivalent; the rateControl enum itself is retired.
    compression.preset = CompressionPreset::Custom;
    compression.quality = legacy.videoQuality;
    if (legacy.videoBitrate.has_value() && *legacy.videoBitrate > 0) {
      compression.videoBitrate = legacy.videoBitrate;
    }
  }
  return compression;
}

// all-Clear metadata intention (the retired stripMetadata control).
MetadataIntention strippedMetadata() {
  MetadataIntention metadata;
  metadata.titleAction = MetadataFieldAction::Clear;
  metadata.artistAction = MetadataFieldAction::Clear;
  metadata.albumAction = MetadataFieldAction::Clear;
  metadata.genreAction = MetadataFieldAction::Clear;
  metadata.dateAction = MetadataFieldAction::Clear;
  metadata.descriptionAction = MetadataFieldAction::Clear;
  metadata.locationAction = MetadataFieldAction::Clear;
  metadata.artworkAction = MetadataFieldAction::Clear;
  return metadata;
}

// Legacy frame rate (double) -> the closest rational representation.
Rational rationalFromDouble(double rate) {
  if (rate <= 0.0) {
    return Rational{};
  }
  std::int64_t num = std::int64_t(std::llround(rate * 1000000.0));
  std::int64_t den = 1000000;
  // Reduce by common powers of two ONLY: halving an odd denominator with
  // integer division silently corrupts the value (24.0 -> 24.0015).
  while (den > 1 && num % 2 == 0 && den % 2 == 0) {
    num /= 2;
    den /= 2;
  }
  if (num > 1000000LL * den) {
    num = std::int64_t(rate);
    den = 1;
  }
  return Rational{num, den};
}

bool isAudioOnlyContainer(Container container) {
  switch (container) {
    case Container::M4a:
    case Container::Mp3:
    case Container::Ogg:
    case Container::Wav:
    case Container::Flac:
      return true;
    default:
      return false;
  }
}

// Audio settings shared by every legacy shape that carries them.
AudioSettings legacyAudioSettings(const LegacyRequest &legacy) {
  AudioSettings audio;
  audio.codec = legacy.audioCodec;
  audio.bitRate = legacy.audioBitrate;
  audio.sampleRate = legacy.audioSampleRate;
  audio.channels = legacy.audioChannels;
  audio.pcmBitDepth = legacy.pcmBitDepth;
  return audio;
}

// The legacy audio track management -> audioTracks rules (mandate §23).
std::vector<AudioTrackRule>
legacyAudioTrackRules(const LegacyRequest &legacy) {
  std::vector<AudioTrackRule> rules;
  if (legacy.replaceAudioUri.has_value()) {
    AudioTrackRule rule;
    rule.sourceTrackIndex = std::max<std::int32_t>(legacy.sourceTrackIndex, 0);
    rule.include = false;
    rule.externalUri = legacy.replaceAudioUri;
    rule.replace = true;
    rules.push_back(std::move(rule));
    return rules;
  }
  if (legacy.allTracks) {
    return {}; // preserve all: the empty rule list
  }
  if (!legacy.audioEnabled) {
    // Audio disabled: one removal rule (index -1 = remove the lane).
    AudioTrackRule rule;
    rule.sourceTrackIndex = -1;
    rule.include = false;
    rules.push_back(std::move(rule));
    return rules;
  }
  if (legacy.sourceTrackIndex >= 0) {
    AudioTrackRule rule;
    rule.sourceTrackIndex = legacy.sourceTrackIndex;
    rule.include = true;
    rules.push_back(std::move(rule));
  }
  return rules;
}

// Legacy subtitle policy -> one subtitleTracks rule (spec §4).
std::vector<SubtitleTrackRule>
legacySubtitleRules(const LegacyRequest &legacy) {
  std::vector<SubtitleTrackRule> rules;
  // 0 preserve -> the empty list (preserve all); the rest carry the policy.
  switch (legacy.subtitlePolicy) {
    case 1: { // remove
      SubtitleTrackRule rule;
      rule.policy = SubtitleTrackPolicy::Remove;
      rules.push_back(std::move(rule));
      break;
    }
    case 3: { // burnIn
      SubtitleTrackRule rule;
      rule.policy = SubtitleTrackPolicy::BurnIn;
      rule.externalUri = legacy.subtitleExternalUri;
      rules.push_back(std::move(rule));
      break;
    }
    default:
      break; // preserve (0) / extract (2): extract handled as its own op
  }
  return rules;
}

// Transforms one legacy row into a v4 (operation, request_json) pair.
std::pair<std::string, std::string>
migrateRow(const std::string &sourceUri, const std::string &sourceName,
           const std::optional<std::string> &outputName,
           const std::string &outputContainer, const std::string &goalKind,
           std::int64_t goalTargetBytes, std::int64_t trimStartUs,
           std::int64_t trimEndUs, bool stripMetadata,
           const std::string &videoJson, const std::string &audioJson,
           const std::string &subsJson) {
  const LegacyRequest legacy =
      parseLegacyRequest(videoJson, audioJson, subsJson);
  const CompressionIntent compression =
      compressionFromLegacy(goalKind, goalTargetBytes, legacy);
  MetadataIntention metadata =
      stripMetadata ? strippedMetadata() : MetadataIntention{};
  if (legacy.coverArtUri.has_value()) {
    metadata.artworkAction = MetadataFieldAction::Set;
    metadata.artworkUri = *legacy.coverArtUri;
  } else if (!legacy.preserveCoverArt) {
    metadata.artworkAction = MetadataFieldAction::Clear;
  }

  OutputSpec output;
  output.name = outputName;
  output.container = containerFromName(outputContainer);

  JobRequest request;
  const bool trimActive = trimStartUs != 0 || trimEndUs != -1;
  const bool hasAudioDsp = legacy.gainDb.has_value() ||
                           legacy.fadeInMs.has_value() ||
                           legacy.fadeOutMs.has_value() || legacy.normalize ||
                           legacy.audioSpeed.has_value() ||
                           legacy.removeSilence;

  if (legacy.subtitlePolicy == 2) { // extract -> the ExtractSubtitles op
    request.operation = Operation::ExtractSubtitles;
    job::ExtractSubtitles payload;
    payload.source = MediaSource{sourceUri, sourceName};
    payload.output = output;
    request.payload = std::move(payload);
  } else if (trimActive) {
    // Trim columns set -> video-cutmerge with a trim payload.
    request.operation = Operation::VideoCutMerge;
    job::Trim payload;
    payload.source = MediaSource{sourceUri, sourceName};
    payload.output = output;
    payload.range.startUs = trimStartUs;
    payload.range.endUs = trimEndUs;
    request.payload = std::move(payload);
  } else if (!legacy.videoEnabled) {
    if (hasAudioDsp) {
      // The audio DSP chain only exists on the Audio Converter now.
      request.operation = Operation::AudioConvert;
      job::AudioConvert payload;
      payload.source = MediaSource{sourceUri, sourceName};
      payload.output = output;
      payload.audio = legacyAudioSettings(legacy);
      payload.adjustments.gainDb = legacy.gainDb;
      payload.adjustments.fadeInMs = legacy.fadeInMs;
      payload.adjustments.fadeOutMs = legacy.fadeOutMs;
      payload.adjustments.normalize = legacy.normalize;
      payload.adjustments.playbackSpeed = legacy.audioSpeed;
      payload.adjustments.removeSilence = legacy.removeSilence;
      payload.compression = compression;
      payload.metadata = metadata;
      request.payload = std::move(payload);
    } else {
      // Video disabled, audio-only container -> extract-audio.
      request.operation = Operation::ExtractAudio;
      job::ExtractAudio payload;
      payload.source = MediaSource{sourceUri, sourceName};
      payload.output = output;
      if (legacy.sourceTrackIndex >= 0) {
        payload.trackIndices.push_back(legacy.sourceTrackIndex);
      }
      payload.audio = legacyAudioSettings(legacy);
      payload.compression = compression;
      payload.metadata = metadata;
      request.payload = std::move(payload);
    }
  } else if (legacy.cropActive) {
    // Crop-in-video-request -> the Crop operation (the only equivalent).
    request.operation = Operation::VideoCrop;
    job::Crop payload;
    payload.source = MediaSource{sourceUri, sourceName};
    payload.output = output;
    payload.crop.x = legacy.cropX;
    payload.crop.y = legacy.cropY;
    payload.crop.width = legacy.cropW;
    payload.crop.height = legacy.cropH;
    request.payload = std::move(payload);
  } else {
    request.operation = Operation::VideoConvert;
    job::VideoConvert payload;
    payload.source = MediaSource{sourceUri, sourceName};
    payload.output = output;
    payload.video.codec = legacy.videoCodec;
    payload.video.resolutionHeight = legacy.resolutionHeight;
    if (legacy.frameRate.has_value()) {
      payload.video.frameRate = rationalFromDouble(*legacy.frameRate);
    }
    payload.audioTracks = legacyAudioTrackRules(legacy);
    payload.audioSettings = legacyAudioSettings(legacy);
    payload.subtitleTracks = legacySubtitleRules(legacy);
    payload.adjustments.rotationDegrees = legacy.rotation;
    payload.adjustments.flipHorizontal = legacy.flipH;
    payload.adjustments.flipVertical = legacy.flipV;
    payload.compression = compression;
    payload.metadata = metadata;
    request.payload = std::move(payload);
  }
  // (Retired advanced controls — profile/level/pixelFormat/gopSize/
  // bFrames, processingMode, the rateControl enum and subtitle timing
  // offsets — deliberately have no v4 equivalent.)
  return {operationName(request.operation), requestToJson(request)};
}

} // namespace

Error SqliteStore::migrate() {
  Error error;
  // Schema version tracking table.
  if (!exec("CREATE TABLE IF NOT EXISTS schema_version ("
            "  version INTEGER NOT NULL"
            ")",
            error)) {
    return error;
  }
  std::int64_t current = 0;
  if (!query("SELECT version FROM schema_version LIMIT 1",
             [&](sqlite3_stmt &stmt) {
               current = sqlite3_column_int64(&stmt, 0);
               return false; // first row only
             },
             error)) {
    return error;
  }

  if (current == 0) {
    // Fresh database: create v4 directly.
    if (!exec(kJobsV4Schema, error)) {
      return error;
    }
    // The CRUD statements address the table as `jobs` (the legacy path
    // renames jobs_v4 after migrating): finalize the fresh database the
    // same way, with the same submission-id index.
    if (!exec("ALTER TABLE jobs_v4 RENAME TO jobs", error)) {
      return error;
    }
    if (!exec("CREATE UNIQUE INDEX IF NOT EXISTS jobs_submission_id "
              "ON jobs (submission_id) WHERE submission_id IS NOT NULL "
              "AND submission_id != ''",
              error)) {
      return error;
    }
    if (!exec("INSERT INTO schema_version (version) VALUES (4)", error)) {
      return error;
    }
    current = 4;
  } else if (current < 4) {
    // Legacy (v1..v3) database: materialize the legacy table shape when a
    // pre-1 database carries an even older one, then transform every row
    // into v4 in ONE transaction (all rows preserved or none migrated).
    if (!exec(kLegacyJobsSchema, error)) {
      return error;
    }
    if (!exec("BEGIN IMMEDIATE", error)) {
      return error;
    }
    if (!exec(kJobsV4Schema, error)) {
      exec("ROLLBACK", error);
      return error;
    }
    // Legacy "checkpointing" -> "finalizing"; "running" stays (it maps to
    // Processing).
    if (!exec("INSERT INTO jobs_v4 (id, submission_id, state, operation, "
              "request_json, source_info_json, pipeline_summary, "
              "uses_hardware, output_path, outputs_json, progress, "
              "input_size, output_size, error, error_code, created_at, "
              "updated_at, started_at, finished_at, schema_version) "
              "SELECT id, NULL, "
              "CASE state WHEN 'checkpointing' THEN 'finalizing' "
              "           ELSE state END, "
              "'', '', source_info_json, '', 0, output_path, '[]', "
              "progress, input_size, output_size, error, NULL, "
              "created_at, updated_at, started_at, finished_at, 4 "
              "FROM jobs",
              error)) {
      exec("ROLLBACK", error);
      return error;
    }
    // Row-by-row typed-request migration (pure computation over the
    // SELECT above already copied the passthrough columns; the request
    // JSON and the operation are filled in here).
    if (!query("SELECT id, source_uri, source_name, output_name, "
               "output_container, goal_kind, goal_target_bytes, "
               "video_json, audio_json, subtitles_json, trim_start_us, "
               "trim_end_us, strip_metadata FROM jobs",
               [&](sqlite3_stmt &stmt) {
                 const std::string id = columnText(stmt, 0);
                 const auto [operation, requestJson] = migrateRow(
                     columnText(stmt, 1), columnText(stmt, 2),
                     columnText(stmt, 3).empty()
                         ? std::optional<std::string>()
                         : std::optional<std::string>(columnText(stmt, 3)),
                     columnText(stmt, 4), columnText(stmt, 5),
                     sqlite3_column_int64(&stmt, 6),
                     sqlite3_column_int64(&stmt, 10),
                     sqlite3_column_int64(&stmt, 11),
                     sqlite3_column_int64(&stmt, 12) != 0,
                     columnText(stmt, 7), columnText(stmt, 8),
                     columnText(stmt, 9));
                 Error updateError;
                 sqlite3_stmt *update = prepare(
                     "UPDATE jobs_v4 SET operation = ?, request_json = ? "
                     "WHERE id = ?",
                     updateError);
                 if (update == nullptr) {
                   return false; // surfaces as a query error below
                 }
                 sqlite3_bind_text(update, 1, operation.c_str(), -1,
                                   SQLITE_TRANSIENT);
                 sqlite3_bind_text(update, 2, requestJson.c_str(), -1,
                                   SQLITE_TRANSIENT);
                 sqlite3_bind_text(update, 3, id.c_str(), -1,
                                   SQLITE_TRANSIENT);
                 const int rc = sqlite3_step(update);
                 sqlite3_finalize(update);
                 return rc == SQLITE_DONE;
               },
               error)) {
      exec("ROLLBACK", error);
      return error;
    }
    if (!exec("DROP TABLE jobs", error)) {
      exec("ROLLBACK", error);
      return error;
    }
    if (!exec("ALTER TABLE jobs_v4 RENAME TO jobs", error)) {
      exec("ROLLBACK", error);
      return error;
    }
    if (!exec("CREATE UNIQUE INDEX IF NOT EXISTS jobs_submission_id "
              "ON jobs (submission_id) WHERE submission_id IS NOT NULL "
              "AND submission_id != ''",
              error)) {
      exec("ROLLBACK", error);
      return error;
    }
    if (!exec("UPDATE schema_version SET version = 4", error)) {
      exec("ROLLBACK", error);
      return error;
    }
    if (!exec("COMMIT", error)) {
      exec("ROLLBACK", error);
      return error;
    }
    current = 4;
  }
  if (!exec(kCheckpointsSchema, error) ||
      !exec(kTelemetryPerfSchema, error) ||
      !exec(kTelemetryPlanningSchema, error)) {
    return error;
  }
  return {};
}

// ---------------------------------------------------------------------------
// JobStore
// ---------------------------------------------------------------------------

namespace {

std::string outputsToJson(const std::vector<JobOutput> &outputs) {
  std::string out = "[";
  for (std::size_t i = 0; i < outputs.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    out += "{\"path\":\"" + json::escape(outputs[i].path) + "\"";
    out += ",\"sizeBytes\":" + std::to_string(outputs[i].sizeBytes);
    out += ",\"kind\":\"" + json::escape(outputs[i].kind) + "\"";
    out += "}";
  }
  out += "]";
  return out;
}

std::vector<JobOutput> outputsFromJson(const std::string &json) {
  std::vector<JobOutput> outputs;
  if (json.empty()) {
    return outputs;
  }
  json::JsonValue root;
  json::JsonReader reader(json);
  if (!reader.parse(root).empty() || root.kind != json::JsonValue::Kind::Array) {
    return outputs;
  }
  for (const auto &item : root.items) {
    if (item.kind != json::JsonValue::Kind::Object) {
      continue;
    }
    JobOutput output;
    if (const auto path = json::getOptString(item, "path")) {
      output.path = *path;
    }
    if (const auto size = json::getOptInt(item, "sizeBytes")) {
      output.sizeBytes = *size;
    }
    if (const auto kind = json::getOptString(item, "kind")) {
      output.kind = *kind;
    }
    outputs.push_back(std::move(output));
  }
  return outputs;
}

} // namespace

Error JobStore::insertJob(const JobRecord &record) {
  Error error;
  const std::string sql =
      "INSERT INTO jobs (id, submission_id, state, operation, request_json, "
      "source_info_json, pipeline_summary, uses_hardware, output_path, "
      "outputs_json, progress, input_size, output_size, error, error_code, "
      "created_at, updated_at, started_at, finished_at, schema_version) "
      "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
  sqlite3_stmt *stmt = db_.prepare(sql, error);
  if (stmt == nullptr) {
    return error;
  }
  sqlite3_bind_text(stmt, 1, record.id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, record.submissionId.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, jobStateName(record.state), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, operationName(record.request.operation), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(
      stmt, 5,
      requestToJson(record.request, record.submissionId).c_str(), -1,
      SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 6,
                    mediaInfoArrayToJson(record.sourceInfo).c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 7, record.pipeline.summary.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 8, record.pipeline.usesHardware ? 1 : 0);
  sqlite3_bind_text(stmt, 9, record.outputPath.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 10, outputsToJson(record.outputs).c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_double(stmt, 11, record.progress);
  sqlite3_bind_int64(stmt, 12, record.inputSizeBytes);
  sqlite3_bind_int64(stmt, 13, record.outputSizeBytes);
  sqlite3_bind_text(stmt, 14, record.error.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 15, record.errorCode.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 16, record.createdAtMs);
  sqlite3_bind_int64(stmt, 17, record.updatedAtMs);
  if (record.startedAtMs > 0) {
    sqlite3_bind_int64(stmt, 18, record.startedAtMs);
  } else {
    sqlite3_bind_null(stmt, 18);
  }
  if (record.finishedAtMs > 0) {
    sqlite3_bind_int64(stmt, 19, record.finishedAtMs);
  } else {
    sqlite3_bind_null(stmt, 19);
  }
  sqlite3_bind_int(stmt, 20, record.schemaVersion);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    return Error::make(ErrorCode::InternalError,
                       std::string("insertJob failed: ") +
                           sqlite3_errmsg(db_.handle()));
  }
  return {};
}

Error JobStore::updateJob(const JobRecord &record) {
  // Simplest correct approach: delete + insert in one transaction (the
  // record is small and updates are rare relative to reads). Only the
  // jobs row is replaced here — the checkpoint rows belong to the
  // recovery system and must survive every state/progress persist
  // (cancel/pause/retry rely on them; wiping them here silently
  // destroyed resumability).
  Error error;
  if (!db_.exec("BEGIN IMMEDIATE", error)) {
    return error;
  }
  if (!deleteJobRow(record.id).ok()) {
    db_.exec("ROLLBACK", error);
    return Error::make(ErrorCode::InternalError, "update transaction failed");
  }
  if (!insertJob(record).ok()) {
    db_.exec("ROLLBACK", error);
    return Error::make(ErrorCode::InternalError, "update transaction failed");
  }
  if (!db_.exec("COMMIT", error)) {
    db_.exec("ROLLBACK", error);
    return error;
  }
  return {};
}

std::optional<JobRecord> JobStore::loadJob(const std::string &id,
                                           Error &error) {
  std::optional<JobRecord> found;
  sqlite3_stmt *stmt =
      db_.prepare("SELECT * FROM jobs WHERE id = ?", error);
  if (stmt == nullptr) {
    return std::nullopt;
  }
  sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    found = rowToRecord(*stmt);
  } else if (rc != SQLITE_DONE) {
    error = Error::make(ErrorCode::InternalError,
                        std::string("loadJob failed: ") +
                            sqlite3_errmsg(db_.handle()));
  }
  sqlite3_finalize(stmt);
  return found;
}

std::vector<JobRecord> JobStore::loadAllJobs(Error &error) {
  std::vector<JobRecord> jobs;
  db_.query("SELECT * FROM jobs ORDER BY created_at DESC",
            [&](sqlite3_stmt &stmt) {
              jobs.push_back(rowToRecord(stmt));
              return true;
            },
            error);
  return jobs;
}

std::vector<JobRecord> JobStore::loadResumableJobs(Error &error) {
  std::vector<JobRecord> jobs;
  // Jobs that were mid-flight (not terminal, not user-held) when the process
  // disappeared, plus jobs already handed to recovery by a graceful shutdown.
  // User-held states (paused, interrupted) are deliberately excluded: their
  // committed checkpoints survive and the user resumes them explicitly.
  // ("running" is the persisted name of Processing; "finalizing" replaced
  // the legacy "checkpointing".)
  db_.query("SELECT * FROM jobs WHERE state IN "
            "('queued','preparing','running','finalizing','validating',"
            "'recovering') "
            "ORDER BY created_at ASC",
            [&](sqlite3_stmt &stmt) {
              jobs.push_back(rowToRecord(stmt));
              return true;
            },
            error);
  return jobs;
}

std::optional<JobRecord>
JobStore::findJobBySubmissionId(const std::string &submissionId,
                                Error &error) {
  if (submissionId.empty()) {
    return std::nullopt;
  }
  std::optional<JobRecord> found;
  sqlite3_stmt *stmt =
      db_.prepare("SELECT * FROM jobs WHERE submission_id = ? "
                  "ORDER BY created_at ASC LIMIT 1",
                  error);
  if (stmt == nullptr) {
    return std::nullopt;
  }
  sqlite3_bind_text(stmt, 1, submissionId.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    found = rowToRecord(*stmt);
  } else if (rc != SQLITE_DONE) {
    error = Error::make(ErrorCode::InternalError,
                        std::string("findJobBySubmissionId failed: ") +
                            sqlite3_errmsg(db_.handle()));
  }
  sqlite3_finalize(stmt);
  return found;
}

Error JobStore::deleteJob(const std::string &id) {
  Error error;
  // Checkpoint rows are meaningless without the job: cascade.
  sqlite3_stmt *stmt =
      db_.prepare("DELETE FROM checkpoints WHERE job_id = ?", error);
  if (stmt == nullptr) {
    return error;
  }
  sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    return Error::make(ErrorCode::InternalError,
                       std::string("checkpoint cascade failed: ") +
                           sqlite3_errmsg(db_.handle()));
  }
  return deleteJobRow(id);
}

Error JobStore::deleteJobRow(const std::string &id) {
  Error error;
  sqlite3_stmt *stmt = db_.prepare("DELETE FROM jobs WHERE id = ?", error);
  if (stmt == nullptr) {
    return error;
  }
  sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    return Error::make(ErrorCode::InternalError,
                       std::string("deleteJobRow failed: ") +
                           sqlite3_errmsg(db_.handle()));
  }
  return {};
}

JobRecord JobStore::rowToRecord(sqlite3_stmt &stmt) {
  // v4 column order (0-based):
  //  0 id, 1 submission_id, 2 state, 3 operation, 4 request_json,
  //  5 source_info_json, 6 pipeline_summary, 7 uses_hardware,
  //  8 output_path, 9 outputs_json, 10 progress, 11 input_size,
  //  12 output_size, 13 error, 14 error_code, 15 created_at,
  //  16 updated_at, 17 started_at, 18 finished_at, 19 schema_version.
  JobRecord record;
  record.id = columnText(stmt, 0);
  record.submissionId = columnText(stmt, 1);
  record.state = jobStateFromName(columnText(stmt, 2)).value_or(JobState::Failed);
  record.outputPath = columnText(stmt, 8);
  record.pipeline.summary = columnText(stmt, 6);
  record.pipeline.usesHardware =
      sqlite3_column_type(&stmt, 7) == SQLITE_INTEGER
          ? sqlite3_column_int(&stmt, 7) != 0
          : false;
  // A pipeline summary is only persisted after planning succeeded; the
  // planned flag and the summary are always written together.
  record.pipeline.planned = !record.pipeline.summary.empty();
  record.outputs = outputsFromJson(columnText(stmt, 9));
  record.progress = sqlite3_column_double(&stmt, 10);
  record.inputSizeBytes = sqlite3_column_int64(&stmt, 11);
  record.outputSizeBytes = sqlite3_column_int64(&stmt, 12);
  record.error = columnText(stmt, 13);
  record.errorCode = columnText(stmt, 14);
  record.createdAtMs = sqlite3_column_int64(&stmt, 15);
  record.updatedAtMs = sqlite3_column_int64(&stmt, 16);
  record.startedAtMs = sqlite3_column_type(&stmt, 17) == SQLITE_NULL
                           ? 0
                           : sqlite3_column_int64(&stmt, 17);
  record.finishedAtMs = sqlite3_column_type(&stmt, 18) == SQLITE_NULL
                            ? 0
                            : sqlite3_column_int64(&stmt, 18);
  record.schemaVersion = sqlite3_column_int(&stmt, 19);

  // Full-fidelity typed request from the stored JSON. A malformed row must
  // never break job loading: the record stays loadable with a neutral
  // request and a visible parse error (the state itself stays untouched).
  const std::string requestJson = columnText(stmt, 4);
  if (!requestJson.empty()) {
    std::string parseError;
    if (auto decoded = requestFromJson(requestJson, parseError)) {
      record.request = decoded->request;
      if (record.submissionId.empty()) {
        record.submissionId = decoded->submissionId;
      }
    } else {
      record.request.operation = Operation::VideoConvert;
      if (record.error.empty()) {
        record.error = "stored request unreadable: " + parseError;
      }
    }
  }
  const std::string sourceInfoJson = columnText(stmt, 5);
  if (!sourceInfoJson.empty()) {
    std::string parseError;
    if (auto infos = mediaInfoArrayFromJson(sourceInfoJson, parseError)) {
      record.sourceInfo = std::move(*infos);
    }
  }
  return record;
}

} // namespace velmux::core
