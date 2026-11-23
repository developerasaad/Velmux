// Job store unit tests: durable persistence round-trips and resumable
// queries. Uses a temp directory database.
#include "velmux/core/job_store.h"

#include <doctest/doctest.h>

#include <cstdio>
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

private:
  static inline int counter_ = 0;
  std::filesystem::path dir_;
  SqliteStore store_;
  std::unique_ptr<JobStore> jobs_;
};

JobRecord sampleJob(const std::string &id, JobState state) {
  JobRecord record;
  record.id = id;
  record.state = state;
  record.request.source.uri = "file:///tmp/sample.mp4";
  record.request.source.displayName = "sample.mp4";
  record.request.outputContainer = Container::Mkv;
  record.request.goal.kind = GoalKind::FitUnderSize;
  record.request.goal.targetSizeBytes = 500'000'000;
  record.request.video.codec = VideoCodec::H264;
  record.request.video.resolutionHeight = 1080;
  record.request.video.bitRate = 4'000'000;
  record.request.audio.codec = AudioCodec::Aac;
  record.request.audio.bitRate = 128'000;
  record.request.subtitles.policy = SubtitlePolicy::Preserve;
  record.request.subtitles.timingOffsetMs = -250;
  record.request.processingMode = ProcessingMode::PreferNative;
  record.pipeline.summary = "Transcode h264 via ffmpeg -> matroska";
  record.pipeline.container = Container::Mkv;
  record.pipeline.videoCodec = VideoCodec::H264;
  record.pipeline.audioCodec = AudioCodec::Aac;
  record.outputPath = "/tmp/out.mkv";
  record.inputSizeBytes = 2'000'000'000;
  record.progress = 0.42;
  record.createdAtMs = 1000;
  record.updatedAtMs = 2000;
  record.startedAtMs = 1500;
  return record;
}

} // namespace

TEST_CASE("insert and load round-trips the full request") {
  TempDb db;
  const JobRecord original = sampleJob("job-1", JobState::Running);
  REQUIRE(db.jobs().insertJob(original).ok());

  Error error;
  const auto loaded = db.jobs().loadJob("job-1", error);
  REQUIRE(loaded.has_value());
  CHECK(loaded->id == "job-1");
  CHECK(loaded->state == JobState::Running);
  CHECK(loaded->request.source.uri == original.request.source.uri);
  CHECK(loaded->request.source.displayName == "sample.mp4");
  CHECK(loaded->request.outputContainer == Container::Mkv);
  CHECK(loaded->request.goal.kind == GoalKind::FitUnderSize);
  CHECK(loaded->request.goal.targetSizeBytes == 500'000'000);
  CHECK(loaded->request.video.codec == VideoCodec::H264);
  CHECK(loaded->request.video.resolutionHeight.value() == 1080);
  CHECK(loaded->request.video.bitRate.value() == 4'000'000);
  CHECK(loaded->request.audio.codec == AudioCodec::Aac);
  CHECK(loaded->request.audio.bitRate.value() == 128'000);
  CHECK(loaded->request.subtitles.policy == SubtitlePolicy::Preserve);
  CHECK(loaded->request.subtitles.timingOffsetMs == -250);
  CHECK(loaded->request.processingMode == ProcessingMode::PreferNative);
  CHECK(loaded->pipeline.summary == original.pipeline.summary);
  CHECK(loaded->outputPath == "/tmp/out.mkv");
  CHECK(loaded->inputSizeBytes == 2'000'000'000);
  CHECK(loaded->progress == doctest::Approx(0.42));
  CHECK(loaded->startedAtMs == 1500);
}

TEST_CASE("update persists state transitions") {
  TempDb db;
  JobRecord record = sampleJob("job-2", JobState::Running);
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
}

TEST_CASE("loadResumableJobs returns only mid-flight jobs") {
  TempDb db;
  REQUIRE(db.jobs().insertJob(sampleJob("queued", JobState::Queued)).ok());
  REQUIRE(db.jobs().insertJob(sampleJob("running", JobState::Running)).ok());
  REQUIRE(db.jobs().insertJob(sampleJob("recovering", JobState::Recovering)).ok());
  REQUIRE(db.jobs().insertJob(sampleJob("done", JobState::Completed)).ok());
  REQUIRE(db.jobs().insertJob(sampleJob("failed", JobState::Failed)).ok());
  // User-held states: never auto-resumed at startup; the user resumes them.
  REQUIRE(db.jobs().insertJob(sampleJob("paused", JobState::Paused)).ok());
  REQUIRE(db.jobs().insertJob(sampleJob("interrupted", JobState::Interrupted)).ok());

  Error error;
  const auto resumable = db.jobs().loadResumableJobs(error);
  REQUIRE(resumable.size() == 3);
  for (const auto &job : resumable) {
    CHECK(job.id != "done");
    CHECK(job.id != "failed");
    CHECK(job.id != "paused");
    CHECK(job.id != "interrupted");
  }
}

TEST_CASE("delete removes the job and its checkpoints") {
  TempDb db;
  REQUIRE(db.jobs().insertJob(sampleJob("gone", JobState::Failed)).ok());
  REQUIRE(db.jobs().deleteJob("gone").ok());

  Error error;
  CHECK_FALSE(db.jobs().loadJob("gone", error).has_value());
}

TEST_CASE("loadAllOrdersNewestFirst") {
  TempDb db;
  JobRecord a = sampleJob("older", JobState::Completed);
  a.createdAtMs = 100;
  JobRecord b = sampleJob("newer", JobState::Completed);
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
  record.sourceInfo.durationSec = 4.0;
  record.sourceInfo.sizeBytes = 1'234'567;
  VideoStream video;
  video.codec = VideoCodec::H264;
  video.width = 320;
  video.height = 240;
  video.frameRate = 12.0;
  record.sourceInfo.video.push_back(video);
  AudioStream audio;
  audio.codec = AudioCodec::Aac;
  audio.channels = 2;
  audio.sampleRate = 44100;
  record.sourceInfo.audio.push_back(audio);
  SubtitleStream english;
  english.format = SubtitleFormat::Srt;
  english.language = "eng";
  record.sourceInfo.subtitles.push_back(english);
  SubtitleStream forced;
  forced.format = SubtitleFormat::Ass;
  forced.isForced = true;
  record.sourceInfo.subtitles.push_back(forced);

  REQUIRE(db.jobs().insertJob(record).ok());
  Error error;
  const auto loaded = db.jobs().loadJob("inspected", error);
  REQUIRE(loaded.has_value());

  // The fields output validation needs must be live after the reload:
  // an empty sourceInfo silently disabled the duration/geometry checks
  // (found by the M12 truncated-source hardening test).
  CHECK(loaded->sourceInfo.durationSec == doctest::Approx(4.0));
  CHECK(loaded->sourceInfo.sizeBytes == 1'234'567);
  REQUIRE(loaded->sourceInfo.hasVideo());
  CHECK(loaded->sourceInfo.video.front().width == 320);
  CHECK(loaded->sourceInfo.video.front().height == 240);
  CHECK(loaded->sourceInfo.video.front().codec == VideoCodec::H264);
  CHECK(loaded->sourceInfo.video.front().frameRate == doctest::Approx(12.0));
  REQUIRE(loaded->sourceInfo.hasAudio());
  CHECK(loaded->sourceInfo.audio.front().sampleRate == 44100);
  CHECK(loaded->sourceInfo.audio.front().channels == 2);
  CHECK(loaded->sourceInfo.audio.front().codec == AudioCodec::Aac);
  // Subtitle tracks drive the planner's burn-in/preserve/extract routing
  // and container compatibility; losing them after a reload broke exactly
  // those policies.
  REQUIRE(loaded->sourceInfo.subtitles.size() == 2);
  CHECK(loaded->sourceInfo.subtitles[0].format == SubtitleFormat::Srt);
  CHECK(loaded->sourceInfo.subtitles[0].language == "eng");
  CHECK(loaded->sourceInfo.subtitles[1].format == SubtitleFormat::Ass);

  // updateJob (delete+insert) preserves it too.
  JobRecord updated = *loaded;
  updated.state = JobState::Completed;
  REQUIRE(db.jobs().updateJob(updated).ok());
  const auto reloaded = db.jobs().loadJob("inspected", error);
  REQUIRE(reloaded.has_value());
  CHECK(reloaded->sourceInfo.durationSec == doctest::Approx(4.0));
  CHECK(reloaded->sourceInfo.video.front().height == 240);
}
