#include "velmux/ffmpeg/codec_mapping.h"

#include <cstring>

namespace velmux::ffmpeg {

using velmux::core::AudioCodec;
using velmux::core::Container;
using velmux::core::SubtitleFormat;
using velmux::core::VideoCodec;

std::optional<VideoCodec> videoCodecFromAv(AVCodecID id) {
  switch (id) {
    case AV_CODEC_ID_H264:
      return VideoCodec::H264;
    case AV_CODEC_ID_HEVC:
      return VideoCodec::HEVC;
    case AV_CODEC_ID_AV1:
      return VideoCodec::AV1;
    case AV_CODEC_ID_VP8:
      return VideoCodec::VP8;
    case AV_CODEC_ID_VP9:
      return VideoCodec::VP9;
    case AV_CODEC_ID_MPEG4:
      return VideoCodec::Mpeg4Part2;
    case AV_CODEC_ID_MPEG2VIDEO:
      return VideoCodec::Mpeg2;
    case AV_CODEC_ID_MPEG1VIDEO:
      return VideoCodec::Mpeg1;
    case AV_CODEC_ID_H263:
      return VideoCodec::H263;
    case AV_CODEC_ID_VC1:
      return VideoCodec::Vc1;
    case AV_CODEC_ID_WMV1:
    case AV_CODEC_ID_WMV2:
    case AV_CODEC_ID_WMV3:
      return VideoCodec::Wmv;
    case AV_CODEC_ID_PRORES:
      return VideoCodec::ProRes;
    case AV_CODEC_ID_DNXHD:
      return VideoCodec::Dnxhd;
    case AV_CODEC_ID_FFV1:
      return VideoCodec::Ffv1;
    case AV_CODEC_ID_MJPEG:
      return VideoCodec::Mjpeg;
    default:
      return std::nullopt;
  }
}

std::optional<AudioCodec> audioCodecFromAv(AVCodecID id) {
  switch (id) {
    case AV_CODEC_ID_AAC:
      return AudioCodec::Aac;
    case AV_CODEC_ID_AAC_LATM:
      return AudioCodec::Aac;
    case AV_CODEC_ID_MP3:
      return AudioCodec::Mp3;
    case AV_CODEC_ID_OPUS:
      return AudioCodec::Opus;
    case AV_CODEC_ID_VORBIS:
      return AudioCodec::Vorbis;
    case AV_CODEC_ID_FLAC:
      return AudioCodec::Flac;
    case AV_CODEC_ID_ALAC:
      return AudioCodec::Alac;
    case AV_CODEC_ID_PCM_S16LE:
    case AV_CODEC_ID_PCM_S16BE:
    case AV_CODEC_ID_PCM_S24LE:
    case AV_CODEC_ID_PCM_S32LE:
    case AV_CODEC_ID_PCM_U8:
    case AV_CODEC_ID_PCM_F32LE:
      return AudioCodec::Pcm;
    case AV_CODEC_ID_AC3:
      return AudioCodec::Ac3;
    case AV_CODEC_ID_EAC3:
      return AudioCodec::Eac3;
    case AV_CODEC_ID_DTS:
      return AudioCodec::Dts;
    case AV_CODEC_ID_TRUEHD:
      return AudioCodec::TrueHd;
    case AV_CODEC_ID_WMAV1:
    case AV_CODEC_ID_WMAV2:
    case AV_CODEC_ID_WMAPRO:
      return AudioCodec::Wma;
    case AV_CODEC_ID_WAVPACK:
      return AudioCodec::WavPack;
    case AV_CODEC_ID_APE:
      return AudioCodec::Ape;
    case AV_CODEC_ID_AMR_NB:
      return AudioCodec::AmrNb;
    case AV_CODEC_ID_AMR_WB:
      return AudioCodec::AmrWb;
    default:
      return std::nullopt;
  }
}

std::optional<SubtitleFormat> subtitleFormatFromAv(AVCodecID id) {
  switch (id) {
    case AV_CODEC_ID_SUBRIP:
    case AV_CODEC_ID_SRT:
      return SubtitleFormat::Srt;
    case AV_CODEC_ID_WEBVTT:
      return SubtitleFormat::WebVtt;
    case AV_CODEC_ID_ASS:
      return SubtitleFormat::Ass;
    case AV_CODEC_ID_SSA:
      return SubtitleFormat::Ssa;
    case AV_CODEC_ID_MOV_TEXT:
      return SubtitleFormat::MovText;
    case AV_CODEC_ID_TTML:
      return SubtitleFormat::Ttml;
    case AV_CODEC_ID_EIA_608:
      return SubtitleFormat::Cea608;
    case AV_CODEC_ID_EIA_708:
      return SubtitleFormat::Cea708;
    case AV_CODEC_ID_HDMV_PGS_SUBTITLE:
      return SubtitleFormat::Pgs;
    case AV_CODEC_ID_DVD_SUBTITLE:
      return SubtitleFormat::VobSub;
    case AV_CODEC_ID_DVB_SUBTITLE:
      return SubtitleFormat::Dvb;
    default:
      return std::nullopt;
  }
}

AVCodecID avCodecFor(VideoCodec codec) {
  switch (codec) {
    case VideoCodec::H264:
      return AV_CODEC_ID_H264;
    case VideoCodec::HEVC:
      return AV_CODEC_ID_HEVC;
    case VideoCodec::AV1:
      return AV_CODEC_ID_AV1;
    case VideoCodec::VP8:
      return AV_CODEC_ID_VP8;
    case VideoCodec::VP9:
      return AV_CODEC_ID_VP9;
    case VideoCodec::Mpeg4Part2:
      return AV_CODEC_ID_MPEG4;
    case VideoCodec::Mpeg2:
      return AV_CODEC_ID_MPEG2VIDEO;
    case VideoCodec::Mpeg1:
      return AV_CODEC_ID_MPEG1VIDEO;
    case VideoCodec::H263:
      return AV_CODEC_ID_H263;
    case VideoCodec::Vc1:
      return AV_CODEC_ID_VC1;
    case VideoCodec::Wmv:
      return AV_CODEC_ID_WMV3;
    case VideoCodec::ProRes:
      return AV_CODEC_ID_PRORES;
    case VideoCodec::Dnxhd:
      return AV_CODEC_ID_DNXHD;
    case VideoCodec::Ffv1:
      return AV_CODEC_ID_FFV1;
    case VideoCodec::Mjpeg:
      return AV_CODEC_ID_MJPEG;
  }
  return AV_CODEC_ID_NONE;
}

AVCodecID avCodecFor(AudioCodec codec) {
  switch (codec) {
    case AudioCodec::Aac:
      return AV_CODEC_ID_AAC;
    case AudioCodec::HeAac:
      return AV_CODEC_ID_AAC; // profile selected at encoder config time
    case AudioCodec::Mp3:
      return AV_CODEC_ID_MP3;
    case AudioCodec::Opus:
      return AV_CODEC_ID_OPUS;
    case AudioCodec::Vorbis:
      return AV_CODEC_ID_VORBIS;
    case AudioCodec::Flac:
      return AV_CODEC_ID_FLAC;
    case AudioCodec::Alac:
      return AV_CODEC_ID_ALAC;
    case AudioCodec::Pcm:
      return AV_CODEC_ID_PCM_S16LE;
    case AudioCodec::Ac3:
      return AV_CODEC_ID_AC3;
    case AudioCodec::Eac3:
      return AV_CODEC_ID_EAC3;
    case AudioCodec::Dts:
      return AV_CODEC_ID_DTS;
    case AudioCodec::TrueHd:
      return AV_CODEC_ID_TRUEHD;
    case AudioCodec::Wma:
      return AV_CODEC_ID_WMAV2;
    case AudioCodec::WavPack:
      return AV_CODEC_ID_WAVPACK;
    case AudioCodec::Ape:
      return AV_CODEC_ID_APE;
    case AudioCodec::AmrNb:
      return AV_CODEC_ID_AMR_NB;
    case AudioCodec::AmrWb:
      return AV_CODEC_ID_AMR_WB;
  }
  return AV_CODEC_ID_NONE;
}

const char *avFormatNameFor(Container container) {
  switch (container) {
    case Container::Mp4:
      return "mp4";
    case Container::M4v:
      return "ipod"; // M4V-compatible ISO-BMFF muxer
    case Container::Mov:
      return "mov";
    case Container::Mkv:
      return "matroska";
    case Container::WebM:
      return "webm";
    case Container::Avi:
      return "avi";
    case Container::MpegTs:
      return "mpegts";
    case Container::Flv:
      return "flv";
    case Container::ThreeGp:
      return "3gp";
    case Container::M4a:
      return "ipod";
    case Container::Ogg:
      return "ogg";
    case Container::Mp3:
      return "mp3";
    case Container::Wav:
      return "wav";
    case Container::Flac:
      return "flac";
    case Container::Asf:
      return "asf";
  }
  return "mp4";
}

std::optional<Container> containerFromAvFormatName(const char *name) {
  if (name == nullptr) {
    return std::nullopt;
  }
  const std::string n = name;
  if (n == "mp4" || n.rfind("mp4,", 0) == 0) {
    return Container::Mp4;
  }
  if (n == "mov,mp4,m4a,3gp,3g2,mj2" || n == "mov") {
    return Container::Mov;
  }
  if (n == "matroska,webm" || n == "matroska") {
    return Container::Mkv;
  }
  if (n == "webm") {
    return Container::WebM;
  }
  if (n == "avi") {
    return Container::Avi;
  }
  if (n == "mpegts") {
    return Container::MpegTs;
  }
  if (n == "flv") {
    return Container::Flv;
  }
  if (n == "3gp" || n.rfind("3gp,", 0) == 0) {
    return Container::ThreeGp;
  }
  if (n == "ipod" || n == "m4a" || n == "m4v") {
    return Container::M4a;
  }
  if (n == "ogg" || n.rfind("ogg,", 0) == 0) {
    return Container::Ogg;
  }
  if (n == "mp3") {
    return Container::Mp3;
  }
  if (n == "wav") {
    return Container::Wav;
  }
  if (n == "flac") {
    return Container::Flac;
  }
  if (n == "asf") {
    return Container::Asf;
  }
  return std::nullopt;
}

} // namespace velmux::ffmpeg
