// FFmpeg backend integration tests: REAL conversions through the libav*
// libraries with INDEPENDENT ffprobe cross-validation of every output.
//
// Fixtures are generated at test time with the built ffmpeg binary (a dev
// tool of the pinned prefix) from deterministic lavfi inputs, so the suite
// is self-contained and reproducible.
#include "velmux/ffmpeg/ffmpeg_backend.h"

#include "velmux/core/engine.h"

#include <doctest/doctest.h>

#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

using namespace velmux::core;
using velmux::ffmpeg::FfmpegBackend;

namespace {

std::string ffprobeBin() {
#ifdef VELMUX_TEST_FFPROBE
  return VELMUX_TEST_FFPROBE;
#else
  return "ffprobe";
#endif
}

std::string ffmpegBin() {
#ifdef VELMUX_TEST_FFMPEG_PREFIX
  return std::string(VELMUX_TEST_FFMPEG_PREFIX) + "/bin/ffmpeg";
#else
  return "ffmpeg";
#endif
}

struct TestEnv {
  std::filesystem::path dir;
  std::unique_ptr<VelmuxEngine> engine;
  velmux::ffmpeg::FfmpegBackend *backend = nullptr;

  TestEnv() {
    dir = std::filesystem::temp_directory_path() /
          ("velmux-ff-" + std::to_string(::getpid()) + "-" +
           std::to_string(::getpid() ^ 0x5a5a) + "-" +
           std::to_string(seq++));
    std::filesystem::create_directories(dir);
    auto owned = std::make_unique<FfmpegBackend>();
    backend = owned.get();
    std::vector<std::unique_ptr<velmux::backend::IBackend>> backends;
    backends.push_back(std::move(owned));
    engine = VelmuxEngine::create(EngineConfig{dir.string()},
                                  std::move(backends));
    REQUIRE(engine != nullptr);
  }
  ~TestEnv() {
    engine.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
  static inline int seq = 0;
};

// Generates a deterministic H.264+AAC MP4 fixture with the pinned build's
// ffmpeg binary (dev tooling; the backend itself never spawns processes).
// Uses -filter_complex source filters (testsrc2/sine): the lavfi input
// device would require libavdevice, which Velmux intentionally excludes.
std::string generateFixture(const std::filesystem::path &path,
                            const std::string &videoExpr,
                            const std::string &audioExpr, double seconds) {
  const std::string cmd =
      ffmpegBin() + " -y -hide_banner -loglevel error " +
      "-filter_complex \"" + videoExpr + "[v];" + audioExpr +
      "[a]\" -map \"[v]\" -map \"[a]\" "
      "-c:v libx264 -preset ultrafast -crf 35 -c:a aac -b:a 96k " +
      "-t " + std::to_string(seconds) + " " +
      // -pix_fmt yuv420p keeps the fixture broadly decodable
      "-pix_fmt yuv420p " + path.string();
  REQUIRE(std::system(cmd.c_str()) == 0);
  REQUIRE(std::filesystem::file_size(path) > 1024);
  return path.string();
}

// Runs ffprobe and extracts a stream's codec name / dimensions via the
// default (compact) format. INDEPENDENT of the backend's own inspector.
struct ProbeResult {
  std::string videoCodec;
  std::string audioCodec;
  int width = 0;
  int height = 0;
  double durationSec = 0.0;
  int audioSampleRate = 0;
};

ProbeResult probeWith(const std::string &path) {
  const std::string cmd =
      ffprobeBin() + " -v error -show_entries "
      "stream=codec_type,codec_name,width,height,sample_rate:format=duration "
      "-of csv " + path + " 2>&1";
  FILE *pipe = ::popen(cmd.c_str(), "r");
  REQUIRE(pipe != nullptr);
  ProbeResult result;
  char line[512];
  while (fgets(line, sizeof(line), pipe) != nullptr) {
    std::string entry = line;
    // csv: stream,codec_type,codec_name[,width,height|,sample_rate]
    // format lines: format,duration
    std::vector<std::string> fields;
    std::size_t pos = 0;
    while (pos <= entry.size()) {
      auto comma = entry.find(',', pos);
      fields.push_back(entry.substr(pos, comma == std::string::npos
                                             ? std::string::npos
                                             : comma - pos));
      if (comma == std::string::npos) {
        break;
      }
      pos = comma + 1;
    }
    for (auto &f : fields) {
      while (!f.empty() && (f.back() == '\n' || f.back() == '\r')) {
        f.pop_back();
      }
    }
    if (fields.size() >= 3 && fields[0] == "stream") {
      // csv order: stream,codec_name,codec_type[,width,height|,sample_rate]
      if (fields[2] == "video" && fields[1] != "unknown") {
        result.videoCodec = fields[1];
        if (fields.size() >= 5) {
          result.width = std::stoi(fields[3]);
          result.height = std::stoi(fields[4]);
        }
      } else if (fields[2] == "audio" && fields[1] != "unknown") {
        result.audioCodec = fields[1];
        if (fields.size() >= 4) {
          try {
            result.audioSampleRate = std::stoi(fields[3]);
          } catch (...) {
          }
        }
      }
    } else if (fields.size() >= 2 && fields[0] == "format") {
      try {
        result.durationSec = std::stod(fields[1]);
      } catch (...) {
      }
    }
  }
  const int rc = ::pclose(pipe);
  REQUIRE(rc == 0);
  return result;
}

ConversionRequest transcodeRequest(const std::string &sourcePath,
                                   Container outContainer,
                                   VideoCodec videoCodec,
                                   AudioCodec audioCodec,
                                   std::optional<std::int32_t> height = {},
                                   std::optional<std::int64_t> videoBitrate = {},
                                   std::optional<std::int64_t> audioBitrate = {}) {
  ConversionRequest request;
  request.source.uri = sourcePath;
  request.source.displayName = "fixture.mp4";
  request.outputContainer = outContainer;
  request.goal.kind = GoalKind::Custom;
  request.video.codec = videoCodec;
  request.video.resolutionHeight = height;
  request.video.bitRate = videoBitrate;
  request.audio.codec = audioCodec;
  request.audio.bitRate = audioBitrate;
  return request;
}

class EventWaiter final : public IJobEventSink {
public:
  void onJobEvent(const JobEvent &event) override {
    std::lock_guard<std::mutex> lock(mutex_);
    last = event;
    if (isTerminal(event.state)) {
      done.notify_all();
    }
  }
  std::optional<JobEvent> waitForTerminal(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    done.wait_for(lock, timeout, [this] { return last && isTerminal(last->state); });
    return last;
  }
  static bool isTerminal(JobState state) {
    return state == JobState::Completed || state == JobState::Failed;
  }

private:
  std::mutex mutex_;
  std::condition_variable done;
  std::optional<JobEvent> last;
};

} // namespace

TEST_CASE("capability report derives from the real compiled build") {
  TestEnv env;
  const BackendCapabilities caps = env.backend->capabilities();
  CHECK(caps.backend == BackendId::Ffmpeg);
  CHECK(caps.canInspect);
  CHECK_FALSE(caps.implementation.empty());
  // The pinned build MUST provide these (validated by the builder too).
  bool hasH264Encode = false, hasAacEncode = false, hasVp9Encode = false,
       hasMp4Write = false;
  for (const auto &v : caps.video) {
    if (v.codec == VideoCodec::H264 && v.support.encode) {
      hasH264Encode = true;
    }
    if (v.codec == VideoCodec::VP9 && v.support.encode) {
      hasVp9Encode = true;
    }
  }
  for (const auto &a : caps.audio) {
    if (a.codec == AudioCodec::Aac && a.support.encode) {
      hasAacEncode = true;
    }
  }
  for (const Container c : caps.writableContainers) {
    if (c == Container::Mp4) {
      hasMp4Write = true;
    }
  }
  CHECK(hasH264Encode);
  CHECK(hasAacEncode);
  CHECK(hasVp9Encode);
  CHECK(hasMp4Write);
}

TEST_CASE("inspection normalizes a real MP4 through libavformat") {
  TestEnv env;
  const std::string fixture =
      generateFixture(env.dir / "in.mp4",
                      "testsrc2=size=320x240:rate=12",
                      "sine=frequency=440:sample_rate=44100", 2.0);
  const Result<MediaInfo> info =
      env.engine->inspect(MediaSource{fixture, "in.mp4"});
  REQUIRE(info.ok());
  CHECK(info.value().container == Container::Mp4);
  REQUIRE_FALSE(info.value().video.empty());
  CHECK(info.value().video.front().codec == VideoCodec::H264);
  CHECK(info.value().video.front().width == 320);
  CHECK(info.value().video.front().height == 240);
  CHECK(info.value().video.front().frameRate == doctest::Approx(12.0).epsilon(0.05));
  REQUIRE_FALSE(info.value().audio.empty());
  CHECK(info.value().audio.front().codec == AudioCodec::Aac);
  CHECK(info.value().audio.front().sampleRate == 44100);
  CHECK(info.value().durationSec == doctest::Approx(2.0).epsilon(0.15));

  // Independent cross-check with ffprobe.
  const ProbeResult probe = probeWith(fixture);
  CHECK(probe.videoCodec == "h264");
  CHECK(probe.width == 320);
  CHECK(probe.height == 240);
  CHECK(probe.audioCodec == "aac");
}

TEST_CASE("malformed media fails inspection honestly") {
  TestEnv env;
  const std::string garbage = (env.dir / "garbage.mp4").string();
  {
    std::ofstream out(garbage, std::ios::binary);
    for (int i = 0; i < 4096; ++i) {
      out.put('\xDE');
    }
  }
  const Result<MediaInfo> info =
      env.engine->inspect(MediaSource{garbage, "garbage.mp4"});
  CHECK_FALSE(info.ok());
  CHECK(info.error().code == ErrorCode::InspectionFailed);
}

TEST_CASE("REAL transcode: H.264/AAC MP4 -> VP9/Opus WebM with validation") {
  TestEnv env;
  const std::string fixture =
      generateFixture(env.dir / "in.mp4",
                      "testsrc2=size=320x240:rate=12",
                      "sine=frequency=440:sample_rate=44100", 2.0);

  EventWaiter waiter;
  env.engine->events().subscribe(&waiter);
  const Result<std::string> jobId = env.engine->startJob(
      transcodeRequest(fixture, Container::WebM, VideoCodec::VP9,
                       AudioCodec::Opus));
  REQUIRE(jobId.ok());

  const auto terminal = waiter.waitForTerminal(std::chrono::seconds(120));
  REQUIRE(terminal.has_value());
  INFO("failure error: " << terminal->error);
  REQUIRE(terminal->state == JobState::Completed);

  // The engine's own validator passed (completion implies it). Now the
  // INDEPENDENT ffprobe cross-check.
  const auto jobs = env.engine->listJobs();
  REQUIRE(jobs.size() == 1);
  const JobSnapshot &job = jobs.front();
  const std::string outputPath = job.outputPath;
  REQUIRE(std::filesystem::exists(outputPath));

  const ProbeResult probe = probeWith(outputPath);
  CHECK(probe.videoCodec == "vp9");
  CHECK(probe.audioCodec == "opus");
  CHECK(probe.width == 320);
  CHECK(probe.height == 240);
  CHECK(probe.durationSec == doctest::Approx(2.0).epsilon(0.25));
  // Output must be a real, non-trivial file.
  CHECK(std::filesystem::file_size(outputPath) > 8192);
}

TEST_CASE("REAL transcode: resolution downscale to 144p") {
  TestEnv env;
  const std::string fixture =
      generateFixture(env.dir / "in.mp4",
                      "testsrc2=size=320x240:rate=10",
                      "sine=frequency=440:sample_rate=44100", 2.0);
  EventWaiter waiter;
  env.engine->events().subscribe(&waiter);
  const Result<std::string> jobId = env.engine->startJob(
      transcodeRequest(fixture, Container::Mp4, VideoCodec::H264,
                       AudioCodec::Aac, /*height=*/144));
  REQUIRE(jobId.ok());

  const auto terminal = waiter.waitForTerminal(std::chrono::seconds(120));
  REQUIRE(terminal.has_value());
  INFO("failure error: " << terminal->error);
  REQUIRE(terminal->state == JobState::Completed);

  const auto jobs = env.engine->listJobs();
  REQUIRE(jobs.size() == 1);
  const std::string outputPath = jobs.front().outputPath;
  const ProbeResult probe = probeWith(outputPath);
  CHECK(probe.videoCodec == "h264");
  CHECK(probe.height == 144); // never upscaled, exactly as requested
  CHECK(probe.audioCodec == "aac");
}

TEST_CASE("REAL remux: same codecs into MKV avoids re-encoding") {
  TestEnv env;
  const std::string fixture =
      generateFixture(env.dir / "in.mp4",
                      "testsrc2=size=320x240:rate=12",
                      "sine=frequency=440:sample_rate=44100", 2.0);
  const auto before = std::filesystem::file_size(fixture);

  EventWaiter waiter;
  env.engine->events().subscribe(&waiter);
  const Result<std::string> jobId = env.engine->startJob(
      transcodeRequest(fixture, Container::Mkv, VideoCodec::H264,
                       AudioCodec::Aac));
  REQUIRE(jobId.ok());

  const auto terminal = waiter.waitForTerminal(std::chrono::seconds(60));
  REQUIRE(terminal.has_value());
  INFO("failure error: " << terminal->error);
  REQUIRE(terminal->state == JobState::Completed);

  const auto jobs = env.engine->listJobs();
  REQUIRE(jobs.size() == 1);
  const std::string outputPath = jobs.front().outputPath;
  const ProbeResult probe = probeWith(outputPath);
  CHECK(probe.videoCodec == "h264"); // copied, not re-encoded
  CHECK(probe.audioCodec == "aac");
  // A remux is byte-comparable to the source payload.
  const auto after = std::filesystem::file_size(outputPath);
  CHECK(double(after) > double(before) * 0.8);
  CHECK(double(after) < double(before) * 1.2);
}

TEST_CASE("invalid codec/container combinations never reach execution") {
  TestEnv env;
  const std::string fixture =
      generateFixture(env.dir / "in.mp4",
                      "testsrc2=size=320x240:rate=12",
                      "sine=frequency=440:sample_rate=44100", 1.0);
  // H.264 cannot be written into WebM.
  const Result<PlanResult> plan = env.engine->plan(
      MediaSource{fixture, "in.mp4"},
      transcodeRequest(fixture, Container::WebM, VideoCodec::H264,
                       AudioCodec::Opus));
  REQUIRE(plan.ok());
  CHECK_FALSE(plan.value().feasible);
}

TEST_CASE("fit-under-size jobs receive a real bitrate budget") {
  TestEnv env;
  const std::string fixture =
      generateFixture(env.dir / "in.mp4",
                      "testsrc2=size=320x240:rate=12",
                      "sine=frequency=440:sample_rate=44100", 2.0);
  ConversionRequest request =
      transcodeRequest(fixture, Container::Mp4, VideoCodec::H264,
                       AudioCodec::Aac, {}, 500'000, 64'000);
  request.goal.kind = GoalKind::FitUnderSize;
  request.goal.targetSizeBytes = 250'000;

  const Result<PlanResult> plan =
      env.engine->plan(MediaSource{fixture, "in.mp4"}, request);
  REQUIRE(plan.ok());
  REQUIRE(plan.value().feasible);
  REQUIRE(plan.value().best.has_value());
  CHECK(plan.value().best->estimatedOutputBytes.has_value());
}
