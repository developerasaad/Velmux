// Job store unit tests: durable persistence round-trips for every typed
// payload field (schema v4), resumable queries, submission idempotency and
// the v3 -> v4 migration of hand-written legacy rows. Uses a temp directory
// database.
#include "persistence/job_store.h"
#include "api/serialization.h"

#include <doctest/doctest.h>
#include <sqlite3.h>

#include <cstdio>
#include <optional>
#include <filesystem>

using namespace velmux::core;

namespace {

class TempDb {
public:
  TempDb() {
    dir_ = std::filesystem::temp_directory_path() /
           ("velmux-test-" + std::to_string(::getpid()) + "-" +
            std::to_string(counter_++));
    std::filesystem::create_directories(dir_);
    REQUIRE(store_.open((dir_ / "jobs.sqlite").string()).ok());
    jobs_ = std::make_unique<JobStore>(store_);
  }
  ~TempDb() {
    jobs_.reset();
    store_.close();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }
  JobStore &jobs() { return *jobs_; }
  SqliteStore &store() { return store_; }
  const std::filesystem::path &dir() const { return dir_; }

private:
  static inline int counter_ = 0;
  std::filesystem::path dir_;
  SqliteStore store_;
  std::unique_ptr<JobStore> jobs_;
};

// The full VideoConvert sample: every typed payload field populated.
JobRecord sampleJob(const std::string &id, JobState state) {
  JobRecord record;
  record.id = id;
  record.submissionId = "u-sample-1";
  record.state = state;
  record.request.operation = Operation::VideoConvert;
  job::VideoConvert payload;
  payload.source.uri = "file:///tmp/sample.mp4";
  payload.source.displayName = "sample.mp4";
  payload.output.name = "renamed";
  payload.output.container = Container::Mkv;
  payload.video.codec = VideoCodec::H264;
  payload.video.resolutionHeight = 1080;
  payload.video.customWidth = 640;
  payload.video.customHeight = 360;
  payload.video.frameRate = Rational{30000, 1001};
  {
    AudioTrackRule keepFirst;
    keepFirst.sourceTrackIndex = 0;
    keepFirst.include = true;
    payload.audioTracks.push_back(keepFirst);
    AudioTrackRule replaceSecond;
    replaceSecond.sourceTrackIndex = 1;
    replaceSecond.include = false;
    replaceSecond.externalUri = "file:///tmp/replacement.m4a";
    replaceSecond.replace = true;
    payload.audioTracks.push_back(replaceSecond);
  }
  payload.audioSettings.codec = AudioCodec::Aac;
  payload.audioSettings.bitRate = 128'000;
  payload.audioSettings.sampleRate = 48'000;
  payload.audioSettings.channels = 2;
  payload.audioSettings.pcmBitDepth = 24;
  {
    SubtitleTrackRule burn;
    burn.sourceTrackIndex = 0;
    burn.policy = SubtitleTrackPolicy::BurnIn;
    burn.externalUri = "file:///tmp/subs.srt";
    burn.fontSize = 28;
    burn.color = "#FFCC00";
    burn.position = 80;
    payload.subtitleTracks.push_back(burn);
  }
  payload.adjustments.rotationDegrees = 90;
  payload.adjustments.flipHorizontal = true;
  payload.adjustments.flipVertical = true;
  payload.adjustments.playbackSpeed = 1.5;
  payload.adjustments.reverse = true;
  payload.adjustments.hdrToSdr = true;
  payload.compression.enabled = true;
  payload.compression.preset = CompressionPreset::TargetSize;
  payload.compression.targetSizeBytes = 500'000'000;
  payload.compression.quality = 42;
  payload.compression.videoBitrate = 4'000'000;
  payload.compression.audioBitrate = 96'000;
  payload.metadata.titleAction = MetadataFieldAction::Set;
  payload.metadata.title = "Holiday";
  payload.metadata.artistAction = MetadataFieldAction::Set;
  payload.metadata.artist = "Velmux";
  payload.metadata.albumAction = MetadataFieldAction::Clear;
  payload.metadata.genreAction = MetadataFieldAction::Set;
  payload.metadata.genre = "Travel";
  payload.metadata.dateAction = MetadataFieldAction::Set;
  payload.metadata.date = "2025-06-01T10:00:00Z";
  payload.metadata.descriptionAction = MetadataFieldAction::Set;
  payload.metadata.description = "A trip";
  payload.metadata.locationAction = MetadataFieldAction::Set;
  payload.metadata.location = "Kyoto";
  payload.metadata.artworkAction = MetadataFieldAction::Set;
  payload.metadata.artworkUri = "file:///tmp/cover.png";
  record.request.payload = std::move(payload);
  record.pipeline.planned = true;
  record.pipeline.summary = "Transcode h264 via ffmpeg -> matroska";
  record.pipeline.usesHardware = false;
  record.outputPath = "/tmp/out.mkv";
  {
    JobOutput primary;
    primary.path = "/tmp/out.mkv";
    primary.sizeBytes = 12345;
    primary.kind = "media";
    record.outputs.push_back(primary);
  }
  record.inputSizeBytes = 2'000'000'000;
  record.outputSizeBytes = 480'000'000;
  record.progress = 0.42;
  record.error = "none";
  record.errorCode = "none";
  record.createdAtMs = 1000;
  record.updatedAtMs = 2000;
  record.startedAtMs = 1500;
  record.finishedAtMs = 2500;
  return record;
}

const job::VideoConvert &videoPayload(const JobRequest &request) {
  return std::get<job::VideoConvert>(request.payload);
}

} // namespace

TEST_CASE("insert and load round-trips EVERY typed payload field") {
  TempDb db;
  const JobRecord original = sampleJob("job-1", JobState::Processing);
  REQUIRE(db.jobs().insertJob(original).ok());

  Error error;
  const auto loaded = db.jobs().loadJob("job-1", error);
  REQUIRE(loaded.has_value());
  CHECK(loaded->id == "job-1");
  CHECK(loaded->submissionId == "u-sample-1");
  CHECK(loaded->state == JobState::Processing);
  CHECK(loaded->request.operation == Operation::VideoConvert);
  const job::VideoConvert &payload = videoPayload(loaded->request);
  // Source + output.
  CHECK(payload.source.uri == "file:///tmp/sample.mp4");
  CHECK(payload.source.displayName == "sample.mp4");
  REQUIRE(payload.output.name.has_value());
  CHECK(*payload.output.name == "renamed");
  REQUIRE(payload.output.container.has_value());
  CHECK(*payload.output.container == Container::Mkv);
  // Video stream intent.
  REQUIRE(payload.video.codec.has_value());
  CHECK(*payload.video.codec == VideoCodec::H264);
  REQUIRE(payload.video.resolutionHeight.has_value());
  CHECK(*payload.video.resolutionHeight == 1080);
  REQUIRE(payload.video.customWidth.has_value());
  CHECK(*payload.video.customWidth == 640);
  REQUIRE(payload.video.customHeight.has_value());
  CHECK(*payload.video.customHeight == 360);
  REQUIRE(payload.video.frameRate.has_value());
  CHECK(payload.video.frameRate->num == 30000);
  CHECK(payload.video.frameRate->den == 1001);
  // Audio track rules (mandate §23).
  REQUIRE(payload.audioTracks.size() == 2);
  CHECK(payload.audioTracks[0].sourceTrackIndex == 0);
  CHECK(payload.audioTracks[0].include);
  CHECK_FALSE(payload.audioTracks[0].replace);
  CHECK(payload.audioTracks[1].sourceTrackIndex == 1);
  CHECK_FALSE(payload.audioTracks[1].include);
  REQUIRE(payload.audioTracks[1].externalUri.has_value());
  CHECK(*payload.audioTracks[1].externalUri == "file:///tmp/replacement.m4a");
  CHECK(payload.audioTracks[1].replace);
  // Audio settings.
  REQUIRE(payload.audioSettings.codec.has_value());
  CHECK(*payload.audioSettings.codec == AudioCodec::Aac);
  REQUIRE(payload.audioSettings.bitRate.has_value());
  CHECK(*payload.audioSettings.bitRate == 128'000);
  REQUIRE(payload.audioSettings.sampleRate.has_value());
  CHECK(*payload.audioSettings.sampleRate == 48'000);
  REQUIRE(payload.audioSettings.channels.has_value());
  CHECK(*payload.audioSettings.channels == 2);
  REQUIRE(payload.audioSettings.pcmBitDepth.has_value());
  CHECK(*payload.audioSettings.pcmBitDepth == 24);
  // Subtitle rules (mandate §24).
  REQUIRE(payload.subtitleTracks.size() == 1);
  CHECK(payload.subtitleTracks[0].sourceTrackIndex == 0);
  CHECK(payload.subtitleTracks[0].policy == SubtitleTrackPolicy::BurnIn);
  REQUIRE(payload.subtitleTracks[0].externalUri.has_value());
  CHECK(*payload.subtitleTracks[0].externalUri == "file:///tmp/subs.srt");
  REQUIRE(payload.subtitleTracks[0].fontSize.has_value());
  CHECK(*payload.subtitleTracks[0].fontSize == 28);
  REQUIRE(payload.subtitleTracks[0].color.has_value());
  CHECK(*payload.subtitleTracks[0].color == "#FFCC00");
  REQUIRE(payload.subtitleTracks[0].position.has_value());
  CHECK(*payload.subtitleTracks[0].position == 80);
  // Adjustments (mandate §25).
  CHECK(payload.adjustments.rotationDegrees == 90);
  CHECK(payload.adjustments.flipHorizontal);
  CHECK(payload.adjustments.flipVertical);
  REQUIRE(payload.adjustments.playbackSpeed.has_value());
  CHECK(*payload.adjustments.playbackSpeed == doctest::Approx(1.5));
  CHECK(payload.adjustments.reverse);
  CHECK(payload.adjustments.hdrToSdr);
  // Compression intent (mandate §20).
  CHECK(payload.compression.enabled);
  CHECK(payload.compression.preset == CompressionPreset::TargetSize);
  CHECK(payload.compression.targetSizeBytes == 500'000'000);
  REQUIRE(payload.compression.quality.has_value());
  CHECK(*payload.compression.quality == 42);
  REQUIRE(payload.compression.videoBitrate.has_value());
  CHECK(*payload.compression.videoBitrate == 4'000'000);
  REQUIRE(payload.compression.audioBitrate.has_value());
  CHECK(*payload.compression.audioBitrate == 96'000);
  // Metadata intentions (mandate §33): every field, both actions.
  CHECK(payload.metadata.titleAction == MetadataFieldAction::Set);
  CHECK(payload.metadata.title == "Holiday");
  CHECK(payload.metadata.artistAction == MetadataFieldAction::Set);
  CHECK(payload.metadata.artist == "Velmux");
  CHECK(payload.metadata.albumAction == MetadataFieldAction::Clear);
  CHECK(payload.metadata.genreAction == MetadataFieldAction::Set);
  CHECK(payload.metadata.genre == "Travel");
  CHECK(payload.metadata.dateAction == MetadataFieldAction::Set);
  CHECK(payload.metadata.date == "2025-06-01T10:00:00Z");
  CHECK(payload.metadata.descriptionAction == MetadataFieldAction::Set);
  CHECK(payload.metadata.description == "A trip");
  CHECK(payload.metadata.locationAction == MetadataFieldAction::Set);
  CHECK(payload.metadata.location == "Kyoto");
  CHECK(payload.metadata.artworkAction == MetadataFieldAction::Set);
  CHECK(payload.metadata.artworkUri == "file:///tmp/cover.png");
  // Record-level fields.
  CHECK(loaded->pipeline.planned);
  CHECK(loaded->pipeline.summary == original.pipeline.summary);
  CHECK(loaded->outputPath == "/tmp/out.mkv");
  REQUIRE(loaded->outputs.size() == 1);
  CHECK(loaded->outputs.front().path == "/tmp/out.mkv");
  CHECK(loaded->outputs.front().sizeBytes == 12345);
  CHECK(loaded->outputs.front().kind == "media");
  CHECK(loaded->inputSizeBytes == 2'000'000'000);
  CHECK(loaded->outputSizeBytes == 480'000'000);
  CHECK(loaded->progress == doctest::Approx(0.42));
  CHECK(loaded->error == "none");
  CHECK(loaded->errorCode == "none");
  CHECK(loaded->startedAtMs == 1500);
  CHECK(loaded->finishedAtMs == 2500);
  CHECK(loaded->schemaVersion == 4);
}

TEST_CASE("defaults stay defaults through the round-trip") {
  TempDb db;
  // A minimal request: everything optional stays unset and reads back as
  // the model's defaults (a serialization that invents values would flip
  // these).
  JobRecord record;
  record.id = "job-plain";
  record.state = JobState::Queued;
  record.request.operation = Operation::VideoConvert;
  job::VideoConvert payload;
  payload.source = MediaSource{"file:///tmp/plain.mp4", "plain.mp4"};
  record.request.payload = std::move(payload);
  REQUIRE(db.jobs().insertJob(record).ok());

  Error error;
  const auto loaded = db.jobs().loadJob("job-plain", error);
  REQUIRE(loaded.has_value());
  const job::VideoConvert &plainPayload = videoPayload(loaded->request);
  CHECK_FALSE(plainPayload.video.codec.has_value());
  CHECK_FALSE(plainPayload.video.resolutionHeight.has_value());
  CHECK_FALSE(plainPayload.video.customWidth.has_value());
  CHECK_FALSE(plainPayload.video.frameRate.has_value());
  CHECK(plainPayload.audioTracks.empty());
  CHECK_FALSE(plainPayload.audioSettings.codec.has_value());
  CHECK_FALSE(plainPayload.audioSettings.bitRate.has_value());
  CHECK(plainPayload.subtitleTracks.empty());
  CHECK(plainPayload.adjustments.rotationDegrees == 0);
  CHECK_FALSE(plainPayload.adjustments.flipHorizontal);
  CHECK_FALSE(plainPayload.adjustments.reverse);
  CHECK_FALSE(plainPayload.adjustments.hdrToSdr);
  CHECK_FALSE(plainPayload.compression.enabled);
  CHECK(plainPayload.compression.preset == CompressionPreset::Balanced);
  CHECK(plainPayload.metadata.titleAction == MetadataFieldAction::Unchanged);
  CHECK(plainPayload.metadata.artistAction == MetadataFieldAction::Unchanged);
  CHECK(plainPayload.metadata.artworkAction == MetadataFieldAction::Unchanged);
  CHECK(loaded->submissionId.empty());
}

TEST_CASE("audio-convert, trim, extract-audio and extract-subtitles payloads round-trip") {
  TempDb db;
  Error error;

  {
    JobRecord record;
    record.id = "audio-convert";
    record.state = JobState::Queued;
    record.request.operation = Operation::AudioConvert;
    job::AudioConvert payload;
    payload.source = MediaSource{"file:///tmp/song.mp3", "song.mp3"};
    payload.output.container = Container::Flac;
    payload.audio.codec = AudioCodec::Flac;
    payload.audio.bitRate = 900'000;
    payload.adjustments.gainDb = -3.5;
    payload.adjustments.normalize = true;
    payload.adjustments.fadeInMs = 100;
    payload.adjustments.fadeOutMs = 250;
    payload.adjustments.playbackSpeed = 0.75;
    payload.adjustments.preservePitch = false;
    payload.adjustments.reverse = true;
    payload.adjustments.removeSilence = true;
    record.request.payload = std::move(payload);
    REQUIRE(db.jobs().insertJob(record).ok());
    const auto loaded = db.jobs().loadJob("audio-convert", error);
    REQUIRE(loaded.has_value());
    const auto &loadedPayload = std::get<job::AudioConvert>(loaded->request.payload);
    CHECK(loaded->request.operation == Operation::AudioConvert);
    REQUIRE(loadedPayload.output.container.has_value());
    CHECK(*loadedPayload.output.container == Container::Flac);
    REQUIRE(loadedPayload.audio.codec.has_value());
    CHECK(*loadedPayload.audio.codec == AudioCodec::Flac);
    REQUIRE(loadedPayload.audio.bitRate.has_value());
    CHECK(*loadedPayload.audio.bitRate == 900'000);
    REQUIRE(loadedPayload.adjustments.gainDb.has_value());
    CHECK(*loadedPayload.adjustments.gainDb == doctest::Approx(-3.5));
    CHECK(loadedPayload.adjustments.normalize);
    REQUIRE(loadedPayload.adjustments.fadeInMs.has_value());
    CHECK(*loadedPayload.adjustments.fadeInMs == 100);
    REQUIRE(loadedPayload.adjustments.fadeOutMs.has_value());
    CHECK(*loadedPayload.adjustments.fadeOutMs == 250);
    REQUIRE(loadedPayload.adjustments.playbackSpeed.has_value());
    CHECK(*loadedPayload.adjustments.playbackSpeed == doctest::Approx(0.75));
    CHECK_FALSE(loadedPayload.adjustments.preservePitch);
    CHECK(loadedPayload.adjustments.reverse);
    CHECK(loadedPayload.adjustments.removeSilence);
  }

  {
    JobRecord record;
    record.id = "trim";
    record.state = JobState::Queued;
    record.request.operation = Operation::VideoCutMerge;
    job::Trim payload;
    payload.source = MediaSource{"file:///tmp/in.mp4", "in.mp4"};
    payload.output.container = Container::Mp4;
    payload.range.startUs = 500'000;
    payload.range.endUs = 1'500'000;
    record.request.payload = std::move(payload);
    REQUIRE(db.jobs().insertJob(record).ok());
    const auto loaded = db.jobs().loadJob("trim", error);
    REQUIRE(loaded.has_value());
    const auto &loadedPayload = std::get<job::Trim>(loaded->request.payload);
    CHECK(loaded->request.operation == Operation::VideoCutMerge);
    CHECK(payload.range.startUs == 500'000);
    CHECK(payload.range.endUs == 1'500'000);
  }

  {
    JobRecord record;
    record.id = "extract-audio";
    record.state = JobState::Queued;
    record.request.operation = Operation::ExtractAudio;
    job::ExtractAudio payload;
    payload.source = MediaSource{"file:///tmp/in.mp4", "in.mp4"};
    payload.output.container = Container::Mp3;
    payload.trackIndices = {1};
    payload.audio.codec = AudioCodec::Mp3;
    payload.range = TimeRange{1'000'000, 2'000'000};
    record.request.payload = std::move(payload);
    REQUIRE(db.jobs().insertJob(record).ok());
    const auto loaded = db.jobs().loadJob("extract-audio", error);
    REQUIRE(loaded.has_value());
    const auto &loadedPayload = std::get<job::ExtractAudio>(loaded->request.payload);
    CHECK(loaded->request.operation == Operation::ExtractAudio);
    REQUIRE(loadedPayload.trackIndices.size() == 1);
    CHECK(loadedPayload.trackIndices.front() == 1);
    REQUIRE(loadedPayload.range.has_value());
    CHECK(loadedPayload.range->startUs == 1'000'000);
    CHECK(loadedPayload.range->endUs == 2'000'000);
    REQUIRE(loadedPayload.audio.codec.has_value());
    CHECK(*loadedPayload.audio.codec == AudioCodec::Mp3);
  }

  {
    JobRecord record;
    record.id = "extract-subtitles";
    record.state = JobState::Queued;
    record.request.operation = Operation::ExtractSubtitles;
    job::ExtractSubtitles payload;
    payload.source = MediaSource{"file:///tmp/in.mp4", "in.mp4"};
    payload.trackIndices = {2};
    payload.targets = {SubtitleTextTarget::Vtt};
    record.request.payload = std::move(payload);
    REQUIRE(db.jobs().insertJob(record).ok());
    const auto loaded = db.jobs().loadJob("extract-subtitles", error);
    REQUIRE(loaded.has_value());
    const auto &loadedPayload =
        std::get<job::ExtractSubtitles>(loaded->request.payload);
    CHECK(loaded->request.operation == Operation::ExtractSubtitles);
    REQUIRE(loadedPayload.trackIndices.size() == 1);
    CHECK(loadedPayload.trackIndices.front() == 2);
    REQUIRE(loadedPayload.targets.size() == 1);
    CHECK(loadedPayload.targets.front() == SubtitleTextTarget::Vtt);
  }
}

TEST_CASE("findJobBySubmissionId gives durable idempotency") {
  TempDb db;
  const JobRecord original = sampleJob("job-sub", JobState::Queued);
  REQUIRE(db.jobs().insertJob(original).ok());

  Error error;
  const auto found = db.jobs().findJobBySubmissionId("u-sample-1", error);
  REQUIRE(found.has_value());
  CHECK(found->id == "job-sub");
  CHECK(db.jobs().findJobBySubmissionId("u-missing", error) ==
        std::nullopt);
  CHECK(db.jobs().findJobBySubmissionId("", error) == std::nullopt);
}

TEST_CASE("update persists state transitions") {
  TempDb db;
  JobRecord record = sampleJob("job-2", JobState::Processing);
  REQUIRE(db.jobs().insertJob(record).ok());

  record.state = JobState::Completed;
  record.progress = 1.0;
  record.outputSizeBytes = 480'000'000;
  record.finishedAtMs = 9000;
  REQUIRE(db.jobs().updateJob(record).ok());

  Error error;
  const auto loaded = db.jobs().loadJob("job-2", error);
  REQUIRE(loaded.has_value());
  CHECK(loaded->state == JobState::Completed);
  CHECK(loaded->progress == doctest::Approx(1.0));
  CHECK(loaded->outputSizeBytes == 480'000'000);
  CHECK(loaded->finishedAtMs == 9000);
  // The submission id survives the delete+insert update.
  CHECK(loaded->submissionId == "u-sample-1");
}

TEST_CASE("loadResumableJobs returns only mid-flight jobs") {
  TempDb db;
  auto insert = [&](const std::string &id, JobState state) {
    JobRecord record = sampleJob(id, state);
    record.submissionId.clear(); // unique index on non-empty ids
    REQUIRE(db.jobs().insertJob(record).ok());
  };
  insert("queued", JobState::Queued);
  insert("processing", JobState::Processing);
  insert("finalizing", JobState::Finalizing);
  insert("recovering", JobState::Recovering);
  insert("done", JobState::Completed);
  insert("failed", JobState::Failed);
  // User-held states: never auto-resumed at startup; the user resumes them.
  insert("paused", JobState::Paused);
  insert("interrupted", JobState::Interrupted);

  Error error;
  const auto resumable = db.jobs().loadResumableJobs(error);
  REQUIRE(resumable.size() == 4);
  for (const auto &job : resumable) {
    CHECK(job.id != "done");
    CHECK(job.id != "failed");
    CHECK(job.id != "paused");
    CHECK(job.id != "interrupted");
  }
}

TEST_CASE("delete removes the job") {
  TempDb db;
  REQUIRE(db.jobs().insertJob(sampleJob("gone", JobState::Failed)).ok());
  REQUIRE(db.jobs().deleteJob("gone").ok());

  Error error;
  CHECK_FALSE(db.jobs().loadJob("gone", error).has_value());
}

TEST_CASE("loadAllOrdersNewestFirst") {
  TempDb db;
  JobRecord a = sampleJob("older", JobState::Completed);
  a.submissionId.clear();
  a.createdAtMs = 100;
  JobRecord b = sampleJob("newer", JobState::Completed);
  b.submissionId = "u-newer";
  b.createdAtMs = 200;
  REQUIRE(db.jobs().insertJob(a).ok());
  REQUIRE(db.jobs().insertJob(b).ok());

  Error error;
  const auto all = db.jobs().loadAllJobs(error);
  REQUIRE(all.size() == 2);
  CHECK(all.front().id == "newer");
}

TEST_CASE("source inspection survives the store round-trip (validation gate)") {
  TempDb db;
  JobRecord record = sampleJob("inspected", JobState::Queued);
  MediaInfo info;
  info.durationSec = 4.0;
  info.sizeBytes = 1'234'567;
  VideoStream video;
  video.codec = VideoCodec::H264;
  video.width = 320;
  video.height = 240;
  video.frameRate = 12.0;
  info.video.push_back(video);
  AudioStream audio;
  audio.codec = AudioCodec::Aac;
  audio.channels = 2;
  audio.sampleRate = 44100;
  info.audio.push_back(audio);
  SubtitleStream english;
  english.format = SubtitleFormat::Srt;
  english.language = "eng";
  info.subtitles.push_back(english);
  SubtitleStream forced;
  forced.format = SubtitleFormat::Ass;
  forced.isForced = true;
  info.subtitles.push_back(forced);
  record.sourceInfo.push_back(info);

  REQUIRE(db.jobs().insertJob(record).ok());
  Error error;
  const auto loaded = db.jobs().loadJob("inspected", error);
  REQUIRE(loaded.has_value());

  // The fields output validation needs must be live after the reload:
  // an empty sourceInfo silently disabled the duration/geometry checks
  // (found by the M12 truncated-source hardening test).
  REQUIRE(loaded->sourceInfo.size() == 1);
  const MediaInfo &source = loaded->sourceInfo.front();
  CHECK(source.durationSec == doctest::Approx(4.0));
  CHECK(source.sizeBytes == 1'234'567);
  REQUIRE(source.hasVideo());
  CHECK(source.video.front().width == 320);
  CHECK(source.video.front().height == 240);
  CHECK(source.video.front().codec == VideoCodec::H264);
  CHECK(source.video.front().frameRate == doctest::Approx(12.0));
  REQUIRE(source.hasAudio());
  CHECK(source.audio.front().sampleRate == 44100);
  CHECK(source.audio.front().channels == 2);
  CHECK(source.audio.front().codec == AudioCodec::Aac);
  // Subtitle tracks drive the planner's burn-in/preserve/extract routing
  // and container compatibility; losing them after a reload broke exactly
  // those policies.
  REQUIRE(source.subtitles.size() == 2);
  CHECK(source.subtitles[0].format == SubtitleFormat::Srt);
  CHECK(source.subtitles[0].language == "eng");
  CHECK(source.subtitles[1].format == SubtitleFormat::Ass);
  CHECK(source.subtitles[1].isForced);

  // updateJob (delete+insert) preserves it too.
  JobRecord updated = *loaded;
  updated.state = JobState::Completed;
  REQUIRE(db.jobs().updateJob(updated).ok());
  const auto reloaded = db.jobs().loadJob("inspected", error);
  REQUIRE(reloaded.has_value());
  CHECK(reloaded->sourceInfo.front().durationSec == doctest::Approx(4.0));
  CHECK(reloaded->sourceInfo.front().video.front().height == 240);
}

// ---------------------------------------------------------------------------
// v3 -> v4 migration: hand-written legacy rows (the OLD serializer's k=v
// strings, exactly as git history produced them) migrate on open.
// ---------------------------------------------------------------------------

namespace {

// Creates a v3-shaped database with raw SQL: the legacy `jobs` table, a
// schema_version of 3 and the given rows. Returns false on SQL failure.
bool writeLegacyDatabase(const std::filesystem::path &path) {
  sqlite3 *db = nullptr;
  if (sqlite3_open(path.string().c_str(), &db) != SQLITE_OK) {
    sqlite3_close(db);
    return false;
  }
  const char *schema =
      "CREATE TABLE jobs ("
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
      ");"
      "CREATE TABLE schema_version (version INTEGER NOT NULL);"
      "INSERT INTO schema_version (version) VALUES (3);";
  char *errMsg = nullptr;
  if (sqlite3_exec(db, schema, nullptr, nullptr, &errMsg) != SQLITE_OK) {
    sqlite3_free(errMsg);
    sqlite3_close(db);
    return false;
  }
  const char *rows[] = {
      // Row 1: a rich conversion exactly as the old serializer wrote it —
      // int goal_kind, k=v pair strings, trim window, stripMetadata, the
      // retired advanced controls (profile/level/pixelFormat were JSON
      // columns? No: they lived in the k=v video section — the legacy
      // writer did not persist them at all, which is why they retire).
      "INSERT INTO jobs (id, state, source_uri, source_name, output_name, "
      "output_path, output_container, goal_kind, goal_target_bytes, "
      "video_json, audio_json, subtitles_json, processing_mode, "
      "progress, input_size, output_size, created_at, updated_at, "
      "trim_start_us, trim_end_us, strip_metadata) VALUES ("
      "'legacy-convert', 'running', 'file:///tmp/sample.mp4', "
      "'sample.mp4', 'out', '/tmp/out.mkv', 'mkv', '3', 500000000, "
      "'enabled=1;codec=h264;height=1080;fps=30.000000;rate=1;"
      "bitrate=4000000;quality=80;rotate=90;fliph=1;', "
      "'enabled=1;codec=aac;bitrate=128000;rate=48000;channels=2;"
      "srctrack=1;replace=file:///tmp/replacement.m4a;"
      "cover=file:///tmp/cover.png;keepcover=0;', "
      "'policy=1;external=file:///tmp/subs.srt;offset=-250;', "
      "'preferNative', 0.42, 2000000000, 480000000, 1000, 2000, "
      "500000, 1500000, 1)",
      // Row 2: a checkpointing-state row with defaults everywhere.
      "INSERT INTO jobs (id, state, source_uri, source_name, "
      "output_container, goal_kind, goal_target_bytes, video_json, "
      "audio_json, subtitles_json, created_at, updated_at, "
      "strip_metadata) VALUES ("
      "'legacy-checkpoint', 'checkpointing', 'file:///tmp/other.mp4', "
      "'other.mp4', 'mp4', '0', 0, 'enabled=1;', 'enabled=1;', "
      "'policy=0;offset=0;', 500, 600, 0)",
      // Row 3: extraction (policy=2) with video disabled, audio-only
      // container.
      "INSERT INTO jobs (id, state, source_uri, source_name, "
      "output_container, goal_kind, goal_target_bytes, video_json, "
      "audio_json, subtitles_json, created_at, updated_at) VALUES ("
      "'legacy-extract-subs', 'completed', 'file:///tmp/subs.mkv', "
      "'subs.mkv', 'mkv', '0', 0, 'enabled=0;', 'enabled=0;', "
      "'policy=2;offset=0;', 100, 200)",
      // Row 4: audio-only conversion with the DSP chain (video disabled,
      // audio-only container) and a goal of custom.
      "INSERT INTO jobs (id, state, source_uri, source_name, "
      "output_container, goal_kind, goal_target_bytes, video_json, "
      "audio_json, subtitles_json, created_at, updated_at) VALUES ("
      "'legacy-audio-dsp', 'failed', 'file:///tmp/song.flac', 'song.flac', "
      "'mp3', '4', 0, 'enabled=0;', "
      "'enabled=1;codec=mp3;bitrate=192000;gain=3.500000;norm=1;"
      "speed=1.500000;desil=1;', 'policy=0;offset=0;', 300, 400)",
  };
  for (const char *row : rows) {
    if (sqlite3_exec(db, row, nullptr, nullptr, &errMsg) != SQLITE_OK) {
      sqlite3_free(errMsg);
      sqlite3_close(db);
      return false;
    }
  }
  sqlite3_close(db);
  return true;
}

} // namespace

TEST_CASE("v3 databases migrate on open: every row, honest mapping") {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() /
      ("velmux-test-migrate-" + std::to_string(::getpid()) + "-" +
       std::to_string(::rand()));
  std::filesystem::create_directories(dir);
  const std::filesystem::path dbPath = dir / "jobs.sqlite";
  REQUIRE(writeLegacyDatabase(dbPath));

  // Open through the store: the migration runs inside open().
  SqliteStore store;
  REQUIRE(store.open(dbPath.string()).ok());
  JobStore jobs(store);
  Error error;

  // Row 1: the rich conversion. The trim window routes it to the trim
  // payload of video-cutmerge; the goal becomes TargetSize.
  const auto converted = jobs.loadJob("legacy-convert", error);
  REQUIRE(converted.has_value());
  CHECK(converted->state == JobState::Processing); // "running" maps to Processing
  CHECK(converted->request.operation == Operation::VideoCutMerge);
  const auto *trimPayload = std::get_if<job::Trim>(&converted->request.payload);
  REQUIRE(trimPayload != nullptr);
  CHECK(trimPayload->range.startUs == 500000);
  CHECK(trimPayload->range.endUs == 1500000);
  CHECK(trimPayload->source.uri == "file:///tmp/sample.mp4");
  REQUIRE(trimPayload->output.container.has_value());
  CHECK(*trimPayload->output.container == Container::Mkv);
  CHECK(trimPayload->output.name == std::optional<std::string>("out"));

  // Row 2: "checkpointing" -> "finalizing"; a default row migrates to a
  // plain VideoConvert with the Balanced compression intent.
  const auto checkpoint = jobs.loadJob("legacy-checkpoint", error);
  REQUIRE(checkpoint.has_value());
  CHECK(checkpoint->state == JobState::Finalizing);
  CHECK(checkpoint->request.operation == Operation::VideoConvert);
  const job::VideoConvert &plain = videoPayload(checkpoint->request);
  CHECK(plain.compression.enabled);
  CHECK(plain.compression.preset == CompressionPreset::Balanced);
  // goal_kind "0" (recommended) -> balanced.

  // Row 3: subtitle extraction (policy=2) -> the ExtractSubtitles operation.
  const auto extracted = jobs.loadJob("legacy-extract-subs", error);
  REQUIRE(extracted.has_value());
  CHECK(extracted->request.operation == Operation::ExtractSubtitles);
  CHECK(std::holds_alternative<job::ExtractSubtitles>(
      extracted->request.payload));
  CHECK(extracted->state == JobState::Completed);

  // Row 4: video disabled + audio-only container + DSP -> AudioConvert
  // with the adjustments carried.
  const auto audioJob = jobs.loadJob("legacy-audio-dsp", error);
  REQUIRE(audioJob.has_value());
  CHECK(audioJob->request.operation == Operation::AudioConvert);
  const auto &ac = std::get<job::AudioConvert>(audioJob->request.payload);
  REQUIRE(ac.audio.codec.has_value());
  CHECK(*ac.audio.codec == AudioCodec::Mp3);
  REQUIRE(ac.audio.bitRate.has_value());
  CHECK(*ac.audio.bitRate == 192000);
  REQUIRE(ac.adjustments.gainDb.has_value());
  CHECK(*ac.adjustments.gainDb == doctest::Approx(3.5));
  CHECK(ac.adjustments.normalize);
  REQUIRE(ac.adjustments.playbackSpeed.has_value());
  CHECK(*ac.adjustments.playbackSpeed == doctest::Approx(1.5));
  CHECK(ac.adjustments.removeSilence);
  CHECK(ac.compression.enabled);
  CHECK(ac.compression.preset == CompressionPreset::Custom);

  store.close();
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST_CASE("migrated rich row maps goals, audio lanes, subtitles and metadata") {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() /
      ("velmux-test-migrate2-" + std::to_string(::getpid()) + "-" +
       std::to_string(::rand()));
  std::filesystem::create_directories(dir);
  const std::filesystem::path dbPath = dir / "jobs.sqlite";
  REQUIRE(writeLegacyDatabase(dbPath));

  // A dedicated row exercising the mapping details: named fitUnderSize
  // goal, allTracks, burn-in subtitles with an external file and
  // strip_metadata=1.
  {
    sqlite3 *db = nullptr;
    REQUIRE(sqlite3_open(dbPath.string().c_str(), &db) == SQLITE_OK);
    const char *row =
        "INSERT INTO jobs (id, state, source_uri, source_name, "
        "output_container, goal_kind, goal_target_bytes, video_json, "
        "audio_json, subtitles_json, created_at, updated_at, "
        "strip_metadata) VALUES ("
        "'legacy-rich', 'queued', 'file:///tmp/rich.mp4', 'rich.mp4', "
        "'mp4', 'fitUnderSize', 777000000, "
        "'enabled=1;codec=hevc;height=720;fps=24.000000;rate=0;', "
        "'enabled=1;codec=aac;bitrate=96000;alltracks=1;', "
        "'policy=3;external=file:///tmp/ext.srt;offset=250;', "
        "700, 800, 1)";
    char *errMsg = nullptr;
    REQUIRE(sqlite3_exec(db, row, nullptr, nullptr, &errMsg) == SQLITE_OK);
    // A second row carrying a crop: the crop-inside-conversion shape
    // migrates to the dedicated Crop operation.
    const char *cropRow =
        "INSERT INTO jobs (id, state, source_uri, source_name, "
        "output_container, goal_kind, goal_target_bytes, video_json, "
        "audio_json, subtitles_json, created_at, updated_at) VALUES ("
        "'legacy-crop', 'queued', 'file:///tmp/crop.mp4', 'crop.mp4', "
        "'mp4', '2', 0, "
        "'enabled=1;codec=h264;crop=10,20,1280,720;rotate=180;', "
        "'enabled=1;', 'policy=0;offset=0;', 900, 950)";
    REQUIRE(sqlite3_exec(db, cropRow, nullptr, nullptr, &errMsg) ==
            SQLITE_OK);
    sqlite3_close(db);
  }

  SqliteStore store;
  REQUIRE(store.open(dbPath.string()).ok());
  JobStore jobs(store);
  Error error;
  const auto rich = jobs.loadJob("legacy-rich", error);
  REQUIRE(rich.has_value());
  CHECK(rich->request.operation == Operation::VideoConvert);
  const job::VideoConvert &vc = videoPayload(rich->request);

  // goal fitUnderSize -> TargetSize with the target bytes.
  CHECK(vc.compression.enabled);
  CHECK(vc.compression.preset == CompressionPreset::TargetSize);
  CHECK(vc.compression.targetSizeBytes == 777000000);

  // Video intent: codec + height; the legacy double fps becomes a
  // rational frame rate.
  REQUIRE(vc.video.codec.has_value());
  CHECK(*vc.video.codec == VideoCodec::HEVC);
  REQUIRE(vc.video.resolutionHeight.has_value());
  CHECK(*vc.video.resolutionHeight == 720);
  REQUIRE(vc.video.frameRate.has_value());
  CHECK(vc.video.frameRate->toDouble() == doctest::Approx(24.0));
  // Audio: codec + bitrate; allTracks -> the empty rule list (preserve
  // every lane).
  REQUIRE(vc.audioSettings.codec.has_value());
  CHECK(*vc.audioSettings.codec == AudioCodec::Aac);
  REQUIRE(vc.audioSettings.bitRate.has_value());
  CHECK(*vc.audioSettings.bitRate == 96'000);
  CHECK(vc.audioTracks.empty());
  // Subtitle policy burn-in (3) with the external file.
  REQUIRE(vc.subtitleTracks.size() == 1);
  CHECK(vc.subtitleTracks.front().policy == SubtitleTrackPolicy::BurnIn);
  REQUIRE(vc.subtitleTracks.front().externalUri.has_value());
  CHECK(*vc.subtitleTracks.front().externalUri == "file:///tmp/ext.srt");
  // strip_metadata=1 -> the all-Clear metadata intention (the moral
  // equivalent of the retired control).
  CHECK(vc.metadata.titleAction == MetadataFieldAction::Clear);
  CHECK(vc.metadata.artistAction == MetadataFieldAction::Clear);
  CHECK(vc.metadata.albumAction == MetadataFieldAction::Clear);
  CHECK(vc.metadata.genreAction == MetadataFieldAction::Clear);
  CHECK(vc.metadata.dateAction == MetadataFieldAction::Clear);
  CHECK(vc.metadata.descriptionAction == MetadataFieldAction::Clear);
  CHECK(vc.metadata.locationAction == MetadataFieldAction::Clear);
  CHECK(vc.metadata.artworkAction == MetadataFieldAction::Clear);

  // The crop row routes to the dedicated Crop operation with the
  // rectangle carried.
  const auto cropped = jobs.loadJob("legacy-crop", error);
  REQUIRE(cropped.has_value());
  CHECK(cropped->request.operation == Operation::VideoCrop);
  const auto *cropPayload = std::get_if<job::Crop>(&cropped->request.payload);
  REQUIRE(cropPayload != nullptr);
  CHECK(cropPayload->crop.x == 10);
  CHECK(cropPayload->crop.y == 20);
  CHECK(cropPayload->crop.width == 1280);
  CHECK(cropPayload->crop.height == 720);
  REQUIRE(cropPayload->output.container.has_value());
  CHECK(*cropPayload->output.container == Container::Mp4);

  store.close();
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}
