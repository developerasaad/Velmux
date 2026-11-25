// Capability registry unit tests: runtime capability reporting and narrow
// failure records.
#include "capabilities/capabilities.h"

#include <doctest/doctest.h>

using namespace velmux::core;

namespace {

BackendCapabilities ffmpegLikeCapabilities() {
  BackendCapabilities caps;
  caps.backend = BackendId::Ffmpeg;
  caps.implementation = "test-ffmpeg";
  caps.canInspect = true;
  caps.video.push_back(VideoCapability{VideoCodec::H264,
                                       {true, true, true}, false, 0, 0});
  caps.video.push_back(VideoCapability{VideoCodec::HEVC,
                                       {true, false, true}, false, 0, 0});
  caps.audio.push_back(AudioCapability{AudioCodec::Aac,
                                       {true, true, true}});
  caps.audio.push_back(AudioCapability{AudioCodec::Flac,
                                       {true, true, true}});
  caps.readableContainers = {Container::Mp4, Container::Mkv};
  caps.writableContainers = {Container::Mp4, Container::Mkv};
  return caps;
}

BackendCapabilities androidLikeCapabilities() {
  BackendCapabilities caps;
  caps.backend = BackendId::Android;
  caps.implementation = "test-media-codec";
  caps.video.push_back(VideoCapability{VideoCodec::H264,
                                       {true, true, true}, true, 3840, 2160});
  caps.readableContainers = {Container::Mp4};
  caps.writableContainers = {Container::Mp4};
  return caps;
}

} // namespace

TEST_CASE("backends can be registered and queried per operation") {
  CapabilityRegistry registry;
  registry.registerBackend(ffmpegLikeCapabilities());
  registry.registerBackend(androidLikeCapabilities());

  const auto h264Encoders = registry.backendsFor(VideoCodec::H264, CodecOp::Encode);
  REQUIRE(h264Encoders.size() == 2);

  const auto hevcEncoders = registry.backendsFor(VideoCodec::HEVC, CodecOp::Encode);
  CHECK(hevcEncoders.empty()); // FFmpeg test caps say decode/copy only

  const auto decoders = registry.backendsFor(VideoCodec::HEVC, CodecOp::Decode);
  REQUIRE(decoders.size() == 1);
  CHECK(decoders.front() == BackendId::Ffmpeg);
}

TEST_CASE("container read/write capabilities are independent") {
  CapabilityRegistry registry;
  registry.registerBackend(ffmpegLikeCapabilities());

  CHECK(registry.backendsThatRead(Container::Mkv).size() == 1);
  CHECK(registry.backendsThatWrite(Container::Mkv).size() == 1);
  CHECK(registry.backendsThatRead(Container::Flv).empty());
  CHECK(registry.backendsThatWrite(Container::Flv).empty());
}

TEST_CASE("invalidate removes a backend completely") {
  CapabilityRegistry registry;
  registry.registerBackend(ffmpegLikeCapabilities());
  registry.registerBackend(androidLikeCapabilities());
  CHECK(registry.registeredBackends().size() == 2);

  registry.invalidate(BackendId::Android);
  CHECK(registry.registeredBackends().size() == 1);
  CHECK(registry.registeredBackends().front() == BackendId::Ffmpeg);
  CHECK(registry.capabilitiesOf(BackendId::Android) == nullptr);
}

TEST_CASE("failure records are narrow, not codec-wide bans") {
  CapabilityRegistry registry;
  registry.registerBackend(ffmpegLikeCapabilities());

  FailedConfigKey key;
  key.backend = BackendId::Ffmpeg;
  key.codec = VideoCodec::H264;
  key.profile = "high";
  key.width = 4096;
  key.height = 2160;
  key.pixelFormat = "yuv420p10le";

  CHECK_FALSE(registry.hasFailed(key));
  registry.recordFailure(key);
  CHECK(registry.hasFailed(key));
  CHECK(registry.failureCount() == 1);

  // Recording the same key twice does not duplicate.
  registry.recordFailure(key);
  CHECK(registry.failureCount() == 1);

  // A different configuration of the same codec is NOT failed.
  FailedConfigKey otherProfile = key;
  otherProfile.profile = "main";
  CHECK_FALSE(registry.hasFailed(otherProfile));

  FailedConfigKey otherSize = key;
  otherSize.width = 1920;
  otherSize.height = 1080;
  CHECK_FALSE(registry.hasFailed(otherSize));

  // Clearing failures for the backend removes them all.
  registry.clearFailures(BackendId::Ffmpeg);
  CHECK(registry.failureCount() == 0);
  CHECK(registry.hasFailed(key) == false);
}

TEST_CASE("hardware capability is reported per implementation") {
  CapabilityRegistry registry;
  registry.registerBackend(androidLikeCapabilities());
  const auto cap = registry.videoCapability(BackendId::Android, VideoCodec::H264);
  REQUIRE(cap.has_value());
  CHECK(cap->hardware);
  CHECK(cap->maxWidth == 3840);
  CHECK(cap->maxHeight == 2160);
}
