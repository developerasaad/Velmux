// Recovery system unit tests: checkpoint persistence, artifact validation,
// reconciliation decisions.
#include "velmux/core/recovery.h"

#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace velmux::core;

namespace {

class TempDb {
public:
  TempDb() {
    dir_ = std::filesystem::temp_directory_path() /
           ("velmux-rec-" + std::to_string(::getpid()) + "-" +
            std::to_string(counter_++));
    std::filesystem::create_directories(dir_);
    REQUIRE(store_.open((dir_ / "rec.sqlite").string()).ok());
    checkpoints_ = std::make_unique<CheckpointStore>(store_);
  }
  ~TempDb() {
    checkpoints_.reset();
    store_.close();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }
  CheckpointStore &checkpoints() { return *checkpoints_; }
  const std::filesystem::path &dir() const { return dir_; }

private:
  static inline int counter_ = 0;
  std::filesystem::path dir_;
  SqliteStore store_;
  std::unique_ptr<CheckpointStore> checkpoints_;
};

std::string writeArtifact(const std::filesystem::path &path,
                          std::int64_t bytes) {
  std::ofstream out(path, std::ios::binary);
  for (std::int64_t i = 0; i < bytes; ++i) {
    out.put('x');
  }
  return path.string();
}

JobRecord runningJob(const std::string &id) {
  JobRecord record;
  record.id = id;
  record.state = JobState::Running;
  record.request.source.uri = "file:///tmp/x.mp4";
  record.request.source.displayName = "x.mp4";
  record.createdAtMs = 1;
  record.updatedAtMs = 2;
  return record;
}

} // namespace

TEST_CASE("recovery strategies map per stage") {
  CHECK(recoveryStrategyFor(StageId::Demux) ==
        RecoveryStrategy::PacketBoundary);
  CHECK(recoveryStrategyFor(StageId::Encode) ==
        RecoveryStrategy::FinalizedSegments);
  CHECK(recoveryStrategyFor(StageId::AudioProcessing) ==
        RecoveryStrategy::SampleSegment);
  CHECK(recoveryStrategyFor(StageId::SubtitleProcessing) ==
        RecoveryStrategy::CueTimeline);
  CHECK(std::string(recoveryStrategyName(RecoveryStrategy::FinalizedSegments)) ==
        "finalized-segments");
}

TEST_CASE("checkpoints persist ordered and reload intact") {
  TempDb db;
  CheckpointRecord first;
  first.jobId = "j1";
  first.sequence = 1;
  first.stage = StageId::Encode;
  first.position = 10'000'000; // presentation timestamp
  first.durationUs = 30'000'000;
  first.artifactPath = writeArtifact(db.dir() / "seg-0001.mkv", 4096);
  first.artifactBytes = 4096;
  first.committedAtMs = 111;
  REQUIRE(db.checkpoints().insertCheckpoint(first).ok());
  CHECK(first.id > 0);

  CheckpointRecord second;
  second.jobId = "j1";
  second.sequence = 2;
  second.stage = StageId::Encode;
  second.position = 60'000'000;
  second.durationUs = 90'000'000;
  second.artifactPath = writeArtifact(db.dir() / "seg-0002.mkv", 8192);
  second.artifactBytes = 8192;
  second.committedAtMs = 222;
  REQUIRE(db.checkpoints().insertCheckpoint(second).ok());

  Error error;
  const auto loaded = db.checkpoints().loadCheckpoints("j1", error);
  REQUIRE(loaded.size() == 2);
  CHECK(loaded[0].sequence == 1);
  CHECK(loaded[1].sequence == 2);
  CHECK(loaded[1].position == 60'000'000);
  CHECK(loaded[1].stage == StageId::Encode);
  CHECK(loaded[0].artifactPath == first.artifactPath);
}

TEST_CASE("artifactLooksValid checks existence and size sanity") {
  CheckpointRecord record;
  record.artifactPath = "/nonexistent/nothing.mkv";
  record.artifactBytes = 100;
  CHECK_FALSE(RecoveryPlanner::artifactLooksValid(record));

  // Position-only checkpoints are valid without artifacts.
  CheckpointRecord positionOnly;
  positionOnly.artifactPath = "";
  CHECK(RecoveryPlanner::artifactLooksValid(positionOnly));

  TempDb db;
  CheckpointRecord ok;
  ok.artifactPath = writeArtifact(db.dir() / "ok.mkv", 2048);
  ok.artifactBytes = 2048;
  CHECK(RecoveryPlanner::artifactLooksValid(ok));

  // A partial artifact (much smaller than recorded) is invalid.
  CheckpointRecord partial;
  partial.artifactPath = writeArtifact(db.dir() / "partial.mkv", 100);
  partial.artifactBytes = 100'000;
  CHECK_FALSE(RecoveryPlanner::artifactLooksValid(partial));
}

TEST_CASE("reconcile decides per job state") {
  TempDb db;
  RecoveryPlanner planner;

  std::vector<JobRecord> jobs = {
      runningJob("terminal-done"), runningJob("never-started"),
      runningJob("mid-flight")};
  jobs[0].state = JobState::Completed;
  jobs[1].state = JobState::Queued;
  jobs[2].state = JobState::Running;

  // No checkpoints for any job: mid-flight restarts, never-started restarts.
  auto decisions = planner.reconcile(jobs, db.checkpoints(),
                                      db.dir().string());
  REQUIRE(decisions.size() == 3);
  CHECK(decisions[0].action == RecoveryAction::NothingToDo);
  CHECK(decisions[1].action == RecoveryAction::RestartJob);
  CHECK(decisions[2].action == RecoveryAction::RestartJob);

  // Commit a valid checkpoint for the mid-flight job: it now resumes.
  CheckpointRecord cp;
  cp.jobId = "mid-flight";
  cp.sequence = 1;
  cp.stage = StageId::Encode;
  cp.position = 42'000'000;
  cp.artifactPath = writeArtifact(db.dir() / "mid-seg.mkv", 4096);
  cp.artifactBytes = 4096;
  cp.committedAtMs = 500;
  REQUIRE(db.checkpoints().insertCheckpoint(cp).ok());

  decisions = planner.reconcile(jobs, db.checkpoints(), db.dir().string());
  REQUIRE(decisions.size() == 3);
  CHECK(decisions[2].action == RecoveryAction::ResumeFromCheckpoint);
  REQUIRE(decisions[2].resumeFrom.has_value());
  CHECK(decisions[2].resumeFrom->position == 42'000'000);
  CHECK_FALSE(decisions[2].message.empty());

  // If the artifact disappears (partial/corrupt), the job restarts.
  std::filesystem::remove(db.dir() / "mid-seg.mkv");
  decisions = planner.reconcile(jobs, db.checkpoints(), db.dir().string());
  CHECK(decisions[2].action == RecoveryAction::RestartJob);
}

TEST_CASE("reconciliation resumes from the newest valid checkpoint") {
  TempDb db;
  RecoveryPlanner planner;

  std::vector<JobRecord> jobs = {runningJob("j")};

  CheckpointRecord older;
  older.jobId = "j";
  older.sequence = 1;
  older.stage = StageId::Encode;
  older.position = 1000;
  older.artifactPath = writeArtifact(db.dir() / "old.mkv", 4096);
  older.artifactBytes = 4096;
  REQUIRE(db.checkpoints().insertCheckpoint(older).ok());

  CheckpointRecord newer;
  newer.jobId = "j";
  newer.sequence = 2;
  newer.stage = StageId::Encode;
  newer.position = 999'000;
  newer.artifactPath = db.dir() / "new.mkv"; // never written -> invalid
  newer.artifactBytes = 4096;
  REQUIRE(db.checkpoints().insertCheckpoint(newer).ok());

  auto decisions = planner.reconcile(jobs, db.checkpoints(),
                                      db.dir().string());
  REQUIRE(decisions.size() == 1);
  CHECK(decisions[0].action == RecoveryAction::ResumeFromCheckpoint);
  REQUIRE(decisions[0].resumeFrom.has_value());
  CHECK(decisions[0].resumeFrom->position == 1000); // fell back to older
}
