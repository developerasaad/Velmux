#include "persistence/job_store.h"

#include <sqlite3.h>

#include <cstring>
#include <ctime>
#include <functional>
#include <mutex>

namespace velmux::core {

namespace {

std::int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string columnText(sqlite3_stmt &stmt, int col) {
  const unsigned char *text = sqlite3_column_text(&stmt, col);
  if (text == nullptr) {
    return {};
  }
  return std::string(reinterpret_cast<const char *>(text));
}

std::string optText(const std::optional<std::string> &value) {
  return value.value_or("");
}

std::optional<std::string> textToOpt(const std::string &value) {
  if (value.empty()) {
    return std::nullopt;
  }
  return value;
}

std::int64_t optInt(const std::optional<std::int64_t> &value) {
  return value.value_or(-1);
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

Error SqliteStore::migrate() {
  Error error;
  // Schema version tracking table.
  if (!exec("CREATE TABLE IF NOT EXISTS schema_version ("
            "  version INTEGER NOT NULL"
            ")", error)) {
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
    if (!exec("INSERT INTO schema_version (version) VALUES (" +
                  std::to_string(JobStore::kSchemaVersion) + ")",
              error)) {
      return error;
    }
    current = JobStore::kSchemaVersion;
  }
  // Forward migrations (v1 is the initial schema).
  if (current >= 1) {
    // v1 tables.
    if (!exec("CREATE TABLE IF NOT EXISTS jobs ("
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
              ")", error)) {
      return error;
    }
    if (current < 2) {
      // v2: trim window on the request (spec §15/§16/§20). ALTER ADD
      // COLUMN appends, so every existing positional read stays valid.
      if (!exec("ALTER TABLE jobs ADD COLUMN trim_start_us INTEGER NOT NULL "
                "DEFAULT 0", error) ||
          !exec("ALTER TABLE jobs ADD COLUMN trim_end_us INTEGER NOT NULL "
                "DEFAULT -1", error) ||
          !exec("UPDATE schema_version SET version = 2", error)) {
        return error;
      }
    }
    if (current < 3) {
      // v3: stripMetadata on the request (spec advanced controls). Same
      // append-only discipline: column 26, existing positional reads stay
      // valid.
      if (!exec("ALTER TABLE jobs ADD COLUMN strip_metadata INTEGER NOT NULL "
                "DEFAULT 0", error) ||
          !exec("UPDATE schema_version SET version = 3", error)) {
        return error;
      }
    }
    if (!exec("CREATE TABLE IF NOT EXISTS checkpoints ("
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
              ")", error)) {
      return error;
    }
    if (!exec("CREATE TABLE IF NOT EXISTS telemetry_perf ("
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
              ")", error)) {
      return error;
    }
    if (!exec("CREATE TABLE IF NOT EXISTS telemetry_planning ("
              "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
              "  planning_ms REAL NOT NULL,"
              "  recorded_at INTEGER NOT NULL"
              ")", error)) {
      return error;
    }
  }
  // Future migrations: if (current < 2) { ...; UPDATE schema_version ... }
  return {};
}

// ---------------------------------------------------------------------------
// JobStore
// ---------------------------------------------------------------------------

namespace {

// --- JSON-ish serialization for the nested request sections. Kept as
// simple "k=v;k=v" pairs rather than a JSON dependency: values never
// contain ';' or '=' because they are enums/numbers/URIs we control.
// Container paths never contain ';' by validation in the bridge.

std::string serializeVideoRequest(const VideoRequest &video) {
  std::string out;
  out += "enabled=" + std::string(video.enabled ? "1" : "0") + ";";
  if (video.codec.has_value()) {
    out += std::string("codec=") + codecName(*video.codec) + ";";
  }
  if (video.resolutionHeight.has_value()) {
    out += "height=" + std::to_string(*video.resolutionHeight) + ";";
  }
  if (video.frameRate.has_value()) {
    out += "fps=" + std::to_string(*video.frameRate) + ";";
  }
  out += "rate=" + std::to_string(int(video.rateControl)) + ";";
  if (video.bitRate.has_value()) {
    out += "bitrate=" + std::to_string(*video.bitRate) + ";";
  }
  if (video.quality.has_value()) {
    out += "quality=" + std::to_string(*video.quality) + ";";
  }
  if (video.crop.has_value() && video.crop->active()) {
    out += "crop=" + std::to_string(video.crop->x) + "," +
           std::to_string(video.crop->y) + "," +
           std::to_string(video.crop->width) + "," +
           std::to_string(video.crop->height) + ";";
  }
  if (video.rotationDegrees != 0) {
    out += "rotate=" + std::to_string(video.rotationDegrees) + ";";
  }
  if (video.flipHorizontal) {
    out += "fliph=1;";
  }
  if (video.flipVertical) {
    out += "flipv=1;";
  }
  return out;
}

std::string serializeAudioRequest(const AudioRequest &audio) {
  std::string out;
  out += "enabled=" + std::string(audio.enabled ? "1" : "0") + ";";
  if (audio.codec.has_value()) {
    out += std::string("codec=") + codecName(*audio.codec) + ";";
  }
  if (audio.bitRate.has_value()) {
    out += "bitrate=" + std::to_string(*audio.bitRate) + ";";
  }
  if (audio.sampleRate.has_value()) {
    out += "rate=" + std::to_string(*audio.sampleRate) + ";";
  }
  if (audio.channels.has_value()) {
    out += "channels=" + std::to_string(*audio.channels) + ";";
  }
  if (audio.gainDb.has_value()) {
    out += "gain=" + std::to_string(*audio.gainDb) + ";";
  }
  if (audio.fadeInMs.has_value()) {
    out += "fadein=" + std::to_string(*audio.fadeInMs) + ";";
  }
  if (audio.fadeOutMs.has_value()) {
    out += "fadeout=" + std::to_string(*audio.fadeOutMs) + ";";
  }
  if (audio.normalizeLoudness) {
    out += "norm=1;";
  }
  if (audio.playbackSpeed.has_value()) {
    out += "speed=" + std::to_string(*audio.playbackSpeed) + ";";
  }
  if (audio.removeSilence) {
    out += "desil=1;";
  }
  if (audio.pcmBitDepth.has_value()) {
    out += "depth=" + std::to_string(*audio.pcmBitDepth) + ";";
  }
  // Multi-track / replace / cover-art controls (spec §9/§10). These MUST
  // survive the job-store round trip: executeJob reloads the record from
  // the store, so a field missing here silently reverts to its default
  // mid-flight (the D-040 dead-validation bug class — observed as strip
  // requests preserving metadata and cover-art/replace requests executing
  // as plain transcodes once the tests that cover them actually ran).
  if (audio.sourceTrackIndex >= 0) {
    out += "srctrack=" + std::to_string(audio.sourceTrackIndex) + ";";
  }
  if (audio.allTracks) {
    out += "alltracks=1;";
  }
  if (audio.replaceAudioUri.has_value()) {
    // '=' and ';' cannot appear in practice (file paths); '=' would break
    // key=value parsing and ';' would split the pair.
    out += "replace=" + *audio.replaceAudioUri + ";";
  }
  if (audio.coverArtUri.has_value()) {
    out += "cover=" + *audio.coverArtUri + ";";
  }
  if (!audio.preserveCoverArt) {
    out += "keepcover=0;";
  }
  return out;
}

std::string serializeSubtitleRequest(const SubtitleRequest &sub) {
  std::string out;
  out += "policy=" + std::to_string(int(sub.policy)) + ";";
  if (sub.externalUri.has_value()) {
    out += "external=" + *sub.externalUri + ";";
  }
  out += "offset=" + std::to_string(sub.timingOffsetMs) + ";";
  return out;
}

void parsePairs(const std::string &data,
                const std::function<void(const std::string &, const std::string &)>
                    &visit) {
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

// Source inspection summary persisted in source_info_json: the fields
// output validation needs after a store round-trip (duration, geometry,
// sample rate, codecs of the first streams). Everything is enum/numeric
// pairs, matching the request serialization style.
std::string serializeSourceInfo(const MediaInfo &info) {
  std::string out;
  out += "duration=" + std::to_string(info.durationSec) + ";";
  out += "size=" + std::to_string(info.sizeBytes) + ";";
  if (info.hasVideo()) {
    const VideoStream &v = info.video.front();
    out += "vw=" + std::to_string(v.width) + ";";
    out += "vh=" + std::to_string(v.height) + ";";
    out += std::string("vcodec=") + codecName(v.codec) + ";";
    out += "vfps=" + std::to_string(v.frameRate) + ";";
  }
  if (info.hasAudio()) {
    const AudioStream &a = info.audio.front();
    out += "ach=" + std::to_string(a.channels) + ";";
    out += "asr=" + std::to_string(a.sampleRate) + ";";
    out += std::string("acodec=") + codecName(a.codec) + ";";
  }
  if (info.attachedPictures > 0) {
    // Cover art count: attached pictures are metadata, not a video lane;
    // the count keeps the source summary faithful for resumed jobs.
    out += "covers=" + std::to_string(info.attachedPictures) + ";";
  }
  if (!info.subtitles.empty()) {
    // The planner's subtitle policies (burn-in/preserve/extract routing
    // and container compatibility) key off the track list; formats and
    // languages per track, pipe-separated.
    out += "subs=";
    for (std::size_t i = 0; i < info.subtitles.size(); ++i) {
      if (i > 0) {
        out += "|";
      }
      out += subtitleFormatName(info.subtitles[i].format);
      if (!info.subtitles[i].language.empty()) {
        out += "," + info.subtitles[i].language;
      }
    }
    out += ";";
  }
  return out;
}

MediaInfo parseSourceInfo(const std::string &data) {
  MediaInfo info;
  parsePairs(data, [&](const std::string &key, const std::string &value) {
    try {
      if (key == "duration") {
        info.durationSec = std::stod(value);
      } else if (key == "size") {
        info.sizeBytes = std::stoll(value);
      } else if (key == "covers") {
        info.attachedPictures = std::stoi(value);
      } else if (key == "vw") {
        if (info.video.empty()) {
          info.video.push_back(VideoStream{});
        }
        info.video.front().width = std::stoi(value);
      } else if (key == "vh") {
        if (info.video.empty()) {
          info.video.push_back(VideoStream{});
        }
        info.video.front().height = std::stoi(value);
      } else if (key == "vcodec") {
        if (info.video.empty()) {
          info.video.push_back(VideoStream{});
        }
        info.video.front().codec = videoCodecFromName(value).value_or(
            VideoCodec::H264);
      } else if (key == "vfps") {
        if (info.video.empty()) {
          info.video.push_back(VideoStream{});
        }
        info.video.front().frameRate = std::stod(value);
      } else if (key == "ach") {
        if (info.audio.empty()) {
          info.audio.push_back(AudioStream{});
        }
        info.audio.front().channels = std::stoi(value);
      } else if (key == "asr") {
        if (info.audio.empty()) {
          info.audio.push_back(AudioStream{});
        }
        info.audio.front().sampleRate = std::stoi(value);
      } else if (key == "acodec") {
        if (info.audio.empty()) {
          info.audio.push_back(AudioStream{});
        }
        info.audio.front().codec =
            audioCodecFromName(value).value_or(AudioCodec::Aac);
      } else if (key == "subs") {
        std::size_t pos = 0;
        while (pos <= value.size()) {
          const std::size_t pipe = value.find('|', pos);
          const std::string track =
              value.substr(pos, pipe == std::string::npos
                                    ? std::string::npos
                                    : pipe - pos);
          const std::size_t comma = track.find(',');
          const std::string fmt = comma == std::string::npos
                                      ? track
                                      : track.substr(0, comma);
          const std::string lang = comma == std::string::npos
                                       ? ""
                                       : track.substr(comma + 1);
          const auto format = subtitleFormatFromName(fmt);
          if (format.has_value()) {
            SubtitleStream sub;
            sub.format = *format;
            sub.language = lang;
            info.subtitles.push_back(sub);
          }
          if (pipe == std::string::npos) {
            break;
          }
          pos = pipe + 1;
        }
      }
    } catch (const std::exception &) {
      // A malformed pair must never break job loading.
    }
  });
  return info;
}

VideoRequest parseVideoRequest(const std::string &data) {
  VideoRequest video;
  parsePairs(data, [&](const std::string &key, const std::string &value) {
    if (key == "enabled") {
      video.enabled = value == "1";
    } else if (key == "codec") {
      video.codec = videoCodecFromName(value);
    } else if (key == "height") {
      video.resolutionHeight = std::stoi(value);
    } else if (key == "fps") {
      video.frameRate = std::stod(value);
    } else if (key == "rate") {
      video.rateControl = RateControl(std::stoi(value));
    } else if (key == "bitrate") {
      video.bitRate = std::stoll(value);
    } else if (key == "quality") {
      video.quality = std::stoi(value);
    } else if (key == "crop") {
      // x,y,w,h
      CropRect crop;
      const auto comma1 = value.find(',');
      const auto comma2 = value.find(',', comma1 + 1);
      const auto comma3 = value.find(',', comma2 + 1);
      if (comma1 != std::string::npos && comma2 != std::string::npos &&
          comma3 != std::string::npos) {
        crop.x = std::stoi(value.substr(0, comma1));
        crop.y = std::stoi(value.substr(comma1 + 1, comma2 - comma1 - 1));
        crop.width = std::stoi(value.substr(comma2 + 1, comma3 - comma2 - 1));
        crop.height = std::stoi(value.substr(comma3 + 1));
        if (crop.active()) {
          video.crop = crop;
        }
      }
    } else if (key == "rotate") {
      video.rotationDegrees = std::stoi(value);
    } else if (key == "fliph") {
      video.flipHorizontal = value == "1";
    } else if (key == "flipv") {
      video.flipVertical = value == "1";
    }
  });
  return video;
}

AudioRequest parseAudioRequest(const std::string &data) {
  AudioRequest audio;
  parsePairs(data, [&](const std::string &key, const std::string &value) {
    if (key == "enabled") {
      audio.enabled = value == "1";
    } else if (key == "codec") {
      audio.codec = audioCodecFromName(value);
    } else if (key == "bitrate") {
      audio.bitRate = std::stoll(value);
    } else if (key == "rate") {
      audio.sampleRate = std::stoi(value);
    } else if (key == "channels") {
      audio.channels = std::stoi(value);
    } else if (key == "gain") {
      audio.gainDb = std::stod(value);
    } else if (key == "fadein") {
      audio.fadeInMs = std::stoi(value);
    } else if (key == "fadeout") {
      audio.fadeOutMs = std::stoi(value);
    } else if (key == "norm") {
      audio.normalizeLoudness = value == "1";
    } else if (key == "speed") {
      audio.playbackSpeed = std::stod(value);
    } else if (key == "desil") {
      audio.removeSilence = value == "1";
    } else if (key == "depth") {
      audio.pcmBitDepth = std::stoi(value);
    } else if (key == "srctrack") {
      audio.sourceTrackIndex = std::stoi(value);
    } else if (key == "alltracks") {
      audio.allTracks = value == "1";
    } else if (key == "replace") {
      audio.replaceAudioUri = value;
    } else if (key == "cover") {
      audio.coverArtUri = value;
    } else if (key == "keepcover") {
      audio.preserveCoverArt = value == "1";
    }
  });
  return audio;
}

SubtitleRequest parseSubtitleRequest(const std::string &data) {
  SubtitleRequest sub;
  parsePairs(data, [&](const std::string &key, const std::string &value) {
    if (key == "policy") {
      sub.policy = SubtitlePolicy(std::stoi(value));
    } else if (key == "external") {
      sub.externalUri = value;
    } else if (key == "offset") {
      sub.timingOffsetMs = std::stoi(value);
    }
  });
  return sub;
}

} // namespace

Error JobStore::insertJob(const JobRecord &record) {
  Error error;
  const std::string sql =
      "INSERT INTO jobs (id, state, source_uri, source_name, output_name, "
      "output_path, output_container, goal_kind, goal_target_bytes, "
      "video_json, audio_json, subtitles_json, processing_mode, "
      "pipeline_json, source_info_json, progress, input_size, output_size, "
      "error, created_at, updated_at, started_at, finished_at, schema_version, "
      "trim_start_us, trim_end_us, strip_metadata) "
      "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
  sqlite3_stmt *stmt = db_.prepare(sql, error);
  if (stmt == nullptr) {
    return error;
  }
  sqlite3_bind_text(stmt, 1, record.id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, jobStateName(record.state), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, record.request.source.uri.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, record.request.source.displayName.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 5, optText(record.request.outputName).c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 6, record.outputPath.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(
      stmt, 7,
      record.request.outputContainer.has_value()
          ? containerName(*record.request.outputContainer)
          : "",
      -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 8, std::to_string(int(record.request.goal.kind)).c_str(),
                    -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 9, record.request.goal.targetSizeBytes);
  sqlite3_bind_text(stmt, 10,
                    serializeVideoRequest(record.request.video).c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 11,
                    serializeAudioRequest(record.request.audio).c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 12,
                    serializeSubtitleRequest(record.request.subtitles).c_str(),
                    -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 13,
                    std::to_string(int(record.request.processingMode)).c_str(),
                    -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 25, record.request.trim.startUs);
  sqlite3_bind_int64(stmt, 26, record.request.trim.endUs);
  sqlite3_bind_int(stmt, 27, record.request.stripMetadata ? 1 : 0);
  sqlite3_bind_text(stmt, 14, record.pipeline.summary.c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 15,
                    serializeSourceInfo(record.sourceInfo).c_str(), -1,
                    SQLITE_TRANSIENT);
  sqlite3_bind_double(stmt, 16, record.progress);
  sqlite3_bind_int64(stmt, 17, record.inputSizeBytes);
  sqlite3_bind_int64(stmt, 18, record.outputSizeBytes);
  sqlite3_bind_text(stmt, 19, record.error.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 20, record.createdAtMs);
  sqlite3_bind_int64(stmt, 21, record.updatedAtMs);
  if (record.startedAtMs > 0) {
    sqlite3_bind_int64(stmt, 22, record.startedAtMs);
  } else {
    sqlite3_bind_null(stmt, 22);
  }
  if (record.finishedAtMs > 0) {
    sqlite3_bind_int64(stmt, 23, record.finishedAtMs);
  } else {
    sqlite3_bind_null(stmt, 23);
  }
  sqlite3_bind_int(stmt, 24, record.schemaVersion);

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
  const std::string sql = "SELECT * FROM jobs WHERE id = '" + id + "'";
  db_.query(sql,
            [&](sqlite3_stmt &stmt) {
              found = rowToRecord(stmt);
              return false;
            },
            error);
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
  db_.query("SELECT * FROM jobs WHERE state IN "
            "('queued','preparing','running','checkpointing','recovering') "
            "ORDER BY created_at ASC",
            [&](sqlite3_stmt &stmt) {
              jobs.push_back(rowToRecord(stmt));
              return true;
            },
            error);
  return jobs;
}

Error JobStore::deleteJob(const std::string &id) {
  Error error;
  // Checkpoint rows are meaningless without the job: cascade.
  if (!db_.exec("DELETE FROM checkpoints WHERE job_id = '" + id + "'", error)) {
    return error;
  }
  return deleteJobRow(id);
}

Error JobStore::deleteJobRow(const std::string &id) {
  Error error;
  if (!db_.exec("DELETE FROM jobs WHERE id = '" + id + "'", error)) {
    return error;
  }
  return {};
}

JobRecord JobStore::rowToRecord(sqlite3_stmt &stmt) {
  JobRecord record;
  record.id = columnText(stmt, 0);
  record.state = jobStateFromName(columnText(stmt, 1)).value_or(JobState::Failed);
  record.request.source.uri = columnText(stmt, 2);
  record.request.source.displayName = columnText(stmt, 3);
  record.request.outputName = textToOpt(columnText(stmt, 4));
  record.outputPath = columnText(stmt, 5);
  record.request.outputContainer =
      containerFromName(columnText(stmt, 6));
  record.request.goal.kind =
      GoalKind(std::stoi(columnText(stmt, 7).empty()
                             ? "0"
                             : columnText(stmt, 7)));
  record.request.goal.targetSizeBytes = sqlite3_column_int64(&stmt, 8);
  record.request.video = parseVideoRequest(columnText(stmt, 9));
  record.request.audio = parseAudioRequest(columnText(stmt, 10));
  record.request.subtitles = parseSubtitleRequest(columnText(stmt, 11));
  record.request.processingMode =
      ProcessingMode(std::stoi(columnText(stmt, 12).empty()
                                   ? "0"
                                   : columnText(stmt, 12)));
  record.pipeline.summary = columnText(stmt, 13);
  record.sourceInfo = parseSourceInfo(columnText(stmt, 14));
  record.progress = sqlite3_column_double(&stmt, 15);
  record.inputSizeBytes = sqlite3_column_int64(&stmt, 16);
  record.outputSizeBytes = sqlite3_column_int64(&stmt, 17);
  // 0-based column order: ... finished_at=22, schema_version=23,
  // trim_start_us=24, trim_end_us=25.
  record.request.trim.startUs =
      sqlite3_column_type(&stmt, 24) == SQLITE_INTEGER
          ? sqlite3_column_int64(&stmt, 24)
          : 0;
  record.request.trim.endUs =
      sqlite3_column_type(&stmt, 25) == SQLITE_INTEGER
          ? sqlite3_column_int64(&stmt, 25)
          : -1;
  // 0-based column order: ... finished_at=22, schema_version=23,
  // trim_start_us=24, trim_end_us=25, strip_metadata=26.
  record.request.stripMetadata =
      sqlite3_column_type(&stmt, 26) == SQLITE_INTEGER
          ? sqlite3_column_int(&stmt, 26) != 0
          : false;
  record.error = columnText(stmt, 18);
  record.createdAtMs = sqlite3_column_int64(&stmt, 19);
  record.updatedAtMs = sqlite3_column_int64(&stmt, 20);
  record.startedAtMs =
      sqlite3_column_type(&stmt, 21) == SQLITE_NULL
          ? 0
          : sqlite3_column_int64(&stmt, 21);
  record.finishedAtMs =
      sqlite3_column_type(&stmt, 22) == SQLITE_NULL
          ? 0
          : sqlite3_column_int64(&stmt, 22);
  record.schemaVersion = sqlite3_column_int(&stmt, 23);
  return record;
}

} // namespace velmux::core
