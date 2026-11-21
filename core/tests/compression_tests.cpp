// Compression intelligence unit tests: bitrate budgets, feasibility,
// goal-derived quality and output estimation.
#include "velmux/core/compression.h"

#include <doctest/doctest.h>

using namespace velmux::core;

namespace {

MediaInfo sampleVideo(std::int64_t sizeBytes = 2000LL * 1000 * 1000,
                      double durationSec = 600.0) {
  MediaInfo info;
  info.durationSec = durationSec;
  info.container = Container::Mkv;
  info.sizeBytes = sizeBytes;
  VideoStream video;
  video.codec = VideoCodec::H264;
  video.width = 3840;
  video.height = 2160;
  video.frameRate = 30.0;
  video.bitRate = 25'000'000;
  info.video.push_back(video);
  AudioStream audio;
  audio.codec = AudioCodec::Aac;
  audio.channels = 2;
  audio.sampleRate = 48000;
  audio.bitRate = 256'000;
  info.audio.push_back(audio);
  return info;
}

ConversionRequest requestForGoal(GoalKind kind, std::int64_t target = 0) {
  ConversionRequest request;
  request.goal.kind = kind;
  request.goal.targetSizeBytes = target;
  request.outputContainer = Container::Mp4;
  return request;
}

} // namespace

TEST_CASE("a realistic fit-under-size budget is feasible and consistent") {
  // 2 GB, 10 minutes -> fit under 500 MB.
  const MediaInfo source = sampleVideo();
  const ConversionRequest request = requestForGoal(GoalKind::FitUnderSize,
                                                   500LL * 1000 * 1000);
  const BitrateBudget budget =
      computeBitrateBudget(source, request, request.goal.targetSizeBytes);
  REQUIRE(budget.feasible);
  REQUIRE(budget.videoBitrate > 0);

  // The resulting average rate must actually fit under the target,
  // accounting for audio and container overhead.
  const double overhead = containerOverheadFactor(Container::Mp4);
  const double totalBits = (double(budget.videoBitrate) +
                            double(budget.audioBitrate)) *
                           source.durationSec;
  const double projectedBytes = totalBits / 8.0 * (1.0 + overhead);
  CHECK(projectedBytes < double(request.goal.targetSizeBytes));
}

TEST_CASE("impossible targets are rejected with an honest message") {
  // 10 minutes of 4K30 video cannot fit in 1 MB.
  const MediaInfo source = sampleVideo();
  const ConversionRequest request = requestForGoal(GoalKind::FitUnderSize,
                                                   1LL * 1000 * 1000);
  const BitrateBudget budget =
      computeBitrateBudget(source, request, request.goal.targetSizeBytes);
  CHECK_FALSE(budget.feasible);
  CHECK_FALSE(budget.message.empty());
}

TEST_CASE("degenerate inputs produce clear infeasibility") {
  const MediaInfo source = sampleVideo();
  const ConversionRequest request = requestForGoal(GoalKind::FitUnderSize,
                                                   100LL * 1000 * 1000);
  SUBCASE("zero target") {
    CHECK_FALSE(
        computeBitrateBudget(source, request, 0).feasible);
  }
  SUBCASE("unknown duration") {
    MediaInfo noDuration = source;
    noDuration.durationSec = 0.0;
    CHECK_FALSE(
        computeBitrateBudget(noDuration, request, 100'000'000).feasible);
  }
}

TEST_CASE("quality factors order goals sensibly") {
  CHECK(qualityFactorForGoal(GoalKind::SmallerFile) <
        qualityFactorForGoal(GoalKind::Recommended));
  CHECK(qualityFactorForGoal(GoalKind::Recommended) <
        qualityFactorForGoal(GoalKind::HighQuality));
}

TEST_CASE("output estimation respects stream copy") {
  const MediaInfo source = sampleVideo();
  ConversionRequest request = requestForGoal(GoalKind::Recommended);
  request.outputContainer = Container::Mkv; // same container, same codecs

  Pipeline copyPipeline;
  copyPipeline.videoCodec = VideoCodec::H264;
  copyPipeline.audioCodec = AudioCodec::Aac;
  copyPipeline.container = Container::Mkv;
  copyPipeline.videoStreamCopy = true;
  copyPipeline.audioStreamCopy = true;

  const auto copyBytes = estimateOutputBytes(source, copyPipeline, request);
  REQUIRE(copyBytes.has_value());
  // Stream copy of same-codec/same-container should be close to source
  // size (within 20%).
  const double ratio = double(*copyBytes) / double(source.sizeBytes);
  CHECK(ratio > 0.8);
  CHECK(ratio < 1.2);

  Pipeline transcodePipeline = copyPipeline;
  transcodePipeline.videoStreamCopy = false;
  request.video.bitRate = 2'000'000; // far below the source's 25 Mbps
  const auto transcodeBytes =
      estimateOutputBytes(source, transcodePipeline, request);
  REQUIRE(transcodeBytes.has_value());
  CHECK(*transcodeBytes < source.sizeBytes / 4);
}

TEST_CASE("unknown duration makes estimation honestly impossible") {
  MediaInfo source = sampleVideo();
  source.durationSec = 0.0;
  ConversionRequest request = requestForGoal(GoalKind::Recommended);
  Pipeline pipeline;
  pipeline.container = Container::Mp4;
  CHECK_FALSE(estimateOutputBytes(source, pipeline, request).has_value());
}

TEST_CASE("resolution ladder never upscales and drops under tight budgets") {
  CHECK(resolutionHeightForBudget(2160, std::nullopt, 100'000) <= 2160);
  CHECK(resolutionHeightForBudget(2160, 1080, 8'000'000) == 1080);
  CHECK(resolutionHeightForBudget(2160, std::nullopt, 300'000) < 2160);
  CHECK(isCanonicalResolutionHeight(720));
  CHECK_FALSE(isCanonicalResolutionHeight(1111));
}

TEST_CASE("container overhead factors are sane") {
  CHECK(containerOverheadFactor(Container::Mp4) <
        containerOverheadFactor(Container::MpegTs));
  CHECK(containerOverheadFactor(Container::Wav) < 0.01);
  CHECK(containerOverheadFactor(Container::MpegTs) > 0.01);
}
