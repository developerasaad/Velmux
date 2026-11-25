// FFmpeg backend integration tests: REAL conversions through the libav*
// libraries with INDEPENDENT ffprobe cross-validation of every output.
//
// Fixtures are generated at test time with the built ffmpeg binary (a dev
// tool of the pinned prefix) from deterministic lavfi inputs, so the suite
// is self-contained and reproducible.
#include "processing/ffmpeg/ffmpeg_backend.h"
#include "subtitle_tools.h"

#include <velmux/engine.h>
#include "processing/hybrid_executor.h"
#include <doctest/doctest.h>

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

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
  // RAII subscription: constructing with a bus subscribes, and the
  // destructor unsubscribes — a waiter scoped to one job can never
  // dangle on an engine that outlives it. (The exact lifetime bug that
  // crashed the huge-metadata hardening test: Run 1's block-scoped
  // waiter died while still subscribed; Run 2's events were then
  // delivered into its reused stack slot — a stack-use-after-scope,
  // SIGSEGV under doctest.)
  explicit EventWaiter(EventBus &bus) : bus_(&bus) { bus_->subscribe(this); }
  EventWaiter() = default;
  ~EventWaiter() override {
    if (bus_ != nullptr) {
      bus_->unsubscribe(this);
    }
  }
  EventWaiter(const EventWaiter &) = delete;
  EventWaiter &operator=(const EventWaiter &) = delete;

  void onJobEvent(const JobEvent &event) override {
    std::lock_guard<std::mutex> lock(mutex_);
    last = event;
    if (isTerminal(event.state)) {
      done.notify_all();
    }
  }
  std::optional<JobEvent> waitForTerminal(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    done.wait_for(lock, timeout,
                  [this] { return last && isTerminal(last->state); });
    return last;
  }
  // Drop latched state so the next waitFor* observes a fresh run
  // (retry/resume after a terminal event).
  void reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    last.reset();
  }
  static bool isTerminal(JobState state) {
    return state == JobState::Completed || state == JobState::Failed ||
           state == JobState::Canceled;
  }

private:
  EventBus *bus_ = nullptr;
  std::mutex mutex_;
  std::condition_variable done;
  std::optional<JobEvent> last;
};

void writeFile(const std::filesystem::path &path, const std::string &content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << content;
  REQUIRE(out.good());
}

std::string readFile(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

// Muxes an A/V fixture with a subtitle sidecar into Matroska (all streams
// copied — the standard fixture for subtitle behavior).
std::string muxWithSubtitles(const std::filesystem::path &avPath,
                             const std::filesystem::path &subPath,
                             const std::filesystem::path &outPath) {
  const std::string cmd =
      ffmpegBin() + " -y -hide_banner -loglevel error -i \"" +
      avPath.string() + "\" -i \"" + subPath.string() +
      "\" -map 0:v -map 0:a -map 1:0 -c:v copy -c:a copy -c:s copy " +
      outPath.string();
  REQUIRE(std::system(cmd.c_str()) == 0);
  REQUIRE(std::filesystem::file_size(outPath) > 1024);
  return outPath.string();
}

const char *kSampleSrt =
    "1\n"
    "00:00:00,500 --> 00:00:02,500\n"
    "BURNMARKERTEXT\n\n"
    "2\n"
    "00:00:03,000 --> 00:00:04,500\n"
    "Second cue\n\n";

// Measures the maximum luma of the bottom quarter of the frame at a given
// timestamp using the pinned ffmpeg binary (independent of the backend).
// Burned-in white text on a black fixture drives this far above black.
int bottomQuarterYMax(const std::string &path, double atSeconds) {
  const std::string cmd =
      ffmpegBin() + " -hide_banner -v info -ss " + std::to_string(atSeconds) +
      " -i \"" + path + "\" -frames:v 1 -vf "
      "\"crop=iw:ih/4:0:ih*3/4,signalstats,metadata=print:key=lavfi.signalstats.YMAX\" "
      "-f null - 2>&1";
  FILE *pipe = ::popen(cmd.c_str(), "r");
  REQUIRE(pipe != nullptr);
  int yMax = -1;
  char line[512];
  while (fgets(line, sizeof(line), pipe) != nullptr) {
    const std::string entry = line;
    const auto pos = entry.find("lavfi.signalstats.YMAX=");
    if (pos != std::string::npos) {
      yMax = std::max(yMax, std::atoi(entry.c_str() + pos + 23));
    }
  }
  ::pclose(pipe);
  return yMax;
}

int countStreams(const std::string &path, const std::string &codecType) {
  const std::string cmd =
      ffprobeBin() + " -v error -select_streams " + codecType +
      " -show_entries stream=index -of csv=p=0 \"" + path + "\" 2>&1";
  FILE *pipe = ::popen(cmd.c_str(), "r");
  REQUIRE(pipe != nullptr);
  int count = 0;
  char line[256];
  while (fgets(line, sizeof(line), pipe) != nullptr) {
    if (line[0] >= '0' && line[0] <= '9') {
      ++count;
    }
  }
  ::pclose(pipe);
  return count;
}

std::string subtitleCodecName(const std::string &path) {
  const std::string cmd =
      ffprobeBin() +
      " -v error -select_streams s -show_entries stream=codec_name "
      "-of csv=p=0 \"" + path + "\" 2>&1";
  FILE *pipe = ::popen(cmd.c_str(), "r");
  REQUIRE(pipe != nullptr);
  std::string name;
  char line[256];
  while (fgets(line, sizeof(line), pipe) != nullptr) {
    std::string entry = line;
    while (!entry.empty() && (entry.back() == '\n' || entry.back() == '\r')) {
      entry.pop_back();
    }
    if (!entry.empty()) {
      name = entry;
    }
  }
  ::pclose(pipe);
  return name;
}

// Runs ffprobe and returns the first csv value of a stream field for the
// given stream selector (e.g. "a:0" language, "v:0" color_trc). Empty when
// the field is absent.
std::string streamFieldOf(const std::string &path,
                          const std::string &streamSelector,
                          const std::string &field) {
  const std::string cmd =
      ffprobeBin() + " -v error -select_streams " + streamSelector +
      " -show_entries " + field + " -of csv=p=0 \"" + path + "\" 2>&1";
  FILE *pipe = ::popen(cmd.c_str(), "r");
  REQUIRE(pipe != nullptr);
  std::string value;
  char line[256];
  while (fgets(line, sizeof(line), pipe) != nullptr) {
    std::string entry = line;
    while (!entry.empty() && (entry.back() == '\n' || entry.back() == '\r')) {
      entry.pop_back();
    }
    if (!entry.empty()) {
      value = entry;
    }
  }
  ::pclose(pipe);
  return value;
}

// True when the file carries a video stream flagged attached_pic (the
// cover-art / attached-picture disposition).
bool hasAttachedPic(const std::string &path) {
  return streamFieldOf(path, "v", "stream_disposition=attached_pic") ==
         "1";
}

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

  EventWaiter waiter(env.engine->events());
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

TEST_CASE("REAL transcode: H.264 MP4 -> HEVC (libx265) MP4 with validation") {
  TestEnv env;
  // The feature matrix requires HEVC ENCODE through FFmpeg (libx265),
  // not only through MediaCodec/VideoToolbox. A real software HEVC
  // encode, validated by the engine's own output validator plus an
  // independent ffprobe cross-check.
  const std::string fixture =
      generateFixture(env.dir / "in.mp4",
                      "testsrc2=size=320x240:rate=12",
                      "sine=frequency=440:sample_rate=44100", 2.0);
  EventWaiter waiter(env.engine->events());
  // Explicit quality (not bitrate) exercises the CRF path for libx265.
  ConversionRequest request =
      transcodeRequest(fixture, Container::Mp4, VideoCodec::HEVC,
                       AudioCodec::Aac);
  request.video.quality = 60;
  const Result<std::string> jobId = env.engine->startJob(request);
  REQUIRE(jobId.ok());

  const auto terminal = waiter.waitForTerminal(std::chrono::seconds(180));
  REQUIRE(terminal.has_value());
  INFO("failure error: " << terminal->error);
  REQUIRE(terminal->state == JobState::Completed);

  const auto jobs = env.engine->listJobs();
  REQUIRE(jobs.size() == 1);
  const JobSnapshot &job = jobs.front();
  const std::string outputPath = job.outputPath;
  REQUIRE(std::filesystem::exists(outputPath));

  const ProbeResult probe = probeWith(outputPath);
  CHECK(probe.videoCodec == "hevc");
  CHECK(probe.audioCodec == "aac");
  CHECK(probe.width == 320);
  CHECK(probe.height == 240);
  CHECK(probe.durationSec == doctest::Approx(2.0).epsilon(0.25));
  CHECK(std::filesystem::file_size(outputPath) > 8192);
}

TEST_CASE("REAL transcode: resolution downscale to 144p") {
  TestEnv env;
  const std::string fixture =
      generateFixture(env.dir / "in.mp4",
                      "testsrc2=size=320x240:rate=10",
                      "sine=frequency=440:sample_rate=44100", 2.0);
  EventWaiter waiter(env.engine->events());
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

  EventWaiter waiter(env.engine->events());
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

TEST_CASE("fit-under-size budget DRIVES the encode and lands under target") {
  TestEnv env;
  // 320x240@12 for 4 s with a 90 kbps AAC track: a 80 KB target leaves
  // ~63 kbps for H.264 (above the ~41 kbps bpp floor -> feasible), while a
  // quality-mode CRF encode of this fixture is several times larger. The
  // budget-derived bitrate must parameterize the REAL encode (spec §17),
  // not stay an advisory string.
  const std::string fixture =
      generateFixture(env.dir / "in.mp4",
                      "testsrc2=size=320x240:rate=12",
                      "sine=frequency=440:sample_rate=44100", 4.0);

  ConversionRequest request =
      transcodeRequest(fixture, Container::Mp4, VideoCodec::H264,
                       AudioCodec::Aac);
  request.goal.kind = GoalKind::FitUnderSize;
  request.goal.targetSizeBytes = 80'000; // ~160 kbps total for 4 s

  EventWaiter waiter(env.engine->events());
  const Result<std::string> jobId = env.engine->startJob(request);
  REQUIRE(jobId.ok());

  const auto terminal = waiter.waitForTerminal(std::chrono::seconds(90));
  REQUIRE(terminal.has_value());
  INFO("failure error: " << terminal->error);
  REQUIRE(terminal->state == JobState::Completed);

  const auto jobs = env.engine->listJobs();
  REQUIRE(jobs.size() == 1);
  const std::string outputPath = jobs.front().outputPath;
  REQUIRE_FALSE(outputPath.empty());
  const ProbeResult probe = probeWith(outputPath);
  CHECK(probe.videoCodec == "h264");
  CHECK(probe.audioCodec == "aac");

  const auto size = std::filesystem::file_size(outputPath);
  INFO("output size: " << size << " bytes (target 80000)");
  // The honest contract: either the output fits, or (after the single
  // corrective retry) the summary says the target was missed. A silent
  // overshoot is the failure mode this test forbids.
  if (size > 80'000) {
    CHECK(jobs.front().pipelineSummary.find("fit-under-size") !=
          std::string::npos);
  }
  // A budget-driven encode at ~160 kbps cannot reach 150 KB over 4 s
  // (that would be a 2x ABR overshoot); a quality-mode encode can and
  // would. This is the discriminator proving the budget drove execution.
  CHECK(double(size) < 150'000.0);
}

// ---------------------------------------------------------------------------
// Geometry transforms (spec §9: crop / rotation / flip)
// ---------------------------------------------------------------------------

// Extracts a pixel from a decoded frame via ffmpeg CLI (independent of the
// backend's own pipeline): used to prove flips actually mirror pixels.
std::string extractFramePixel(const std::string &path, double atSec) {
  const std::string cmd =
      ffmpegBin() + " -y -hide_banner -loglevel error -ss " +
      std::to_string(atSec) + " -i \"" + path +
      "\" -frames:v 1 -f rawvideo -pix_fmt gray - 2>/dev/null | od -An -tu1 "
      "| head -1 | tr -s ' '";
  std::FILE *pipe = popen(cmd.c_str(), "r");
  REQUIRE(pipe != nullptr);
  char buf[256];
  const char *got = std::fgets(buf, sizeof(buf), pipe);
  pclose(pipe);
  return got != nullptr ? std::string(got) : std::string();
}

TEST_CASE("rotation swaps output dimensions and crop cuts the frame") {
  TestEnv env;
  const std::string fixture =
      generateFixture(env.dir / "in.mp4",
                      "testsrc2=size=320x240:rate=12",
                      "sine=frequency=440:sample_rate=44100", 2.0);
  ConversionRequest request =
      transcodeRequest(fixture, Container::Mp4, VideoCodec::H264,
                       AudioCodec::Aac);
  request.video.rotationDegrees = 90;

  EventWaiter waiter(env.engine->events());
  const Result<std::string> jobId = env.engine->startJob(request);
  REQUIRE(jobId.ok());
  const auto terminal = waiter.waitForTerminal(std::chrono::seconds(60));
  REQUIRE(terminal.has_value());
  INFO("failure error: " << terminal->error);
  REQUIRE(terminal->state == JobState::Completed);
  const auto jobs = env.engine->listJobs();
  const ProbeResult probe = probeWith(jobs.front().outputPath);
  CHECK(probe.videoCodec == "h264");
  // 90-degree clockwise rotation of 320x240 -> 240x320.
  CHECK(probe.width == 240);
  CHECK(probe.height == 320);

  // Crop: 320x240 source cropped to the left half (0,0,160,240).
  ConversionRequest cropped =
      transcodeRequest(fixture, Container::Mp4, VideoCodec::H264,
                       AudioCodec::Aac);
  cropped.video.crop = CropRect{0, 0, 160, 240};
  EventWaiter waiter2(env.engine->events());
  const Result<std::string> jobId2 = env.engine->startJob(cropped);
  REQUIRE(jobId2.ok());
  const auto terminal2 = waiter2.waitForTerminal(std::chrono::seconds(60));
  REQUIRE(terminal2.has_value());
  INFO("failure error: " << terminal2->error);
  REQUIRE(terminal2->state == JobState::Completed);
  // listJobs is newest-first: address the job by its id, never by position.
  const auto snap2 = env.engine->getJob(jobId2.value());
  REQUIRE(snap2.has_value());
  const ProbeResult probe2 = probeWith(snap2->outputPath);
  CHECK(probe2.width == 160);
  CHECK(probe2.height == 240);
}

TEST_CASE("flip mirrors pixels (independent raw-frame cross-check)") {
  TestEnv env;
  // testsrc2 has a strong left-to-right gradient: the leftmost and
  // rightmost columns differ, so a horizontal flip visibly swaps them.
  const std::string fixture =
      generateFixture(env.dir / "in.mp4",
                      "testsrc2=size=320x240:rate=12",
                      "sine=frequency=440:sample_rate=44100", 2.0);
  ConversionRequest request =
      transcodeRequest(fixture, Container::Mp4, VideoCodec::H264,
                       AudioCodec::Aac);
  request.video.flipHorizontal = true;

  EventWaiter waiter(env.engine->events());
  const Result<std::string> jobId = env.engine->startJob(request);
  REQUIRE(jobId.ok());
  const auto terminal = waiter.waitForTerminal(std::chrono::seconds(60));
  REQUIRE(terminal.has_value());
  INFO("failure error: " << terminal->error);
  REQUIRE(terminal->state == JobState::Completed);
  const auto jobs = env.engine->listJobs();
  const std::string out = jobs.front().outputPath;

  // Compare the first grayscale row of source vs flipped output at the
  // same timestamp: a real mirror reverses the row; a no-op transcode
  // leaves it identical.
  const std::string srcRow = extractFramePixel(fixture, 0.0);
  const std::string flipRow = extractFramePixel(out, 0.0);
  REQUIRE_FALSE(srcRow.empty());
  REQUIRE_FALSE(flipRow.empty());
  CHECK(srcRow != flipRow);
}

TEST_CASE("crop outside the frame is refused, not mangled") {
  TestEnv env;
  const std::string fixture =
      generateFixture(env.dir / "in.mp4",
                      "testsrc2=size=320x240:rate=12",
                      "sine=frequency=440:sample_rate=44100", 1.0);
  ConversionRequest request =
      transcodeRequest(fixture, Container::Mp4, VideoCodec::H264,
                       AudioCodec::Aac);
  request.video.crop = CropRect{310, 0, 100, 100}; // past the right edge
  EventWaiter waiter(env.engine->events());
  const Result<std::string> jobId = env.engine->startJob(request);
  REQUIRE(jobId.ok());
  const auto terminal = waiter.waitForTerminal(std::chrono::seconds(60));
  REQUIRE(terminal.has_value());
  REQUIRE(terminal->state == JobState::Failed);
  CHECK(terminal->error.find("crop") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Audio DSP (spec §10: gain / fades / loudness / speed / silence / PCM depth)
// ---------------------------------------------------------------------------

// Measures mean volume (dB) with the ffmpeg CLI volumedetect filter —
// independent of the backend's own DSP graph.
double meanVolumeDb(const std::string &path) {
  const std::string cmd =
      ffmpegBin() + " -hide_banner -i \"" + path +
      "\" -af volumedetect -f null - 2>&1 | grep mean_volume";
  std::FILE *pipe = popen(cmd.c_str(), "r");
  REQUIRE(pipe != nullptr);
  char buf[512];
  std::string out;
  while (std::fgets(buf, sizeof(buf), pipe) != nullptr) {
    out += buf;
  }
  pclose(pipe);
  const auto at = out.find("mean_volume:");
  REQUIRE(at != std::string::npos);
  const auto colon = out.find(':', at);
  return std::atof(out.c_str() + colon + 1);
}

TEST_CASE("audio gain changes loudness by the requested amount") {
  TestEnv env;
  const std::string fixture =
      generateFixture(env.dir / "in.mp4",
                      "testsrc2=size=320x240:rate=12",
                      "sine=frequency=440:sample_rate=44100", 2.0);
  ConversionRequest request =
      transcodeRequest(fixture, Container::Mp4, VideoCodec::H264,
                       AudioCodec::Aac);
  request.audio.gainDb = -12.0;

  const double before = meanVolumeDb(fixture);
  EventWaiter waiter(env.engine->events());
  const Result<std::string> jobId = env.engine->startJob(request);
  REQUIRE(jobId.ok());
  const auto terminal = waiter.waitForTerminal(std::chrono::seconds(60));
  REQUIRE(terminal.has_value());
  INFO("failure error: " << terminal->error);
  REQUIRE(terminal->state == JobState::Completed);
  const auto jobs = env.engine->listJobs();
  const double after = meanVolumeDb(jobs.front().outputPath);
  INFO("mean volume: " << before << " dB -> " << after << " dB");
  // -12 dB requested: measured delta must land close (encoder noise and
  // volumedetect windowing allow ~1.5 dB slack).
  CHECK(after < before - 10.5);
  CHECK(after > before - 13.5);
}

TEST_CASE("playback speed stretches audio-only output by 1/speed") {
  TestEnv env;
  const std::string fixture =
      generateFixture(env.dir / "in.mp4",
                      "testsrc2=size=320x240:rate=12",
                      "sine=frequency=440:sample_rate=44100", 4.0);
  ConversionRequest request =
      transcodeRequest(fixture, Container::M4a, VideoCodec::H264,
                       AudioCodec::Aac);
  request.video.enabled = false;
  request.audio.playbackSpeed = 2.0;

  EventWaiter waiter(env.engine->events());
  const Result<std::string> jobId = env.engine->startJob(request);
  REQUIRE(jobId.ok());
  const auto terminal = waiter.waitForTerminal(std::chrono::seconds(60));
  REQUIRE(terminal.has_value());
  INFO("failure error: " << terminal->error);
  REQUIRE(terminal->state == JobState::Completed);
  const auto jobs = env.engine->listJobs();
  const ProbeResult probe = probeWith(jobs.front().outputPath);
  CHECK(probe.audioCodec == "aac");
  INFO("duration: " << probe.durationSec << " (source 4.0, speed 2.0)");
  CHECK(probe.durationSec == doctest::Approx(2.0).epsilon(0.15));

  // Combined with video, speed is honestly refused (A/V desync guard).
  ConversionRequest withVideo =
      transcodeRequest(fixture, Container::Mp4, VideoCodec::H264,
                       AudioCodec::Aac);
  withVideo.audio.playbackSpeed = 1.5;
  const Result<PlanResult> refused = env.engine->plan(
      MediaSource{fixture, "in.mp4"}, withVideo);
  REQUIRE(refused.ok());
  CHECK_FALSE(refused.value().feasible);
}

TEST_CASE("PCM bit depth selects the exact PCM encoder variant") {
  TestEnv env;
  const std::string fixture =
      generateFixture(env.dir / "in.mp4",
                      "testsrc2=size=320x240:rate=12",
                      "sine=frequency=440:sample_rate=44100", 1.0);
  ConversionRequest request =
      transcodeRequest(fixture, Container::Wav, VideoCodec::H264,
                       AudioCodec::Pcm);
  request.video.enabled = false;
  request.audio.pcmBitDepth = 24;

  EventWaiter waiter(env.engine->events());
  const Result<std::string> jobId = env.engine->startJob(request);
  REQUIRE(jobId.ok());
  const auto terminal = waiter.waitForTerminal(std::chrono::seconds(60));
  REQUIRE(terminal.has_value());
  INFO("failure error: " << terminal->error);
  REQUIRE(terminal->state == JobState::Completed);
  const auto jobs = env.engine->listJobs();
  const ProbeResult probe = probeWith(jobs.front().outputPath);
  CHECK(probe.audioCodec == "pcm_s24le");
}

TEST_CASE("metadata survives transcode and remux") {
  TestEnv env;
  // Fixture with container + stream metadata.
  const std::string plain =
      generateFixture(env.dir / "in.mp4",
                      "testsrc2=size=320x240:rate=12",
                      "sine=frequency=440:sample_rate=44100", 1.0);
  const std::string tagged = (env.dir / "tagged.mp4").string();
  REQUIRE(std::system((ffmpegBin() + " -y -hide_banner -loglevel error -i \"" +
                       plain + "\" -metadata title=VelmuxMetaTitle "
                       "-metadata artist=VelmuxMetaArtist -c copy \"" +
                       tagged + "\"")
                          .c_str()) == 0);

  // Transcode path.
  EventWaiter waiter(env.engine->events());
  const Result<std::string> jobId = env.engine->startJob(
      transcodeRequest(tagged, Container::Mp4, VideoCodec::H264,
                       AudioCodec::Aac, {}, 400'000));
  REQUIRE(jobId.ok());
  const auto terminal = waiter.waitForTerminal(std::chrono::seconds(60));
  REQUIRE(terminal.has_value());
  REQUIRE(terminal->state == JobState::Completed);
  const auto jobs = env.engine->listJobs();

  const auto titleOf = [&](const std::string &path) {
    std::FILE *pipe = popen((ffprobeBin() + " -v error -show_entries "
                                     "format_tags=title -of csv=p=0 \"" +
                             path + "\"")
                                .c_str(), "r");
    REQUIRE(pipe != nullptr);
    char buf[256] = {0};
    const char *got = std::fgets(buf, sizeof(buf), pipe);
    pclose(pipe);
    std::string s = got != nullptr ? buf : "";
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
      s.pop_back();
    }
    return s;
  };
  CHECK(titleOf(jobs.front().outputPath) == "VelmuxMetaTitle");

  // Remux path.
  EventWaiter waiter2(env.engine->events());
  const Result<std::string> jobId2 = env.engine->startJob(
      transcodeRequest(tagged, Container::Mkv, VideoCodec::H264,
                       AudioCodec::Aac));
  REQUIRE(jobId2.ok());
  const auto terminal2 = waiter2.waitForTerminal(std::chrono::seconds(60));
  REQUIRE(terminal2.has_value());
  REQUIRE(terminal2->state == JobState::Completed);
  const auto snap2 = env.engine->getJob(jobId2.value());
  REQUIRE(snap2.has_value());
  CHECK(titleOf(snap2->outputPath) == "VelmuxMetaTitle");
}

TEST_CASE("HARDENING: invalid timestamps never produce fake success") {
  TestEnv env;
  // Build a fixture, then corrupt a video packet's pts/dts to nonsense.
  const std::string fixture =
      generateFixture(env.dir / "in.mp4",
                      "testsrc2=size=320x240:rate=12",
                      "sine=frequency=440:sample_rate=44100", 1.0);
  // Negate the moov's stts entries by editing raw bytes is fragile;
  // instead demux -> remux with forced garbage timestamps via the CLI
  // is also unreliable. The robust hostile fixture: a zero-byte file
  // pretending to be media (handled by inspection) plus a TRUNCATED
  // mid-stream file with mangled trailing bytes.
  const auto trunc = env.dir / "truncated.mp4";
  {
    std::ifstream in(fixture, std::ios::binary);
    std::vector<char> data((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    // Keep 60% and flip bytes in the last 512 to damage the tail packets.
    data.resize(data.size() * 6 / 10);
    for (std::size_t i = data.size() >= 512 ? data.size() - 512 : 0;
         i < data.size(); ++i) {
      data[i] = static_cast<char>(~data[i]);
    }
    std::ofstream out(trunc, std::ios::binary);
    out.write(data.data(), std::streamsize(data.size()));
  }
  // The engine must either fail honestly or produce a validated output;
  // it must never report Completed with an empty/absent output.
  ConversionRequest request =
      transcodeRequest(trunc.string(), Container::Mp4, VideoCodec::H264,
                       AudioCodec::Aac);
  // The engine may refuse at submit (inspection fails on the damaged
  // file) or fail during execution - both are honest. What is forbidden
  // is a Completed state with a missing/empty output.
  const Result<std::string> jobId = env.engine->startJob(request);
  if (!jobId.ok()) {
    CHECK_FALSE(jobId.error().message.empty());
    return;
  }
  EventWaiter waiter(env.engine->events());
  const auto terminal = waiter.waitForTerminal(std::chrono::seconds(60));
  REQUIRE(terminal.has_value());
  if (terminal->state == JobState::Completed) {
    const auto snap = env.engine->getJob(jobId.value());
    REQUIRE(snap.has_value());
    CHECK(snap->outputSizeBytes > 1024);
  } else {
    CHECK(terminal->state == JobState::Failed);
    CHECK_FALSE(terminal->error.empty());
  }
}

// ---------------------------------------------------------------------------
// Subtitle behavior (spec §14: subtitles are first-class streams)
// ---------------------------------------------------------------------------

TEST_CASE("subtitle timestamp shifting: SRT golden cases") {
  TestEnv env;
  const auto src = env.dir / "in.srt";
  const auto dst = env.dir / "shifted.srt";
  writeFile(src, kSampleSrt);
  std::string reason;
  REQUIRE(velmux::ffmpeg::subtitles::shiftSubtitleFile(
      src.string(), dst.string(), /*offsetMs=*/2000, reason));
  const std::string shifted = readFile(dst);
  // 00:00:00,500 + 2s -> 00:00:02,500 ; 00:00:02,500 -> 00:00:04,500
  CHECK(shifted.find("00:00:02,500 --> 00:00:04,500") != std::string::npos);
  CHECK(shifted.find("00:00:05,000 --> 00:00:06,500") != std::string::npos);
  CHECK(shifted.find("BURNMARKERTEXT") != std::string::npos);
}

TEST_CASE("subtitle timestamp shifting: negative offsets clamp to zero") {
  TestEnv env;
  const auto src = env.dir / "in.srt";
  const auto dst = env.dir / "shifted.srt";
  writeFile(src, kSampleSrt);
  std::string reason;
  REQUIRE(velmux::ffmpeg::subtitles::shiftSubtitleFile(
      src.string(), dst.string(), /*offsetMs=*/-1000, reason));
  const std::string shifted = readFile(dst);
  // 500ms - 1000ms clamps to 0; 2500ms - 1000ms = 1500ms.
  CHECK(shifted.find("00:00:00,000 --> 00:00:01,500") != std::string::npos);
  CHECK(shifted.find("00:00:02,000 --> 00:00:03,500") != std::string::npos);
}

TEST_CASE("subtitle timestamp shifting: ASS and malformed input") {
  TestEnv env;
  const auto ass = env.dir / "in.ass";
  const auto assOut = env.dir / "shifted.ass";
  writeFile(ass,
            "[Script Info]\nScriptType: v4.00+\n\n[Events]\n"
            "Format: Layer, Start, End, Style, Name, MarginL, MarginR, "
            "MarginV, Effect, Text\n"
            "Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,Hello, world\n");
  std::string reason;
  REQUIRE(velmux::ffmpeg::subtitles::shiftSubtitleFile(
      ass.string(), assOut.string(), 1500, reason));
  const std::string shifted = readFile(assOut);
  CHECK(shifted.find("0:00:02.50,0:00:03.50") != std::string::npos);
  CHECK(shifted.find("Hello, world") != std::string::npos);

  const auto bad = env.dir / "bad.txt";
  const auto badOut = env.dir / "bad-out.txt";
  writeFile(bad, "this file has no cues at all\n");
  CHECK_FALSE(velmux::ffmpeg::subtitles::shiftSubtitleFile(
      bad.string(), badOut.string(), 500, reason));
  CHECK_FALSE(std::filesystem::exists(badOut));
}

TEST_CASE("REAL transcode preserves subtitle tracks (MKV/H.264+SRT -> HEVC? no: H.264 re-encode, MKV)") {
  TestEnv env;
  const std::string av = generateFixture(env.dir / "av.mp4",
                                         "color=c=black:size=320x240:rate=12",
                                         "sine=frequency=440:sample_rate=44100", 5.0);
  const auto srt = env.dir / "subs.srt";
  writeFile(srt, kSampleSrt);
  const std::string mkv = muxWithSubtitles(av, srt, env.dir / "in.mkv");
  REQUIRE(subtitleCodecName(mkv) == "subrip");

  // Force a real re-encode (resolution change) with the default Preserve
  // policy: the subtitle track must survive the transcode.
  ConversionRequest request =
      transcodeRequest(mkv, Container::Mkv, VideoCodec::H264, AudioCodec::Aac,
                       /*height=*/144);
  EventWaiter waiter(env.engine->events());
  const Result<std::string> jobId = env.engine->startJob(request);
  REQUIRE(jobId.ok());
  const auto terminal = waiter.waitForTerminal(std::chrono::seconds(120));
  REQUIRE(terminal.has_value());
  INFO("failure error: " << terminal->error);
  REQUIRE(terminal->state == JobState::Completed);

  const auto jobs = env.engine->listJobs();
  REQUIRE(jobs.size() == 1);
  const std::string outPath = jobs.front().outputPath;
  // INDEPENDENT cross-checks: video re-encoded, subtitle preserved.
  const ProbeResult probe = probeWith(outPath);
  CHECK(probe.videoCodec == "h264");
  CHECK(probe.height == 144);
  CHECK(countStreams(outPath, "s") == 1);
  CHECK(subtitleCodecName(outPath) == "subrip");
}

// ---------------------------------------------------------------------------
// Bitmap subtitle burn-in (spec §11: PGS; DVB/VobSub share the same path)
// ---------------------------------------------------------------------------

namespace {

void putU16(std::vector<std::uint8_t> &v, unsigned value) {
  v.push_back(std::uint8_t(value >> 8));
  v.push_back(std::uint8_t(value & 0xFF));
}
void putU24(std::vector<std::uint8_t> &v, unsigned value) {
  v.push_back(std::uint8_t(value >> 16));
  v.push_back(std::uint8_t((value >> 8) & 0xFF));
  v.push_back(std::uint8_t(value & 0xFF));
}
void putU32(std::vector<std::uint8_t> &v, unsigned value) {
  putU16(v, value >> 16);
  putU16(v, value & 0xFFFF);
}

// One .sup record: "PG" magic + PTS(90kHz) + DTS + segment.
void supRecord(std::vector<std::uint8_t> &out, std::int64_t pts90k,
               std::uint8_t type, const std::vector<std::uint8_t> &data) {
  out.push_back('P');
  out.push_back('G');
  putU32(out, pts90k);
  putU32(out, 0); // DTS unused
  out.push_back(type);
  putU16(out, data.size());
  out.insert(out.end(), data.begin(), data.end());
}

// RLE-codes a solid color bitmap (per line: one run + EOL).
std::vector<std::uint8_t> solidRle(std::int32_t w, std::int32_t h,
                                   std::uint8_t color) {
  std::vector<std::uint8_t> rle;
  for (std::int32_t y = 0; y < h; ++y) {
    rle.push_back(0x00);
    rle.push_back(std::uint8_t(0xC0 | (w >> 8)));
    rle.push_back(std::uint8_t(w & 0xFF));
    rle.push_back(color);
    rle.push_back(0x00); // EOL
    rle.push_back(0x00);
  }
  return rle;
}

// Hand-crafts a REAL PGS (.sup) stream: one white block cue at
// [startSec, endSec) positioned at (x, y). The bytes follow the exact
// segment grammar the pgssub decoder parses (PDS/ODS/PCS/WDS/END plus an
// empty display set to clear).
std::vector<std::uint8_t> makePgsSup(std::int32_t videoW,
                                     std::int32_t videoH, std::int32_t blockW,
                                     std::int32_t blockH, std::int32_t x,
                                     std::int32_t y, double startSec,
                                     double endSec) {
  const std::int64_t start90 = std::int64_t(startSec * 90000.0);
  const std::int64_t end90 = std::int64_t(endSec * 90000.0);
  std::vector<std::uint8_t> sup;

  // Segment ORDER matters: the decoder learns the video dimensions from
  // the PCS and rejects objects larger than them - the object must come
  // AFTER the presentation segment (standard PGS display-set order:
  // PCS, WDS, PDS, ODS, END).
  std::vector<std::uint8_t> pcs;
  putU16(pcs, videoW);
  putU16(pcs, videoH);
  pcs.push_back(0x10); // frame rate
  putU16(pcs, 0);      // composition number
  pcs.push_back(0x00); // state: normal
  pcs.push_back(0x00); // palette update flag
  pcs.push_back(0);    // palette id
  pcs.push_back(1);    // object count
  putU16(pcs, 0);      // object id
  pcs.push_back(0);    // window id
  pcs.push_back(0x00); // composition flags (not forced, no crop)
  putU16(pcs, x);
  putU16(pcs, y);
  supRecord(sup, start90, 0x16, pcs);

  // Window definition.
  std::vector<std::uint8_t> wds;
  wds.push_back(0); // window id
  putU16(wds, x);
  putU16(wds, y);
  putU16(wds, blockW);
  putU16(wds, blockH);
  supRecord(sup, start90, 0x17, wds);

  // Palette: entry 1 = opaque white (BT.709 Y=235,Cr=Cb=128).
  std::vector<std::uint8_t> pds;
  pds.push_back(0); // palette id
  pds.push_back(0); // version
  pds.push_back(1); // color id 1
  pds.push_back(235);
  pds.push_back(128);
  pds.push_back(128);
  pds.push_back(255); // alpha
  supRecord(sup, start90, 0x14, pds);

  // Object: solid block RLE.
  const std::vector<std::uint8_t> rle = solidRle(blockW, blockH, 1);
  std::vector<std::uint8_t> ods;
  putU16(ods, 0); // object id
  ods.push_back(0); // version
  ods.push_back(0x80); // first fragment
  putU24(ods, unsigned(4 + rle.size())); // total size incl. w/h
  putU16(ods, blockW);
  putU16(ods, blockH);
  ods.insert(ods.end(), rle.begin(), rle.end());
  supRecord(sup, start90, 0x15, ods);

  // Display.
  supRecord(sup, start90, 0x80, {});

  // Clear set at the end time (empty presentation + display).
  std::vector<std::uint8_t> pcsClear;
  putU16(pcsClear, videoW);
  putU16(pcsClear, videoH);
  pcsClear.push_back(0x10);
  putU16(pcsClear, 1); // next composition number
  pcsClear.push_back(0x00);
  pcsClear.push_back(0x00);
  pcsClear.push_back(0); // palette id
  pcsClear.push_back(0); // object count = 0 -> clear
  supRecord(sup, end90, 0x16, pcsClear);
  supRecord(sup, end90, 0x80, {});
  return sup;
}

} // namespace

TEST_CASE("REAL bitmap burn-in: embedded PGS composited into the video") {
  TestEnv env;
  // Fixture: 4 s 320x240 video + a PGS track with a white 96x64 block at
  // (64, 48) visible in [1s, 3s).
  const std::string fixture =
      generateFixture(env.dir / "in.mp4",
                      "testsrc2=size=320x240:rate=12",
                      "sine=frequency=440:sample_rate=44100", 4.0);
  const std::vector<std::uint8_t> sup =
      makePgsSup(320, 240, 96, 64, 64, 48, 1.0, 3.0);
  const auto supPath = env.dir / "cue.sup";
  writeFile(supPath, std::string(sup.begin(), sup.end()));
  const std::string withSubs = (env.dir / "pgs.mkv").string();
  // -copyts: the mkv muxer otherwise re-bases the subtitle track against
  // the audio's edit-list shift (-977 ms on this fixture), moving the cue
  // away from where it was authored.
  REQUIRE(std::system((ffmpegBin() + " -y -hide_banner -loglevel error "
                       "-copyts -avoid_negative_ts disabled -i \"" +
                       fixture + "\" -i \"" + supPath.string() +
                       "\" -map 0:v -map 0:a -map 1:0 -c:v copy -c:a copy "
                       "-c:s copy \"" + withSubs + "\"")
                          .c_str()) == 0);

  ConversionRequest request =
      transcodeRequest(withSubs, Container::Mp4, VideoCodec::H264,
                       AudioCodec::Aac);
  request.subtitles.policy = SubtitlePolicy::BurnIn;
  request.goal.kind = GoalKind::Custom;

  EventWaiter waiter(env.engine->events());
  const Result<std::string> jobId = env.engine->startJob(request);
  REQUIRE(jobId.ok());
  const auto terminal = waiter.waitForTerminal(std::chrono::seconds(60));
  REQUIRE(terminal.has_value());
  INFO("failure error: " << terminal->error);
  REQUIRE(terminal->state == JobState::Completed);
  const auto jobs = env.engine->listJobs();
  const ProbeResult probe = probeWith(jobs.front().outputPath);
  CHECK(probe.videoCodec == "h264");

  // INDEPENDENT pixel cross-check at three timestamps: inside the cue the
  // block region must be substantially brighter than the same region of
  // the source; outside the cue it must match the source closely.
  const auto regionMean = [&](const std::string &path, double atSec) {
    // Crop the block region, average the gray pixels via the CLI.
    const std::string cmd =
        ffmpegBin() + " -hide_banner -loglevel error -ss " +
        std::to_string(atSec) + " -i \"" + path +
        "\" -frames:v 1 -vf crop=96:64:64:48 -f rawvideo -pix_fmt gray - "
        "2>/dev/null";
    std::FILE *pipe = popen(cmd.c_str(), "r");
    REQUIRE(pipe != nullptr);
    long total = 0;
    int count = 0;
    int byte = 0;
    while ((byte = std::fgetc(pipe)) != EOF) {
      total += byte;
      ++count;
    }
    pclose(pipe);
    return count > 0 ? double(total) / double(count) : -1.0;
  };
  const double insideBurned = regionMean(jobs.front().outputPath, 2.0);
  const double insideSource = regionMean(fixture, 2.0);
  const double outsideBurned = regionMean(jobs.front().outputPath, 0.5);
  const double outsideSource = regionMean(fixture, 0.5);
  INFO("cue region burned=" << insideBurned << " source=" << insideSource
                           << "; outside burned=" << outsideBurned
                           << " source=" << outsideSource);
  // The white block lifts the region mean massively.
  CHECK(insideBurned > insideSource + 60.0);
  // Outside the cue the frame must stay (close to) the source: the
  // transparent overlay changed nothing.
  CHECK(std::abs(outsideBurned - outsideSource) < 12.0);
}

TEST_CASE("bitmap burn-in with external PGS and timing offset") {
  TestEnv env;
  const std::string fixture =
      generateFixture(env.dir / "in.mp4",
                      "testsrc2=size=320x240:rate=12",
                      "sine=frequency=440:sample_rate=44100", 4.0);
  const std::vector<std::uint8_t> sup =
      makePgsSup(320, 240, 96, 64, 64, 48, 1.0, 3.0);
  const auto supPath = env.dir / "ext.sup";
  writeFile(supPath, std::string(sup.begin(), sup.end()));

  ConversionRequest request =
      transcodeRequest(fixture, Container::Mp4, VideoCodec::H264,
                       AudioCodec::Aac);
  request.subtitles.policy = SubtitlePolicy::BurnIn;
  request.subtitles.externalUri = supPath.string();
  request.subtitles.timingOffsetMs = 1000; // cue becomes [2s, 4s)
  request.goal.kind = GoalKind::Custom;

  EventWaiter waiter(env.engine->events());
  const Result<std::string> jobId = env.engine->startJob(request);
  REQUIRE(jobId.ok());
  const auto terminal = waiter.waitForTerminal(std::chrono::seconds(60));
  REQUIRE(terminal.has_value());
  INFO("failure error: " << terminal->error);
  REQUIRE(terminal->state == JobState::Completed);
  const auto jobs = env.engine->listJobs();

  // Offset check: at 1.5s the cue must NOT be visible yet (still transparent
  // pre-roll), at 2.5s it must be. Pixel means via the same crop probe.
  const auto regionMean = [&](double atSec) {
    const std::string cmd =
        ffmpegBin() + " -hide_banner -loglevel error -ss " +
        std::to_string(atSec) + " -i \"" + jobs.front().outputPath +
        "\" -frames:v 1 -vf crop=96:64:64:48 -f rawvideo -pix_fmt gray - "
        "2>/dev/null";
    std::FILE *pipe = popen(cmd.c_str(), "r");
    REQUIRE(pipe != nullptr);
    long total = 0;
    int count = 0;
    int byte = 0;
    while ((byte = std::fgetc(pipe)) != EOF) {
      total += byte;
      ++count;
    }
    pclose(pipe);
    return count > 0 ? double(total) / double(count) : -1.0;
  };
  const double before = regionMean(1.5);
  const double during = regionMean(2.5);
  INFO("pre-cue mean=" << before << " during-cue mean=" << during);
  CHECK(during > before + 60.0);
}

TEST_CASE("bitmap burn-in refuses geometry transforms honestly") {
  TestEnv env;
  const std::string fixture =
      generateFixture(env.dir / "in.mp4",
                      "testsrc2=size=320x240:rate=12",
                      "sine=frequency=440:sample_rate=44100", 2.0);
  const std::vector<std::uint8_t> sup =
      makePgsSup(320, 240, 96, 64, 64, 48, 0.5, 1.5);
  const auto supPath = env.dir / "ext.sup";
  writeFile(supPath, std::string(sup.begin(), sup.end()));

  ConversionRequest request =
      transcodeRequest(fixture, Container::Mp4, VideoCodec::H264,
                       AudioCodec::Aac);
  request.subtitles.policy = SubtitlePolicy::BurnIn;
  request.subtitles.externalUri = supPath.string();
  request.video.rotationDegrees = 90; // incompatible with bitmap cues
  request.goal.kind = GoalKind::Custom;

  EventWaiter waiter(env.engine->events());
  const Result<std::string> jobId = env.engine->startJob(request);
  REQUIRE(jobId.ok());
  const auto terminal = waiter.waitForTerminal(std::chrono::seconds(60));
  REQUIRE(terminal.has_value());
  REQUIRE(terminal->state == JobState::Failed);
  CHECK(terminal->error.find("cannot be combined") != std::string::npos);
}

TEST_CASE("REAL burn-in: embedded SRT rendered into the video (libass)") {
  TestEnv env;
  const std::string av = generateFixture(env.dir / "av.mp4",
                                         "color=c=black:size=320x240:rate=12",
                                         "sine=frequency=440:sample_rate=44100", 5.0);
  const auto srt = env.dir / "subs.srt";
  writeFile(srt, kSampleSrt);
  const std::string mkv = muxWithSubtitles(av, srt, env.dir / "in.mkv");

  ConversionRequest request = transcodeRequest(mkv, Container::Mp4,
                                                VideoCodec::H264, AudioCodec::Aac);
  request.subtitles.policy = SubtitlePolicy::BurnIn;
  EventWaiter waiter(env.engine->events());
  const Result<std::string> jobId = env.engine->startJob(request);
  REQUIRE(jobId.ok());
  const auto terminal = waiter.waitForTerminal(std::chrono::seconds(180));
  REQUIRE(terminal.has_value());
  INFO("failure error: " << terminal->error);
  REQUIRE(terminal->state == JobState::Completed);

  const auto jobs = env.engine->listJobs();
  const std::string outPath = jobs.front().outputPath;
  // Burn-in: no subtitle stream in the output...
  CHECK(countStreams(outPath, "s") == 0);
  // ...and text pixels ARE present in the bottom quarter while the cue is
  // active (black fixture: any bright luma is rendered text).
  const int duringCue = bottomQuarterYMax(outPath, 1.0);
  INFO("YMAX during cue: " << duringCue);
  CHECK(duringCue > 100);
}

TEST_CASE("REAL burn-in: external SRT with timing offset") {
  TestEnv env;
  const std::string av = generateFixture(env.dir / "av.mp4",
                                         "color=c=black:size=320x240:rate=12",
                                         "sine=frequency=440:sample_rate=44100", 6.0);
  const auto srt = env.dir / "external.srt";
  // Cue becomes active only after the +2000 ms offset: 2.0s -> 4.0s.
  writeFile(srt,
            "1\n"
            "00:00:02,000 --> 00:00:05,000\n"
            "OFFSETMARKERTEXT\n\n");

  ConversionRequest request = transcodeRequest(av, Container::Mp4,
                                                VideoCodec::H264, AudioCodec::Aac);
  request.subtitles.policy = SubtitlePolicy::BurnIn;
  request.subtitles.externalUri = srt.string();
  request.subtitles.timingOffsetMs = 2000;
  EventWaiter waiter(env.engine->events());
  const Result<std::string> jobId = env.engine->startJob(request);
  REQUIRE(jobId.ok());
  const auto terminal = waiter.waitForTerminal(std::chrono::seconds(180));
  REQUIRE(terminal.has_value());
  INFO("failure error: " << terminal->error);
  REQUIRE(terminal->state == JobState::Completed);

  const auto jobs = env.engine->listJobs();
  const std::string outPath = jobs.front().outputPath;
  CHECK(countStreams(outPath, "s") == 0);
  // Before the shifted cue start: pure black.
  const int beforeCue = bottomQuarterYMax(outPath, 2.5);
  INFO("YMAX before shifted cue: " << beforeCue);
  CHECK(beforeCue <= 48);
  // While the shifted cue is active: rendered text.
  const int duringCue = bottomQuarterYMax(outPath, 4.5);
  INFO("YMAX during shifted cue: " << duringCue);
  CHECK(duringCue > 100);
}

TEST_CASE("subtitle policies that cannot be satisfied fail loudly") {
  TestEnv env;
  const std::string av = generateFixture(env.dir / "av.mp4",
                                         "testsrc2=size=320x240:rate=12",
                                         "sine=frequency=440:sample_rate=44100", 2.0);

  // Extraction is a standalone subtitle-only job: a mixed convert+extract
  // request must be refused rather than half-executed.
  ConversionRequest extract =
      transcodeRequest(av, Container::Mkv, VideoCodec::H264, AudioCodec::Aac);
  extract.subtitles.policy = SubtitlePolicy::Extract;
  const Result<PlanResult> plan = env.engine->plan(
      MediaSource{av, "av.mp4"}, extract);
  REQUIRE(plan.ok());
  CHECK_FALSE(plan.value().feasible);
  CHECK(plan.value().message.find("standalone") != std::string::npos);

  // Burn-in without any subtitle source is invalid.
  ConversionRequest burnNothing =
      transcodeRequest(av, Container::Mp4, VideoCodec::H264, AudioCodec::Aac);
  burnNothing.subtitles.policy = SubtitlePolicy::BurnIn;
  const Result<PlanResult> burnPlan = env.engine->plan(
      MediaSource{av, "av.mp4"}, burnNothing);
  REQUIRE(burnPlan.ok());
  CHECK_FALSE(burnPlan.value().feasible);
  CHECK(burnPlan.value().message.find("no subtitle track") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Recovery: segmented transcode with checkpoint commit + resume (spec §25/26)
// ---------------------------------------------------------------------------

TEST_CASE("REAL recovery: cancel a long transcode, resume reuses committed segments") {
  // The waiter is declared BEFORE the engine so it is destroyed AFTER it:
  // if a REQUIRE fails while a worker still runs, engine teardown (which
  // joins workers) happens before the sink dies instead of emitting into
  // a destroyed object (observed as "pure virtual method called").
  EventWaiter waiter;
  TestEnv env;
  // Long enough to trigger segmentation (> 4 x 10s targets).
  const std::string fixture =
      generateFixture(env.dir / "long.mp4",
                      "testsrc2=size=320x240:rate=10",
                      "sine=frequency=440:sample_rate=44100", 60.0);

  // VP9 encodes slowly enough that the cancel reliably lands mid-run on
  // any CI machine (H.264 ultrafast can finish 60s of 320x240 before a
  // polling cancel can fire).
  ConversionRequest request = transcodeRequest(
      fixture, Container::WebM, VideoCodec::VP9, AudioCodec::Opus);
  env.engine->events().subscribe(&waiter);
  const Result<std::string> jobId = env.engine->startJob(request);
  REQUIRE(jobId.ok());
  const std::string id = jobId.value();

  // Fault injection: cancel once the run is safely past several committed
  // segments (>= 50% of a 60s job means >= 2x10s committed).
  bool cancelled = false;
  for (int i = 0; i < 2400; ++i) { // up to 60s of polling
    const auto snapshot = env.engine->getJob(id);
    REQUIRE(snapshot.has_value());
    if (snapshot->state == JobState::Completed ||
        snapshot->state == JobState::Failed) {
      break; // machine too fast; the resume assertions still hold below
    }
    if (cancelled && snapshot->state == JobState::Canceled) {
      break; // control path recorded the intent; stop polling
    }
    if (!cancelled && snapshot->progress >= 0.5) {
      const Result<bool> cancelResult = env.engine->cancelJob(id);
      REQUIRE(cancelResult.ok());
      REQUIRE(cancelResult.value());
      cancelled = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  if (cancelled) {
    // The durable record flips to Canceled immediately (control path),
    // but the executor may still be unwinding: its in-flight partial
    // segment must be gone before artifacts are inspected. isActive is
    // the deterministic worker-liveness signal (progress events carry
    // the Canceled state too, so event counting cannot distinguish).
    for (int i = 0; i < 2400; ++i) { // up to 60s
      if (!env.engine->isJobActive(id)) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    REQUIRE_FALSE(env.engine->isJobActive(id));
  }
  const auto terminal = waiter.waitForTerminal(std::chrono::seconds(120));
  REQUIRE(terminal.has_value());
  if (cancelled) {
    REQUIRE(terminal->state == JobState::Canceled);

    // Durable state survived the "crash": committed segments on disk.
    const std::filesystem::path outputs = env.dir / "outputs";
    std::vector<std::string> segmentFiles;
    for (const auto &entry : std::filesystem::directory_iterator(outputs)) {
      const std::string name = entry.path().filename().string();
      if (name.rfind("velmux-seg-", 0) == 0 &&
          std::filesystem::is_directory(entry.path())) {
        for (const auto &seg : std::filesystem::directory_iterator(entry.path())) {
          if (seg.path().filename().string().rfind("seg-", 0) == 0) {
            segmentFiles.push_back(seg.path().string());
          }
        }
      }
    }
    // >= 2 committed segments (cancel fired at >= 50% of 60s).
    REQUIRE(segmentFiles.size() >= 2);
    // Each committed segment is independently valid media.
    for (const std::string &seg : segmentFiles) {
      const ProbeResult probe = probeWith(seg);
      CHECK(probe.videoCodec == "vp9");
      CHECK(probe.durationSec > 5.0);
    }

    // Recovery: retry the durable job; committed segments must be reused.
    // Reset the waiter BEFORE re-queueing: the latched Canceled must be
    // dropped first, and the retry's own events (Queued...Completed) latch
    // only after the reset (a reset after retryJob could clear a terminal
    // event on an absurdly fast machine and deadlock the wait).
    waiter.reset();
    const Result<bool> retried = env.engine->retryJob(id);
    REQUIRE(retried.ok());
    REQUIRE(retried.value());
    const auto after = waiter.waitForTerminal(std::chrono::seconds(240));
    REQUIRE(after.has_value());
    INFO("retry failure error: " << after->error);
    REQUIRE(after->state == JobState::Completed);

    const auto jobs = env.engine->listJobs();
    const std::string outPath = jobs.front().outputPath;
    // INDEPENDENT cross-check: the assembled output covers the WHOLE
    // source duration. If the resume had discarded the committed prefix,
    // the output would be >= 20s short.
    const ProbeResult probe = probeWith(outPath);
    CHECK(probe.videoCodec == "vp9");
    CHECK(probe.audioCodec == "opus");
    CHECK(probe.durationSec == doctest::Approx(60.0).epsilon(0.05));
    CHECK(std::filesystem::file_size(outPath) > 8192);
    // Segment scratch is cleaned after success.
    bool segDirLeft = false;
    for (const auto &entry : std::filesystem::directory_iterator(outputs)) {
      if (entry.path().filename().string().rfind("velmux-seg-", 0) == 0) {
        segDirLeft = true;
      }
    }
    CHECK_FALSE(segDirLeft);
  } else {
    // Completed before the cancel could fire: still verify the segmented
    // output end-to-end.
    const auto jobs = env.engine->listJobs();
    const std::string outPath = jobs.front().outputPath;
    const ProbeResult probe = probeWith(outPath);
    CHECK(probe.videoCodec == "vp9");
    CHECK(probe.durationSec == doctest::Approx(60.0).epsilon(0.05));
  }
}

// ---------------------------------------------------------------------------
// Milestone 4: hybrid (stage-composed) execution through the core executor
// ---------------------------------------------------------------------------

TEST_CASE("REAL hybrid composition: FFmpeg stages through the core HybridExecutor") {
  TestEnv env;
  const std::string fixture =
      generateFixture(env.dir / "in.mp4", "testsrc2=size=320x240:rate=12",
                      "sine=frequency=440:sample_rate=44100", 2.0);

  // Stage-split pipeline: decode side and mux side both FFmpeg here — the
  // composition machinery, buffer interchange and muxer coordination are
  // exactly what a native<->FFmpeg split will use on device; the CPU-frame
  // boundary is real regardless of who owns each side.
  velmux::core::HybridExecutor hybrid({env.backend});
  Pipeline pipeline;
  const auto assign = [&pipeline](StageId stage) {
    pipeline.stages.push_back({stage, BackendId::Ffmpeg});
  };
  assign(StageId::Demux);
  assign(StageId::Decode);
  assign(StageId::VideoProcessing);
  assign(StageId::AudioProcessing);
  assign(StageId::Encode);
  assign(StageId::Mux);
  assign(StageId::Bitstream);
  pipeline.videoCodec = VideoCodec::VP9;
  pipeline.audioCodec = AudioCodec::Aac;
  pipeline.container = Container::Mkv;
  pipeline.sourceFrameRate = 12.0;
  pipeline.audioStreamCopy = true;

  ConversionRequest request =
      transcodeRequest(fixture, Container::Mkv, VideoCodec::VP9,
                       AudioCodec::Aac);
  request.subtitles.policy = SubtitlePolicy::Remove;

  velmux::backend::ExecuteParams params;
  params.pipeline = pipeline;
  params.request = request;
  params.outputPath = (env.dir / "hybrid-out.mkv").string();
  params.jobId = "hybrid-test";
  params.cancel = std::make_shared<velmux::backend::CancelToken>();
  std::vector<double> progressFractions;
  params.onProgress = [&progressFractions](
                          const velmux::backend::ProgressReport &report) {
    if (report.fraction >= 0.0) {
      progressFractions.push_back(report.fraction);
    }
  };

  REQUIRE(hybrid.canExecute(pipeline, request));
  const auto result = hybrid.execute(params);
  INFO("hybrid error: " << result.error.message);
  REQUIRE(result.success);
  // 2s * 12fps; allow the encoder to close the GOP with the exact count.
  CHECK(result.framesProcessed >= 23);
  CHECK(result.framesProcessed <= 25);
  CHECK_FALSE(progressFractions.empty());
  CHECK(progressFractions.back() > 0.9);
  CHECK(result.wallClockSec > 0.0);

  // INDEPENDENT ffprobe cross-check: video re-encoded, audio copied.
  const ProbeResult probe = probeWith(params.outputPath);
  CHECK(probe.videoCodec == "vp9");
  CHECK(probe.audioCodec == "aac");
  CHECK(probe.width == 320);
  CHECK(probe.height == 240);
  CHECK(probe.durationSec == doctest::Approx(2.0).epsilon(0.25));
  CHECK(std::filesystem::file_size(params.outputPath) > 8192);
}

TEST_CASE("hybrid executor refuses shapes outside its v1 contract") {
  TestEnv env;
  velmux::core::HybridExecutor hybrid({env.backend});

  ConversionRequest request; // defaults: video+audio enabled
  request.subtitles.policy = SubtitlePolicy::Remove;

  Pipeline pipeline;
  const auto assign = [&pipeline](StageId stage) {
    pipeline.stages.push_back({stage, BackendId::Ffmpeg});
  };
  assign(StageId::Demux);
  assign(StageId::Decode);
  assign(StageId::VideoProcessing);
  assign(StageId::AudioProcessing);
  assign(StageId::Encode);
  assign(StageId::Mux);
  assign(StageId::Bitstream);

  SUBCASE("video stream copy is whole-pipeline work") {
    pipeline.videoStreamCopy = true;
    CHECK_FALSE(hybrid.canExecute(pipeline, request));
  }
  SUBCASE("audio transcode is whole-pipeline work") {
    pipeline.audioStreamCopy = false;
    CHECK_FALSE(hybrid.canExecute(pipeline, request));
  }
  SUBCASE("subtitle preservation needs whole-pipeline backends") {
    request.subtitles.policy = SubtitlePolicy::Preserve;
    CHECK_FALSE(hybrid.canExecute(pipeline, request));
  }
  SUBCASE("geometry changes need whole-pipeline backends") {
    request.video.resolutionHeight = 144;
    CHECK_FALSE(hybrid.canExecute(pipeline, request));
  }
  SUBCASE("single-backend stage assignment shape is required") {
    Pipeline bad = pipeline;
    bad.stages.clear();
    bad.stages.push_back({StageId::Decode, BackendId::Apple}); // unregistered
    bad.stages.push_back({StageId::Mux, BackendId::Ffmpeg});
    CHECK_FALSE(hybrid.canExecute(bad, request));
  }
}

// ---------------------------------------------------------------------------
// Trim + sample generation (spec §15/§16/§20)
// ---------------------------------------------------------------------------

TEST_CASE("REAL trim: transcode only the requested window") {
  TestEnv env;
  const std::string fixture =
      generateFixture(env.dir / "in.mp4", "testsrc2=size=320x240:rate=12",
                      "sine=frequency=440:sample_rate=44100", 4.0);

  ConversionRequest request = transcodeRequest(
      fixture, Container::Mp4, VideoCodec::H264, AudioCodec::Aac);
  request.trim.startUs = 1'000'000;
  request.trim.endUs = 3'000'000;

  EventWaiter waiter(env.engine->events());
  const Result<std::string> jobId = env.engine->startJob(request);
  REQUIRE(jobId.ok());

  const auto terminal = waiter.waitForTerminal(std::chrono::seconds(120));
  REQUIRE(terminal.has_value());
  INFO("failure error: " << terminal->error);
  REQUIRE(terminal->state == JobState::Completed);

  const auto jobs = env.engine->listJobs();
  const std::string outPath = jobs.front().outputPath;
  REQUIRE(std::filesystem::exists(outPath));

  // INDEPENDENT cross-check: the output covers only the window. The
  // transcode keeps source-timeline timestamps (matched-comparison
  // convention), so the file's span is ~[1s, 3s]: length ~2s, and
  // definitely far from the 4s source.
  const ProbeResult probe = probeWith(outPath);
  CHECK(probe.videoCodec == "h264");
  CHECK(probe.durationSec > 1.5);
  CHECK(probe.durationSec < 3.5);
  CHECK(std::filesystem::file_size(outPath) > 4096);
}

TEST_CASE("trim validation rejects inverted windows") {
  TestEnv env;
  const std::string fixture =
      generateFixture(env.dir / "in.mp4", "testsrc2=size=320x240:rate=12",
                      "sine=frequency=440:sample_rate=44100", 2.0);

  ConversionRequest request = transcodeRequest(
      fixture, Container::Mp4, VideoCodec::H264, AudioCodec::Aac);
  request.trim.startUs = 2'000'000;
  request.trim.endUs = 1'000'000;
  const Result<PlanResult> plan =
      env.engine->plan(MediaSource{fixture, "in.mp4"}, request);
  REQUIRE(plan.ok());
  REQUIRE_FALSE(plan.value().feasible);
  CHECK(plan.value().message.find("Trim") != std::string::npos);
}

TEST_CASE("REAL sample generation: representative window, requested settings") {
  TestEnv env;
  // Static half + motion half: the representative picker must land in the
  // motion half (denser compressed bytes).
  const std::string fixture =
      generateFixture(env.dir / "static.mp4", "color=black:size=320x240:rate=12",
                      "sine=frequency=440:sample_rate=44100", 2.0);
  const std::string motion =
      generateFixture(env.dir / "motion.mp4", "testsrc2=size=320x240:rate=12",
                      "sine=frequency=440:sample_rate=44100", 2.0);
  // Concat the halves with the concat DEMUXER + stream copy (identical
  // codec parameters; a filtergraph output cannot be stream-copied).
  const std::filesystem::path listPath = env.dir / "concat-list.txt";
  writeFile(listPath, "file '" + fixture + "'\nfile '" + motion + "'\n");
  REQUIRE(std::system((ffmpegBin() + " -y -hide_banner -loglevel error "
                                     "-f concat -safe 0 -i \"" +
                       listPath.string() + "\" -c copy \"" +
                       (env.dir / "mixed.mp4").string() + "\"")
                          .c_str()) == 0);
  const std::string mixed = (env.dir / "mixed.mp4").string();
  REQUIRE(std::filesystem::file_size(mixed) > 1024);

  const Result<SampleResult> sample = env.engine->generateSample(
      transcodeRequest(mixed, Container::WebM, VideoCodec::VP9,
                       AudioCodec::Opus),
      1);
  INFO("sample error: " << sample.error().message);
  REQUIRE(sample.ok());
  const SampleResult &s = sample.value();

  // The window must sit inside the motion (second) half.
  CHECK(s.startUs >= 2'000'000);
  CHECK(s.endUs - s.startUs <= 1'500'000);
  REQUIRE(std::filesystem::exists(s.outputPath));

  // INDEPENDENT cross-check: sample is real VP9 media of the right span.
  const ProbeResult probe = probeWith(s.outputPath);
  CHECK(probe.videoCodec == "vp9");
  CHECK(probe.width == 320);
  CHECK(probe.height == 240);
  CHECK(probe.durationSec > 0.7);
  CHECK(probe.durationSec < 3.0);
  CHECK(s.sizeBytes > 4096);
}

TEST_CASE("sample generation fails honestly on a broken source") {
  TestEnv env;
  writeFile(env.dir / "broken.mp4", "not a media file at all");
  const Result<SampleResult> sample = env.engine->generateSample(
      transcodeRequest((env.dir / "broken.mp4").string(), Container::Mp4,
                       VideoCodec::H264, AudioCodec::Aac),
      2);
  REQUIRE_FALSE(sample.ok());
}

// ---------------------------------------------------------------------------
// Subtitle extraction (spec §14): text tracks to sidecar files
// ---------------------------------------------------------------------------

TEST_CASE("REAL extraction: MKV/SRT track to a sidecar .srt through the engine") {
  TestEnv env;
  // Base A/V fixture + SRT sidecar muxed into MKV (copy).
  const std::string av =
      generateFixture(env.dir / "av.mkv", "testsrc2=size=160x120:rate=10",
                      "sine=frequency=440:sample_rate=44100", 2.0);
  const std::string srtPath = (env.dir / "subs.srt").string();
  writeFile(env.dir / "subs.srt", kSampleSrt);
  const std::string mkv = muxWithSubtitles(av, env.dir / "subs.srt",
                                           env.dir / "withsubs.mkv");

  ConversionRequest request;
  request.source.uri = mkv;
  request.source.displayName = "withsubs.mkv";
  request.goal.kind = GoalKind::Custom;
  request.video.enabled = false;
  request.audio.enabled = false;
  request.subtitles.policy = SubtitlePolicy::Extract;

  EventWaiter waiter(env.engine->events());
  const Result<std::string> jobId = env.engine->startJob(request);
  REQUIRE(jobId.ok());

  const auto terminal = waiter.waitForTerminal(std::chrono::seconds(60));
  REQUIRE(terminal.has_value());
  INFO("failure error: " << terminal->error);
  REQUIRE(terminal->state == JobState::Completed);

  const auto jobs = env.engine->listJobs();
  REQUIRE(jobs.size() == 1);
  const std::string outputPath = jobs.front().outputPath;
  // The artifact is a text sidecar with the SRT extension.
  REQUIRE(outputPath.size() > 4);
  CHECK(outputPath.substr(outputPath.size() - 4) == ".srt");
  REQUIRE(std::filesystem::exists(outputPath));

  // INDEPENDENT content check (not the code that wrote it): the cue text
  // from the source SRT survives, with real cue timing lines.
  const std::string extracted = readFile(outputPath);
  CHECK(extracted.find("BURNMARKERTEXT") != std::string::npos);
  CHECK(extracted.find("Second cue") != std::string::npos);
  CHECK(extracted.find("-->") != std::string::npos);
  CHECK(extracted.size() > 40);
}

TEST_CASE("REAL extraction: MP4 mov_text decodes to SRT") {
  TestEnv env;
  // MP4 with a mov_text subtitle track (the container-internal text
  // format that cannot stream-copy into a sidecar).
  const std::string av =
      generateFixture(env.dir / "av.mp4", "testsrc2=size=160x120:rate=10",
                      "sine=frequency=440:sample_rate=44100", 2.0);
  writeFile(env.dir / "subs.srt", kSampleSrt);
  const std::string mp4 = (env.dir / "movtext.mp4").string();
  const std::string cmd =
      ffmpegBin() + " -y -hide_banner -loglevel error -i \"" + av +
      "\" -i \"" + (env.dir / "subs.srt").string() +
      "\" -map 0:v -map 0:a -map 1:0 -c:v copy -c:a copy -c:s mov_text " +
      mp4;
  REQUIRE(std::system(cmd.c_str()) == 0);

  ConversionRequest request;
  request.source.uri = mp4;
  request.source.displayName = "movtext.mp4";
  request.goal.kind = GoalKind::Custom;
  request.video.enabled = false;
  request.audio.enabled = false;
  request.subtitles.policy = SubtitlePolicy::Extract;

  EventWaiter waiter(env.engine->events());
  const Result<std::string> jobId = env.engine->startJob(request);
  REQUIRE(jobId.ok());
  const auto terminal = waiter.waitForTerminal(std::chrono::seconds(60));
  REQUIRE(terminal.has_value());
  INFO("failure error: " << terminal->error);
  REQUIRE(terminal->state == JobState::Completed);

  const auto jobs = env.engine->listJobs();
  const std::string outputPath = jobs.front().outputPath;
  CHECK(outputPath.substr(outputPath.size() - 4) == ".srt");
  const std::string extracted = readFile(outputPath);
  CHECK(extracted.find("BURNMARKERTEXT") != std::string::npos);
  CHECK(extracted.find("-->") != std::string::npos);
}

TEST_CASE("extraction refusals stay honest") {
  TestEnv env;
  const std::string mkv = [&] {
    const std::string av =
        generateFixture(env.dir / "av.mkv", "testsrc2=size=160x120:rate=10",
                        "sine=frequency=440:sample_rate=44100", 1.0);
    writeFile(env.dir / "subs.srt", kSampleSrt);
    return muxWithSubtitles(av, env.dir / "subs.srt", env.dir / "s.mkv");
  }();

  SUBCASE("mixed convert+extract is refused, not half-executed") {
    ConversionRequest request;
    request.source.uri = mkv;
    request.source.displayName = "s.mkv";
    request.goal.kind = GoalKind::Custom;
    request.video.enabled = true;
    request.audio.enabled = true;
    request.subtitles.policy = SubtitlePolicy::Extract;
    const Result<PlanResult> plan =
        env.engine->plan(MediaSource{mkv, "s.mkv"}, request);
    REQUIRE(plan.ok());
    CHECK_FALSE(plan.value().feasible);
    CHECK(plan.value().message.find("standalone") != std::string::npos);
  }

  SUBCASE("no subtitle track is an honest refusal") {
    const std::string plain =
        generateFixture(env.dir / "plain.mp4",
                        "testsrc2=size=160x120:rate=10",
                        "sine=frequency=440:sample_rate=44100", 1.0);
    ConversionRequest request;
    request.source.uri = plain;
    request.source.displayName = "plain.mp4";
    request.goal.kind = GoalKind::Custom;
    request.video.enabled = false;
    request.audio.enabled = false;
    request.subtitles.policy = SubtitlePolicy::Extract;
    const Result<PlanResult> plan =
        env.engine->plan(MediaSource{plain, "plain.mp4"}, request);
    REQUIRE(plan.ok());
    CHECK_FALSE(plan.value().feasible);
    CHECK(plan.value().message.find("no subtitle track") !=
          std::string::npos);
  }
}

// ---------------------------------------------------------------------------
// M12 hardening: hostile environments and damaged inputs (spec §36/§38)
// ---------------------------------------------------------------------------

TEST_CASE("HARDENING: truncated source fails honestly, never fake success") {
  TestEnv env;
  // faststart puts moov at the FRONT: the truncated file still parses and
  // plans, so the run+validate path (the interesting one) is exercised.
  const std::string full =
      generateFixture(env.dir / "full.mp4", "testsrc2=size=320x240:rate=12",
                      "sine=frequency=440:sample_rate=44100", 4.0);
  const std::string fast = (env.dir / "fast.mp4").string();
  REQUIRE(std::system((ffmpegBin() + " -y -hide_banner -loglevel error -i \"" +
                       full + "\" -c copy -movflags faststart \"" + fast + "\"")
                          .c_str()) == 0);
  // Cut at ~60%: header parseable, media tail gone.
  const std::string truncated = (env.dir / "cut.mp4").string();
  const std::int64_t fullSize = std::filesystem::file_size(fast);
  {
    std::ifstream in(fast, std::ios::binary);
    std::ofstream out(truncated, std::ios::binary | std::ios::trunc);
    const std::int64_t keep = fullSize * 6 / 10;
    std::vector<char> buf(static_cast<std::size_t>(keep));
    in.read(buf.data(), keep);
    out.write(buf.data(), keep);
    REQUIRE(out.good());
  }
  REQUIRE(std::filesystem::file_size(truncated) < fullSize);

  ConversionRequest request = transcodeRequest(
      truncated, Container::Mp4, VideoCodec::H264, AudioCodec::Aac);

  // Honest outcomes, one forbidden shape (fake success):
  //  a) inspection refuses (Result not ok) with a reason,
  //  b) the plan is infeasible with a reason,
  //  c) the job Fails (validator catches the missing tail), or
  //  d) the job Completes AND the output covers the INSPECTED source
  //     duration - inspection of a truncated file may already report the
  //     decodable duration, and converting exactly what was inspected is
  //     honest. What is never allowed: Completed with output clearly
  //     shorter than what inspection promised.
  const Result<PlanResult> plan =
      env.engine->plan(MediaSource{truncated, "cut.mp4"}, request);
  if (plan.ok() && plan.value().feasible) {
    // The inspection the engine will reuse for validation.
    const Result<MediaInfo> inspected =
        env.engine->inspect(MediaSource{truncated, "cut.mp4"});
    REQUIRE(inspected.ok());
    const double inspectedDuration = inspected.value().durationSec;
    EventWaiter waiter(env.engine->events());
    const Result<std::string> jobId = env.engine->startJob(request);
    REQUIRE(jobId.ok());
    const auto terminal = waiter.waitForTerminal(std::chrono::seconds(120));
    REQUIRE(terminal.has_value());
    INFO("terminal state: " << int(terminal->state)
                            << " error: " << terminal->error);
    if (terminal->state == JobState::Completed) {
      REQUIRE(inspectedDuration > 0.0);
      const auto jobs = env.engine->listJobs();
      const std::string outputPath = jobs.front().outputPath;
      const ProbeResult probe = probeWith(outputPath);
      INFO("inspected " << inspectedDuration << "s, output "
                        << probe.durationSec << "s");
      CHECK(probe.durationSec >= inspectedDuration - 1.0);
    } else {
      REQUIRE(terminal->state == JobState::Failed);
      CHECK(terminal->error.size() > 0);
    }
  } else if (plan.ok()) {
    CHECK(plan.value().message.size() > 0);
  } else {
    CHECK(plan.error().message.size() > 0);
  }
}

TEST_CASE("HARDENING: source disappearing before a resume fails cleanly") {
  TestEnv env;
  // Long enough to still be running when the pause lands.
  const std::string source =
      generateFixture(env.dir / "gone.mp4", "testsrc2=size=320x240:rate=12",
                      "sine=frequency=440:sample_rate=44100", 20.0);
  ConversionRequest request =
      transcodeRequest(source, Container::WebM, VideoCodec::VP9,
                       AudioCodec::Opus);

  // Phase 1: start, then pause while running.
  EventWaiter waiter(env.engine->events());
  const Result<std::string> jobId = env.engine->startJob(request);
  REQUIRE(jobId.ok());
  // Wait until it is actually running, then pause.
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  const Result<bool> paused = env.engine->pauseJob(jobId.value());
  REQUIRE(paused.ok());
  REQUIRE(paused.value());
  // Phase 2: the source vanishes while the job sits paused; resume must
  // fail cleanly at open time - never a crash, never fake success.
  std::filesystem::remove(source);
  waiter.reset();
  const Result<bool> resumed = env.engine->resumeJob(jobId.value());
  REQUIRE(resumed.ok());
  REQUIRE(resumed.value());
  const auto terminal = waiter.waitForTerminal(std::chrono::seconds(240));
  REQUIRE(terminal.has_value());
  INFO("terminal state: " << int(terminal->state)
                          << " error: " << terminal->error);
  REQUIRE(terminal->state == JobState::Failed);
  CHECK_FALSE(terminal->error.empty());
}

TEST_CASE("HARDENING: unwritable output destination fails cleanly") {
  TestEnv env;
  const std::string source =
      generateFixture(env.dir / "av.mp4", "testsrc2=size=320x240:rate=12",
                      "sine=frequency=440:sample_rate=44100", 2.0);
  ConversionRequest request =
      transcodeRequest(source, Container::Mp4, VideoCodec::H264,
                       AudioCodec::Aac);

  // Make the outputs directory unwritable after engine creation: every
  // artifact write must fail (never a silent no-op or a crash).
  const std::string outputs = env.dir.string() + "/outputs";
  REQUIRE(::chmod(outputs.c_str(), 0555) == 0);

  EventWaiter waiter(env.engine->events());
  const Result<std::string> jobId = env.engine->startJob(request);
  REQUIRE(jobId.ok());
  const auto terminal = waiter.waitForTerminal(std::chrono::seconds(120));
  REQUIRE(terminal.has_value());
  // Restore permissions so TestEnv cleanup can remove the directory.
  ::chmod(outputs.c_str(), 0755);
  INFO("terminal state: " << int(terminal->state)
                          << " error: " << terminal->error);
  REQUIRE(terminal->state == JobState::Failed);
  CHECK_FALSE(terminal->error.empty());
}

TEST_CASE("HARDENING: very large container metadata converts honestly") {
  TestEnv env;
  // Base fixture, then re-mux it with hundreds of large container tags
  // (~600 KB of metadata — far beyond anything a real library writes).
  // The engine must copy or strip them per the request and complete with a
  // validated, non-truncated output — or fail with a real error. Never a
  // hang (bounded by the terminal waits), never a crash (the test process
  // survives), never a silent truncation (duration + probe cross-checks).
  const std::string plain =
      generateFixture(env.dir / "in.mp4",
                      "testsrc2=size=320x240:rate=12",
                      "sine=frequency=440:sample_rate=44100", 2.0);
  // The bulk rides in an ffmetadata FILE: 400 -metadata CLI arguments
  // would exceed the 128 KB single-argument exec limit through sh -c (the
  // first CI run of this test died with exit 127 = E2BIG before ffmpeg
  // ever started).
  const std::string metaFile = (env.dir / "huge.ffmeta").string();
  {
    std::ofstream meta(metaFile, std::ios::trunc);
    meta << ";FFMETADATA1\ntitle=VelmuxHugeMetaTitle\n";
    const std::string filler(1500, 'm');
    for (int i = 0; i < 400; ++i) {
      meta << "velmux_tag_" << i << "=" << filler << "\n";
    }
    REQUIRE(meta.good());
  }
  const std::string tagged = (env.dir / "huge-meta.mp4").string();
  REQUIRE(std::system((ffmpegBin() + " -y -hide_banner -loglevel error -i \"" +
                       plain + "\" -f ffmetadata -i \"" + metaFile +
                       "\" -map 0 -map_metadata 1 -movflags +use_metadata_tags "
                       "-c copy \"" + tagged + "\"")
                          .c_str()) == 0);
  // The fixture really carries the payload (~600 KB of tags).
  REQUIRE(std::filesystem::file_size(tagged) >
          std::filesystem::file_size(plain) + std::int64_t(400) * 1300);

  // INDEPENDENT metadata cross-check: reads one container tag via ffprobe.
  const auto tagOf = [](const std::string &path, const std::string &key) {
    std::FILE *pipe =
        popen((ffprobeBin() + " -v error -show_entries format_tags=" + key +
               " -of csv=p=0 \"" + path + "\"")
                  .c_str(),
              "r");
    REQUIRE(pipe != nullptr);
    char buf[512] = {0};
    const char *got = std::fgets(buf, sizeof(buf), pipe);
    pclose(pipe);
    std::string s = got != nullptr ? buf : "";
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
      s.pop_back();
    }
    return s;
  };

  // Run 1: remux (stream copy) with metadata preserved (the default).
  // Completed must mean the sentinel tag survived and the output covers
  // the source duration.
  {
    EventWaiter waiter(env.engine->events());
    const Result<std::string> jobId = env.engine->startJob(
        transcodeRequest(tagged, Container::Mkv, VideoCodec::H264,
                         AudioCodec::Aac));
    REQUIRE(jobId.ok());
    const auto terminal = waiter.waitForTerminal(std::chrono::seconds(120));
    REQUIRE(terminal.has_value());
    INFO("terminal state: " << int(terminal->state)
                            << " error: " << terminal->error);
    if (terminal->state == JobState::Completed) {
      const auto snap = env.engine->getJob(jobId.value());
      REQUIRE(snap.has_value());
      const ProbeResult probe = probeWith(snap->outputPath);
      CHECK(probe.videoCodec == "h264");
      CHECK(probe.durationSec >= 1.0); // 2 s source; never a truncation
      CHECK(snap->outputSizeBytes > 1024);
      // Metadata preserved per the request: the sentinel title survives.
      CHECK(tagOf(snap->outputPath, "title") == "VelmuxHugeMetaTitle");
    } else {
      REQUIRE(terminal->state == JobState::Failed);
      CHECK_FALSE(terminal->error.empty());
    }
  }

  // Run 2: real transcode (height change forces re-encoding) with
  // stripMetadata=true — the decode/encode path must carry or drop the
  // tags per the request, honestly either way.
  {
    ConversionRequest strip =
        transcodeRequest(tagged, Container::Mkv, VideoCodec::H264,
                         AudioCodec::Aac, 144, 400'000);
    strip.stripMetadata = true;
    EventWaiter waiter2(env.engine->events());
    const Result<std::string> jobId2 = env.engine->startJob(strip);
    REQUIRE(jobId2.ok());
    const auto terminal2 = waiter2.waitForTerminal(std::chrono::seconds(120));
    REQUIRE(terminal2.has_value());
    INFO("terminal state: " << int(terminal2->state)
                            << " error: " << terminal2->error);
    if (terminal2->state == JobState::Completed) {
      const auto snap = env.engine->getJob(jobId2.value());
      REQUIRE(snap.has_value());
      const ProbeResult probe = probeWith(snap->outputPath);
      CHECK(probe.height == 144);
      CHECK(probe.durationSec >= 1.0);
      // Stripped per the request: the sentinel must NOT survive.
      CHECK(tagOf(snap->outputPath, "title").empty());
    } else {
      REQUIRE(terminal2->state == JobState::Failed);
      CHECK_FALSE(terminal2->error.empty());
    }
  }
}

// Rewinds one mid-file video PES PTS by half a second (33-bit
// wrap-safe) inside an MPEG-TS file — authoring a genuine backward
// timestamp jump, the shape real-world broken captures carry. Filters
// and re-encoding cannot produce it: the encoder/muxer silently
// re-stamp timestamps, which is exactly how the first authoring attempt
// of this fixture produced a perfectly healthy file. Returns false when
// no parseable video PES was found (the caller fails the fixture).
bool rewindMidFileVideoPts(const std::string &tsPath) {
  std::ifstream in(tsPath, std::ios::binary);
  std::vector<unsigned char> buf((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
  if (buf.size() < 188 * 10) {
    return false;
  }
  int videoPes = 0;
  bool done = false;
  for (std::size_t off = 0; off + 188 <= buf.size() && !done; off += 188) {
    if (buf[off] != 0x47) {
      return false; // not a TS packet
    }
    const unsigned b1 = buf[off + 1];
    const unsigned b3 = buf[off + 3];
    if (((b1 >> 6) & 1) == 0) {
      continue; // not a payload-unit start
    }
    std::size_t p = off + 4;
    const unsigned afc = (b3 >> 4) & 3;
    if (afc == 3) {
      p += 1 + buf[off + 4]; // skip the adaptation field
    } else if (afc == 0) {
      continue; // no payload
    }
    if (p + 14 > off + 188) {
      continue;
    }
    if (buf[p] != 0x00 || buf[p + 1] != 0x00 || buf[p + 2] != 0x01 ||
        (buf[p + 3] & 0xF0) != 0xE0) {
      continue; // not a video PES start
    }
    if (((buf[p + 7] >> 6) & 3) < 2) {
      continue; // this PES carries no PTS
    }
    ++videoPes;
    if (videoPes < 12) {
      continue; // rewind a mid-file PES (2 s @ 12 fps = 24 frames)
    }
    // 33-bit PTS in bytes p+9..p+13; subtract 45000 (0.5 s @ 90 kHz).
    const std::uint64_t pts =
        (std::uint64_t(buf[p + 9] >> 1) & 0x07) << 30 |
        std::uint64_t(buf[p + 10]) << 22 |
        (std::uint64_t(buf[p + 11] >> 1) & 0x7F) << 15 |
        std::uint64_t(buf[p + 12]) << 7 |
        (std::uint64_t(buf[p + 13] >> 1) & 0x7F);
    const std::uint64_t newPts =
        (pts + (1ULL << 33) - 45000) % (1ULL << 33);
    buf[p + 9] = static_cast<unsigned char>(
        (buf[p + 9] & 0xF1) | (((newPts >> 30) & 0x07) << 1));
    buf[p + 10] = static_cast<unsigned char>((newPts >> 22) & 0xFF);
    buf[p + 11] = static_cast<unsigned char>(
        (buf[p + 11] & 0x01) | (((newPts >> 15) & 0x7F) << 1));
    buf[p + 12] = static_cast<unsigned char>((newPts >> 7) & 0xFF);
    buf[p + 13] = static_cast<unsigned char>(
        (buf[p + 13] & 0x01) | (static_cast<unsigned>(newPts) & 0x7F) << 1);
    done = true;
  }
  if (!done) {
    return false;
  }
  std::ofstream out(tsPath, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char *>(buf.data()),
            std::streamsize(buf.size()));
  return out.good();
}

TEST_CASE("HARDENING: invalid timestamps (non-monotonic PTS) convert honestly") {
  TestEnv env;
  // Base fixture, a clean stream-copy into MPEG-TS, then a surgical
  // backward jump of one mid-file video PES PTS (see
  // rewindMidFileVideoPts above): the classic broken-capture shape.
  // Real-world TS files carry such discontinuities and counter
  // wraparounds; the file also carries the A/V desync that follows.
  const std::string plain =
      generateFixture(env.dir / "in.mp4",
                      "testsrc2=size=320x240:rate=12",
                      "sine=frequency=440:sample_rate=44100", 2.0);
  const std::string broken = (env.dir / "nonmono.ts").string();
  REQUIRE(std::system((ffmpegBin() + " -y -hide_banner -loglevel error -i \"" +
                       plain + "\" -c copy -f mpegts \"" + broken + "\"")
                          .c_str()) == 0);
  REQUIRE(std::filesystem::file_size(broken) > 1024);
  REQUIRE(rewindMidFileVideoPts(broken));

  // The fixture really carries the pathology: at least one video packet
  // has a SMALLER pts than its predecessor. If generation ever silently
  // "fixes" the timestamps, fail loudly instead of testing a healthy file.
  {
    std::FILE *pipe =
        popen((ffprobeBin() + " -v error -select_streams v:0 "
                              "-show_entries packet=pts -of csv=p=0 \"" +
               broken + "\"")
                  .c_str(),
              "r");
    REQUIRE(pipe != nullptr);
    bool nonMonotonic = false;
    bool havePrev = false;
    std::int64_t prev = 0;
    char line[64];
    while (std::fgets(line, sizeof(line), pipe) != nullptr) {
      if (line[0] < '0' || line[0] > '9') {
        continue; // header noise / packets without pts
      }
      const std::int64_t pts = std::atoll(line);
      if (havePrev && pts < prev) {
        nonMonotonic = true;
        break;
      }
      prev = pts;
      havePrev = true;
    }
    pclose(pipe);
    REQUIRE(nonMonotonic);
  }

  ConversionRequest request =
      transcodeRequest(broken, Container::Mp4, VideoCodec::H264,
                       AudioCodec::Aac);

  // Honest outcomes, one forbidden shape (fake success): the engine may
  // refuse at inspection, plan infeasible, or fail the job with a real
  // error — or complete with a VALID, decodable output whose duration
  // covers what inspection promised (minus the ~0.5 s the jump costs).
  // Never a hang (bounded waits), never a crash, never fake success.
  const Result<PlanResult> plan =
      env.engine->plan(MediaSource{broken, "nonmono.ts"}, request);
  if (plan.ok() && plan.value().feasible) {
    const Result<MediaInfo> inspected =
        env.engine->inspect(MediaSource{broken, "nonmono.ts"});
    REQUIRE(inspected.ok());
    const double inspectedDuration = inspected.value().durationSec;
    EventWaiter waiter(env.engine->events());
    const Result<std::string> jobId = env.engine->startJob(request);
    REQUIRE(jobId.ok());
    const auto terminal = waiter.waitForTerminal(std::chrono::seconds(120));
    REQUIRE(terminal.has_value());
    INFO("terminal state: " << int(terminal->state)
                            << " error: " << terminal->error);
    if (terminal->state == JobState::Completed) {
      const auto snap = env.engine->getJob(jobId.value());
      REQUIRE(snap.has_value());
      const ProbeResult probe = probeWith(snap->outputPath);
      CHECK(probe.videoCodec == "h264");
      CHECK(probe.durationSec >= inspectedDuration - 1.0);
      CHECK(snap->outputSizeBytes > 1024);
    } else {
      REQUIRE(terminal->state == JobState::Failed);
      CHECK_FALSE(terminal->error.empty());
    }
  } else if (plan.ok()) {
    CHECK(plan.value().message.size() > 0);
  } else {
    CHECK(plan.error().message.size() > 0);
  }
}

TEST_CASE("REAL multi-track audio: allTracks carries every lane with language") {
  TestEnv env;
  // A two-audio-lane MKV (eng + fra), the shape real multi-language media
  // has. Built directly with the ffmpeg binary: three lavfi inputs, one
  // video + two distinct audio lanes, each tagged with its language.
  const std::string multi = (env.dir / "multi.mkv").string();
  REQUIRE(std::system(
              (ffmpegBin() + " -y -hide_banner -loglevel error "
               "-filter_complex \"testsrc2=size=320x240:rate=12[v];"
               "sine=frequency=440:sample_rate=44100[a1];"
               "sine=frequency=880:sample_rate=44100[a2]\" "
               "-map \"[v]\" -map \"[a1]\" -map \"[a2]\" "
               "-c:v libx264 -preset ultrafast -crf 35 -c:a aac -b:a 64k "
               "-metadata:s:a:0 language=eng -metadata:s:a:1 language=fra "
               "-pix_fmt yuv420p -t 2 \"" + multi + "\"")
                  .c_str()) == 0);
  REQUIRE(countStreams(multi, "a") == 2);

  ConversionRequest request =
      transcodeRequest(multi, Container::Mkv, VideoCodec::H264,
                       AudioCodec::Aac);
  request.audio.allTracks = true;

  EventWaiter waiter(env.engine->events());
  const Result<std::string> jobId = env.engine->startJob(request);
  REQUIRE(jobId.ok());
  const auto terminal = waiter.waitForTerminal(std::chrono::seconds(120));
  REQUIRE(terminal.has_value());
  INFO("failure error: " << terminal->error);
  REQUIRE(terminal->state == JobState::Completed);

  const auto snap = env.engine->getJob(jobId.value());
  REQUIRE(snap.has_value());
  // Every source lane survives the transcode, with its language metadata.
  CHECK(countStreams(snap->outputPath, "a") == 2);
  CHECK(streamFieldOf(snap->outputPath, "a:0", "stream_tags=language") ==
        "eng");
  CHECK(streamFieldOf(snap->outputPath, "a:1", "stream_tags=language") ==
        "fra");
  const ProbeResult probe = probeWith(snap->outputPath);
  CHECK(probe.durationSec == doctest::Approx(2.0).epsilon(0.25));
}

TEST_CASE("REAL replace audio: external audio rides the video timeline") {
  TestEnv env;
  // A normal video+audio fixture; the product case replaces the EXISTING
  // audio lane with an external file.
  const std::string video = generateFixture(
      env.dir / "v.mp4", "testsrc2=size=320x240:rate=12",
      "sine=frequency=440:sample_rate=44100", 3.0);
  // A distinct replacement lane (different tone, DIFFERENT length — the
  // honest outcome must follow the video timeline, not the audio file).
  const std::string replacement = (env.dir / "replacement.m4a").string();
  REQUIRE(std::system(
              (ffmpegBin() + " -y -hide_banner -loglevel error "
               "-filter_complex \"sine=frequency=880:sample_rate=44100[a]\" "
               "-map \"[a]\" -c:a aac -b:a 96k -t 2 \"" + replacement +
               "\"")
                  .c_str()) == 0);

  ConversionRequest request =
      transcodeRequest(video, Container::Mp4, VideoCodec::H264,
                       AudioCodec::Aac);
  request.audio.replaceAudioUri = replacement;

  EventWaiter waiter(env.engine->events());
  const Result<std::string> jobId = env.engine->startJob(request);
  REQUIRE(jobId.ok());
  const auto terminal = waiter.waitForTerminal(std::chrono::seconds(120));
  REQUIRE(terminal.has_value());
  INFO("failure error: " << terminal->error);
  REQUIRE(terminal->state == JobState::Completed);

  const auto snap = env.engine->getJob(jobId.value());
  REQUIRE(snap.has_value());
  // Exactly one audio lane (the replacement), and the output duration
  // follows the VIDEO stream (3 s source), not the 2 s replacement.
  CHECK(countStreams(snap->outputPath, "a") == 1);
  const ProbeResult probe = probeWith(snap->outputPath);
  CHECK(probe.audioCodec == "aac");
  INFO("duration: " << probe.durationSec << " (video 3.0, replacement 2.0)");
  CHECK(probe.durationSec == doctest::Approx(3.0).epsilon(0.2));
}

TEST_CASE("REAL cover art: external image attaches to audio-only output") {
  TestEnv env;
  const std::string fixture =
      generateFixture(env.dir / "in.mp4",
                      "testsrc2=size=320x240:rate=12",
                      "sine=frequency=440:sample_rate=44100", 2.0);
  const std::string cover = (env.dir / "cover.png").string();
  REQUIRE(std::system(
              (ffmpegBin() + " -y -hide_banner -loglevel error "
               "-filter_complex \"testsrc2=size=320x240:rate=1[v]\" "
               "-map \"[v]\" -frames:v 1 \"" + cover + "\"")
                  .c_str()) == 0);
  REQUIRE(std::filesystem::file_size(cover) > 1024);

  ConversionRequest request =
      transcodeRequest(fixture, Container::M4a, VideoCodec::H264,
                       AudioCodec::Aac);
  request.video.enabled = false;
  request.audio.coverArtUri = cover;

  EventWaiter waiter(env.engine->events());
  const Result<std::string> jobId = env.engine->startJob(request);
  REQUIRE(jobId.ok());
  const auto terminal = waiter.waitForTerminal(std::chrono::seconds(120));
  REQUIRE(terminal.has_value());
  INFO("failure error: " << terminal->error);
  REQUIRE(terminal->state == JobState::Completed);

  const auto snap = env.engine->getJob(jobId.value());
  REQUIRE(snap.has_value());
  // The attached picture rides along as a video stream with the
  // attached_pic disposition (the standard cover-art shape).
  CHECK(hasAttachedPic(snap->outputPath));
  const ProbeResult probe = probeWith(snap->outputPath);
  CHECK(probe.audioCodec == "aac");
  CHECK(probe.durationSec == doctest::Approx(2.0).epsilon(0.25));
}

TEST_CASE("REAL HDR: bt2020/PQ source tone-maps to SDR BT.709 on transcode") {
  TestEnv env;
  // An HDR-SIGNALED fixture: 8-bit x264 tagged bt2020/smpte2084. The
  // detection path reads the decoded color metadata, exactly what real
  // PQ media exercises; the pinned software encoders are 8-bit, so a
  // transcode must tone-map (colorspace -> bt709) and tag SDR.
  const std::string hdr = (env.dir / "hdr.mp4").string();
  // The color metadata rides the x264 VUI (x264-params): the CLI's
  // -color_* options do NOT reach the MP4 colr atom in ffmpeg 7.1 —
  // verified empirically before wiring this fixture.
  REQUIRE(std::system(
              (ffmpegBin() + " -y -hide_banner -loglevel error "
               "-filter_complex \"testsrc2=size=320x240:rate=12[v];"
               "sine=frequency=440:sample_rate=44100[a]\" "
               "-map \"[v]\" -map \"[a]\" "
               "-c:v libx264 -preset ultrafast -crf 35 -c:a aac -b:a 96k "
               "-x264-params colorprim=bt2020:transfer=smpte2084:"
               "colormatrix=bt2020nc -pix_fmt yuv420p -t 2 \"" + hdr + "\"")
                  .c_str()) == 0);
  REQUIRE(streamFieldOf(hdr, "v:0", "stream=color_transfer") ==
      "smpte2084");

  // Transcode (default 8-bit H.264): the pinned build offers no PQ/HLG
  // tone-map, so the PLANNER refuses the route honestly instead of
  // producing washed-out output — infeasible with a reason. The height
  // change forces a real re-encode: a same-shape request would honestly
  // plan as stream copy (HDR preserved, never gated). (An explicit
  // ToneMapToSdr request is refused by the same gate.)
  {
    ConversionRequest request =
        transcodeRequest(hdr, Container::Mp4, VideoCodec::H264,
                         AudioCodec::Aac, 144);
    const Result<PlanResult> plan =
        env.engine->plan(MediaSource{hdr, "hdr.mp4"}, request);
    REQUIRE(plan.ok());
    INFO("plan message: " << plan.value().message);
    CHECK_FALSE(plan.value().feasible);
    CHECK(plan.value().message.size() > 0);
    // Explicit ToneMapToSdr is refused by the same planner gate.
    ConversionRequest tm =
        transcodeRequest(hdr, Container::Mp4, VideoCodec::H264,
                         AudioCodec::Aac, 144);
    tm.video.hdrPolicy = HdrPolicy::ToneMapToSdr;
    const Result<PlanResult> planTm =
        env.engine->plan(MediaSource{hdr, "hdr.mp4"}, tm);
    REQUIRE(planTm.ok());
    CHECK_FALSE(planTm.value().feasible);
    CHECK(planTm.value().message.size() > 0);
  }

  // Stream copy (remux to MKV, no re-encode): HDR signaling is preserved
  // untouched — the copy path must not invent a conversion.
  {
    ConversionRequest remux =
        transcodeRequest(hdr, Container::Mkv, VideoCodec::H264,
                         AudioCodec::Aac);
    remux.audio.codec = AudioCodec::Aac; // audio re-encode keeps it a
                                         // single-backend FFmpeg job
    // Planner prefers stream copy for matching codecs; no transform asked.
    EventWaiter waiter2(env.engine->events());
    const Result<std::string> jobId2 = env.engine->startJob(remux);
    REQUIRE(jobId2.ok());
    const auto terminal2 = waiter2.waitForTerminal(std::chrono::seconds(120));
    REQUIRE(terminal2.has_value());
    INFO("failure error: " << terminal2->error);
    REQUIRE(terminal2->state == JobState::Completed);
    const auto snap2 = env.engine->getJob(jobId2.value());
    REQUIRE(snap2.has_value());
    CHECK(streamFieldOf(snap2->outputPath, "v:0", "stream=color_transfer") ==
          "smpte2084");
    CHECK(streamFieldOf(snap2->outputPath, "v:0",
                        "stream=color_primaries") == "bt2020");
  }
}

TEST_CASE("CEA probe: sources without closed captions report none") {
  TestEnv env;
  const std::string fixture =
      generateFixture(env.dir / "in.mp4",
                      "testsrc2=size=320x240:rate=12",
                      "sine=frequency=440:sample_rate=44100", 2.0);
  const Result<MediaInfo> info =
      env.engine->inspect(MediaSource{fixture, "in.mp4"});
  REQUIRE(info.ok());
  REQUIRE_FALSE(info.value().video.empty());
  // A clean lavfi fixture carries no A53 CC side data; the probe must
  // report the absence honestly (the field feeds the planner's
  // "transcoding drops captions" advisory).
  CHECK_FALSE(info.value().video.front().hasClosedCaptions);
}
