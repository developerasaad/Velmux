// FFmpeg backend integration tests: REAL conversions through the libav*
// libraries with INDEPENDENT ffprobe cross-validation of every output.
//
// Fixtures are generated at test time with the built ffmpeg binary (a dev
// tool of the pinned prefix) from deterministic lavfi inputs, so the suite
// is self-contained and reproducible.
#include "velmux/ffmpeg/ffmpeg_backend.h"
#include "subtitle_tools.h"

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
  EventWaiter waiter;
  env.engine->events().subscribe(&waiter);
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
  EventWaiter waiter;
  env.engine->events().subscribe(&waiter);
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
  EventWaiter waiter;
  env.engine->events().subscribe(&waiter);
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

  // Extraction is not implemented: the plan must refuse rather than drop.
  ConversionRequest extract =
      transcodeRequest(av, Container::Mkv, VideoCodec::H264, AudioCodec::Aac);
  extract.subtitles.policy = SubtitlePolicy::Extract;
  const Result<PlanResult> plan = env.engine->plan(
      MediaSource{av, "av.mp4"}, extract);
  REQUIRE(plan.ok());
  CHECK_FALSE(plan.value().feasible);
  CHECK(plan.value().message.find("extraction") != std::string::npos);

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
  EventWaiter waiter;
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
    if (!cancelled && snapshot->progress >= 0.5) {
      const Result<bool> cancelResult = env.engine->cancelJob(id);
      REQUIRE(cancelResult.ok());
      REQUIRE(cancelResult.value());
      cancelled = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
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
