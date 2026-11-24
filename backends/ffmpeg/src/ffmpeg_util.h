// Internal shared helpers for the FFmpeg backend translation units. Pure
// functions only; backend identity state stays in ffmpeg_backend.cpp.
#pragma once

#include "velmux/core/types.h"

#include <algorithm>
#include <cerrno>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}

namespace velmux::ffmpeg {

using velmux::core::AudioCodec;
using velmux::core::Error;
using velmux::core::ErrorCode;
using velmux::core::VideoCodec;

inline std::string avErrorString(int errnum) {
  char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
  av_strerror(errnum, buf, sizeof(buf));
  return buf;
}

inline Error makeAvError(ErrorCode code, const std::string &context,
                         int errnum) {
  return Error::make(code, context + ": " + avErrorString(errnum), "ffmpeg");
}

// Output-write failure mapping. A full disk is a terminal environmental
// error (ErrorCode::StorageFull): the recovery layer must surface it to
// the user, never retry or silently truncate. The errno check is
// authoritative (libav* reports write errors as AVERROR(ENOSPC)); the
// strerror text is a fallback for paths where only a message survives.
inline bool isNoSpaceLeft(int errnum) {
  return errnum == AVERROR(ENOSPC) ||
         avErrorString(errnum).find("No space left") != std::string::npos;
}

inline Error outputWriteError(int errnum, const std::string &context,
                              const std::string &outputPath) {
  if (isNoSpaceLeft(errnum)) {
    return Error::make(ErrorCode::StorageFull,
                       context + ": no space left on device while writing " +
                           outputPath,
                       "ffmpeg");
  }
  if (errnum < 0) {
    // Negative = an AVERROR code from libav*; keep the established
    // "context: strerror" style. Zero = unknown cause (e.g. a failed
    // stream write without a code): the bare context stays honest.
    return Error::make(ErrorCode::ExecutionFailed,
                       context + ": " + avErrorString(errnum), "ffmpeg");
  }
  return Error::make(ErrorCode::ExecutionFailed, context, "ffmpeg");
}

// Map a Velmux quality request (0..100) to the encoder's CRF-style quality
// parameter (higher quality -> lower CRF).
inline int qualityToCrf(VideoCodec codec, std::int32_t quality) {
  const int q = std::clamp(quality, 0, 100);
  switch (codec) {
    case VideoCodec::H264: // libx264 crf 0..51
    case VideoCodec::HEVC: // libx265 crf 0..51
      return 51 - (q * 51) / 100;
    case VideoCodec::AV1: // libsvtav1 crf 0..63
    case VideoCodec::VP9: // libvpx-vp9 crf 0..63
    case VideoCodec::VP8:
      return 63 - (q * 63) / 100;
    default:
      return 23;
  }
}

// Concrete libavcodec encoder names for the feature-selected build: prefer
// the external libraries Velmux pins (tools/ffmpeg-builder/versions.json).
inline const char *encoderNameFor(VideoCodec codec) {
  switch (codec) {
    case VideoCodec::H264:
      return "libx264";
    case VideoCodec::HEVC:
      // Pinned since the x265 3.6 build (feature matrix: HEVC D+E+C).
      return "libx265";
    case VideoCodec::AV1:
      return "libsvtav1";
    case VideoCodec::VP8:
      return "libvpx-vp8";
    case VideoCodec::VP9:
      return "libvpx-vp9";
    case VideoCodec::Mpeg4Part2:
      return "mpeg4";
    case VideoCodec::Ffv1:
      return "ffv1";
    case VideoCodec::Mjpeg:
      return "mjpeg";
    default:
      return nullptr; // no software encoder for this codec in the pinned build
  }
}

inline const char *encoderNameFor(AudioCodec codec) {
  switch (codec) {
    case AudioCodec::Aac:
    case AudioCodec::HeAac:
      return "aac"; // FFmpeg-native AAC encoder
    case AudioCodec::Mp3:
      return "libmp3lame";
    case AudioCodec::Opus:
      return "libopus";
    case AudioCodec::Vorbis:
      return "libvorbis";
    case AudioCodec::Flac:
      return "flac";
    case AudioCodec::Alac:
      return "alac";
    case AudioCodec::Pcm:
      return "pcm_s16le";
    case AudioCodec::Ac3:
      return "ac3";
    case AudioCodec::Eac3:
      return "eac3";
    case AudioCodec::WavPack:
      return "wavpack";
    default:
      return nullptr;
  }
}

} // namespace velmux::ffmpeg
