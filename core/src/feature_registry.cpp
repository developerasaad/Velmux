#include "velmux/core/feature_registry.h"

#include <algorithm>

namespace velmux::core {

using VC = VideoCodec;
using AC = AudioCodec;
using CT = Container;
using SF = SubtitleFormat;

namespace {

// Container groups used to keep the tables readable.
std::vector<Container> mp4Family() {
  return {CT::Mp4, CT::M4v, CT::Mov, CT::ThreeGp};
}
std::vector<Container> mkvFamily() { return {CT::Mkv, CT::WebM}; }
std::vector<Container> isoBmff() { return {CT::Mp4, CT::M4v, CT::Mov}; }
std::vector<Container> allVideoContainers() {
  return {CT::Mp4,  CT::M4v, CT::Mov, CT::Mkv, CT::WebM, CT::Avi,
          CT::MpegTs, CT::Flv, CT::ThreeGp, CT::Asf};
}
std::vector<Container> webmOnly() { return {CT::WebM, CT::Mkv}; }
std::vector<Container> tsFamily() { return {CT::MpegTs}; }

std::vector<Container> audioContainers() {
  return {CT::Mp4, CT::M4a, CT::Mkv, CT::Ogg, CT::Mp3, CT::Wav,
          CT::Flac, CT::MpegTs, CT::Asf, CT::Avi, CT::ThreeGp, CT::M4v};
}

} // namespace

FeatureRegistry::FeatureRegistry() {
  // --- Video codecs (docs/FEATURE_MATRIX.md) ---
  video_ = {
      // Modern / common
      {VC::H264, {true, true, true}, allVideoContainers(),
       "primary delivery codec"},
      {VC::HEVC, {true, true, true}, allVideoContainers(),
       "HDR passthrough where compatible"},
      {VC::AV1, {true, true, true},
       {CT::Mkv, CT::WebM, CT::Mp4, CT::M4v, CT::Mov},
       "native where device supports; FFmpeg dav1d/SVT-AV1 otherwise"},
      {VC::VP8, {true, true, true}, webmOnly(), ""},
      {VC::VP9, {true, true, true}, webmOnly(), "profiles 0 and 2"},
      // Compatibility / legacy
      {VC::Mpeg4Part2, {true, true, true}, allVideoContainers(), ""},
      {VC::Mpeg2, {true, false, true}, {CT::MpegTs, CT::Avi, CT::Mkv},
       "decode/copy only"},
      {VC::Mpeg1, {true, false, true}, {CT::MpegTs, CT::Avi, CT::Mkv},
       "decode/copy only"},
      {VC::H263, {true, false, true}, {CT::ThreeGp, CT::Mkv},
       "decode/copy only"},
      {VC::Vc1, {true, false, true}, {CT::Asf, CT::Mkv, CT::Avi},
       "decode/copy only"},
      {VC::Wmv, {true, false, true}, {CT::Asf, CT::Mkv, CT::Avi},
       "decode/copy only"},
      // Professional / archival
      {VC::ProRes, {true, false, true}, {CT::Mov, CT::Mkv},
       "decode/copy only in V1"},
      {VC::Dnxhd, {true, false, true}, {CT::Mov, CT::Mkv},
       "decode/copy only in V1"},
      {VC::Ffv1, {true, true, true}, {CT::Mkv, CT::Avi}, "archival"},
      {VC::Mjpeg, {true, true, true}, {CT::Mkv, CT::Avi, CT::Mov}, ""},
  };

  // --- Audio codecs ---
  audio_ = {
      {AC::Aac, {true, true, true}, audioContainers(), "AAC-LC"},
      {AC::HeAac, {true, true, true}, {CT::Mp4, CT::M4a, CT::M4v, CT::Mkv, CT::MpegTs, CT::Flv, CT::ThreeGp}, "HE-AAC"},
      {AC::Mp3, {true, true, true}, {CT::Mp3, CT::Mkv, CT::Avi, CT::MpegTs, CT::Asf}, ""},
      {AC::Opus, {true, true, true}, {CT::Ogg, CT::WebM, CT::Mkv, CT::Mp4, CT::M4a}, ""},
      {AC::Vorbis, {true, true, true}, {CT::Ogg, CT::WebM, CT::Mkv}, ""},
      {AC::Flac, {true, true, true}, {CT::Flac, CT::Mkv, CT::Ogg, CT::Mp4, CT::M4a}, ""},
      {AC::Alac, {true, true, true}, {CT::Mp4, CT::M4a, CT::M4v, CT::Mkv}, ""},
      {AC::Pcm, {true, true, true}, {CT::Wav, CT::Mkv, CT::Avi, CT::Mov}, "bit-depth control where meaningful"},
      {AC::Ac3, {true, true, true}, {CT::Mkv, CT::MpegTs, CT::Avi, CT::Mp4}, ""},
      {AC::Eac3, {true, true, true}, {CT::Mkv, CT::MpegTs, CT::Mp4}, "encode where practical"},
      {AC::Dts, {true, false, true}, {CT::Mkv, CT::MpegTs, CT::Avi}, "decode/copy only"},
      {AC::TrueHd, {true, false, true}, {CT::Mkv, CT::MpegTs}, "decode/copy only"},
      {AC::Wma, {true, false, true}, {CT::Asf, CT::Mkv, CT::Avi}, "decode/copy only"},
      {AC::WavPack, {true, true, true}, {CT::Mkv, CT::Wav}, ""},
      {AC::Ape, {true, false, false}, {CT::Mkv, CT::Ogg}, "decode only"},
      {AC::AmrNb, {true, true, true}, {CT::ThreeGp, CT::Mkv}, ""},
      {AC::AmrWb, {true, true, true}, {CT::ThreeGp, CT::Mkv}, ""},
  };

  // --- Subtitle formats ---
  subtitles_ = {
      {SF::Srt, true, false, {CT::Mkv, CT::Ogg, CT::MpegTs}},
      {SF::WebVtt, true, false, {CT::WebM, CT::Mkv, CT::Mp4}},
      {SF::Ass, true, false, {CT::Mkv, CT::Ogg}},
      {SF::Ssa, true, false, {CT::Mkv, CT::Ogg}},
      {SF::MovText, true, false, mp4Family()},
      {SF::Ttml, true, false, {CT::Mp4, CT::Mkv}},
      {SF::Cea608, false, true, {CT::MpegTs, CT::Mp4, CT::Mov, CT::Mkv}},
      {SF::Cea708, false, true, {CT::MpegTs, CT::Mp4, CT::Mov, CT::Mkv}},
      {SF::Pgs, false, true, {CT::Mkv, CT::MpegTs, CT::Mp4}},
      {SF::VobSub, false, true, {CT::Mkv, CT::MpegTs, CT::Avi}},
      {SF::Dvb, false, true, {CT::MpegTs, CT::Mkv}},
  };

  containers_ = {
      CT::Mp4,   CT::M4v,  CT::Mov,   CT::Mkv,    CT::WebM, CT::Avi,
      CT::MpegTs, CT::Flv, CT::ThreeGp, CT::M4a,  CT::Ogg,  CT::Mp3,
      CT::Wav,   CT::Flac, CT::Asf,
  };
}

const FeatureRegistry &FeatureRegistry::instance() {
  static const FeatureRegistry registry;
  return registry;
}

const std::vector<VideoCodecFeature> &FeatureRegistry::videoCodecs() const {
  return video_;
}
const std::vector<AudioCodecFeature> &FeatureRegistry::audioCodecs() const {
  return audio_;
}
const std::vector<SubtitleFormatFeature> &
FeatureRegistry::subtitleFormats() const {
  return subtitles_;
}

bool FeatureRegistry::knowsVideoCodec(VideoCodec codec) const {
  return std::any_of(video_.begin(), video_.end(),
                     [codec](const auto &f) { return f.codec == codec; });
}

bool FeatureRegistry::knowsAudioCodec(AudioCodec codec) const {
  return std::any_of(audio_.begin(), audio_.end(),
                     [codec](const auto &f) { return f.codec == codec; });
}

CodecSupport FeatureRegistry::videoSupport(VideoCodec codec) const {
  for (const auto &f : video_) {
    if (f.codec == codec) {
      return f.support;
    }
  }
  return {};
}

CodecSupport FeatureRegistry::audioSupport(AudioCodec codec) const {
  for (const auto &f : audio_) {
    if (f.codec == codec) {
      return f.support;
    }
  }
  return {};
}

bool FeatureRegistry::containerAccepts(Container container,
                                        VideoCodec codec) const {
  for (const auto &f : video_) {
    if (f.codec == codec) {
      return std::find(f.containers.begin(), f.containers.end(), container) !=
             f.containers.end();
    }
  }
  return false;
}

bool FeatureRegistry::containerAccepts(Container container,
                                        AudioCodec codec) const {
  for (const auto &f : audio_) {
    if (f.codec == codec) {
      return std::find(f.containers.begin(), f.containers.end(), container) !=
             f.containers.end();
    }
  }
  return false;
}

bool FeatureRegistry::containerAccepts(Container container,
                                        SubtitleFormat format) const {
  for (const auto &f : subtitles_) {
    if (f.format == format) {
      return std::find(f.containers.begin(), f.containers.end(), container) !=
             f.containers.end();
    }
  }
  return false;
}

bool FeatureRegistry::knowsContainer(Container container) const {
  return std::find(containers_.begin(), containers_.end(), container) !=
         containers_.end();
}

std::vector<Container>
FeatureRegistry::containersFor(const VideoCodec *video,
                               const AudioCodec *audio) const {
  std::vector<Container> result;
  for (Container c : containers_) {
    const bool videoOk = !video || containerAccepts(c, *video);
    const bool audioOk = !audio || containerAccepts(c, *audio);
    if (videoOk && audioOk) {
      result.push_back(c);
    }
  }
  return result;
}

} // namespace velmux::core
