// Android stage workers — implementation. See android_stages.h.
//
// Real body compiles only on Android (__ANDROID__), exactly like
// media_transcoder.cpp; host builds keep the factory symbols (honest nulls)
// so android_backend.cpp links everywhere.
#include "android_stages.h"

#ifdef __ANDROID__

#include "media_util.h"

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaMuxer.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>

#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

namespace velmux::android::media {

using namespace velmux::core;
using velmux::backend::CpuFramePtr;
using velmux::backend::ICpuFrame;
using velmux::backend::InterchangeFormat;
using velmux::backend::InterchangePixel;
using velmux::backend::PacketView;
using velmux::backend::StageEncoderConfig;

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

namespace {

// Velmux codec -> MediaCodec mime. Mirrors media_transcoder.cpp's
// videoMimeFor (an anonymous-namespace symbol there; duplicated here rather
// than exported, so both files stay independently reviewable).
const char *stageVideoMimeFor(VideoCodec codec) {
  switch (codec) {
  case VideoCodec::H264:
    return "video/avc";
  case VideoCodec::HEVC:
    return "video/hevc";
  case VideoCodec::VP8:
    return "video/x-vnd.on2.vp8";
  case VideoCodec::VP9:
    return "video/x-vnd.on2.vp9";
  case VideoCodec::AV1:
    return "video/av01";
  case VideoCodec::Mpeg4Part2:
    return "video/mp4v-es";
  case VideoCodec::H263:
    return "video/3gpp";
  case VideoCodec::Mpeg2:
    return "video/mpeg2";
  case VideoCodec::Mpeg1:
    return "video/mpeg";
  default:
    return nullptr; // no Android platform mapping for this Velmux codec
  }
}

// Opens a writable file descriptor for AMediaMuxer (the NDK muxer is
// fd-based; the path-based constructor is Java-only). Mirrors
// media_transcoder.cpp.
int openStageMuxerFd(const std::string &path) {
  return ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
}

// MediaCodecInfo.CodecCapabilities color-format values. The NDK headers do
// not define them; the numbers are the public Java constants.
constexpr int32_t kColorFormatYuv420Planar = 19;      // I420: Y, U, V
constexpr int32_t kColorFormatYuv420PackedPlanar = 20;
constexpr int32_t kColorFormatYuv420SemiPlanar = 21;  // NV12: Y, interleaved UV
constexpr int32_t kColorFormatYuv420Flexible = 0x7F420888;

// Extractor sample buffer that grows to the largest sample seen (mirrors
// media_transcoder.cpp's SampleBuffer).
class StageSampleBuffer final {
public:
  uint8_t *data() { return buffer_.data(); }
  size_t capacity() const { return buffer_.size(); }
  void ensure(size_t size) {
    if (size > buffer_.size()) {
      buffer_.resize(std::max(size, buffer_.size() * 2));
    }
  }

private:
  std::vector<uint8_t> buffer_;
};

} // namespace

// ---------------------------------------------------------------------------
// CPU frame (heap planar Yuv420P)
// ---------------------------------------------------------------------------

namespace {

// ICpuFrame over a packed heap Yuv420P buffer: Y (w*h), U (w*h/4), V
// (w*h/4), linesizes w / w/2 / w/2.
class AndroidCpuFrame final : public ICpuFrame {
public:
  bool allocate(int width, int height) {
    if (width <= 0 || height <= 0 || (width & 1) != 0 || (height & 1) != 0) {
      return false;
    }
    width_ = width;
    height_ = height;
    data_.assign(size_t(width) * size_t(height) * 3 / 2, 0);
    return true;
  }

  // Copies one AImage plane (rowStride/pixelStride layout, planar or
  // semi-planar) into the packed interchange plane.
  bool copyPlane(int plane, const uint8_t *src, int32_t srcLen,
                 int32_t rowStride, int32_t pixelStride) {
    if (src == nullptr || rowStride <= 0 || pixelStride <= 0) {
      return false;
    }
    const int pw = plane == 0 ? width_ : width_ / 2;
    const int ph = plane == 0 ? height_ : height_ / 2;
    const std::int64_t lastByte =
        std::int64_t(ph - 1) * rowStride + std::int64_t(pw - 1) * pixelStride;
    if (lastByte >= std::int64_t(srcLen)) {
      return false; // the plane is smaller than described: never read past it
    }
    uint8_t *dst = planePtr(plane);
    if (dst == nullptr) {
      return false;
    }
    const int dstStride = planeLinesize(plane);
    for (int y = 0; y < ph; ++y) {
      const uint8_t *row = src + std::int64_t(y) * rowStride;
      uint8_t *out = dst + std::int64_t(y) * dstStride;
      if (pixelStride == 1) {
        std::memcpy(out, row, size_t(pw));
      } else {
        for (int x = 0; x < pw; ++x) {
          out[x] = row[std::int64_t(x) * pixelStride];
        }
      }
    }
    return true;
  }

  int planeCount() const override { return 3; } // Yuv420P
  const std::uint8_t *planeData(int plane) const override {
    if (plane < 0 || plane > 2 || data_.empty()) {
      return nullptr;
    }
    return data_.data() + planeOffset(plane);
  }
  int planeLinesize(int plane) const override {
    if (plane < 0 || plane > 2) {
      return 0;
    }
    return plane == 0 ? width_ : width_ / 2;
  }
  std::int64_t ptsUs() const override { return ptsUs_; }
  std::int64_t durationUs() const override { return durationUs_; }

  void setTiming(std::int64_t pts, std::int64_t duration) {
    ptsUs_ = pts;
    durationUs_ = duration;
  }

private:
  size_t planeOffset(int plane) const {
    const size_t luma = size_t(width_) * size_t(height_);
    if (plane <= 0) {
      return 0;
    }
    if (plane == 1) {
      return luma;
    }
    return luma + luma / 4;
  }
  std::uint8_t *planePtr(int plane) {
    if (plane < 0 || plane > 2 || data_.empty()) {
      return nullptr;
    }
    return data_.data() + planeOffset(plane);
  }

  int width_ = 0;
  int height_ = 0;
  std::vector<std::uint8_t> data_;
  std::int64_t ptsUs_ = 0;
  std::int64_t durationUs_ = 0;
};

} // namespace

// ---------------------------------------------------------------------------
// Decoder: MediaExtractor -> MediaCodec -> AImageReader -> heap frames
// ---------------------------------------------------------------------------

struct AndroidStageDecoder::Impl {
  AMediaExtractor *extractor = nullptr;
  AMediaCodec *codec = nullptr;
  AImageReader *reader = nullptr;
  // Borrowed from the reader via AImageReader_getWindow: owned by the
  // reader (NDK header: 'Do NOT call ANativeWindow_release on it.
  // Instead, use AImageReader_delete'). Held only to configure the codec.
  ANativeWindow *window = nullptr;
  int extractorFd = -1; // source fd; closed after the extractor dies
  int videoTrack = -1;
  std::string videoMime;
  bool opened = false;
  bool inputEos = false;  // end-of-stream queued to the codec input
  bool outputEos = false; // end-of-stream observed on the codec output
  std::chrono::steady_clock::time_point outputEosAt{};
  StageSampleBuffer sample;
  InterchangeFormat format;
  std::int64_t duration = 0; // microseconds
  double frameRate = 0.0;    // from the track format, 0 = unknown
  // Last emitted frame pts (us); -1 = nothing emitted yet. The output
  // timeline must be strictly increasing (see next()).
  std::int64_t lastPtsUs = -1;
};

AndroidStageDecoder::AndroidStageDecoder()
    : impl_(std::make_unique<Impl>()) {}

AndroidStageDecoder::~AndroidStageDecoder() {
  if (impl_ == nullptr) {
    return;
  }
  Impl &d = *impl_;
  if (d.codec != nullptr) {
    AMediaCodec_stop(d.codec);
    AMediaCodec_delete(d.codec);
  }
  // The window is NOT released here: AImageReader_getWindow returns a
  // pointer owned by the reader ('Do NOT call ANativeWindow_release on
  // it. Instead, use AImageReader_delete' — NdkImageReader.h). Releasing
  // it destroyed the reader's own Surface underneath it, and the
  // subsequent AImageReader_delete dereferenced the freed object —
  // SIGSEGV in ~AImageReader/RefBase::decStrong, observed on the API 29
  // and 34 emulators (the instrumented hybrid tests, 2026-11-24). The
  // codec must be deleted first (it holds its own surface reference from
  // configure); AImageReader_delete then tears the window down safely.
  if (d.reader != nullptr) {
    AImageReader_delete(d.reader);
  }
  if (d.extractor != nullptr) {
    AMediaExtractor_delete(d.extractor);
  }
  if (d.extractorFd >= 0) {
    ::close(d.extractorFd);
    d.extractorFd = -1;
  }
}

Error AndroidStageDecoder::open(const std::string &sourcePath,
                                InterchangePixel pixel) {
  if (pixel != InterchangePixel::Yuv420P) {
    return Error::make(ErrorCode::UnsupportedOperation,
                       "unsupported interchange pixel format", "android");
  }
  Impl &d = *impl_;
  d.extractor = AMediaExtractor_new();
  // fd-based source open: the path-based call fails on API 29
  // ('can't create http service' — see media_util.h).
  if (d.extractor == nullptr ||
      !setExtractorDataSourceFd(d.extractor, sourcePath, d.extractorFd)) {
    return Error::make(ErrorCode::SourceUnavailable,
                       "cannot open the source", "android");
  }
  const size_t trackCount = AMediaExtractor_getTrackCount(d.extractor);
  AMediaFormat *trackFormat = nullptr;
  for (size_t i = 0; i < trackCount; ++i) {
    AMediaFormat *format = AMediaExtractor_getTrackFormat(d.extractor, i);
    if (format == nullptr) {
      continue;
    }
    const char *mime = nullptr;
    if (!AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &mime) ||
        mime == nullptr || std::strncmp(mime, "video/", 6) != 0) {
      AMediaFormat_delete(format);
      continue;
    }
    d.videoTrack = int(i);
    d.videoMime = mime;
    trackFormat = format;
    AMediaExtractor_selectTrack(d.extractor, i);
    break;
  }
  if (trackFormat == nullptr) {
    return Error::make(ErrorCode::InvalidArgument,
                       "the source has no video track", "android");
  }
  int32_t width = 0;
  int32_t height = 0;
  std::int64_t durationUs = 0;
  AMediaFormat_getInt32(trackFormat, AMEDIAFORMAT_KEY_WIDTH, &width);
  AMediaFormat_getInt32(trackFormat, AMEDIAFORMAT_KEY_HEIGHT, &height);
  AMediaFormat_getInt64(trackFormat, AMEDIAFORMAT_KEY_DURATION, &durationUs);
  float frameRateF = 0.0f;
  if (!AMediaFormat_getFloat(trackFormat, "frame-rate", &frameRateF)) {
    int32_t frameRateI = 0;
    if (AMediaFormat_getInt32(trackFormat, "frame-rate", &frameRateI)) {
      frameRateF = float(frameRateI);
    }
  }
  if (width <= 0 || height <= 0) {
    AMediaFormat_delete(trackFormat);
    return Error::make(ErrorCode::InvalidArgument,
                       "the source video track has no geometry", "android");
  }
  d.duration = durationUs > 0 ? durationUs : 0;
  d.frameRate = frameRateF > 0.0f ? double(frameRateF) : 0.0;

  // Decode target: an AImageReader Surface carrying YUV_420_888 frames the
  // CPU can read back (CPU_READ_OFTEN); GPU_COLOR_OUTPUT keeps the buffer
  // allocation compatible with codec Surface output. maxImages 3 gives the
  // decoder enough slack while still applying backpressure.
  if (AImageReader_newWithUsage(
          width, height, AIMAGE_FORMAT_YUV_420_888,
          AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
              AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT,
          3, &d.reader) != AMEDIA_OK ||
      d.reader == nullptr ||
      AImageReader_getWindow(d.reader, &d.window) != AMEDIA_OK ||
      d.window == nullptr) {
    return Error::make(ErrorCode::ExecutionFailed,
                       "cannot create the decode image reader", "android");
  }
  d.codec = AMediaCodec_createDecoderByType(d.videoMime.c_str());
  if (d.codec == nullptr ||
      AMediaCodec_configure(d.codec, trackFormat, d.window, nullptr, 0) !=
          AMEDIA_OK ||
      AMediaCodec_start(d.codec) != AMEDIA_OK) {
    AMediaFormat_delete(trackFormat);
    return Error::make(ErrorCode::ExecutionFailed,
                       "cannot start the video decoder", "android");
  }
  AMediaFormat_delete(trackFormat);
  // v1 hybrid carries the coded geometry 1:1 (the planner forbids transform
  // requests on hybrid routes); rotation metadata is intentionally not
  // re-applied here.
  d.format.pixel = pixel;
  d.format.width = width;
  d.format.height = height;
  d.opened = true;
  return Error::make(ErrorCode::None, "");
}

InterchangeFormat AndroidStageDecoder::outputFormat() const {
  return impl_->format;
}

std::int64_t AndroidStageDecoder::durationUs() const {
  return impl_->duration;
}

Error AndroidStageDecoder::next(CpuFramePtr &frame) {
  frame = nullptr;
  Impl &d = *impl_;
  if (!d.opened) {
    return Error::make(ErrorCode::InvalidArgument, "decoder not opened",
                       "android");
  }
  auto lastProgress = std::chrono::steady_clock::now();
  for (;;) {
    bool progressed = false;

    // 1. Feed the codec input (non-blocking; the EOS queue may repeat).
    if (!d.inputEos) {
      const ssize_t inIdx = AMediaCodec_dequeueInputBuffer(d.codec, 0);
      if (inIdx >= 0) {
        const int trackIdx = AMediaExtractor_getSampleTrackIndex(d.extractor);
        if (trackIdx < 0) {
          // Source exhausted: end of stream for the selected track.
          if (AMediaCodec_queueInputBuffer(
                  d.codec, size_t(inIdx), 0, 0, 0,
                  AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) == AMEDIA_OK) {
            d.inputEos = true;
            progressed = true;
          }
        } else if (trackIdx != d.videoTrack) {
          AMediaExtractor_advance(d.extractor); // unselected track: skip
          progressed = true;
        } else {
          size_t capacity = 0;
          uint8_t *dst =
              AMediaCodec_getInputBuffer(d.codec, size_t(inIdx), &capacity);
          const ssize_t sampleSize =
              AMediaExtractor_getSampleSize(d.extractor);
          if (dst == nullptr || sampleSize < 0 ||
              size_t(sampleSize) > capacity) {
            // Defensive: oversized samples for this codec are skipped,
            // never silently truncated (mirrors media_transcoder.cpp).
            AMediaCodec_queueInputBuffer(d.codec, size_t(inIdx), 0, 0, 0, 0);
            if (sampleSize > 0) {
              AMediaExtractor_advance(d.extractor);
            }
            progressed = true;
          } else {
            d.sample.ensure(size_t(sampleSize));
            const ssize_t copied = AMediaExtractor_readSampleData(
                d.extractor, d.sample.data(), d.sample.capacity());
            if (copied == sampleSize) {
              const std::int64_t ptsUs =
                  AMediaExtractor_getSampleTime(d.extractor);
              std::memcpy(dst, d.sample.data(), size_t(sampleSize));
              AMediaCodec_queueInputBuffer(
                  d.codec, size_t(inIdx), 0, size_t(sampleSize),
                  uint64_t(ptsUs < 0 ? 0 : ptsUs), 0);
              AMediaExtractor_advance(d.extractor);
              progressed = true;
            } else {
              // Capacity grew; retry the same sample on the next pass.
              AMediaCodec_queueInputBuffer(d.codec, size_t(inIdx), 0, 0, 0,
                                           0);
            }
          }
        }
      }
    }

    // 2. Acquire a rendered frame (non-blocking; the acquire fails while
    // no image has arrived yet).
    AImage *image = nullptr;
    if (AImageReader_acquireNextImage(d.reader, &image) == AMEDIA_OK &&
        image != nullptr) {
      int32_t imgWidth = 0;
      int32_t imgHeight = 0;
      AImage_getWidth(image, &imgWidth);
      AImage_getHeight(image, &imgHeight);
      const int w = imgWidth > 0 ? imgWidth : d.format.width;
      const int h = imgHeight > 0 ? imgHeight : d.format.height;
      auto out = std::make_unique<AndroidCpuFrame>();
      if (!out->allocate(w, h)) {
        AImage_delete(image);
        return Error::make(ErrorCode::ExecutionFailed,
                           "cannot allocate the interchange frame", "android");
      }
      bool planesOk = true;
      for (int p = 0; p < 3 && planesOk; ++p) {
        int32_t rowStride = 0;
        int32_t pixelStride = 0;
        std::uint8_t *src = nullptr;
        int32_t srcLen = 0;
        if (AImage_getPlaneRowStride(image, p, &rowStride) != AMEDIA_OK ||
            AImage_getPlanePixelStride(image, p, &pixelStride) != AMEDIA_OK ||
            AImage_getPlaneData(image, p, &src, &srcLen) != AMEDIA_OK) {
          planesOk = false;
          break;
        }
        // Handles both planar (pixelStride 1) and semi-planar
        // (pixelStride 2, U/V interleaved) AImage layouts.
        if (!out->copyPlane(p, src, srcLen, rowStride, pixelStride)) {
          planesOk = false;
        }
      }
      std::int64_t timestampNs = 0;
      AImage_getTimestamp(image, &timestampNs);
      AImage_delete(image);
      if (!planesOk) {
        return Error::make(ErrorCode::ExecutionFailed,
                           "cannot read the decoded frame planes", "android");
      }
      // AImage timestamps are nanoseconds on the source timeline (the
      // codec preserves the queued microsecond pts through the Surface).
      std::int64_t ptsUs = timestampNs / 1000;
      const std::int64_t durUs =
          d.frameRate > 0.0
              ? std::int64_t(1000000.0 / d.frameRate)
              : 0;
      // The interchange contract requires a strictly increasing frame
      // timeline. Platform decoders have been observed to deliver rendered
      // buffers with duplicate timestamps (seen on the x86_64 emulator
      // codecs); MediaMuxer tolerates equal pts but the FFmpeg muxer
      // rejects non-increasing dts and fails the whole conversion. Rebase
      // non-increasing (or negative) timestamps onto the previous frame +
      // one frame duration: order and pacing are preserved, and a
      // platform quirk cannot poison the downstream muxers.
      if (ptsUs < 0) {
        ptsUs = 0;
      }
      if (d.lastPtsUs >= 0 && ptsUs <= d.lastPtsUs) {
        ptsUs = d.lastPtsUs + (durUs > 0 ? durUs : 1);
      }
      d.lastPtsUs = ptsUs;
      out->setTiming(ptsUs, durUs);
      frame = std::move(out);
      // Frames rendered before the EOS buffer may still be in flight;
      // restart the EOS grace window whenever one arrives.
      if (d.outputEos) {
        d.outputEosAt = std::chrono::steady_clock::now();
      }
      return Error::make(ErrorCode::None, "");
    }

    // 3. Drain the codec output: rendering hands frames to the reader.
    AMediaCodecBufferInfo info{};
    const ssize_t outIdx =
        AMediaCodec_dequeueOutputBuffer(d.codec, &info, 10000);
    if (outIdx >= 0) {
      const bool eos =
          (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
      // Render=true forwards the decoded frame into the AImageReader
      // surface; the EOS buffer carries no frame (rendering it is a no-op).
      AMediaCodec_releaseOutputBuffer(d.codec, size_t(outIdx), true);
      if (eos && !d.outputEos) {
        d.outputEos = true;
        d.outputEosAt = std::chrono::steady_clock::now();
      }
      progressed = true;
    } else if (outIdx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
      AMediaFormat *format = AMediaCodec_getOutputFormat(d.codec);
      if (format != nullptr) {
        int32_t w = 0;
        int32_t h = 0;
        if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &w) &&
            w > 0) {
          d.format.width = w;
        }
        if (AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &h) &&
            h > 0) {
          d.format.height = h;
        }
        AMediaFormat_delete(format);
      }
      progressed = true;
    }

    // 4. Termination conditions.
    if (d.outputEos) {
      // Give frames rendered before EOS a bounded grace window to reach
      // the reader, then report end of stream (null frame, no error).
      if (std::chrono::steady_clock::now() - d.outputEosAt >
          std::chrono::milliseconds(500)) {
        return Error::make(ErrorCode::None, "");
      }
    } else if (progressed) {
      lastProgress = std::chrono::steady_clock::now();
    }
    if (std::chrono::steady_clock::now() - lastProgress >
        std::chrono::seconds(30)) {
      return Error::make(ErrorCode::ExecutionFailed,
                         "the stage decoder stalled", "android");
    }
  }
}

// ---------------------------------------------------------------------------
// Encoder: MediaCodec ByteBuffer mode
// ---------------------------------------------------------------------------

struct AndroidStageEncoder::Impl {
  AMediaCodec *codec = nullptr;
  bool opened = false;
  bool flushed = false;
  bool eosOutputSeen = false;
  int width = 0;
  int height = 0;
  std::int64_t frameDurationUs = 0;
  std::int64_t lastPtsUs = -1;
  std::vector<std::uint8_t> config; // TLV of the output format csd buffers
  bool configCaptured = false;
  std::vector<std::uint8_t> packet; // owned packet storage (until next pull)
};

AndroidStageEncoder::AndroidStageEncoder()
    : impl_(std::make_unique<Impl>()) {}

AndroidStageEncoder::~AndroidStageEncoder() {
  if (impl_ == nullptr) {
    return;
  }
  if (impl_->codec != nullptr) {
    AMediaCodec_stop(impl_->codec);
    AMediaCodec_delete(impl_->codec);
  }
}

Error AndroidStageEncoder::open(const StageEncoderConfig &config,
                                InterchangePixel pixel) {
  if (pixel != InterchangePixel::Yuv420P) {
    return Error::make(ErrorCode::UnsupportedOperation,
                       "unsupported interchange pixel format", "android");
  }
  const char *mime = stageVideoMimeFor(config.codec);
  if (mime == nullptr) {
    return Error::make(
        ErrorCode::UnsupportedOperation,
        std::string("no Android encoder mime for codec: ") +
            codecName(config.codec),
        "android");
  }
  if (config.width <= 0 || config.height <= 0) {
    return Error::make(ErrorCode::InvalidArgument,
                       "encoder geometry is not set", "android");
  }
  if ((config.width % 2) != 0 || (config.height % 2) != 0) {
    // 4:2:0 chroma subsampling needs even dimensions; refuse instead of
    // silently cropping.
    return Error::make(ErrorCode::InvalidArgument,
                       "odd geometry is not supported by the Android stage "
                       "encoder",
                       "android");
  }
  Impl &e = *impl_;
  const double fps = config.frameRate > 0.0 ? config.frameRate : 30.0;
  std::int64_t bitRate = config.bitRate;
  if (bitRate <= 0) {
    // Documented quality heuristic: bits-per-pixel scaling between 0.01
    // (quality 0) and 0.13 (quality 100) of the raw pixel rate, clamped
    // into a sane AVC/HEVC range.
    const int quality = std::clamp(config.quality.value_or(60), 0, 100);
    const double bitsPerPixel = 0.01 + 0.12 * double(quality) / 100.0;
    bitRate = std::int64_t(bitsPerPixel * double(config.width) *
                           double(config.height) * fps);
    bitRate = std::clamp<std::int64_t>(bitRate, 250000, 80000000);
  }
  AMediaFormat *format = AMediaFormat_new();
  AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, mime);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, config.width);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, config.height);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_FRAME_RATE,
                        int32_t(std::max(1, int(std::lround(fps)))));
  AMediaFormat_setInt32(
      format, AMEDIAFORMAT_KEY_BIT_RATE,
      int32_t(std::min<std::int64_t>(bitRate,
                                     std::numeric_limits<int32_t>::max())));
  AMediaFormat_setInt32(format, "i-frame-interval", 1);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT,
                        kColorFormatYuv420Flexible);
  e.codec = AMediaCodec_createEncoderByType(mime);
  if (e.codec == nullptr ||
      AMediaCodec_configure(e.codec, format, nullptr, nullptr,
                            AMEDIACODEC_CONFIGURE_FLAG_ENCODE) != AMEDIA_OK ||
      AMediaCodec_start(e.codec) != AMEDIA_OK) {
    AMediaFormat_delete(format);
    return Error::make(ErrorCode::ExecutionFailed,
                       "cannot start the video encoder", "android");
  }
  AMediaFormat_delete(format);
  e.width = config.width;
  e.height = config.height;
  e.frameDurationUs = std::int64_t(1000000.0 / fps);
  e.lastPtsUs = -1;
  e.opened = true;
  // config->globalHeader needs no MediaCodec equivalent: the parameter
  // sets always travel through the output format's csd buffers, which the
  // mux stage attaches to the track (see android_stages.h).
  return Error::make(ErrorCode::None, "");
}

namespace {

// Row copy helper: `pw` bytes per row, `ph` rows.
void copyPlaneRows(std::uint8_t *dst, size_t dstStride, const std::uint8_t *src,
                   size_t srcStride, int pw, int ph) {
  for (int y = 0; y < ph; ++y) {
    std::memcpy(dst + size_t(y) * dstStride, src + size_t(y) * srcStride,
                size_t(pw));
  }
}

} // namespace

Error AndroidStageEncoder::push(const ICpuFrame &frame) {
  Impl &e = *impl_;
  if (!e.opened) {
    return Error::make(ErrorCode::InvalidArgument, "encoder not opened",
                       "android");
  }
  if (e.flushed) {
    return Error::make(ErrorCode::InvalidArgument,
                       "encoder already flushed", "android");
  }
  if (frame.planeCount() != 3 || frame.planeData(0) == nullptr ||
      frame.planeData(1) == nullptr || frame.planeData(2) == nullptr) {
    return Error::make(ErrorCode::InvalidArgument,
                       "frame is not in the negotiated Yuv420P interchange",
                       "android");
  }
  // Bounded input dequeue in ~10ms slices (with a flush stop-check): the
  // hybrid executor interleaves push/pull, so a healthy encoder frees an
  // input buffer quickly.
  ssize_t idx = -1;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(10);
  for (;;) {
    if (e.flushed) {
      return Error::make(ErrorCode::InvalidArgument,
                         "encoder already flushed", "android");
    }
    idx = AMediaCodec_dequeueInputBuffer(e.codec, 10000);
    if (idx >= 0) {
      break;
    }
    if (std::chrono::steady_clock::now() > deadline) {
      return Error::make(ErrorCode::ExecutionFailed,
                         "the stage encoder accepted no input buffer",
                         "android");
    }
  }
  size_t capacity = 0;
  std::uint8_t *dst = AMediaCodec_getInputBuffer(e.codec, size_t(idx), &capacity);
  if (dst == nullptr) {
    AMediaCodec_queueInputBuffer(e.codec, size_t(idx), 0, 0, 0, 0);
    return Error::make(ErrorCode::ExecutionFailed,
                       "cannot access the encoder input buffer", "android");
  }
  // Per-buffer input format (API 29+): color format + stride +
  // slice-height describe the exact layout this buffer expects.
  AMediaFormat *bufferFormat = AMediaCodec_getBufferFormat(e.codec, size_t(idx));
  int32_t colorFormat = 0;
  const bool hasColor =
      bufferFormat != nullptr &&
      AMediaFormat_getInt32(bufferFormat, AMEDIAFORMAT_KEY_COLOR_FORMAT,
                            &colorFormat);
  int32_t stride = 0;
  const bool hasStride =
      bufferFormat != nullptr &&
      AMediaFormat_getInt32(bufferFormat, AMEDIAFORMAT_KEY_STRIDE, &stride);
  int32_t sliceHeight = 0;
  const bool hasSlice =
      bufferFormat != nullptr &&
      AMediaFormat_getInt32(bufferFormat, AMEDIAFORMAT_KEY_SLICE_HEIGHT,
                            &sliceHeight);
  if (bufferFormat != nullptr) {
    AMediaFormat_delete(bufferFormat);
  }
  const int w = e.width;
  const int h = e.height;
  if (!hasStride || stride < w) {
    stride = w;
  }
  if (!hasSlice || sliceHeight < h) {
    sliceHeight = h;
  }
  bool semiPlanar = true;
  if (hasColor) {
    if (colorFormat == kColorFormatYuv420Planar ||
        colorFormat == kColorFormatYuv420PackedPlanar) {
      semiPlanar = false;
    } else if (colorFormat == kColorFormatYuv420SemiPlanar ||
               colorFormat == kColorFormatYuv420Flexible) {
      // Flexible reports the geometry via stride/slice-height; the
      // concrete c2 layouts resolve to NV12.
      semiPlanar = true;
    } else {
      AMediaCodec_queueInputBuffer(e.codec, size_t(idx), 0, 0, 0, 0);
      return Error::make(
          ErrorCode::UnsupportedOperation,
          "unsupported encoder input color format: " +
              std::to_string(colorFormat),
          "android");
    }
  }
  // Modern c2 encoders that report no explicit color format resolve
  // flexible input to semi-planar NV12 with the reported geometry.
  const size_t needed =
      size_t(stride) * size_t(sliceHeight) * 3 / 2;
  if (needed > capacity) {
    AMediaCodec_queueInputBuffer(e.codec, size_t(idx), 0, 0, 0, 0);
    return Error::make(ErrorCode::ExecutionFailed,
                       "the encoder input buffer is too small for the frame "
                       "layout",
                       "android");
  }
  // Luma.
  copyPlaneRows(dst, size_t(stride), frame.planeData(0),
                size_t(frame.planeLinesize(0)), w, h);
  if (semiPlanar) {
    // NV12: interleaved UV at the luma plane end, chroma row stride =
    // luma stride, pixel stride 2.
    std::uint8_t *uv = dst + size_t(stride) * size_t(sliceHeight);
    const std::uint8_t *u = frame.planeData(1);
    const std::uint8_t *v = frame.planeData(2);
    const size_t uStride = size_t(frame.planeLinesize(1));
    const size_t vStride = size_t(frame.planeLinesize(2));
    for (int y = 0; y < h / 2; ++y) {
      std::uint8_t *row = uv + size_t(y) * size_t(stride);
      const std::uint8_t *uRow = u + size_t(y) * uStride;
      const std::uint8_t *vRow = v + size_t(y) * vStride;
      for (int x = 0; x < w / 2; ++x) {
        row[2 * x] = uRow[x];
        row[2 * x + 1] = vRow[x];
      }
    }
  } else {
    // I420: Y, then U, then V; chroma row stride = stride/2.
    std::uint8_t *uDst = dst + size_t(stride) * size_t(sliceHeight);
    const size_t chromaStride = size_t(stride) / 2;
    std::uint8_t *vDst = uDst + chromaStride * size_t(sliceHeight / 2);
    copyPlaneRows(uDst, chromaStride, frame.planeData(1),
                  size_t(frame.planeLinesize(1)), w / 2, h / 2);
    copyPlaneRows(vDst, chromaStride, frame.planeData(2),
                  size_t(frame.planeLinesize(2)), w / 2, h / 2);
  }
  // MediaCodec requires monotonically increasing presentation times; the
  // interchange timeline is monotonic by contract, this only guards a
  // producer that misbehaves.
  std::int64_t pts = frame.ptsUs();
  if (pts < 0 || (e.lastPtsUs >= 0 && pts <= e.lastPtsUs)) {
    pts = e.lastPtsUs >= 0 ? e.lastPtsUs + std::max(e.frameDurationUs,
                                                    std::int64_t(1))
                           : 0;
  }
  e.lastPtsUs = pts;
  AMediaCodec_queueInputBuffer(e.codec, size_t(idx), 0, int32_t(needed),
                               uint64_t(pts), 0);
  return Error::make(ErrorCode::None, "");
}

// Captures the encoder's codec-specific data ("csd-0"/"csd-1" buffers of
// AMediaCodec_getOutputFormat — the exact shape the whole-pipeline path
// hands to AMediaMuxer_addTrack) into the TLV config blob documented in
// android_stages.h:
//     u32 BE sectionCount, then per section u32 BE byteLength + bytes.
void captureConfigFromFormat(AndroidStageEncoder::Impl &e) {
  if (e.configCaptured || e.codec == nullptr) {
    return;
  }
  AMediaFormat *format = AMediaCodec_getOutputFormat(e.codec);
  if (format == nullptr) {
    return;
  }
  struct Section {
    const void *data;
    size_t size;
  };
  Section sections[2];
  size_t sectionCount = 0;
  for (size_t i = 0; i < 2; ++i) {
    char key[8];
    std::snprintf(key, sizeof(key), "csd-%d", int(i));
    void *data = nullptr;
    size_t size = 0;
    if (AMediaFormat_getBuffer(format, key, &data, &size) && data != nullptr &&
        size > 0) {
      sections[sectionCount].data = data;
      sections[sectionCount].size = size;
      ++sectionCount;
    }
  }
  if (sectionCount == 0) {
    // The format does not carry csd yet; retry on the next opportunity.
    AMediaFormat_delete(format);
    return;
  }
  e.config.clear();
  const auto appendU32 = [&e](uint32_t value) {
    e.config.push_back(std::uint8_t((value >> 24) & 0xFF));
    e.config.push_back(std::uint8_t((value >> 16) & 0xFF));
    e.config.push_back(std::uint8_t((value >> 8) & 0xFF));
    e.config.push_back(std::uint8_t(value & 0xFF));
  };
  appendU32(uint32_t(sectionCount));
  for (size_t i = 0; i < sectionCount; ++i) {
    appendU32(uint32_t(sections[i].size));
    const auto *bytes =
        static_cast<const std::uint8_t *>(sections[i].data);
    e.config.insert(e.config.end(), bytes, bytes + sections[i].size);
  }
  e.configCaptured = true;
  AMediaFormat_delete(format);
}

Error AndroidStageEncoder::pull(PacketView &packet) {
  packet = PacketView{};
  Impl &e = *impl_;
  if (!e.opened) {
    return Error::make(ErrorCode::InvalidArgument, "encoder not opened",
                       "android");
  }
  auto drainDeadline = std::chrono::steady_clock::now() +
                       std::chrono::seconds(30);
  for (;;) {
    // Post-flush pulls must not report "nothing ready" before the codec
    // fully drained: the hybrid executor treats an empty pull as the end
    // of the drain. Pre-flush, an empty pull just means "try again after
    // the next push" (queued packets are never lost).
    const bool draining = e.flushed && !e.eosOutputSeen;
    const int64_t timeoutUs = draining ? 10000 : 0;
    AMediaCodecBufferInfo info{};
    const ssize_t idx = AMediaCodec_dequeueOutputBuffer(e.codec, &info,
                                                        timeoutUs);
    if (idx >= 0) {
      const bool eos =
          (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
      const bool codecConfig =
          (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) != 0;
      if (codecConfig) {
        // Codec configuration rides the track format, never the sample
        // stream; capture it from the encoder's own output format.
        AMediaCodec_releaseOutputBuffer(e.codec, size_t(idx), false);
        captureConfigFromFormat(e);
        continue;
      }
      if (info.size > 0) {
        size_t capacity = 0;
        std::uint8_t *data =
            AMediaCodec_getOutputBuffer(e.codec, size_t(idx), &capacity);
        if (data != nullptr) {
          if (!e.configCaptured) {
            captureConfigFromFormat(e);
          }
          e.packet.assign(data + info.offset,
                          data + info.offset + info.size);
          AMediaCodec_releaseOutputBuffer(e.codec, size_t(idx), false);
          packet.data = e.packet.data();
          packet.size = e.packet.size();
          packet.keyframe =
              (info.flags & AMEDIACODEC_BUFFER_FLAG_KEY_FRAME) != 0;
          packet.ptsUs = info.presentationTimeUs;
          packet.durationUs = e.frameDurationUs;
          if (!e.config.empty()) {
            packet.configData = e.config.data();
            packet.configSize = e.config.size();
          }
          if (eos) {
            e.eosOutputSeen = true;
          }
          return Error::make(ErrorCode::None, "");
        }
      }
      AMediaCodec_releaseOutputBuffer(e.codec, size_t(idx), false);
      if (eos) {
        e.eosOutputSeen = true;
      }
      continue;
    }
    if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
      captureConfigFromFormat(e);
      continue;
    }
    // Try-again / benign events.
    if (draining) {
      if (std::chrono::steady_clock::now() > drainDeadline) {
        return Error::make(ErrorCode::ExecutionFailed,
                           "the stage encoder did not drain", "android");
      }
      continue;
    }
    return Error::make(ErrorCode::None, "");
  }
}

Error AndroidStageEncoder::flush() {
  Impl &e = *impl_;
  if (!e.opened || e.flushed) {
    return Error::make(ErrorCode::None, "");
  }
  // Bounded wait for an input buffer, then queue the end-of-stream signal.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  for (;;) {
    const ssize_t idx = AMediaCodec_dequeueInputBuffer(e.codec, 10000);
    if (idx >= 0) {
      const std::int64_t eosPts =
          e.lastPtsUs >= 0
              ? e.lastPtsUs + std::max(e.frameDurationUs, std::int64_t(1))
              : 0;
      AMediaCodec_queueInputBuffer(e.codec, size_t(idx), 0, 0,
                                   uint64_t(eosPts),
                                   AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
      e.flushed = true;
      return Error::make(ErrorCode::None, "");
    }
    if (std::chrono::steady_clock::now() > deadline) {
      return Error::make(ErrorCode::ExecutionFailed,
                         "cannot signal the encoder end of stream", "android");
    }
  }
}

// ---------------------------------------------------------------------------
// Mux stage: AMediaMuxer + source audio stream copy
// ---------------------------------------------------------------------------

namespace {

// Serialized video packet parked before the muxer started (no track format
// without the codec configuration yet).
struct QueuedVideoPacket {
  std::vector<std::uint8_t> data;
  std::int64_t ptsUs = 0;
  bool keyframe = false;
};

bool readConfigU32(const std::uint8_t *data, size_t size, size_t &offset,
                   uint32_t &out) {
  if (offset + 4 > size) {
    return false;
  }
  out = (uint32_t(data[offset]) << 24) | (uint32_t(data[offset + 1]) << 16) |
        (uint32_t(data[offset + 2]) << 8) | uint32_t(data[offset + 3]);
  offset += 4;
  return true;
}

} // namespace

struct AndroidMuxStage::Impl {
  AMediaMuxer *muxer = nullptr;
  int muxerFd = -1;
  OutputFormat outputFormat = AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4;
  bool globalHeader = false;
  TrackOptions tracks;
  std::string outputPath;
  // Audio copy (second extractor on the same source).
  AMediaExtractor *audioExtractor = nullptr;
  int audioExtractorFd = -1; // source fd; closed after the extractor dies
  int audioTrack = -1;
  ssize_t audioMuxTrack = -1;
  StageSampleBuffer audioSample;
  bool havePendingAudio = false;
  size_t pendingAudioSize = 0;
  std::int64_t pendingAudioPts = 0;
  uint32_t pendingAudioFlags = 0;
  // Video track / muxer start state.
  ssize_t videoMuxTrack = -1;
  bool started = false;
  bool finalized = false;
  bool wroteVideo = false;
  std::vector<QueuedVideoPacket> videoBacklog;
};

AndroidMuxStage::AndroidMuxStage() : impl_(std::make_unique<Impl>()) {}

AndroidMuxStage::~AndroidMuxStage() {
  if (impl_ == nullptr) {
    return;
  }
  Impl &m = *impl_;
  if (m.muxer != nullptr) {
    AMediaMuxer_delete(m.muxer);
  }
  // The muxer keeps the fd for its lifetime; the caller closes it.
  if (m.muxerFd >= 0) {
    ::close(m.muxerFd);
    m.muxerFd = -1;
  }
  if (m.audioExtractor != nullptr) {
    AMediaExtractor_delete(m.audioExtractor);
  }
  if (m.audioExtractorFd >= 0) {
    ::close(m.audioExtractorFd);
    m.audioExtractorFd = -1;
  }
}

Error AndroidMuxStage::open(const std::string &outputPath,
                            const TrackOptions &tracks) {
  Impl &m = *impl_;
  m.tracks = tracks;
  m.outputPath = outputPath;
  switch (tracks.container) {
  case Container::Mp4:
  case Container::M4v:
  case Container::M4a:
    m.outputFormat = AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4;
    m.globalHeader = true; // parameter sets ride the track format (avcC)
    break;
  case Container::ThreeGp:
    m.outputFormat = AMEDIAMUXER_OUTPUT_FORMAT_THREE_GPP;
    m.globalHeader = true;
    break;
  case Container::WebM:
    m.outputFormat = AMEDIAMUXER_OUTPUT_FORMAT_WEBM;
    m.globalHeader = false;
    break;
  default:
    // No NDK muxer for this container (e.g. MPEG-T): refuse honestly
    // rather than silently writing a different container.
    return Error::make(
        ErrorCode::UnsupportedOperation,
        std::string("the Android stage muxer cannot write container: ") +
            containerName(tracks.container),
        "android");
  }
  m.muxerFd = openStageMuxerFd(outputPath);
  if (m.muxerFd < 0) {
    return Error::make(ErrorCode::SourceUnavailable,
                       "cannot create the output file", "android");
  }
  m.muxer = AMediaMuxer_new(m.muxerFd, m.outputFormat);
  if (m.muxer == nullptr) {
    return Error::make(ErrorCode::ExecutionFailed,
                       "cannot create the output muxer", "android");
  }
  if (tracks.copyAudio) {
    m.audioExtractor = AMediaExtractor_new();
    // fd-based source open (path-based fails on API 29 — media_util.h).
    if (m.audioExtractor == nullptr ||
        !setExtractorDataSourceFd(m.audioExtractor, tracks.sourcePath,
                                  m.audioExtractorFd)) {
      return Error::make(ErrorCode::SourceUnavailable,
                         "cannot open the audio copy source", "android");
    }
    const size_t trackCount =
        AMediaExtractor_getTrackCount(m.audioExtractor);
    for (size_t i = 0; i < trackCount; ++i) {
      AMediaFormat *format =
          AMediaExtractor_getTrackFormat(m.audioExtractor, i);
      if (format == nullptr) {
        continue;
      }
      const char *mime = nullptr;
      if (!AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &mime) ||
          mime == nullptr || std::strncmp(mime, "audio/", 6) != 0) {
        AMediaFormat_delete(format);
        continue;
      }
      m.audioTrack = int(i);
      AMediaExtractor_selectTrack(m.audioExtractor, i);
      // addTrack copies what it needs; the format object is ours to free.
      m.audioMuxTrack = AMediaMuxer_addTrack(m.muxer, format);
      AMediaFormat_delete(format);
      break;
    }
    if (m.audioTrack < 0) {
      return Error::make(ErrorCode::InvalidArgument,
                         "audio copy requested but the source has no audio "
                         "track",
                         "android");
    }
    if (m.audioMuxTrack < 0) {
      return Error::make(ErrorCode::ExecutionFailed,
                         "cannot add the audio track to the muxer", "android");
    }
  }
  return Error::make(ErrorCode::None, "");
}

bool AndroidMuxStage::wantsGlobalHeader() const {
  return impl_ != nullptr && impl_->globalHeader;
}

Error AndroidMuxStage::writeVideoPacket(const PacketView &packet) {
  Impl &m = *impl_;
  if (m.finalized) {
    return Error::make(ErrorCode::InvalidArgument, "mux stage finalized",
                       "android");
  }
  if (packet.size == 0) {
    return Error::make(ErrorCode::None, "");
  }
  if (!m.started) {
    if (m.videoMuxTrack < 0) {
      if (packet.configData == nullptr || packet.configSize == 0) {
        // No codec configuration yet: buffer early packets until it rides
        // one (bounded; a configuration-less encoder is a hard failure).
        if (m.videoBacklog.size() >= 64) {
          return Error::make(ErrorCode::ExecutionFailed,
                             "the encoder produced no codec configuration "
                             "before 64 packets",
                             "android");
        }
        QueuedVideoPacket queued;
        queued.data.assign(packet.data, packet.data + packet.size);
        queued.ptsUs = packet.ptsUs;
        queued.keyframe = packet.keyframe;
        m.videoBacklog.push_back(std::move(queued));
        return Error::make(ErrorCode::None, "");
      }
      const char *mime = stageVideoMimeFor(m.tracks.videoCodec);
      if (mime == nullptr) {
        return Error::make(ErrorCode::UnsupportedOperation,
                           "no Android mime for the output video codec",
                           "android");
      }
      // Rebuild the track format: negotiated geometry + the csd sections
      // serialized by the encoder (see android_stages.h for the TLV).
      AMediaFormat *format = AMediaFormat_new();
      AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, mime);
      AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, m.tracks.width);
      AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, m.tracks.height);
      if (m.tracks.frameRate > 0.0) {
        AMediaFormat_setInt32(
            format, AMEDIAFORMAT_KEY_FRAME_RATE,
            int32_t(std::max(1, int(std::lround(m.tracks.frameRate)))));
      }
      size_t offset = 0;
      uint32_t sections = 0;
      int attached = 0;
      if (!readConfigU32(packet.configData, packet.configSize, offset,
                         sections) ||
          sections == 0 || sections > 8) {
        AMediaFormat_delete(format);
        return Error::make(ErrorCode::ExecutionFailed,
                           "the packet's codec configuration could not be "
                           "parsed",
                           "android");
      }
      for (uint32_t s = 0; s < sections && attached < 2; ++s) {
        uint32_t length = 0;
        if (!readConfigU32(packet.configData, packet.configSize, offset,
                           length) ||
            offset + length > packet.configSize) {
          AMediaFormat_delete(format);
          return Error::make(ErrorCode::ExecutionFailed,
                             "the packet's codec configuration could not be "
                             "parsed",
                             "android");
        }
        char key[8];
        std::snprintf(key, sizeof(key), "csd-%d", attached);
        AMediaFormat_setBuffer(format, key, packet.configData + offset,
                               size_t(length));
        offset += length;
        ++attached;
      }
      if (attached == 0) {
        AMediaFormat_delete(format);
        return Error::make(ErrorCode::ExecutionFailed,
                           "the packet carries no codec configuration",
                           "android");
      }
      m.videoMuxTrack = AMediaMuxer_addTrack(m.muxer, format);
      AMediaFormat_delete(format);
      if (m.videoMuxTrack < 0) {
        return Error::make(ErrorCode::ExecutionFailed,
                           "cannot add the video track to the muxer",
                           "android");
      }
    }
    // All tracks exist (audio was added at open): start the muxer once.
    if (AMediaMuxer_start(m.muxer) != AMEDIA_OK) {
      return Error::make(ErrorCode::ExecutionFailed,
                         "cannot start the muxer", "android");
    }
    m.started = true;
    for (QueuedVideoPacket &queued : m.videoBacklog) {
      AMediaCodecBufferInfo info{};
      info.offset = 0;
      info.size = int32_t(queued.data.size());
      info.presentationTimeUs = queued.ptsUs;
      info.flags =
          queued.keyframe ? AMEDIACODEC_BUFFER_FLAG_KEY_FRAME : 0;
      if (AMediaMuxer_writeSampleData(m.muxer, size_t(m.videoMuxTrack),
                                      queued.data.data(), &info) !=
          AMEDIA_OK) {
        return Error::make(ErrorCode::ExecutionFailed,
                           "cannot write the video packet", "android");
      }
      m.wroteVideo = true;
    }
    m.videoBacklog.clear();
    m.videoBacklog.shrink_to_fit();
  }
  AMediaCodecBufferInfo info{};
  info.offset = 0;
  info.size = int32_t(packet.size);
  info.presentationTimeUs = packet.ptsUs;
  info.flags = packet.keyframe ? AMEDIACODEC_BUFFER_FLAG_KEY_FRAME : 0;
  // Older NDK headers declare the data pointer non-const; the muxer never
  // modifies the sample bytes.
  std::uint8_t *data = const_cast<std::uint8_t *>(packet.data);
  if (AMediaMuxer_writeSampleData(m.muxer, size_t(m.videoMuxTrack), data,
                                  &info) != AMEDIA_OK) {
    return Error::make(ErrorCode::ExecutionFailed,
                       "cannot write the video packet", "android");
  }
  m.wroteVideo = true;
  return drainAudioUpTo(packet.ptsUs);
}

Error AndroidMuxStage::drainAudioUpTo(std::int64_t boundaryPtsUs) {
  Impl &m = *impl_;
  if (m.audioMuxTrack < 0 || m.audioTrack < 0 || !m.started) {
    return Error::make(ErrorCode::None, "");
  }
  for (;;) {
    if (!m.havePendingAudio) {
      const int trackIdx =
          AMediaExtractor_getSampleTrackIndex(m.audioExtractor);
      if (trackIdx < 0) {
        break; // audio exhausted
      }
      if (trackIdx != m.audioTrack) {
        AMediaExtractor_advance(m.audioExtractor); // unselected: skip
        continue;
      }
      const ssize_t sampleSize =
          AMediaExtractor_getSampleSize(m.audioExtractor);
      if (sampleSize <= 0) {
        AMediaExtractor_advance(m.audioExtractor);
        continue;
      }
      m.audioSample.ensure(size_t(sampleSize));
      const ssize_t copied = AMediaExtractor_readSampleData(
          m.audioExtractor, m.audioSample.data(), m.audioSample.capacity());
      if (copied != sampleSize) {
        continue; // capacity grew; re-read the same sample
      }
      m.pendingAudioSize = size_t(sampleSize);
      m.pendingAudioPts = AMediaExtractor_getSampleTime(m.audioExtractor);
      m.pendingAudioFlags = AMediaExtractor_getSampleFlags(m.audioExtractor);
      m.havePendingAudio = true;
      AMediaExtractor_advance(m.audioExtractor);
    }
    // Lookahead: hold samples that belong to a later interleave window
    // (mirrors FfmpegMuxStage::drainAudioUpTo).
    if (boundaryPtsUs >= 0 && m.pendingAudioPts > boundaryPtsUs) {
      return Error::make(ErrorCode::None, "");
    }
    AMediaCodecBufferInfo info{};
    info.offset = 0;
    info.size = int32_t(m.pendingAudioSize);
    info.presentationTimeUs = m.pendingAudioPts;
    info.flags = (m.pendingAudioFlags & AMEDIAEXTRACTOR_SAMPLE_FLAG_SYNC)
                     ? AMEDIACODEC_BUFFER_FLAG_KEY_FRAME
                     : 0;
    if (AMediaMuxer_writeSampleData(m.muxer, size_t(m.audioMuxTrack),
                                    m.audioSample.data(), &info) !=
        AMEDIA_OK) {
      return Error::make(ErrorCode::ExecutionFailed,
                         "cannot write the audio packet", "android");
    }
    m.havePendingAudio = false;
    m.pendingAudioSize = 0;
  }
  return Error::make(ErrorCode::None, "");
}

Error AndroidMuxStage::finalize() {
  Impl &m = *impl_;
  if (m.finalized) {
    return Error::make(ErrorCode::None, "");
  }
  if (!m.started || !m.wroteVideo) {
    return Error::make(ErrorCode::ExecutionFailed,
                       "no video packet was written; refusing to finalize "
                       "an empty hybrid output",
                       "android");
  }
  Error err = drainAudioUpTo(-1);
  if (!err.ok()) {
    return err;
  }
  if (AMediaMuxer_stop(m.muxer) != AMEDIA_OK) {
    return Error::make(ErrorCode::ExecutionFailed,
                       "cannot stop the muxer", "android");
  }
  m.finalized = true;
  return Error::make(ErrorCode::None, "");
}

// ---------------------------------------------------------------------------
// Stage backend factories
// ---------------------------------------------------------------------------

velmux::backend::IVideoStageDecoder *AndroidStageBackend::createVideoDecoder() {
  return new AndroidStageDecoder();
}

velmux::backend::IVideoStageEncoder *AndroidStageBackend::createVideoEncoder() {
  return new AndroidStageEncoder();
}

velmux::backend::IMuxStage *AndroidStageBackend::createMux() {
  return new AndroidMuxStage();
}

} // namespace velmux::android::media

#else // !__ANDROID__

// Host builds: no NDK media APIs. The backend never reports stage
// participation there (capabilities gate on the platform stack actually
// initializing), so these factories are never consulted; they stay honest
// nulls — never fake workers.
#include <memory>

namespace velmux::android::media {

velmux::backend::IVideoStageDecoder *
AndroidStageBackend::createVideoDecoder() {
  return nullptr;
}

velmux::backend::IVideoStageEncoder *
AndroidStageBackend::createVideoEncoder() {
  return nullptr;
}

velmux::backend::IMuxStage *AndroidStageBackend::createMux() {
  return nullptr;
}

} // namespace velmux::android::media

#endif // __ANDROID__
