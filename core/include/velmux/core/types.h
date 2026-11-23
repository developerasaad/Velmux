// Velmux core — foundational types shared by every module.
//
// The C++ core owns policy (feature registry, capability registry, planning,
// jobs, recovery). Backends own execution. This header defines the value
// vocabulary used across that boundary.
//
// Copyright: Velmux project. License: see repository LICENSE.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace velmux::core {

// ---------------------------------------------------------------------------
// Identifiers
// ---------------------------------------------------------------------------

enum class BackendId : std::uint8_t {
  Ffmpeg,
  Android,
  Apple,
};

inline constexpr const char *backendName(BackendId id) {
  switch (id) {
    case BackendId::Ffmpeg:
      return "ffmpeg";
    case BackendId::Android:
      return "android";
    case BackendId::Apple:
      return "apple";
  }
  return "unknown";
}

enum class StageId : std::uint8_t {
  Demux,
  Decode,
  VideoProcessing,
  AudioProcessing,
  SubtitleProcessing,
  Encode,
  Mux,
  Bitstream,
};

inline constexpr const char *stageName(StageId id) {
  switch (id) {
    case StageId::Demux:
      return "demux";
    case StageId::Decode:
      return "decode";
    case StageId::VideoProcessing:
      return "video-processing";
    case StageId::AudioProcessing:
      return "audio-processing";
    case StageId::SubtitleProcessing:
      return "subtitle-processing";
    case StageId::Encode:
      return "encode";
    case StageId::Mux:
      return "mux";
    case StageId::Bitstream:
      return "bitstream";
  }
  return "unknown";
}

// The processing pipeline stages a planner assigns. Order matters: it mirrors
// the data flow of a conversion.
inline constexpr std::array<StageId, 8> kAllStages = {
    StageId::Demux,       StageId::Decode,     StageId::VideoProcessing,
    StageId::AudioProcessing, StageId::SubtitleProcessing, StageId::Encode,
    StageId::Mux,         StageId::Bitstream};

// ---------------------------------------------------------------------------
// Media vocabulary (product-controlled; see docs/FEATURE_MATRIX.md)
// ---------------------------------------------------------------------------

enum class VideoCodec : std::uint16_t {
  H264,      // H.264 / AVC
  HEVC,      // H.265 / HEVC
  AV1,
  VP8,
  VP9,
  Mpeg4Part2,
  Mpeg2,
  Mpeg1,
  H263,
  Vc1,
  Wmv,
  ProRes,
  Dnxhd,
  Ffv1,
  Mjpeg,
};

enum class AudioCodec : std::uint16_t {
  Aac,    // AAC-LC
  HeAac,  // HE-AAC
  Mp3,
  Opus,
  Vorbis,
  Flac,
  Alac,
  Pcm,
  Ac3,
  Eac3,
  Dts,
  TrueHd,
  Wma,
  WavPack,
  Ape,
  AmrNb,
  AmrWb,
};

enum class Container : std::uint16_t {
  Mp4,
  M4v,
  Mov,
  Mkv,
  WebM,
  Avi,
  MpegTs,
  Flv,
  ThreeGp,
  M4a,
  Ogg,
  Mp3,
  Wav,
  Flac,
  Asf,
};

enum class SubtitleFormat : std::uint8_t {
  Srt,
  WebVtt,
  Ass,
  Ssa,
  MovText,
  Ttml,
  Cea608,
  Cea708,
  Pgs,
  VobSub,
  Dvb,
};

enum class MediaKind : std::uint8_t { Video, Audio, Subtitle, Data };

// Output pixel formats the request model can name (advanced controls,
// spec §18). Encoders accept a subset; the planner only routes such
// requests to backends reporting `videoEncoderTuning`.
enum class PixelFormat : std::uint8_t {
  Yuv420P,     // 4:2:0 8-bit — the universal default
  Yuv422P,     // 4:2:2 8-bit
  Yuv444P,     // 4:4:4 8-bit
  Yuv420P10Le, // 4:2:0 10-bit little-endian
  Yuv422P10Le, // 4:2:2 10-bit little-endian
  Yuv444P10Le, // 4:4:4 10-bit little-endian
};

// HDR handling policy for transcodes (spec §9: preserve HDR where
// compatible, convert HDR -> SDR where supported).
enum class HdrPolicy : std::uint8_t {
  Auto,        // preserve HDR signaling when the output path can carry it
  ToneMapToSdr, // convert HDR (PQ/HLG, wide gamut) to 8-bit SDR BT.709
};

enum class CodecOp : std::uint8_t { Decode, Encode, StreamCopy };

// Per-codec operation support (shared by feature and capability
// registries).
struct CodecSupport {
  bool decode = false;
  bool encode = false;
  bool streamCopy = false;
};

// Canonical names (used in JSON persistence, the bridge and logs).
const char *codecName(VideoCodec codec);
const char *codecName(AudioCodec codec);
const char *containerName(Container container);
const char *subtitleFormatName(SubtitleFormat format);

// Parsing from canonical names (returns nullopt for unknown names, used by
// the bridge when translating UI selections).
std::optional<VideoCodec> videoCodecFromName(const std::string &name);
std::optional<AudioCodec> audioCodecFromName(const std::string &name);
std::optional<Container> containerFromName(const std::string &name);
std::optional<SubtitleFormat> subtitleFormatFromName(const std::string &name);
const char *pixelFormatName(PixelFormat format);
std::optional<PixelFormat> pixelFormatFromName(const std::string &name);

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

enum class ErrorCode : std::uint8_t {
  None = 0,
  InvalidArgument,
  SourceUnavailable,
  UnsupportedFeature,
  NoCompatiblePipeline,
  InspectionFailed,
  ExecutionFailed,
  BackendFailure,
  OutputValidationFailed,
  StorageFull,
  Cancelled,
  Interrupted,
  InternalError,
};

const char *errorCodeName(ErrorCode code);

struct Error {
  ErrorCode code = ErrorCode::None;
  std::string message;
  // Backend that produced the error, when applicable.
  std::string backend;

  bool ok() const { return code == ErrorCode::None; }
  static Error make(ErrorCode code, std::string message,
                    std::string backend = {}) {
    return Error{code, std::move(message), std::move(backend)};
  }
};

// Minimal Result<T>: either a value or an Error. Explicit, no exceptions.
template <typename T> class Result {
public:
  Result(T value) : value_(std::move(value)) {}
  Result(Error error) : error_(std::move(error)) {}

  bool ok() const { return !error_.has_value(); }
  explicit operator bool() const { return ok(); }

  const T &value() const & { return value_; }
  T &value() & { return value_; }
  T &&take() { return std::move(value_); }
  const Error &error() const { return *error_; }

private:
  T value_{};
  std::optional<Error> error_;
};

template <> class Result<void> {
public:
  Result() = default;
  Result(Error error) : error_(std::move(error)) {}
  bool ok() const { return !error_.has_value(); }
  explicit operator bool() const { return ok(); }
  const Error &error() const { return *error_; }

private:
  std::optional<Error> error_;
};

// ---------------------------------------------------------------------------
// Time / rates
// ---------------------------------------------------------------------------

// Rational number used for frame rates and aspect ratios.
struct Rational {
  std::int64_t num = 0;
  std::int64_t den = 1;
  double toDouble() const { return den == 0 ? 0.0 : double(num) / double(den); }
  bool valid() const { return den != 0 && num != 0; }
};

} // namespace velmux::core
