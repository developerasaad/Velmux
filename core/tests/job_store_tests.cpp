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
  REQUIRE(db.jobs().insertJob(sampleJob("done", JobState::Completed)).ok());
  REQUIRE(db.jobs().insertJob(sampleJob("failed", JobState::Failed)).ok());
  REQUIRE(db.jobs().insertJob(sampleJob("paused", JobState::Paused)).ok());

  Error error;
  const auto resumable = db.jobs().loadResumableJobs(error);
  REQUIRE(resumable.size() == 3);
  for (const auto &job : resumable) {
    CHECK(job.id != "done");
    CHECK(job.id != "failed");
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
