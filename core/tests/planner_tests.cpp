// Planner unit tests: candidate generation, compatibility validation, cost
// ranking, stream-copy preference and warm-path planning latency.
#include "planning/planner.h"

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
    ffmpeg.timelineOps = true;
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

  // A plain VideoConvert request (the conversion-family workhorse).
  JobRequest convertRequest(Container out, VideoCodec video,
                            AudioCodec audio) {
    JobRequest request;
    request.operation = Operation::VideoConvert;
    job::VideoConvert payload;
    payload.source = MediaSource{"test-src.mp4", "test-src.mp4"};
    payload.output.container = out;
    payload.video.codec = video;
    payload.audioSettings.codec = audio;
    request.payload = std::move(payload);
    return request;
  }

  PlanResult planOf(const JobRequest &request, const MediaInfo &source) {
    const Result<PlanResult> planned = planner.plan(request, {source});
    REQUIRE(planned.ok());
    return planned.value();
  }
};

} // namespace

TEST_CASE_FIXTURE(Fixture, "same-codec remux prefers stream copy") {
  const MediaInfo source = h264AacSource();
  const JobRequest request =
      convertRequest(Container::Mkv, VideoCodec::H264, AudioCodec::Aac);

  const PlanResult plan = planOf(request, source);
  REQUIRE(plan.feasible);
  REQUIRE(plan.best.has_value());
  CHECK(plan.best->pipeline.videoStreamCopy);
  CHECK(plan.best->pipeline.audioStreamCopy);
  // Summary names the remux path (capital R).
  CHECK(plan.best->pipeline.summary.find("Remux") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "automatic codec routing preserves the source codec") {
  // nullopt codec = Automatic: keep the compatible source codec (H.264+AAC
  // into MKV) — the plan is a stream-copy remux, never a pointless
  // re-encode (mandate §19).
  const MediaInfo source = h264AacSource();
  JobRequest request;
  request.operation = Operation::VideoConvert;
  job::VideoConvert payload;
  payload.source = MediaSource{"test-src.mp4", "test-src.mp4"};
  payload.output.container = Container::Mkv;
  request.payload = std::move(payload);

  const PlanResult plan = planOf(request, source);
  REQUIRE(plan.feasible);
  REQUIRE(plan.best.has_value());
  CHECK(plan.best->pipeline.videoCodec == VideoCodec::H264);
  CHECK(plan.best->pipeline.audioCodec == AudioCodec::Aac);
  CHECK(plan.best->pipeline.videoStreamCopy);
}

TEST_CASE_FIXTURE(Fixture, "transcode requests re-encode with valid stages") {
  const MediaInfo source = h264AacSource();
  const JobRequest request =
      convertRequest(Container::WebM, VideoCodec::VP9, AudioCodec::Opus);

  const PlanResult plan = planOf(request, source);
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
  // H.264 cannot go into WebM (feature registry says no).
  const JobRequest request =
      convertRequest(Container::WebM, VideoCodec::H264, AudioCodec::Opus);

  const PlanResult plan = planOf(request, source);
  CHECK_FALSE(plan.feasible);
  CHECK_FALSE(plan.message.empty());
}

TEST_CASE_FIXTURE(Fixture,
                  "subtitle preservation incompatibility is surfaced, not dropped") {
  MediaInfo source = h264AacSource();
  SubtitleStream sub;
  sub.format = SubtitleFormat::Ass; // ASS is not valid in MP4
  source.subtitles.push_back(sub);

  // Empty subtitle rules = preserve all (mandate §24); the ASS track cannot
  // survive MP4, so the plan must say so instead of silently dropping it.
  JobRequest request;
  request.operation = Operation::VideoConvert;
  job::VideoConvert payload;
  payload.source = MediaSource{"test-src.mp4", "test-src.mp4"};
  payload.output.container = Container::Mp4;
  payload.video.codec = VideoCodec::H264;
  request.payload = std::move(payload);

  const PlanResult plan = planOf(request, source);
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

  JobRequest request;
  request.operation = Operation::VideoConvert;
  job::VideoConvert payload;
  payload.source = MediaSource{"test-src.mp4", "test-src.mp4"};
  payload.output.container = Container::Mp4;
  payload.video.codec = VideoCodec::H264;
  SubtitleTrackRule rule;
  rule.sourceTrackIndex = 0;
  rule.policy = SubtitleTrackPolicy::Remove;
  payload.subtitleTracks.push_back(rule);
  request.payload = std::move(payload);

  const PlanResult plan = planOf(request, source);
  CHECK(plan.feasible);
}

TEST_CASE_FIXTURE(Fixture, "no capable backend means honest infeasibility") {
  const MediaInfo source = h264AacSource();
  const JobRequest request =
      convertRequest(Container::Mkv, VideoCodec::Ffv1, AudioCodec::Aac);

  const PlanResult plan = planOf(request, source);
  CHECK_FALSE(plan.feasible);
  CHECK(plan.message.find("backend") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "validity outranks availability: the only capable backend wins") {
  const MediaInfo source = h264AacSource();
  // Only FFmpeg in this fixture serves VP9/Opus/WebM; the Android hardware
  // backend is registered but cannot serve this output — the plan must pick
  // the valid route (the retired PreferNative knob's core assertion).
  const JobRequest request =
      convertRequest(Container::WebM, VideoCodec::VP9, AudioCodec::Opus);

  const PlanResult plan = planOf(request, source);
  REQUIRE(plan.feasible);
  REQUIRE(plan.best.has_value());
  CHECK(plan.best->pipeline.assigns(StageId::Encode, BackendId::Ffmpeg));
}

TEST_CASE_FIXTURE(Fixture, "telemetry improves cost estimates") {
  const MediaInfo source = h264AacSource();
  const JobRequest request =
      convertRequest(Container::Mkv, VideoCodec::AV1, AudioCodec::Opus);

  const PlanResult cold = planOf(request, source);
  REQUIRE(cold.feasible);
  telemetry.learned_ = true;
  const PlanResult warm = planOf(request, source);
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
  const JobRequest request =
      convertRequest(Container::WebM, VideoCodec::VP9, AudioCodec::Opus);

  // Warm up once (branch predictors, caches).
  (void)planner.plan(request, {source});

  const auto start = std::chrono::steady_clock::now();
  constexpr int kIterations = 1000;
  for (int i = 0; i < kIterations; ++i) {
    (void)planner.plan(request, {source});
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
  const JobRequest request =
      convertRequest(Container::Mp4, VideoCodec::H264, AudioCodec::Aac);

  const PlanResult plan = planOf(request, source);
  REQUIRE(plan.feasible);
  REQUIRE(plan.alternates.size() >= 2);
  for (std::size_t i = 1; i < plan.alternates.size(); ++i) {
    CHECK(plan.alternates[i - 1].cost <= plan.alternates[i].cost);
  }
}

// ---------------------------------------------------------------------------
// Typed operation payloads
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "timeline and frame operations route through the FFmpeg executor") {
  const MediaInfo source = h264AacSource();

  SUBCASE("multi-range Cut") {
    JobRequest request;
    request.operation = Operation::VideoCutMerge;
    job::Cut payload;
    payload.source = MediaSource{"test-src.mp4", "test-src.mp4"};
    payload.output.container = Container::Mp4;
    TimeRange removed;
    removed.startUs = 500000;
    removed.endUs = 1500000;
    payload.removed.push_back(removed);
    request.payload = std::move(payload);
    const PlanResult plan = planOf(request, source);
    REQUIRE(plan.feasible);
    REQUIRE(plan.best.has_value());
    CHECK(plan.best->pipeline.assigns(StageId::Mux, BackendId::Ffmpeg));
  }
  SUBCASE("Split") {
    JobRequest request;
    request.operation = Operation::VideoCutMerge;
    job::Split payload;
    payload.source = MediaSource{"test-src.mp4", "test-src.mp4"};
    payload.output.container = Container::Mp4;
    payload.rules.mode = SplitMode::EqualParts;
    payload.rules.partCount = 3;
    request.payload = std::move(payload);
    const PlanResult plan = planOf(request, source);
    REQUIRE(plan.feasible);
    REQUIRE(plan.best.has_value());
    CHECK(plan.best->pipeline.assigns(StageId::Mux, BackendId::Ffmpeg));
  }
  SUBCASE("Merge") {
    JobRequest request;
    request.operation = Operation::VideoCutMerge;
    job::Merge payload;
    payload.sources = {MediaSource{"a.mp4", "a.mp4"},
                       MediaSource{"b.mp4", "b.mp4"}};
    payload.output.container = Container::Mp4;
    payload.rules.layout = MergeLayout::Sequential;
    request.payload = std::move(payload);
    const PlanResult plan = planOf(request, source);
    REQUIRE(plan.feasible);
    REQUIRE(plan.best.has_value());
    CHECK(plan.best->pipeline.assigns(StageId::Mux, BackendId::Ffmpeg));
  }
  SUBCASE("ExtractFrames") {
    JobRequest request;
    request.operation = Operation::ExtractFrames;
    job::ExtractFrames payload;
    payload.source = MediaSource{"test-src.mp4", "test-src.mp4"};
    payload.selection.mode = FrameSelectionMode::AllInRange;
    request.payload = std::move(payload);
    const PlanResult plan = planOf(request, source);
    REQUIRE(plan.feasible);
    REQUIRE(plan.best.has_value());
    // Dedicated image pipeline: demux + decode + image encode (no mux
    // stage — the artifact set is a manifest + image files).
    CHECK(plan.best->pipeline.assigns(StageId::Decode, BackendId::Ffmpeg));
    CHECK(plan.best->pipeline.sidecarExtension == "json");
  }
}

TEST_CASE_FIXTURE(Fixture, "Crop plans a transcode with the source codec when encodable") {
  const MediaInfo source = h264AacSource();
  JobRequest request;
  request.operation = Operation::VideoCrop;
  job::Crop payload;
  payload.source = MediaSource{"test-src.mp4", "test-src.mp4"};
  payload.output.container = Container::Mp4;
  payload.crop.x = 100;
  payload.crop.y = 100;
  payload.crop.width = 1280;
  payload.crop.height = 720;
  request.payload = std::move(payload);

  const PlanResult plan = planOf(request, source);
  REQUIRE(plan.feasible);
  REQUIRE(plan.best.has_value());
  // Crop rewrites pixels: never stream copy; H.264 (the encodable source
  // codec) is preserved (mandate §27 spatial).
  CHECK_FALSE(plan.best->pipeline.videoStreamCopy);
  CHECK(plan.best->pipeline.videoCodec == VideoCodec::H264);
}

TEST_CASE_FIXTURE(Fixture, "Crop validates the rectangle against the source") {
  const MediaInfo source = h264AacSource();
  JobRequest request;
  request.operation = Operation::VideoCrop;
  job::Crop payload;
  payload.source = MediaSource{"test-src.mp4", "test-src.mp4"};
  payload.output.container = Container::Mp4;
  payload.crop.x = 1500;
  payload.crop.y = 900;
  payload.crop.width = 1280;
  payload.crop.height = 720;
  request.payload = std::move(payload);

  const PlanResult plan = planOf(request, source);
  CHECK_FALSE(plan.feasible);
  CHECK(plan.errorCode == ErrorCode::InvalidCrop);
}

// ---------------------------------------------------------------------------
// Milestone 4: hybrid candidate generation
// ---------------------------------------------------------------------------

namespace {

// Scenario registry: FFmpeg decodes VP9 (and stage-decodes to CPU frames)
// but cannot encode HEVC; Android encodes HEVC (stage-encodes from CPU
// frames) but cannot decode VP9. Only a hybrid pipeline can serve
// VP9 -> HEVC.
CapabilityRegistry hybridScenarioCaps(bool vp9StreamCopy = true) {
  CapabilityRegistry caps;
  BackendCapabilities ffmpeg;
  ffmpeg.backend = BackendId::Ffmpeg;
  ffmpeg.implementation = "scenario";
  ffmpeg.canInspect = true;
  ffmpeg.video.push_back(VideoCapability{VideoCodec::VP9,
                                         {true, false, vp9StreamCopy}, false,
                                         0, 0});
  ffmpeg.video.push_back(VideoCapability{VideoCodec::H264,
                                         {true, true, true}, false, 0, 0});
  ffmpeg.audio.push_back(AudioCapability{AudioCodec::Aac, {true, true, true}});
  ffmpeg.readableContainers = {Container::Mp4};
  ffmpeg.writableContainers = {Container::Mp4};
  ffmpeg.stageDecodeToCpuFrames = true;
  ffmpeg.timeTrim = true;
  ffmpeg.timelineOps = true;
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

  const JobRequest request =
      convertRequest(Container::Mp4, VideoCodec::HEVC, AudioCodec::Aac);

  const Result<PlanResult> planned = scenarioPlanner.plan(request, {source});
  REQUIRE(planned.ok());
  const PlanResult plan = planned.value();
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
  MediaInfo h264 = h264AacSource();
  const JobRequest single =
      convertRequest(Container::Mp4, VideoCodec::HEVC, AudioCodec::Aac);
  const PlanResult classic = planOf(single, h264);
  REQUIRE(classic.feasible);
  REQUIRE(classic.best.has_value());
  CHECK(classic.best->pipeline.assigns(StageId::Decode, BackendId::Ffmpeg));
  CHECK(classic.best->pipeline.assigns(StageId::Encode, BackendId::Ffmpeg));
}

TEST_CASE_FIXTURE(Fixture, "trimmed requests route only to trim-capable backends") {
  // A trim window is honored exclusively by backends reporting timeTrim;
  // anything else would silently produce full-length output.
  CapabilityRegistry scenario = hybridScenarioCaps(false);
  Planner scenarioPlanner{FeatureRegistry::instance(), scenario, nullptr};

  MediaInfo source = h264AacSource();
  source.video.front().codec = VideoCodec::VP9;

  JobRequest trimRequest;
  trimRequest.operation = Operation::VideoCutMerge;
  job::Trim payload;
  payload.source = MediaSource{"test-src.mp4", "test-src.mp4"};
  payload.output.container = Container::Mp4;
  payload.range.startUs = 500000;
  payload.range.endUs = 1500000;
  trimRequest.payload = std::move(payload);

  // Without the trim this exact scenario plans a hybrid route (tested
  // above); with the trim it must refuse rather than mis-execute — the
  // v1 hybrid interchange carries no trim.
  const Result<PlanResult> hybridPlanned =
      scenarioPlanner.plan(trimRequest, {source});
  REQUIRE(hybridPlanned.ok());
  CHECK_FALSE(hybridPlanned.value().feasible);

  // Positive control: the Fixture FFmpeg reports timeTrim and serves the
  // same trimmed request end-to-end.
  MediaInfo plain = h264AacSource();
  const PlanResult plan = planOf(trimRequest, plain);
  REQUIRE(plan.feasible);
  REQUIRE(plan.best.has_value());
  CHECK(plan.best->pipeline.assigns(StageId::Demux, BackendId::Ffmpeg));
  CHECK(plan.best->pipeline.assigns(StageId::Mux, BackendId::Ffmpeg));
  // The retained window is honest work: the summary says so.
  CHECK(plan.best->pipeline.summary.find("trim") != std::string::npos);

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
  const Result<PlanResult> refused =
      nativePlanner.plan(trimRequest, {plain});
  REQUIRE(refused.ok());
  CHECK_FALSE(refused.value().feasible);
}

TEST_CASE_FIXTURE(Fixture, "subtitle extraction plans a sidecar pipeline") {
  MediaInfo source = h264AacSource();
  SubtitleStream sub;
  sub.format = SubtitleFormat::Srt;
  source.subtitles.push_back(sub);

  JobRequest request;
  request.operation = Operation::ExtractSubtitles;
  job::ExtractSubtitles payload;
  payload.source = MediaSource{"test-src.mp4", "test-src.mp4"};
  request.payload = std::move(payload);

  const PlanResult plan = planOf(request, source);
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
  const PlanResult movText = planOf(request, source);
  REQUIRE(movText.feasible);
  CHECK(movText.best->pipeline.sidecarExtension == "srt");

  source.subtitles.front().format = SubtitleFormat::Ass;
  const PlanResult ass = planOf(request, source);
  REQUIRE(ass.feasible);
  CHECK(ass.best->pipeline.sidecarExtension == "ass");

  // Explicit text targets pick their own extension (the executor writes
  // the requested representation).
  source.subtitles.front().format = SubtitleFormat::MovText;
  JobRequest vttRequest;
  vttRequest.operation = Operation::ExtractSubtitles;
  job::ExtractSubtitles vttPayload;
  vttPayload.source = MediaSource{"test-src.mp4", "test-src.mp4"};
  vttPayload.targets.push_back(SubtitleTextTarget::Vtt);
  vttRequest.payload = std::move(vttPayload);
  const PlanResult vtt = planOf(vttRequest, source);
  REQUIRE(vtt.feasible);
  CHECK(vtt.best->pipeline.sidecarExtension == "vtt");

  JobRequest srtRequest;
  srtRequest.operation = Operation::ExtractSubtitles;
  job::ExtractSubtitles srtPayload;
  srtPayload.source = MediaSource{"test-src.mp4", "test-src.mp4"};
  srtPayload.targets.push_back(SubtitleTextTarget::Ass);
  srtRequest.payload = std::move(srtPayload);
  const PlanResult srtTarget = planOf(srtRequest, source);
  REQUIRE(srtTarget.feasible);
  CHECK(srtTarget.best->pipeline.sidecarExtension == "ass");
}

TEST_CASE_FIXTURE(Fixture, "subtitle extraction refuses impossible shapes") {
  MediaInfo source = h264AacSource();

  SUBCASE("bitmap formats need OCR (outside V1)") {
    SubtitleStream sub;
    sub.format = SubtitleFormat::Pgs;
    source.subtitles.push_back(sub);
    JobRequest request;
    request.operation = Operation::ExtractSubtitles;
    job::ExtractSubtitles payload;
    payload.source = MediaSource{"test-src.mp4", "test-src.mp4"};
    request.payload = std::move(payload);
    const PlanResult plan = planOf(request, source);
    CHECK_FALSE(plan.feasible);
    CHECK(plan.message.find("OCR") != std::string::npos);
  }

  SUBCASE("multi-track sidecar sets are not implemented in this build") {
    SubtitleStream first;
    first.format = SubtitleFormat::Srt;
    SubtitleStream second;
    second.format = SubtitleFormat::Srt;
    source.subtitles.push_back(first);
    source.subtitles.push_back(second);
    JobRequest request;
    request.operation = Operation::ExtractSubtitles;
    job::ExtractSubtitles payload;
    payload.source = MediaSource{"test-src.mp4", "test-src.mp4"};
    payload.trackIndices = {0, 1};
    request.payload = std::move(payload);
    const PlanResult plan = planOf(request, source);
    CHECK_FALSE(plan.feasible);
    CHECK(plan.errorCode == ErrorCode::UnsupportedOperation);
    CHECK(plan.message.find("one track") != std::string::npos);
  }

  SUBCASE("a track index outside the source is a request error") {
    SubtitleStream sub;
    sub.format = SubtitleFormat::Srt;
    source.subtitles.push_back(sub);
    JobRequest request;
    request.operation = Operation::ExtractSubtitles;
    job::ExtractSubtitles payload;
    payload.source = MediaSource{"test-src.mp4", "test-src.mp4"};
    payload.trackIndices = {7};
    request.payload = std::move(payload);
    const PlanResult plan = planOf(request, source);
    CHECK_FALSE(plan.feasible);
    CHECK(plan.errorCode == ErrorCode::InvalidTrackIdentity);
  }
}
