// Feature registry unit tests: the product-controlled catalog.
#include "catalog/features.h"
#include <velmux/jobs.h>

#include <doctest/doctest.h>

using namespace velmux::core;
using VC = VideoCodec;
using AC = AudioCodec;
using CT = Container;

TEST_CASE("feature registry contains the intentional V1 video catalog") {
  const FeatureRegistry &reg = FeatureRegistry::instance();
  CHECK(reg.knowsVideoCodec(VC::H264));
  CHECK(reg.knowsVideoCodec(VC::HEVC));
  CHECK(reg.knowsVideoCodec(VC::AV1));
  CHECK(reg.knowsVideoCodec(VC::VP8));
  CHECK(reg.knowsVideoCodec(VC::VP9));
  CHECK(reg.knowsVideoCodec(VC::Mpeg4Part2));
  CHECK(reg.knowsVideoCodec(VC::Mpeg2));
  CHECK(reg.knowsVideoCodec(VC::Mpeg1));
  CHECK(reg.knowsVideoCodec(VC::H263));
  CHECK(reg.knowsVideoCodec(VC::Vc1));
  CHECK(reg.knowsVideoCodec(VC::Wmv));
  CHECK(reg.knowsVideoCodec(VC::ProRes));
  CHECK(reg.knowsVideoCodec(VC::Dnxhd));
  CHECK(reg.knowsVideoCodec(VC::Ffv1));
  CHECK(reg.knowsVideoCodec(VC::Mjpeg));
}

TEST_CASE("legacy formats are decode/copy only where documented") {
  const FeatureRegistry &reg = FeatureRegistry::instance();
  CHECK_FALSE(reg.videoSupport(VC::Mpeg2).encode);
  CHECK(reg.videoSupport(VC::Mpeg2).decode);
  CHECK(reg.videoSupport(VC::Mpeg2).streamCopy);
  CHECK_FALSE(reg.videoSupport(VC::Vc1).encode);
  CHECK_FALSE(reg.videoSupport(VC::ProRes).encode);
  CHECK(reg.videoSupport(VC::H264).encode);
  CHECK(reg.videoSupport(VC::AV1).encode);
}

TEST_CASE("container compatibility is explicit, never inferred") {
  const FeatureRegistry &reg = FeatureRegistry::instance();
  // Valid combinations.
  CHECK(reg.containerAccepts(CT::Mp4, VC::H264));
  CHECK(reg.containerAccepts(CT::WebM, VC::VP9));
  CHECK(reg.containerAccepts(CT::Mkv, VC::Ffv1));
  CHECK(reg.containerAccepts(CT::Mov, VC::ProRes));
  CHECK(reg.containerAccepts(CT::Ogg, AC::Vorbis));
  CHECK(reg.containerAccepts(CT::Flac, AC::Flac));
  // Invalid combinations must never be exposed.
  CHECK_FALSE(reg.containerAccepts(CT::Mp3, VC::H264));
  CHECK_FALSE(reg.containerAccepts(CT::Wav, VC::H264));
  CHECK_FALSE(reg.containerAccepts(CT::WebM, VC::H264)); // WebM is VP*/AV1/Opus/Vorbis
  CHECK_FALSE(reg.containerAccepts(CT::Avi, AC::Flac));
}

TEST_CASE("containersFor yields only valid combinations for a pair") {
  const FeatureRegistry &reg = FeatureRegistry::instance();
  const VC h264 = VC::H264;
  const AC aac = AC::Aac;
  const auto containers = reg.containersFor(&h264, &aac);
  CHECK_FALSE(containers.empty());
  for (const CT c : containers) {
    CHECK(reg.containerAccepts(c, h264));
    CHECK(reg.containerAccepts(c, aac));
  }
}

TEST_CASE("name round-trips are stable") {
  CHECK(std::string(codecName(VC::H264)) == "h264");
  CHECK(videoCodecFromName("h264").value() == VC::H264);
  CHECK_FALSE(videoCodecFromName("not-a-codec").has_value());
  CHECK(std::string(containerName(CT::ThreeGp)) == "3gp");
  CHECK(containerFromName("3gp").value() == CT::ThreeGp);
  CHECK(std::string(codecName(AC::WavPack)) == "wavpack");
  CHECK(audioCodecFromName("wavpack").value() == AC::WavPack);
  CHECK(std::string(subtitleFormatName(SubtitleFormat::VobSub)) == "vobsub");
  CHECK(subtitleFormatFromName("vobsub").value() == SubtitleFormat::VobSub);
}

TEST_CASE("error codes and states have stable names") {
  CHECK(std::string(errorCodeName(ErrorCode::NoCompatiblePipeline)) ==
        "no-compatible-pipeline");
  CHECK(std::string(jobStateName(JobState::Checkpointing)) == "checkpointing");
  CHECK(jobStateFromName("recovering").value() == JobState::Recovering);
  CHECK(std::string(stageName(StageId::Bitstream)) == "bitstream");
}
