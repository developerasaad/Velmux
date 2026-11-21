// Bridge integration tests: the stable C facade exercised exactly like a
// React Native TurboModule host would (JSON in, JSON out), against the REAL
// engine + FFmpeg backend. Output validity is cross-checked with ffprobe,
// never only with the code that produced the output.
#include "velmux_bridge.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::string ffmpegBin() {
#ifdef VELMUX_TEST_FFMPEG_PREFIX
  return std::string(VELMUX_TEST_FFMPEG_PREFIX) + "/bin/ffmpeg";
#else
  return "ffmpeg";
#endif
}

std::string ffprobeJson(const std::string &path) {
  const std::string cmd =
#ifdef VELMUX_TEST_FFPROBE
      std::string(VELMUX_TEST_FFPROBE) +
#else
      "ffprobe" +
#endif
      " -v error -show_entries format=duration,size -show_entries "
      "stream=codec_type,codec_name,width,height,sample_rate "
      "-of json \"" +
      path + "\"";
  FILE *pipe = ::popen(cmd.c_str(), "r");
  REQUIRE(pipe != nullptr);
  std::string out;
  char buf[4096];
  while (size_t n = std::fread(buf, 1, sizeof(buf), pipe)) {
    out.append(buf, n);
  }
  ::pclose(pipe);
  return out;
}

// Deterministic H.264+AAC MP4 fixture via the pinned build's ffmpeg binary
// (dev tooling only; the production backend never spawns processes).
std::string generateFixture(const std::filesystem::path &path,
                            double seconds) {
  const std::string cmd = ffmpegBin() +
      " -y -hide_banner -loglevel error "
      "-filter_complex \"testsrc2=size=320x240:rate=15[v];"
      "sine=frequency=440:sample_rate=44100[a]\" -map \"[v]\" -map \"[a]\" "
      "-c:v libx264 -preset ultrafast -crf 35 -c:a aac -b:a 64k -t " +
      std::to_string(seconds) + " " + path.string();
  REQUIRE(std::system(cmd.c_str()) == 0);
  REQUIRE(std::filesystem::file_size(path) > 0);
  return path.string();
}

std::string pollText(const char *s) { return s != nullptr ? s : "(null)"; }

struct BridgeEnv {
  std::filesystem::path dir;
  VelmuxEngineHandle *engine = nullptr;
  static inline int seq = 0;

  BridgeEnv() {
    dir = std::filesystem::temp_directory_path() /
          ("velmux-bridge-" + std::to_string(::getpid()) + "-" +
           std::to_string(seq++));
    std::filesystem::create_directories(dir);
    engine = velmux_engine_create(dir.string().c_str());
    REQUIRE(engine != nullptr);
  }
  ~BridgeEnv() {
    velmux_engine_destroy(engine);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
};

// Polls the facade until a terminal job state shows up in an event or the
// getJob snapshot, or the timeout expires.
struct BridgeJob {
  std::string id;
  std::string state;
  std::string outputPath;
};

std::string jsonStringField(const std::string &json, const std::string &key) {
  // Minimal extraction for test assertions (values are produced by our own
  // serializer, so plain substring search is adequate here).
  const std::size_t keyPos = json.find("\"" + key + "\":");
  if (keyPos == std::string::npos) {
    return "";
  }
  std::size_t pos = keyPos + key.size() + 3;
  while (pos < json.size() && (json[pos] == ' ')) {
    ++pos;
  }
  if (pos < json.size() && json[pos] == '"') {
    const std::size_t end = json.find('"', pos + 1);
    return json.substr(pos + 1, end - pos - 1);
  }
  const std::size_t end = json.find_first_of(",}", pos);
  return json.substr(pos, end - pos);
}

bool isTerminal(const std::string &state) {
  return state == "completed" || state == "failed" || state == "canceled";
}

BridgeJob waitForTerminal(BridgeEnv &env, const std::string &jobId,
                          int timeoutSec) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
  BridgeJob job{id: jobId, state: "unknown"};
  while (std::chrono::steady_clock::now() < deadline) {
    char *events = velmux_poll_events(env.engine);
    if (events != nullptr) {
      const std::string text = events;
      velmux_free_string(events);
      // Events are JSON objects in an array; scan for terminal states.
      std::size_t pos = 0;
      while ((pos = text.find("\"jobId\":", pos)) != std::string::npos) {
        const std::string eventJob = jsonStringField(text.substr(pos), "jobId");
        const std::string eventState =
            jsonStringField(text.substr(pos), "state");
        if (eventJob == jobId && isTerminal(eventState)) {
          job.state = eventState;
        }
        pos += 8;
      }
    }
    char *snapshotJson = velmux_get_job(env.engine, jobId.c_str());
    if (snapshotJson != nullptr) {
      const std::string snapshot = snapshotJson;
      velmux_free_string(snapshotJson);
      if (snapshot != "null") {
        const std::string state = jsonStringField(snapshot, "state");
        if (isTerminal(state)) {
          job.state = state;
          job.outputPath = jsonStringField(snapshot, "outputPath");
          return job;
        }
        if (job.state.empty() || !isTerminal(job.state)) {
          job.state = state;
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return job;
}

std::string validRequest(const std::string &uri) {
  return std::string("{\"source\":{\"uri\":\"") + uri +
         "\",\"displayName\":\"sample.mp4\"},"
         "\"outputContainer\":\"mp4\","
         "\"goal\":{\"kind\":\"custom\"},"
         "\"video\":{\"enabled\":true,\"codec\":\"h264\","
         "\"resolutionHeight\":144,\"rateControl\":\"bitrate\","
         "\"bitRate\":300000},"
         "\"audio\":{\"enabled\":true,\"codec\":\"aac\",\"bitRate\":64000,"
         "\"sampleRate\":44100,\"channels\":2},"
         "\"subtitles\":{\"policy\":\"preserve\",\"timingOffsetMs\":0},"
         "\"processingMode\":\"auto\"}";
}

} // namespace

TEST_CASE("requestFromJson validates the bridge contract") {
  std::string error;
  CHECK_FALSE(velmux::bridge::requestFromJson("not json", error).has_value());
  CHECK_FALSE(error.empty());

  CHECK_FALSE(
      velmux::bridge::requestFromJson("{\"source\":{}}", error).has_value());

  CHECK_FALSE(velmux::bridge::requestFromJson(
                  "{\"source\":{\"uri\":\"x\"},\"outputContainer\":\"zzz\"}",
                  error)
                  .has_value());

  CHECK_FALSE(velmux::bridge::requestFromJson(
                  "{\"source\":{\"uri\":\"x\"},\"goal\":{\"kind\":"
                  "\"fitUnderSize\"}}",
                  error)
                  .has_value());

  const auto request = velmux::bridge::requestFromJson(
      validRequest("/tmp/a.mp4"), error);
  REQUIRE(request.has_value());
  CHECK(error.empty());
  CHECK(request->source.uri == "/tmp/a.mp4");
  CHECK(request->outputContainer.has_value());
  CHECK(*request->outputContainer == velmux::core::Container::Mp4);
  CHECK(request->goal.kind == velmux::core::GoalKind::Custom);
  CHECK(request->video.codec == velmux::core::VideoCodec::H264);
  CHECK(request->video.resolutionHeight.has_value());
  CHECK(*request->video.resolutionHeight == 144);
  CHECK(request->video.bitRate == 300000);
  CHECK(request->audio.channels == 2);
  CHECK(request->subtitles.policy == velmux::core::SubtitlePolicy::Preserve);
  CHECK(request->processingMode == velmux::core::ProcessingMode::Auto);

  // Unicode + escapes survive the round trip.
  const auto unicode = velmux::bridge::requestFromJson(
      "{\"source\":{\"uri\":\"/tmp/\xc3\xa9\xe2\x9c\x93.mp4\","
      "\"displayName\":\"caf\\u00e9 \\\"quoted\\\" movie\"}}",
      error);
  REQUIRE(unicode.has_value());
  CHECK(unicode->source.uri.find("\xc3\xa9\xe2\x9c\x93") != std::string::npos);
  CHECK(unicode->source.displayName == "caf\303\251 \"quoted\" movie");
}

TEST_CASE("facade: inspect, plan, start, complete, validate via JSON") {
  BridgeEnv env;
  const std::string fixture =
      generateFixture(env.dir / "in.mp4", 2.0);

  SUBCASE("inspect returns real MediaInfo JSON") {
    char *info = velmux_inspect(env.engine, fixture.c_str(), "sample.mp4");
    REQUIRE(info != nullptr);
    const std::string text = info;
    velmux_free_string(info);
    CHECK(text.find("\"error\"") == std::string::npos);
    CHECK(text.find("\"container\":\"mp4\"") != std::string::npos);
    CHECK(text.find("\"codec\":\"h264\"") != std::string::npos);
    CHECK(text.find("\"codec\":\"aac\"") != std::string::npos);
    CHECK(text.find("\"durationSec\":2.") != std::string::npos);
  }

  SUBCASE("missing source reports an error object") {
    char *info = velmux_inspect(env.engine,
                                (env.dir / "nope.mp4").string().c_str(), "x");
    REQUIRE(info != nullptr);
    const std::string text = info;
    velmux_free_string(info);
    CHECK(text.find("\"error\"") != std::string::npos);
  }

  SUBCASE("plan is feasible for a real transcode request") {
    char *plan = velmux_plan(env.engine, validRequest(fixture).c_str());
    REQUIRE(plan != nullptr);
    const std::string text = plan;
    velmux_free_string(plan);
    CHECK(text.find("\"feasible\":true") != std::string::npos);
    CHECK(text.find("\"pipelineSummary\"") != std::string::npos);
  }

  SUBCASE("startJob runs a real conversion and poll observes completion") {
    char *started = velmux_start_job(env.engine, validRequest(fixture).c_str());
    REQUIRE(started != nullptr);
    const std::string startText = started;
    velmux_free_string(started);
    CHECK(startText.find("\"error\"") == std::string::npos);
    const std::string jobId = jsonStringField(startText, "jobId");
    CHECK_FALSE(jobId.empty());

    const BridgeJob job = waitForTerminal(env, jobId, 120);
    CHECK(job.state == "completed");
    CHECK(std::filesystem::exists(job.outputPath));

    // Independent validation: ffprobe must parse the output and see the
    // requested codecs and resolution.
    const std::string probe = ffprobeJson(job.outputPath);
    CHECK(probe.find("\"codec_name\": \"h264\"") != std::string::npos);
    CHECK(probe.find("\"codec_name\": \"aac\"") != std::string::npos);
    CHECK(probe.find("\"height\": 144") != std::string::npos);
  }

  SUBCASE("invalid request JSON reports an error object") {
    char *started = velmux_start_job(env.engine, "{\"bogus\":1}");
    REQUIRE(started != nullptr);
    const std::string text = started;
    velmux_free_string(started);
    CHECK(text.find("\"error\"") != std::string::npos);
  }
}

TEST_CASE("facade: listJobs and describeBackends speak JSON") {
  BridgeEnv env;
  char *backends = velmux_describe_backends(env.engine);
  REQUIRE(backends != nullptr);
  const std::string backendText = backends;
  velmux_free_string(backends);
  CHECK(backendText.find("FFmpeg") != std::string::npos);

  char *jobs = velmux_list_jobs(env.engine);
  REQUIRE(jobs != nullptr);
  const std::string jobsText = jobs;
  velmux_free_string(jobs);
  CHECK(jobsText == "[]");
}
