// Compression intelligence unit tests: bitrate budgets, feasibility,
// preset-derived quality and output estimation.
#include "compression/compression.h"
#include "processing/pipeline.h"

#include <doctest/doctest.h>

using namespace velmux::core;

namespace {

MediaInfo sampleVideo(std::int64_t sizeBytes = 620'000'000,
                      double durationSec = 600.0) {
  MediaInfo info;
  info.durationSec = durationSec;
  info.container = Container::Mkv;
  info.sizeBytes = sizeBytes;
  VideoStream video;
  video.codec = VideoCodec::H264;
  video.width = 1920;
  video.height = 1080;
  video.frameRate = 30.0;
  video.bitRate = 8'000'000;
  info.video.push_back(video);
  AudioStream audio;
  audio.codec = AudioCodec::Aac;
  audio.channels = 2;
  audio.sampleRate = 48000;
  audio.bitRate = 256'000;
  info.audio.push_back(audio);
  return info;
}

BudgetInputs inputsFor(Container container, AudioSettings audio = {}) {
  BudgetInputs inputs;
  inputs.outputContainer = container;
  inputs.audioEnabled = true;
  inputs.videoEnabled = true;
  inputs.videoCodec = VideoCodec::H264;
  inputs.audio = audio;
  return inputs;
}

CompressionIntent presetIntent(CompressionPreset preset,
                               std::int64_t target = 0) {
  CompressionIntent compression;
  compression.enabled = true;
  compression.preset = preset;
  compression.targetSizeBytes = target;
  return compression;
}

} // namespace

TEST_CASE("a realistic fit-under-size budget is feasible and consistent") {
  // 2 GB, 10 minutes -> fit under 500 MB.
  const MediaInfo source = sampleVideo();
  const BudgetInputs inputs = inputsFor(Container::Mp4);
  const std::int64_t target = 500LL * 1000 * 1000;
  const BitrateBudget budget = computeBitrateBudget(source, inputs, target);
  REQUIRE(budget.feasible);
  REQUIRE(budget.videoBitrate > 0);

  // The resulting average rate must actually fit under the target,
  // accounting for audio and container overhead.
  const double overhead = containerOverheadFactor(Container::Mp4);
  const double totalBits = (double(budget.videoBitrate) +
                            double(budget.audioBitrate)) *
                           source.durationSec;
  const double projectedBytes = totalBits / 8.0 * (1.0 + overhead);
  CHECK(projectedBytes < double(target));
}

TEST_CASE("impossible targets are rejected with an honest message") {
  // 10 minutes of 4K30 video cannot fit in 1 MB.
  MediaInfo uhd = sampleVideo();
  uhd.video.front().width = 3840;
  uhd.video.front().height = 2160;
  uhd.video.front().bitRate = 25'000'000;
  const BudgetInputs inputs = inputsFor(Container::Mp4);
  const BitrateBudget budget =
      computeBitrateBudget(uhd, inputs, 1LL * 1000 * 1000);
  CHECK_FALSE(budget.feasible);
  CHECK_FALSE(budget.message.empty());

  // 4K content under a tight-but-large target is also honestly infeasible
  // at the source resolution (the floor bitrate alone exceeds it).
  const BitrateBudget tight =
      computeBitrateBudget(uhd, inputs, 500LL * 1000 * 1000);
  CHECK_FALSE(tight.feasible);
  CHECK_FALSE(tight.message.empty());
}

TEST_CASE("degenerate inputs produce clear infeasibility") {
  const MediaInfo source = sampleVideo();
  const BudgetInputs inputs = inputsFor(Container::Mp4);
  SUBCASE("zero target") {
    CHECK_FALSE(
        computeBitrateBudget(source, inputs, 0).feasible);
  }
  SUBCASE("unknown duration") {
    MediaInfo noDuration = source;
    noDuration.durationSec = 0.0;
    CHECK_FALSE(
        computeBitrateBudget(noDuration, inputs, 100'000'000).feasible);
  }
}

TEST_CASE("quality factors order presets sensibly") {
  CHECK(qualityFactorForPreset(CompressionPreset::SmallerFile) <
        qualityFactorForPreset(CompressionPreset::Balanced));
  CHECK(qualityFactorForPreset(CompressionPreset::Balanced) <
        qualityFactorForPreset(CompressionPreset::HighQuality));
}

TEST_CASE("resolveRatePolicy maps presets onto the user quality scale") {
  const MediaInfo source = sampleVideo();
  const BudgetInputs inputs = inputsFor(Container::Mp4);
  // Disabled intent on a forced transcode: high-fidelity default.
  const ResolvedRatePolicy disabled =
      resolveRatePolicy(source, CompressionIntent{}, inputs);
  CHECK(disabled.mode == ResolvedRatePolicy::Mode::Quality);
  CHECK(disabled.quality == 65);
  // Presets map to 85/60/35 (mandate §20).
  CHECK(resolveRatePolicy(source, presetIntent(CompressionPreset::HighQuality),
                          inputs)
            .quality == 85);
  CHECK(resolveRatePolicy(source, presetIntent(CompressionPreset::Balanced),
                          inputs)
            .quality == 60);
  CHECK(resolveRatePolicy(source,
                          presetIntent(CompressionPreset::SmallerFile), inputs)
            .quality == 35);
  // Custom honors the explicit quality.
  CompressionIntent custom = presetIntent(CompressionPreset::Custom);
  custom.quality = 42;
  CHECK(resolveRatePolicy(source, custom, inputs).quality == 42);
  // Custom with an explicit bitrate goes to bitrate mode.
  CompressionIntent customRate = presetIntent(CompressionPreset::Custom);
  customRate.videoBitrate = 1'500'000;
  const ResolvedRatePolicy rate =
      resolveRatePolicy(source, customRate, inputs);
  CHECK(rate.mode == ResolvedRatePolicy::Mode::Bitrate);
  CHECK(rate.bitRate == 1'500'000);
  // TargetSize computes a real budget when feasible.
  const ResolvedRatePolicy sized =
      resolveRatePolicy(source,
                        presetIntent(CompressionPreset::TargetSize,
                                     500LL * 1000 * 1000),
                        inputs);
  CHECK(sized.mode == ResolvedRatePolicy::Mode::Bitrate);
  CHECK(sized.bitRate > 0);
}

TEST_CASE("output estimation respects stream copy") {
  const MediaInfo source = sampleVideo();
  const BudgetInputs inputs = inputsFor(Container::Mkv);

  Pipeline copyPipeline;
  copyPipeline.videoCodec = VideoCodec::H264;
  copyPipeline.audioCodec = AudioCodec::Aac;
  copyPipeline.container = Container::Mkv;
  copyPipeline.videoStreamCopy = true;
  copyPipeline.audioStreamCopy = true;

  const auto copyBytes =
      estimateOutputBytes(source, copyPipeline, inputs, ResolvedRatePolicy{});
  REQUIRE(copyBytes.has_value());
  // Stream copy of same-codec/same-container should be close to source
  // size (within 20%).
  const double ratio = double(*copyBytes) / double(source.sizeBytes);
  CHECK(ratio > 0.8);
  CHECK(ratio < 1.2);

  Pipeline transcodePipeline = copyPipeline;
  transcodePipeline.videoStreamCopy = false;
  const ResolvedRatePolicy rate = [&] {
    ResolvedRatePolicy policy;
    policy.mode = ResolvedRatePolicy::Mode::Bitrate;
    policy.bitRate = 1'500'000; // far below the source's rate
    return policy;
  }();
  const auto transcodeBytes =
      estimateOutputBytes(source, transcodePipeline, inputs, rate);
  REQUIRE(transcodeBytes.has_value());
  CHECK(*transcodeBytes < source.sizeBytes / 4);
}

TEST_CASE("unknown duration makes estimation honestly impossible") {
  MediaInfo source = sampleVideo();
  source.durationSec = 0.0;
  Pipeline pipeline;
  pipeline.container = Container::Mp4;
  CHECK_FALSE(estimateOutputBytes(source, pipeline,
                                  inputsFor(Container::Mp4),
                                  ResolvedRatePolicy{})
                  .has_value());
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
