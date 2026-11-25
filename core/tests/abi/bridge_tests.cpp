// Bridge integration tests: the stable C facade exercised exactly like a
// React Native TurboModule host would (JSON in, JSON out), against the REAL
// engine + FFmpeg backend. Output validity is cross-checked with ffprobe,
// never only with the code that produced the output.
//
// ABI v2: requests travel as the typed JobRequest JSON envelope
// ({"operation":"video-convert","video-convert":{...}}), submitted through
// velmux_submit_job; the request codec under test is the core's strict
// requestFromJson (serialization.cpp) — the same validation path the
// facade and the durable job store use.
#include <velmux/abi.h>
#include "api/serialization.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

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
  return state == "completed" || state == "failed" || state == "canceled" ||
         state == "target-not-met";
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

// The typed request JSON the ABI v2 facade accepts: a real transcode with
// an explicit resolution, bitrate-driven video and concrete audio settings.
// Mirrors the old flat bridge request (outputContainer mp4, video h264
// height 144 bitrate 300k, audio aac 64k/44100/stereo, policy preserve).
std::string validRequest(const std::string &uri) {
  return std::string("{\"operation\":\"video-convert\","
                     "\"submissionId\":\"u-bridge-sample\","
                     "\"video-convert\":{"
                     "\"source\":{\"uri\":\"") +
         uri + "\",\"name\":\"sample.mp4\"},"
               "\"output\":{\"name\":null,\"container\":\"mp4\"},"
               "\"video\":{\"codec\":\"h264\",\"resolutionHeight\":144},"
               "\"audioSettings\":{\"codec\":\"aac\",\"bitRate\":64000,"
               "\"sampleRate\":44100,\"channels\":2},"
               "\"compression\":{\"enabled\":true,\"preset\":\"custom\","
               "\"targetSizeBytes\":0,\"quality\":null,"
               "\"videoBitrate\":300000,\"audioBitrate\":null}}}";
}

} // namespace

TEST_CASE("requestFromJson validates the bridge contract") {
  std::string error;
  CHECK_FALSE(velmux::core::requestFromJson("not json", error).has_value());
  CHECK_FALSE(error.empty());

  // Missing the operation's payload object.
  CHECK_FALSE(velmux::core::requestFromJson(
                  "{\"operation\":\"video-convert\"}", error)
                  .has_value());

  // Source without a uri.
  CHECK_FALSE(velmux::core::requestFromJson(
                  "{\"operation\":\"video-convert\",\"video-convert\":{"
                  "\"source\":{\"name\":\"x\"},\"output\":{},\"video\":{}}}",
                  error)
                  .has_value());

  // Unknown output container.
  CHECK_FALSE(velmux::core::requestFromJson(
                  "{\"operation\":\"video-convert\",\"video-convert\":{"
                  "\"source\":{\"uri\":\"x\",\"name\":\"x\"},"
                  "\"output\":{\"container\":\"zzz\"},\"video\":{}}}",
                  error)
                  .has_value());

  // Target-size compression without a usable target (the old
  // fitUnderSize-without-target refusal).
  CHECK_FALSE(velmux::core::requestFromJson(
                  "{\"operation\":\"video-convert\",\"video-convert\":{"
                  "\"source\":{\"uri\":\"x\",\"name\":\"x\"},"
                  "\"output\":{},\"video\":{},"
                  "\"compression\":{\"enabled\":true,"
                  "\"preset\":\"target-size\",\"targetSizeBytes\":0}}}",
                  error)
                  .has_value());

  const auto decoded =
      velmux::core::requestFromJson(validRequest("/tmp/a.mp4"), error);
  REQUIRE(decoded.has_value());
  CHECK(error.empty());
  CHECK(decoded->submissionId == "u-bridge-sample");
  CHECK(decoded->request.operation == velmux::core::Operation::VideoConvert);
  const auto &payload =
      std::get<velmux::core::job::VideoConvert>(decoded->request.payload);
  CHECK(payload.source.uri == "/tmp/a.mp4");
  CHECK(payload.source.displayName == "sample.mp4");
  REQUIRE(payload.output.container.has_value());
  CHECK(*payload.output.container == velmux::core::Container::Mp4);
  REQUIRE(payload.video.codec.has_value());
  CHECK(*payload.video.codec == velmux::core::VideoCodec::H264);
  REQUIRE(payload.video.resolutionHeight.has_value());
  CHECK(*payload.video.resolutionHeight == 144);
  REQUIRE(payload.compression.videoBitrate.has_value());
  CHECK(*payload.compression.videoBitrate == 300000);
  CHECK(payload.audioSettings.channels == 2);
  REQUIRE(payload.audioSettings.sampleRate.has_value());
  CHECK(*payload.audioSettings.sampleRate == 44100);
  CHECK(payload.audioSettings.bitRate == 64000);
  // No subtitle rules: preserve all (the old default policy).
  CHECK(payload.subtitleTracks.empty());

  // Unicode + escapes survive the round trip.
  const auto unicode = velmux::core::requestFromJson(
      "{\"operation\":\"video-convert\",\"video-convert\":{"
      "\"source\":{\"uri\":\"/tmp/\xc3\xa9\xe2\x9c\x93.mp4\","
      "\"name\":\"caf\\u00e9 \\\"quoted\\\" movie\"},"
      "\"output\":{},\"video\":{}}}",
      error);
  REQUIRE(unicode.has_value());
  const auto &unicodePayload =
      std::get<velmux::core::job::VideoConvert>(unicode->request.payload);
  CHECK(unicodePayload.source.uri.find("\xc3\xa9\xe2\x9c\x93") !=
        std::string::npos);
  CHECK(unicodePayload.source.displayName ==
        "caf\303\251 \"quoted\" movie");
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

  SUBCASE("plan is feasible for a real conversion request") {
    // ABI v2 plans through configuration sessions: create + prepare runs
    // the real inspection and the feasibility assessment.
    char *session = velmux_create_session(
        env.engine,
        ("{\"operation\":\"video-convert\",\"sources\":[{\"uri\":\"" +
         fixture + "\",\"name\":\"in.mp4\"}]}")
            .c_str());
    REQUIRE(session != nullptr);
    const std::string sessionText = session;
    velmux_free_string(session);
    CHECK(sessionText.find("\"error\"") == std::string::npos);
    const std::string sessionId = jsonStringField(sessionText, "sessionId");
    REQUIRE_FALSE(sessionId.empty());

    char *prepared = velmux_prepare_session(env.engine, sessionId.c_str());
    REQUIRE(prepared != nullptr);
    const std::string text = prepared;
    velmux_free_string(prepared);
    CHECK(text.find("\"error\"") == std::string::npos);
    CHECK(text.find("\"state\":\"ready\"") != std::string::npos);
    CHECK(text.find("\"configurationValid\":true") != std::string::npos);
    CHECK(text.find("\"estimate\"") != std::string::npos);
    CHECK(velmux_close_session(env.engine, sessionId.c_str()) == 0);
  }

  SUBCASE("submitJob runs a real conversion and poll observes completion") {
    char *started = velmux_submit_job(env.engine, validRequest(fixture).c_str());
    REQUIRE(started != nullptr);
    const std::string startText = started;
    velmux_free_string(started);
    INFO("submit response: " << startText);
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
    char *started = velmux_submit_job(env.engine, "{\"bogus\":1}");
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

TEST_CASE("facade: resource updates and job retry/delete") {
  BridgeEnv env;

  // Resource updates: well-formed payloads apply, garbage is rejected.
  CHECK(velmux_update_resources(env.engine,
                                "{\"cpuCores\":4,\"thermalPressure\":0.8,"
                                "\"onBattery\":true}") == 0);
  CHECK(velmux_update_resources(env.engine, "{\"cpuCores\":\"many\"}") == 0);
  CHECK(velmux_update_resources(env.engine, "not json") == -1);
  CHECK(velmux_update_resources(env.engine, nullptr) == -1);

  // A real conversion job submitted through the typed JSON (with the
  // subtitle-removal rule, the v1 policy the facade carries).
  const std::string fixture = generateFixture(env.dir / "in.mp4", 1.0);
  const std::string requestJson = R"({
    "operation": "video-convert",
    "video-convert": {
      "source": {"uri": ")" + fixture + R"(", "name": "in.mp4"},
      "output": {"name": null, "container": "mp4"},
      "video": {"codec": "h264"},
      "audioSettings": {"codec": "aac"},
      "subtitleTracks": [{"index": -1, "policy": "remove"}]
    }
  })";
  char *started = velmux_submit_job(env.engine, requestJson.c_str());
  REQUIRE(started != nullptr);
  const std::string startedText = started;
  velmux_free_string(started);
  REQUIRE(startedText.find("\"jobId\"") != std::string::npos);
  const auto idBegin = startedText.find("\"jobId\":\"") + 9;
  const auto idEnd = startedText.find('"', idBegin);
  REQUIRE(idEnd != std::string::npos);
  const std::string jobId = startedText.substr(idBegin, idEnd - idBegin);

  // Wait for terminal state through the event tap.
  std::string terminalState;
  for (int i = 0; i < 600 && terminalState.empty(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    char *events = velmux_poll_events(env.engine);
    if (events != nullptr) {
      const std::string text = events;
      velmux_free_string(events);
      if (text.find("\"completed\"") != std::string::npos) {
        terminalState = "completed";
      } else if (text.find("\"failed\"") != std::string::npos) {
        terminalState = "failed";
      }
    }
  }
  REQUIRE(terminalState == "completed");

  // Worker liveness: a terminal job has no executing worker, unknown ids
  // fail cleanly, and null arguments are rejected.
  CHECK(velmux_is_job_active(env.engine, jobId.c_str()) == 0);
  CHECK(velmux_is_job_active(env.engine, "no-such-job") == 0);
  CHECK(velmux_is_job_active(nullptr, jobId.c_str()) == -1);
  CHECK(velmux_is_job_active(env.engine, nullptr) == -1);

  // Retrying a completed job is refused (nothing to retry).
  CHECK(velmux_retry_job(env.engine, jobId.c_str()) == -1);

  // Deleting a terminal job removes the durable record.
  CHECK(velmux_delete_job(env.engine, jobId.c_str()) == 1);
  char *after = velmux_list_jobs(env.engine);
  REQUIRE(after != nullptr);
  const std::string afterText = after;
  velmux_free_string(after);
  CHECK(afterText == "[]");
  // Unknown/already-deleted jobs fail cleanly.
  CHECK(velmux_delete_job(env.engine, jobId.c_str()) == -1);
  CHECK(velmux_retry_job(env.engine, "no-such-job") == -1);
}
