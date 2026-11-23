// Planner unit tests: candidate generation, compatibility validation, cost
// ranking, stream-copy preference and warm-path planning latency.
#include "velmux/core/planner.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cctype>

using namespace velmux::core;

namespace {

class FakeTelemetry final : public ITelemetryQuery {
public:
  std::optional<ThroughputSample>
  throughputFor(BackendId backend, const std::optional<VideoCodec> &video,
                const std::optional<AudioCodec> &audio) const override {
    if (!learned_) {
      return std::nullopt;
    }
    ThroughputSample sample;
    sample.backend = backend;
    sample.videoCodec = video;
    sample.audioCodec = audio;
    sample.framesPerSecond = 123.0;
    sample.setupMs = 5.0;
    sample.transferMsPerFrame = 0.1;
    sample.sampleCount = 3;
    return sample;
  }
  double planningLatencyP50Ms() const override { return 0.5; }
  std::int32_t planningSampleCount() const override { return 10; }
  bool learned_ = false;
};

struct Fixture {
  CapabilityRegistry caps;
  FakeTelemetry telemetry;
  Planner planner{FeatureRegistry::instance(), caps, &telemetry};

  Fixture() {
    BackendCapabilities ffmpeg;
    ffmpeg.backend = BackendId::Ffmpeg;
    ffmpeg.implementation = "test";
    ffmpeg.canInspect = true;
    for (const VideoCodec v :
         {VideoCodec::H264, VideoCodec::HEVC, VideoCodec::AV1,
          VideoCodec::VP9, VideoCodec::Mpeg4Part2}) {
      ffmpeg.video.push_back(
          VideoCapability{v, {true, true, true}, false, 0, 0});
    }
    for (const AudioCodec a :
         {AudioCodec::Aac, AudioCodec::Mp3, AudioCodec::Opus,
          AudioCodec::Flac, AudioCodec::Pcm}) {
      ffmpeg.audio.push_back(AudioCapability{a, {true, true, true}});
    }
    ffmpeg.readableContainers = {Container::Mp4, Container::Mkv,
                                 Container::WebM, Container::Mov};
    ffmpeg.writableContainers = {Container::Mp4, Container::Mkv,
                                 Container::WebM, Container::Mov};
    ffmpeg.subtitleExtract = true;
    ffmpeg.timeTrim = true;
    ffmpeg.videoScaling = true;
    ffmpeg.audioResampling = true;

    BackendCapabilities android;
    android.backend = BackendId::Android;
    android.implementation = "test-hw";
    android.canInspect = true;
    android.video.push_back(
        VideoCapability{VideoCodec::H264, {true, true, true}, true, 3840,
                        2160});
    android.audio.push_back(
        AudioCapability{AudioCodec::Aac, {true, true, true}});
    android.readableContainers = {Container::Mp4, Container::Mkv};
    android.writableContainers = {Container::Mp4};

    caps.registerBackend(ffmpeg);
    caps.registerBackend(android);
  }

  MediaInfo h264AacSource() {
    MediaInfo info;
    info.durationSec = 60.0;
    info.container = Container::Mp4;
    info.sizeBytes = 50'000'000;
    VideoStream video;
    video.codec = VideoCodec::H264;
    video.width = 1920;
    video.height = 1080;
    video.frameRate = 30.0;
    video.bitRate = 6'000'000;
    info.video.push_back(video);
    AudioStream audio;
    audio.codec = AudioCodec::Aac;
    audio.channels = 2;
    audio.sampleRate = 48000;
    audio.bitRate = 128'000;
    info.audio.push_back(audio);
    return info;
  }
};

} // namespace

TEST_CASE_FIXTURE(Fixture, "same-codec remux prefers stream copy") {
  const MediaInfo source = h264AacSource();
  ConversionRequest request;
  request.outputContainer = Container::Mkv; // h264+aac both valid in mkv
  request.video.codec = VideoCodec::H264;
  request.audio.codec = AudioCodec::Aac;

  const PlanResult plan = planner.plan(source, request);
  REQUIRE(plan.feasible);
  REQUIRE(plan.best.has_value());
  CHECK(plan.best->pipeline.videoStreamCopy);
  CHECK(plan.best->pipeline.audioStreamCopy);
  // Summary names the remux path (capital R).
  CHECK(plan.best->pipeline.summary.find("Remux") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "transcode requests re-encode with valid stages") {
  const MediaInfo source = h264AacSource();
  ConversionRequest request;
  request.outputContainer = Container::WebM;
  request.video.codec = VideoCodec::VP9;
  request.audio.codec = AudioCodec::Opus;

  const PlanResult plan = planner.plan(source, request);
  REQUIRE(plan.feasible);
  REQUIRE(plan.best.has_value());
  CHECK_FALSE(plan.best->pipeline.videoStreamCopy);
  CHECK(plan.best->pipeline.videoCodec == VideoCodec::VP9);
  CHECK(plan.best->pipeline.audioCodec == AudioCodec::Opus);
  CHECK(plan.best->pipeline.container == Container::WebM);
  // All essential stages are assigned.
  CHECK(plan.best->pipeline.assigns(StageId::Demux, BackendId::Ffmpeg));
  CHECK(plan.best->pipeline.assigns(StageId::Decode, BackendId::Ffmpeg));
  CHECK(plan.best->pipeline.assigns(StageId::Encode, BackendId::Ffmpeg));
  CHECK(plan.best->pipeline.assigns(StageId::Mux, BackendId::Ffmpeg));
}

TEST_CASE_FIXTURE(Fixture, "invalid codec/container pairs are rejected") {
  const MediaInfo source = h264AacSource();
  ConversionRequest request;
  // H.264 cannot go into WebM (feature registry says no).
  request.outputContainer = Container::WebM;
  request.video.codec = VideoCodec::H264;

  const PlanResult plan = planner.plan(source, request);
  CHECK_FALSE(plan.feasible);
  CHECK_FALSE(plan.message.empty());
}

TEST_CASE_FIXTURE(Fixture,
                  "subtitle preservation incompatibility is surfaced, not dropped") {
  MediaInfo source = h264AacSource();
  SubtitleStream sub;
  sub.format = SubtitleFormat::Ass; // ASS is not valid in MP4
  source.subtitles.push_back(sub);

  ConversionRequest request;
  request.outputContainer = Container::Mp4;
  request.video.codec = VideoCodec::H264;
  request.subtitles.policy = SubtitlePolicy::Preserve;

  const PlanResult plan = planner.plan(source, request);
  CHECK_FALSE(plan.feasible);
  // Case-insensitive: the message names the subtitle problem.
  std::string lower;
  for (char c : plan.message) {
    lower.push_back(char(std::tolower(static_cast<unsigned char>(c))));
  }
  CHECK(lower.find("subtitle") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "explicit subtitle removal avoids the conflict") {
  MediaInfo source = h264AacSource();
  SubtitleStream sub;
  sub.format = SubtitleFormat::Ass;
  source.subtitles.push_back(sub);

  ConversionRequest request;
  request.outputContainer = Container::Mp4;
  request.video.codec = VideoCodec::H264;
  request.subtitles.policy = SubtitlePolicy::Remove;

  const PlanResult plan = planner.plan(source, request);
  CHECK(plan.feasible);
}

TEST_CASE_FIXTURE(Fixture, "no capable backend means honest infeasibility") {
  const MediaInfo source = h264AacSource();
  ConversionRequest request;
  request.video.codec = VideoCodec::Ffv1; // no backend in this fixture encodes FFV1
  request.outputContainer = Container::Mkv;

  const PlanResult plan = planner.plan(source, request);
  CHECK_FALSE(plan.feasible);
  CHECK(plan.message.find("backend") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "processing preference influences ranking") {
  const MediaInfo source = h264AacSource();
  ConversionRequest request;
  request.video.codec = VideoCodec::VP9; // only FFmpeg serves VP9 here
  request.audio.codec = AudioCodec::Opus;
  request.outputContainer = Container::WebM;

  // Prefer native: no native backend can do VP9, so FFmpeg still wins —
  // validity outranks preference (spec §4.6).
  request.processingMode = ProcessingMode::PreferNative;
  const PlanResult plan = planner.plan(source, request);
  REQUIRE(plan.feasible);
  REQUIRE(plan.best.has_value());
  CHECK(plan.best->pipeline.assigns(StageId::Encode, BackendId::Ffmpeg));
}

TEST_CASE_FIXTURE(Fixture, "telemetry improves cost estimates") {
  const MediaInfo source = h264AacSource();
  ConversionRequest request;
  request.video.codec = VideoCodec::AV1;
  request.audio.codec = AudioCodec::Opus;
  request.outputContainer = Container::Mkv;

  const PlanResult cold = planner.plan(source, request);
  REQUIRE(cold.feasible);
  telemetry.learned_ = true;
  const PlanResult warm = planner.plan(source, request);
  REQUIRE(warm.feasible);
  // With learned throughput the estimated throughput reflects the sample.
  CHECK(warm.best->expectedThroughput == doctest::Approx(123.0));
}

TEST_CASE_FIXTURE(Fixture, "warm-path planning latency target") {
  // Spec §4.2: pipeline planning p95 under ~5 ms on the warm path. The
  // registry is pre-populated (warm); planning is pure computation. The
  // assertion budget is generous (CI machines vary) but still catches
  // accidental probing/enumeration inside plan().
  const MediaInfo source = h264AacSource();
  ConversionRequest request;
  request.video.codec = VideoCodec::VP9;
  request.audio.codec = AudioCodec::Opus;
  request.outputContainer = Container::WebM;

  // Warm up once (branch predictors, caches).
  (void)planner.plan(source, request);

  const auto start = std::chrono::steady_clock::now();
  constexpr int kIterations = 1000;
  for (int i = 0; i < kIterations; ++i) {
    (void)planner.plan(source, request);
  }
  const double perCallMs =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                start)
          .count() /
      kIterations;
  CHECK(perCallMs < 5.0);
}

TEST_CASE_FIXTURE(Fixture, "alternates are ranked best-first") {
  const MediaInfo source = h264AacSource();
  ConversionRequest request;
  request.video.codec = VideoCodec::H264;
  request.audio.codec = AudioCodec::Aac;
  request.outputContainer = Container::Mp4;

  const PlanResult plan = planner.plan(source, request);
  REQUIRE(plan.feasible);
  REQUIRE(plan.alternates.size() >= 2);
  for (std::size_t i = 1; i < plan.alternates.size(); ++i) {
    CHECK(plan.alternates[i - 1].cost <= plan.alternates[i].cost);
  }
}

// ---------------------------------------------------------------------------
// Milestone 4: hybrid candidate generation
// ---------------------------------------------------------------------------

namespace {

// Scenario registry: FFmpeg decodes VP9 (and stage-decodes to CPU frames)
// but cannot encode HEVC; Android encodes HEVC (stage-encodes from CPU
// frames) but cannot decode VP9. Only a hybrid pipeline can serve
// VP9 -> HEVC.
CapabilityRegistry hybridScenarioCaps() {
  CapabilityRegistry caps;
  BackendCapabilities ffmpeg;
  ffmpeg.backend = BackendId::Ffmpeg;
  ffmpeg.implementation = "scenario";
  ffmpeg.canInspect = true;
  ffmpeg.video.push_back(VideoCapability{VideoCodec::VP9,
                                         {true, false, true}, false, 0, 0});
  ffmpeg.video.push_back(VideoCapability{VideoCodec::H264,
                                         {true, true, true}, false, 0, 0});
  ffmpeg.audio.push_back(AudioCapability{AudioCodec::Aac, {true, true, true}});
  ffmpeg.readableContainers = {Container::Mp4};
  ffmpeg.writableContainers = {Container::Mp4};
  ffmpeg.stageDecodeToCpuFrames = true;
  ffmpeg.timeTrim = true;
  ffmpeg.videoScaling = true;
  ffmpeg.audioResampling = true;

  BackendCapabilities android;
  android.backend = BackendId::Android;
  android.implementation = "scenario-hw";
  android.canInspect = true;
  android.video.push_back(VideoCapability{VideoCodec::HEVC,
                                          {false, true, true}, true, 3840,
                                          2160});
  android.audio.push_back(AudioCapability{AudioCodec::Aac, {true, true, true}});
  android.readableContainers = {Container::Mp4};
  android.writableContainers = {Container::Mp4};
  android.stageEncodeFromCpuFrames = true;

  caps.registerBackend(ffmpeg);
  caps.registerBackend(android);
  return caps;
}

} // namespace

TEST_CASE_FIXTURE(Fixture, "hybrid candidates compose backends when no single backend suffices") {
  CapabilityRegistry scenario = hybridScenarioCaps();
  Planner scenarioPlanner{FeatureRegistry::instance(), scenario, nullptr};

  MediaInfo source = h264AacSource();
  source.video.front().codec = VideoCodec::VP9;

  ConversionRequest request;
  request.outputContainer = Container::Mp4;
  request.goal.kind = GoalKind::Custom;
  request.video.codec = VideoCodec::HEVC;
  request.audio.codec = AudioCodec::Aac;

  const PlanResult plan = scenarioPlanner.plan(source, request);
  REQUIRE(plan.feasible);
  REQUIRE(plan.best.has_value());
  const Pipeline &pipeline = plan.best->pipeline;
  CHECK_FALSE(pipeline.videoStreamCopy);
  // The decode side is FFmpeg (the only VP9 decoder); the encode/mux side
  // is Android (the only HEVC encoder).
  CHECK(pipeline.assigns(StageId::Decode, BackendId::Ffmpeg));
  CHECK(pipeline.assigns(StageId::Demux, BackendId::Ffmpeg));
  CHECK(pipeline.assigns(StageId::Encode, BackendId::Android));
  CHECK(pipeline.assigns(StageId::Mux, BackendId::Android));
  CHECK(pipeline.summary.find("Hybrid") != std::string::npos);
  // Inspected source rate reaches the pipeline for encoder/muxer timing.
  CHECK(pipeline.sourceFrameRate == doctest::Approx(30.0));

  // Sanity: single-backend scenarios never degrade to hybrid (the normal
  // Fixture registry serves H.264 -> HEVC through FFmpeg alone).
  ConversionRequest single = request;
  single.video.codec = VideoCodec::HEVC;
  source.video.front().codec = VideoCodec::H264;
  const PlanResult classic = planner.plan(source, single);
  REQUIRE(classic.feasible);
  REQUIRE(classic.best.has_value());
  CHECK(classic.best->pipeline.assigns(StageId::Decode, BackendId::Ffmpeg));
  CHECK(classic.best->pipeline.assigns(StageId::Encode, BackendId::Ffmpeg));
}

TEST_CASE_FIXTURE(Fixture, "trimmed requests route only to trim-capable backends") {
  // A trim window is honored exclusively by backends reporting timeTrim;
  // anything else would silently produce full-length output.
  CapabilityRegistry scenario = hybridScenarioCaps();
  Planner scenarioPlanner{FeatureRegistry::instance(), scenario, nullptr};

  MediaInfo source = h264AacSource();
  source.video.front().codec = VideoCodec::VP9;

  ConversionRequest request;
  request.outputContainer = Container::Mp4;
  request.goal.kind = GoalKind::Custom;
  request.video.codec = VideoCodec::HEVC;
  request.audio.codec = AudioCodec::Aac;
  request.trim.startUs = 500000;
  request.trim.endUs = 1500000;

  // Without the trim this exact scenario plans a hybrid route (tested
  // above); with the trim it must refuse rather than mis-execute.
  const PlanResult hybridShape = scenarioPlanner.plan(source, request);
  CHECK_FALSE(hybridShape.feasible);

  // Positive control: the Fixture FFmpeg reports timeTrim and serves the
  // same trimmed request end-to-end.
  MediaInfo plain = h264AacSource();
  ConversionRequest routed = request;
  routed.video.codec = VideoCodec::H264;
  const PlanResult plan = planner.plan(plain, routed);
  REQUIRE(plan.feasible);
  REQUIRE(plan.best.has_value());
  CHECK(plan.best->pipeline.assigns(StageId::Decode, BackendId::Ffmpeg));
  CHECK(plan.best->pipeline.assigns(StageId::Mux, BackendId::Ffmpeg));

  // A trim-incapable-only registry refuses: native Android alone (no
  // timeTrim) must never receive the trimmed request.
  CapabilityRegistry nativeOnly;
  BackendCapabilities android;
  android.backend = BackendId::Android;
  android.implementation = "native";
  android.canInspect = true;
  android.video.push_back(
      VideoCapability{VideoCodec::H264, {true, true, true}, true, 3840, 2160});
  android.audio.push_back(
      AudioCapability{AudioCodec::Aac, {true, true, true}});
  android.readableContainers = {Container::Mp4};
  android.writableContainers = {Container::Mp4};
  nativeOnly.registerBackend(android);
  Planner nativePlanner{FeatureRegistry::instance(), nativeOnly, nullptr};
  const PlanResult refused = nativePlanner.plan(plain, routed);
  CHECK_FALSE(refused.feasible);
}

TEST_CASE_FIXTURE(Fixture, "subtitle extraction plans a sidecar pipeline") {
  MediaInfo source = h264AacSource();
  SubtitleStream sub;
  sub.format = SubtitleFormat::Srt;
  source.subtitles.push_back(sub);

  ConversionRequest request;
  request.outputContainer = Container::Mkv;
  request.goal.kind = GoalKind::Custom;
  request.video.enabled = false;
  request.audio.enabled = false;
  request.subtitles.policy = SubtitlePolicy::Extract;

  const PlanResult plan = planner.plan(source, request);
  REQUIRE(plan.feasible);
  REQUIRE(plan.best.has_value());
  const Pipeline &pipeline = plan.best->pipeline;
  CHECK(pipeline.assigns(StageId::Demux, BackendId::Ffmpeg));
  CHECK(pipeline.assigns(StageId::SubtitleProcessing, BackendId::Ffmpeg));
  CHECK(pipeline.stages.size() == 2);
  CHECK(pipeline.sidecarExtension == "srt");
  CHECK(pipeline.summary.find("sidecar") != std::string::npos);

  // Container-internal text decodes to SRT; ASS-family keeps its format.
  source.subtitles.front().format = SubtitleFormat::MovText;
  const PlanResult movText = planner.plan(source, request);
  REQUIRE(movText.feasible);
  CHECK(movText.best->pipeline.sidecarExtension == "srt");

  source.subtitles.front().format = SubtitleFormat::Ass;
  const PlanResult ass = planner.plan(source, request);
  REQUIRE(ass.feasible);
  CHECK(ass.best->pipeline.sidecarExtension == "ass");
}

TEST_CASE_FIXTURE(Fixture, "subtitle extraction refuses impossible shapes") {
  MediaInfo source = h264AacSource();

  SUBCASE("bitmap formats need OCR (outside V1)") {
    SubtitleStream sub;
    sub.format = SubtitleFormat::Pgs;
    source.subtitles.push_back(sub);
    ConversionRequest request;
    request.goal.kind = GoalKind::Custom;
    request.video.enabled = false;
    request.audio.enabled = false;
    request.subtitles.policy = SubtitlePolicy::Extract;
    const PlanResult plan = planner.plan(source, request);
    CHECK_FALSE(plan.feasible);
    CHECK(plan.message.find("OCR") != std::string::npos);
  }

  SUBCASE("mixed convert+extract is refused") {
    SubtitleStream sub;
    sub.format = SubtitleFormat::Srt;
    source.subtitles.push_back(sub);
    ConversionRequest request;
    request.goal.kind = GoalKind::Custom;
    request.video.enabled = true;
    request.subtitles.policy = SubtitlePolicy::Extract;
    const PlanResult plan = planner.plan(source, request);
    CHECK_FALSE(plan.feasible);
    CHECK(plan.message.find("standalone") != std::string::npos);
  }
}
