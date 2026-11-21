// Planner unit tests: candidate generation, compatibility validation, cost
// ranking, stream-copy preference and warm-path planning latency.
#include "velmux/core/planner.h"

#include <doctest/doctest.h>

#include <chrono>

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
  CHECK(plan.best->pipeline.summary.find("remux") != std::string::npos);
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
  CHECK(plan.message.find("subtitle") != std::string::npos);
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
  request.audio.codec = VideoCodec::Aac;
  request.outputContainer = Container::Mp4;

  const PlanResult plan = planner.plan(source, request);
  REQUIRE(plan.feasible);
  REQUIRE(plan.alternates.size() >= 2);
  for (std::size_t i = 1; i < plan.alternates.size(); ++i) {
    CHECK(plan.alternates[i - 1].cost <= plan.alternates[i].cost);
  }
}
