// Apple stage workers — implementation. See apple_stages.h for the
// documented encoder->mux packet representation.
#import "apple_stages.h"

#ifdef __APPLE__

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>

#import <algorithm>
#import <cmath>
#import <condition_variable>
#import <cstring>
#import <deque>
#import <mutex>
#import <vector>

namespace velmux::apple::media {

using namespace velmux::core;
using velmux::backend::CpuFramePtr;
using velmux::backend::ICpuFrame;
using velmux::backend::InterchangeFormat;
using velmux::backend::InterchangePixel;
using velmux::backend::PacketView;
using velmux::backend::StageEncoderConfig;

namespace {

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

std::int64_t cmTimeToUs(CMTime time) {
  if (!CMTIME_IS_NUMERIC(time)) {
    return 0;
  }
  const double seconds = CMTimeGetSeconds(time);
  if (!std::isfinite(seconds) || seconds < 0.0) {
    return 0;
  }
  return std::llround(seconds * 1e6);
}

// CMVideoCodecType from the product vocabulary. VideoToolbox publicly
// encodes H.264 and HEVC (the same set the whole-pipeline AVAssetWriter
// path maps in apple_pipeline.mm's videoCodecCode); anything else is an
// honest refusal.
FourCharCode codecTypeFor(VideoCodec codec, bool &ok) {
  ok = true;
  switch (codec) {
  case VideoCodec::H264:
    return kCMVideoCodecType_H264;
  case VideoCodec::HEVC:
    return kCMVideoCodecType_HEVC;
  default:
    ok = false;
    return 0;
  }
}

NSURL *fileUrlFor(const std::string &path) {
  return [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
}

// Loads the track list of a local asset synchronously (bounded by a
// semaphore; local files resolve quickly — same pattern as
// apple_pipeline.mm's inspectFile).
BOOL loadAssetTracks(AVURLAsset *asset, std::string &error) {
  __block BOOL loaded = NO;
  dispatch_semaphore_t sema = dispatch_semaphore_create(0);
  [asset loadValuesAsynchronouslyForKeys:@[ @"tracks" ]
                        completionHandler:^{
                          loaded = YES;
                          dispatch_semaphore_signal(sema);
                        }];
  dispatch_semaphore_wait(
      sema, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));
  if (!loaded || [asset statusOfValueForKey:@"tracks" error:nil] ==
                     AVKeyValueStatusFailed) {
    error = "cannot read the source metadata";
    return NO;
  }
  return YES;
}

// CPU frame in heap planar Yuv420P. Plane strides are compact (width /
// width/2): consumers read row by row through planeLinesize(), so no
// padding is required and none is invented.
class PlanarCpuFrame final : public ICpuFrame {
public:
  PlanarCpuFrame(int width, int height, std::int64_t pts, std::int64_t dur)
      : y_(std::size_t(width) * std::size_t(height), 16),
        u_(std::size_t(width / 2) * std::size_t(height / 2), 128),
        v_(std::size_t(width / 2) * std::size_t(height / 2), 128),
        ptsUs_(pts), durationUs_(dur), width_(width), height_(height) {}

  std::uint8_t *y() { return y_.data(); }
  std::uint8_t *u() { return u_.data(); }
  std::uint8_t *v() { return v_.data(); }

  int planeCount() const override { return 3; }
  const std::uint8_t *planeData(int plane) const override {
    switch (plane) {
    case 0:
      return y_.data();
    case 1:
      return u_.data();
    case 2:
      return v_.data();
    default:
      return nullptr;
    }
  }
  int planeLinesize(int plane) const override {
    switch (plane) {
    case 0:
      return width_;
    case 1:
    case 2:
      return width_ / 2;
    default:
      return 0;
    }
  }
  std::int64_t ptsUs() const override { return ptsUs_; }
  std::int64_t durationUs() const override { return durationUs_; }

private:
  std::vector<std::uint8_t> y_;
  std::vector<std::uint8_t> u_;
  std::vector<std::uint8_t> v_;
  std::int64_t ptsUs_ = 0;
  std::int64_t durationUs_ = 0;
  int width_ = 0;
  int height_ = 0;
};

// Splits an Annex-B bytestream into NAL units (without start codes).
// Accepts the 4-byte 00 00 00 01 and 3-byte 00 00 01 start codes.
std::vector<std::vector<std::uint8_t>> splitAnnexB(const std::uint8_t *data,
                                                   std::size_t size) {
  std::vector<std::vector<std::uint8_t>> nals;
  std::size_t i = 0;
  std::size_t start = 0;
  bool haveStart = false;
  while (i + 2 < size) {
    if (data[i] == 0x00 && data[i + 1] == 0x00) {
      if (data[i + 2] == 0x01) {
        if (haveStart) {
          nals.emplace_back(data + start, data + i);
        }
        i += 3;
        start = i;
        haveStart = true;
        continue;
      }
      if (i + 3 < size && data[i + 2] == 0x00 && data[i + 3] == 0x01) {
        if (haveStart) {
          nals.emplace_back(data + start, data + i);
        }
        i += 4;
        start = i;
        haveStart = true;
        continue;
      }
    }
    ++i;
  }
  if (haveStart && start < size) {
    nals.emplace_back(data + start, data + size);
  }
  return nals;
}

} // namespace

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

struct AppleStageDecoder::Impl {
  AVAssetReader *reader = nil;
  AVAssetReaderTrackOutput *output = nil;
  InterchangeFormat format;
  std::int64_t duration = 0; // microseconds
  bool opened = false;
  bool eos = false;
};

AppleStageDecoder::AppleStageDecoder() : impl_(std::make_unique<Impl>()) {}
AppleStageDecoder::~AppleStageDecoder() {
  if (impl_ != nullptr && impl_->reader != nil) {
    @try {
      [impl_->reader cancelReading];
    } @catch (NSException *) {
      // Teardown must never throw.
    }
  }
}

Error AppleStageDecoder::open(const std::string &sourcePath,
                              InterchangePixel pixel) {
  if (pixel != InterchangePixel::Yuv420P) {
    return Error::make(ErrorCode::UnsupportedFeature,
                       "unsupported interchange pixel format", "apple");
  }
  AVURLAsset *asset = [AVURLAsset assetWithURL:fileUrlFor(sourcePath)];
  if (asset == nil) {
    return Error::make(ErrorCode::SourceUnavailable, "cannot open the source",
                       "apple");
  }
  std::string loadError;
  if (!loadAssetTracks(asset, loadError)) {
    return Error::make(ErrorCode::SourceUnavailable, loadError, "apple");
  }
  AVAssetTrack *track = nil;
  for (AVAssetTrack *candidate in asset.tracks) {
    if (candidate.mediaType == AVMediaTypeVideo) {
      track = candidate;
      break;
    }
  }
  if (track == nil) {
    return Error::make(ErrorCode::InvalidArgument,
                       "the source has no video track", "apple");
  }
  NSError *nsError = nil;
  impl_->reader = [AVAssetReader assetReaderWithAsset:asset error:&nsError];
  if (impl_->reader == nil) {
    return Error::make(ErrorCode::SourceUnavailable,
                       nsError != nil
                           ? nsError.localizedDescription.UTF8String
                           : "cannot create the reader",
                       "apple");
  }
  // Decode to biplanar video-range 4:2:0: the closest native surface to
  // the Yuv420P interchange (one copy + chroma deinterleave per frame).
  NSDictionary *settings = @{
    (NSString *)kCVPixelBufferPixelFormatTypeKey :
        @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
  };
  impl_->output = [[AVAssetReaderTrackOutput alloc]
      initWithTrack:track
      outputSettings:settings];
  if (impl_->output == nil || ![impl_->reader canAddOutput:impl_->output]) {
    return Error::make(ErrorCode::UnsupportedFeature,
                       "cannot decode the video track", "apple");
  }
  [impl_->reader addOutput:impl_->output];
  if (![impl_->reader startReading]) {
    return Error::make(
        ErrorCode::ExecutionFailed,
        impl_->reader.error != nil
            ? impl_->reader.error.localizedDescription.UTF8String
            : "cannot start reading",
        "apple");
  }
  // Coded geometry (no rotation metadata on the interchange path — see
  // the header's v1 limitations). The decoded CVPixelBuffers share it.
  impl_->format.pixel = pixel;
  impl_->format.width = int(track.naturalSize.width);
  impl_->format.height = int(track.naturalSize.height);
  if (impl_->format.width <= 0 || impl_->format.height <= 0) {
    return Error::make(ErrorCode::UnsupportedFeature,
                       "the source has no usable video geometry", "apple");
  }
  if (CMTIME_IS_NUMERIC(asset.duration) && asset.duration.value > 0) {
    impl_->duration = cmTimeToUs(asset.duration);
  }
  impl_->opened = true;
  impl_->eos = false;
  return Error::make(ErrorCode::None, "");
}

InterchangeFormat AppleStageDecoder::outputFormat() const {
  return impl_->format;
}

std::int64_t AppleStageDecoder::durationUs() const { return impl_->duration; }

Error AppleStageDecoder::next(CpuFramePtr &frame) {
  frame = nullptr;
  if (!impl_->opened) {
    return Error::make(ErrorCode::InvalidArgument, "decoder not opened",
                       "apple");
  }
  if (impl_->eos) {
    return Error::make(ErrorCode::None, "");
  }
  CMSampleBufferRef sample = [impl_->output copyNextSampleBuffer];
  if (sample == NULL) {
    // End of stream: null frame, no error (idempotent on later calls).
    impl_->eos = true;
    return Error::make(ErrorCode::None, "");
  }
  CVPixelBufferRef pixels = CMSampleBufferGetImageBuffer(sample);
  if (pixels == NULL || CVPixelBufferGetPlaneCount(pixels) < 2) {
    CFRelease(sample);
    return Error::make(ErrorCode::ExecutionFailed,
                       "decoder produced no planar pixel buffer", "apple");
  }
  const std::int64_t ptsUs =
      cmTimeToUs(CMSampleBufferGetPresentationTimeStamp(sample));
  const std::int64_t durUs = cmTimeToUs(CMSampleBufferGetDuration(sample));
  const int width = int(CVPixelBufferGetWidth(pixels));
  const int height = int(CVPixelBufferGetHeight(pixels));
  if (width <= 0 || height <= 0 || (width % 2) != 0 || (height % 2) != 0) {
    CFRelease(sample);
    return Error::make(ErrorCode::UnsupportedFeature,
                       "decoded geometry is not usable for Yuv420P interchange",
                       "apple");
  }

  auto out = std::make_unique<PlanarCpuFrame>(width, height, ptsUs, durUs);
  CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
  const std::uint8_t *yBase =
      static_cast<const std::uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(
          pixels, 0));
  const std::uint8_t *uvBase =
      static_cast<const std::uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(
          pixels, 1));
  const std::size_t yStride = CVPixelBufferGetBytesPerRowOfPlane(pixels, 0);
  const std::size_t uvStride = CVPixelBufferGetBytesPerRowOfPlane(pixels, 1);
  if (yBase != nullptr && uvBase != nullptr) {
    // Y plane: row copy honoring the source stride.
    for (int row = 0; row < height; ++row) {
      std::memcpy(out->y() + std::size_t(row) * std::size_t(width),
                  yBase + std::size_t(row) * yStride, std::size_t(width));
    }
    // Chroma plane is CbCr interleaved: deinterleave into U (Cb) and V
    // (Cr), honoring the source stride.
    const int chromaWidth = width / 2;
    const int chromaHeight = height / 2;
    for (int row = 0; row < chromaHeight; ++row) {
      const std::uint8_t *src =
          uvBase + std::size_t(row) * uvStride;
      std::uint8_t *dstU =
          out->u() + std::size_t(row) * std::size_t(chromaWidth);
      std::uint8_t *dstV =
          out->v() + std::size_t(row) * std::size_t(chromaWidth);
      for (int col = 0; col < chromaWidth; ++col) {
        dstU[col] = src[std::size_t(2) * col];
        dstV[col] = src[std::size_t(2) * col + 1];
      }
    }
  }
  CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
  CFRelease(sample);
  if (yBase == nullptr || uvBase == nullptr) {
    return Error::make(ErrorCode::ExecutionFailed,
                       "cannot access the decoded pixel planes", "apple");
  }
  frame = std::move(out);
  return Error::make(ErrorCode::None, "");
}

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------

struct AppleStageEncoder::Impl {
  VTCompressionSessionRef session = nullptr;
  FourCharCode codecType = 0;
  bool isHevc = false;
  int width = 0;
  int height = 0;
  double frameRate = 30.0;
  bool opened = false;
  bool flushed = false;

  // Output queue fed by the compression callback (mutex + condition).
  std::mutex mutex;
  std::condition_variable signal;
  std::deque<CMSampleBufferRef> queue;
  int outstanding = 0;   // frames submitted, callback not yet delivered
  int callbacksLive = 0; // callbacks currently executing (dtor safety)
  bool terminated = false;

  // Owned packet payloads (valid until the next pull / for the lifetime
  // of the config buffer respectively).
  std::vector<std::uint8_t> packetBytes;
  std::vector<std::uint8_t> configBytes;
  bool configBuilt = false;

  static void outputCallback(void *refCon, void * /*sourceFrameRefCon*/,
                             OSStatus status,
                             VTEncodeInfoFlags /*infoFlags*/,
                             CMSampleBufferRef sampleBuffer) {
    auto *self = static_cast<Impl *>(refCon);
    {
      std::lock_guard<std::mutex> lock(self->mutex);
      ++self->callbacksLive;
    }
    if (status == noErr && sampleBuffer != NULL) {
      CFRetain(sampleBuffer); // owned by our queue now
      std::lock_guard<std::mutex> lock(self->mutex);
      self->queue.push_back(sampleBuffer);
    }
    {
      std::lock_guard<std::mutex> lock(self->mutex);
      --self->callbacksLive;
      --self->outstanding;
      self->signal.notify_all();
    }
  }

  void buildConfigBytes(CMFormatDescriptionRef format) {
    if (configBuilt || format == NULL) {
      return;
    }
    const std::uint8_t startCode[4] = {0x00, 0x00, 0x00, 0x01};
    const auto appendParameterSet =
        [&](const std::uint8_t *ps, std::size_t size) {
          if (ps == nullptr || size == 0) {
            return;
          }
          configBytes.insert(configBytes.end(), startCode, startCode + 4);
          configBytes.insert(configBytes.end(), ps, ps + size);
        };
    if (!isHevc) {
      std::size_t count = 0;
      for (std::size_t index = 0;; ++index) {
        const std::uint8_t *ps = nullptr;
        std::size_t size = 0;
        std::size_t total = 0;
        // parameterSetCountOut is the 5th argument (size_t*);
        // NALUnitHeaderLengthOut (6th) is int* and is not needed here —
        // only the parameter sets themselves are extracted.
        const OSStatus status =
            CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                format, index, &ps, &size, &total, nullptr);
        if (status != noErr) {
          break;
        }
        if (index == 0 && total > 0) {
          count = total;
        }
        appendParameterSet(ps, size);
        if (count > 0 && index + 1 >= count) {
          break;
        }
      }
    } else if (@available(macOS 10.13, iOS 11.0, *)) {
      // HEVC parameter sets (VPS/SPS/PPS) — API-gated variant.
      std::size_t count = 0;
      for (std::size_t index = 0;; ++index) {
        const std::uint8_t *ps = nullptr;
        std::size_t size = 0;
        std::size_t total = 0;
        const OSStatus status =
            CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                format, index, &ps, &size, &total, nullptr);
        if (status != noErr) {
          break;
        }
        if (index == 0 && total > 0) {
          count = total;
        }
        appendParameterSet(ps, size);
        if (count > 0 && index + 1 >= count) {
          break;
        }
      }
    }
    configBuilt = !configBytes.empty();
  }
};

AppleStageEncoder::AppleStageEncoder() : impl_(std::make_unique<Impl>()) {}
AppleStageEncoder::~AppleStageEncoder() {
  if (impl_ == nullptr) {
    return;
  }
  if (impl_->session != nullptr) {
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      impl_->terminated = true;
    }
    // Invalidate first: no new callbacks after this returns. Callbacks
    // already executing finish safely (the impl stays alive until they
    // are done — bounded wait below).
    VTCompressionSessionInvalidate(impl_->session);
    {
      std::unique_lock<std::mutex> lock(impl_->mutex);
      impl_->signal.wait_for(lock, std::chrono::seconds(5), [this] {
        return impl_->callbacksLive == 0;
      });
    }
    CFRelease(impl_->session);
    impl_->session = nullptr;
  }
  for (CMSampleBufferRef sample : impl_->queue) {
    CFRelease(sample);
  }
  impl_->queue.clear();
}

Error AppleStageEncoder::open(const StageEncoderConfig &config,
                              InterchangePixel pixel) {
  if (pixel != InterchangePixel::Yuv420P) {
    return Error::make(ErrorCode::UnsupportedFeature,
                       "unsupported interchange pixel format", "apple");
  }
  bool codecOk = false;
  const FourCharCode codecType = codecTypeFor(config.codec, codecOk);
  if (!codecOk) {
    return Error::make(ErrorCode::UnsupportedFeature,
                       "VideoToolbox cannot encode this output codec",
                       "apple");
  }
  // VideoToolbox requires even dimensions; a mismatched request is an
  // honest refusal, never a silent crop.
  if (config.width <= 0 || config.height <= 0 || (config.width % 2) != 0 ||
      (config.height % 2) != 0) {
    return Error::make(ErrorCode::UnsupportedFeature,
                       "VideoToolbox needs positive even dimensions "
                       "(got " +
                           std::to_string(config.width) + "x" +
                           std::to_string(config.height) + ")",
                       "apple");
  }
  impl_->codecType = codecType;
  impl_->isHevc = config.codec == VideoCodec::HEVC;
  impl_->width = config.width;
  impl_->height = config.height;
  impl_->frameRate = config.frameRate > 0.0 ? config.frameRate : 30.0;

  std::int64_t bitRate = config.bitRate;
  if (bitRate <= 0) {
    // Documented quality heuristic: bits per pixel grows with the
    // requested quality (0..100); the floor keeps dark static scenes
    // from collapsing, the ceiling keeps runaway requests bounded.
    const double quality =
        std::clamp(double(config.quality.value_or(60)), 0.0, 100.0);
    const double pixels = double(config.width) * double(config.height);
    bitRate = std::llround((0.01 + 0.12 * quality / 100.0) * pixels *
                           impl_->frameRate);
    bitRate = std::clamp<std::int64_t>(bitRate, 250000, 80000000);
  }

  OSStatus status = VTCompressionSessionCreate(
      kCFAllocatorDefault, config.width, config.height, codecType, nullptr,
      nullptr, nullptr, &Impl::outputCallback, impl_.get(), &impl_->session);
  if (status != noErr) {
    impl_->session = nullptr;
    return Error::make(ErrorCode::UnsupportedFeature,
                       "cannot create the VideoToolbox session (status " +
                           std::to_string(status) + ")",
                       "apple");
  }
  const auto setInt = [&](CFStringRef key, std::int64_t value) {
    CFNumberRef number =
        CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &value);
    const OSStatus err =
        VTSessionSetProperty(impl_->session, key, number);
    CFRelease(number);
    return err;
  };
  if (setInt(kVTCompressionPropertyKey_AverageBitRate, bitRate) != noErr) {
    return Error::make(ErrorCode::ExecutionFailed,
                       "cannot set the average bitrate", "apple");
  }
  const std::int64_t keyframeInterval =
      std::max<std::int64_t>(1, std::llround(impl_->frameRate));
  setInt(kVTCompressionPropertyKey_MaxKeyFrameInterval, keyframeInterval);
  setInt(kVTCompressionPropertyKey_ExpectedFrameRate,
         std::max<std::int64_t>(1, std::llround(impl_->frameRate)));
  // globalHeader: nothing to negotiate — VideoToolbox always delivers
  // parameter sets out of band through the format description (see the
  // header's packet contract); the muxer attaches them from configData.
  (void)config.globalHeader;

  impl_->opened = true;
  impl_->flushed = false;
  return Error::make(ErrorCode::None, "");
}

Error AppleStageEncoder::push(const ICpuFrame &frame) {
  if (!impl_->opened) {
    return Error::make(ErrorCode::InvalidArgument, "encoder not opened",
                       "apple");
  }
  if (frame.planeCount() != 3 || frame.planeData(0) == nullptr ||
      frame.planeData(1) == nullptr || frame.planeData(2) == nullptr) {
    return Error::make(ErrorCode::InvalidArgument,
                       "frame is not in the negotiated Yuv420P interchange",
                       "apple");
  }
  // The negotiated geometry is authoritative (the executor derives the
  // encoder config from the decoder's output format); strides may pad.
  if (frame.planeLinesize(0) < impl_->width ||
      frame.planeLinesize(1) < impl_->width / 2 ||
      frame.planeLinesize(2) < impl_->width / 2) {
    return Error::make(ErrorCode::InvalidArgument,
                       "frame strides do not match the negotiated geometry",
                       "apple");
  }

  CVPixelBufferRef pixels = nullptr;
  OSStatus status = CVPixelBufferCreate(
      kCFAllocatorDefault, impl_->width, impl_->height,
      kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, nullptr, &pixels);
  if (status != noErr || pixels == nullptr) {
    return Error::make(ErrorCode::ExecutionFailed,
                       "cannot allocate the encoder pixel buffer", "apple");
  }
  CVPixelBufferLockBaseAddress(pixels, 0);
  std::uint8_t *yDst =
      static_cast<std::uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(pixels, 0));
  std::uint8_t *uvDst =
      static_cast<std::uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(pixels, 1));
  const std::size_t yStride = CVPixelBufferGetBytesPerRowOfPlane(pixels, 0);
  const std::size_t uvStride = CVPixelBufferGetBytesPerRowOfPlane(pixels, 1);
  if (yDst != nullptr && uvDst != nullptr) {
    for (int row = 0; row < impl_->height; ++row) {
      std::memcpy(yDst + std::size_t(row) * yStride,
                  frame.planeData(0) +
                      std::size_t(row) * std::size_t(frame.planeLinesize(0)),
                  std::size_t(impl_->width));
    }
    const int chromaWidth = impl_->width / 2;
    const int chromaHeight = impl_->height / 2;
    for (int row = 0; row < chromaHeight; ++row) {
      const std::uint8_t *srcU =
          frame.planeData(1) +
          std::size_t(row) * std::size_t(frame.planeLinesize(1));
      const std::uint8_t *srcV =
          frame.planeData(2) +
          std::size_t(row) * std::size_t(frame.planeLinesize(2));
      std::uint8_t *dst = uvDst + std::size_t(row) * uvStride;
      for (int col = 0; col < chromaWidth; ++col) {
        dst[std::size_t(2) * col] = srcU[col];       // Cb
        dst[std::size_t(2) * col + 1] = srcV[col];   // Cr
      }
    }
  }
  CVPixelBufferUnlockBaseAddress(pixels, 0);
  if (yDst == nullptr || uvDst == nullptr) {
    CFRelease(pixels);
    return Error::make(ErrorCode::ExecutionFailed,
                       "cannot access the encoder pixel planes", "apple");
  }

  const CMTime pts = CMTimeMake(frame.ptsUs(), 1000000);
  const CMTime duration =
      frame.durationUs() > 0 ? CMTimeMake(frame.durationUs(), 1000000)
                             : kCMTimeInvalid;
  status = VTCompressionSessionEncodeFrame(impl_->session, pixels, pts,
                                           duration, nullptr, nullptr,
                                           nullptr);
  CFRelease(pixels);
  if (status != noErr) {
    return Error::make(ErrorCode::ExecutionFailed,
                       "VideoToolbox rejected the frame (status " +
                           std::to_string(status) + ")",
                       "apple");
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    ++impl_->outstanding;
  }
  return Error::make(ErrorCode::None, "");
}

Error AppleStageEncoder::pull(PacketView &packet) {
  packet = PacketView{};
  if (!impl_->opened) {
    return Error::make(ErrorCode::InvalidArgument, "encoder not opened",
                       "apple");
  }
  CMSampleBufferRef sample = nullptr;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->queue.empty()) {
      // Nothing ready (or drained): empty view, no error.
      return Error::make(ErrorCode::None, "");
    }
    sample = impl_->queue.front();
    impl_->queue.pop_front();
  }

  CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
  if (block == NULL) {
    CFRelease(sample);
    return Error::make(ErrorCode::ExecutionFailed,
                       "encoded sample carries no data", "apple");
  }
  const std::size_t size = CMBlockBufferGetDataLength(block);
  if (size == 0) {
    CFRelease(sample);
    return Error::make(ErrorCode::None, "");
  }
  impl_->packetBytes.resize(size);
  const OSStatus status =
      CMBlockBufferCopyDataBytes(block, 0, size, impl_->packetBytes.data());
  if (status != noErr) {
    CFRelease(sample);
    return Error::make(ErrorCode::ExecutionFailed,
                       "cannot serialize the encoded sample", "apple");
  }
  packet.data = impl_->packetBytes.data();
  packet.size = size;

  // Keyframe: NotSync absent or false on the sample attachments.
  CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, 1);
  bool keyframe = true;
  if (attachments != nullptr && CFArrayGetCount(attachments) > 0) {
    CFDictionaryRef dict =
        static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments, 0));
    if (dict != nullptr) {
      const CFBooleanRef notSync = static_cast<CFBooleanRef>(
          CFDictionaryGetValue(dict, kCMSampleAttachmentKey_NotSync));
      if (notSync != nullptr && CFBooleanGetValue(notSync)) {
        keyframe = false;
      }
    }
  }
  packet.keyframe = keyframe;
  packet.ptsUs = cmTimeToUs(CMSampleBufferGetPresentationTimeStamp(sample));
  packet.durationUs = cmTimeToUs(CMSampleBufferGetDuration(sample));

  // Codec configuration: Annex-B concatenation of the format
  // description's parameter sets, built from the first sample (the
  // pointer stays stable afterwards, so every packet can carry it —
  // same semantics as the FFmpeg encoder's extradata view).
  impl_->buildConfigBytes(CMSampleBufferGetFormatDescription(sample));
  if (impl_->configBuilt) {
    packet.configData = impl_->configBytes.data();
    packet.configSize = impl_->configBytes.size();
  }
  CFRelease(sample);
  return Error::make(ErrorCode::None, "");
}

Error AppleStageEncoder::flush() {
  if (!impl_->opened || impl_->flushed) {
    return Error::make(ErrorCode::None, "");
  }
  // Complete every pending frame, then wait until the encoder has
  // delivered all of them to our queue (the executor's post-flush pull
  // loop must not observe a premature "empty").
  OSStatus status = VTCompressionSessionCompleteFrames(impl_->session,
                                                       kCMTimeInvalid);
  if (status != noErr) {
    return Error::make(ErrorCode::ExecutionFailed,
                       "cannot flush the VideoToolbox session", "apple");
  }
  impl_->flushed = true;
  {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    const bool delivered = impl_->signal.wait_for(
        lock, std::chrono::seconds(30),
        [this] { return impl_->outstanding == 0; });
    if (!delivered) {
      return Error::make(ErrorCode::ExecutionFailed,
                         "timed out waiting for the encoder to flush",
                         "apple");
    }
  }
  return Error::make(ErrorCode::None, "");
}

// ---------------------------------------------------------------------------
// Mux stage
// ---------------------------------------------------------------------------

struct AppleMuxStage::Impl {
  AVAssetWriter *writer = nil;
  AVAssetWriterInput *videoInput = nil;
  AVAssetWriterInput *audioInput = nil;
  AVAssetReader *audioReader = nil;
  AVAssetReaderTrackOutput *audioOutput = nil;
  CMSampleBufferRef pendingAudio = nullptr; // one-sample lookahead
  TrackOptions tracks;
  std::string outputPath;
  CMVideoFormatDescriptionRef videoFormat = nullptr;
  bool started = false;
  bool finalized = false;
};

AppleMuxStage::AppleMuxStage() : impl_(std::make_unique<Impl>()) {}
AppleMuxStage::~AppleMuxStage() {
  if (impl_ == nullptr) {
    return;
  }
  if (impl_->pendingAudio != nullptr) {
    CFRelease(impl_->pendingAudio);
    impl_->pendingAudio = nullptr;
  }
  if (impl_->writer != nil && impl_->started && !impl_->finalized) {
    // Honest teardown: an abandoned run never leaves a half-finalized
    // file that could masquerade as a valid output.
    @try {
      [impl_->writer cancelWriting];
    } @catch (NSException *) {
      // Teardown must never throw.
    }
  }
  if (impl_->videoFormat != nullptr) {
    CFRelease(impl_->videoFormat);
    impl_->videoFormat = nullptr;
  }
}

Error AppleMuxStage::open(const std::string &outputPath,
                          const TrackOptions &tracks) {
  impl_->outputPath = outputPath;
  impl_->tracks = tracks;
  NSString *fileType = nil;
  switch (tracks.container) {
  case Container::Mp4:
    fileType = AVFileTypeMPEG4;
    break;
  case Container::M4v:
    fileType = AVFileTypeAppleM4V;
    break;
  case Container::Mov:
    fileType = AVFileTypeQuickTimeMovie;
    break;
  case Container::ThreeGp:
    fileType = AVFileType3GPP;
    break;
  default:
    return Error::make(ErrorCode::UnsupportedFeature,
                       "AVAssetWriter cannot write this container", "apple");
  }
  NSError *nsError = nil;
  impl_->writer = [AVAssetWriter assetWriterWithURL:fileUrlFor(outputPath)
                                          fileType:fileType
                                             error:&nsError];
  if (impl_->writer == nil) {
    return Error::make(ErrorCode::ExecutionFailed,
                       nsError != nil
                           ? nsError.localizedDescription.UTF8String
                           : "cannot create the writer",
                       "apple");
  }

  if (tracks.copyAudio) {
    // Compressed-audio passthrough: read the source's first audio track
    // as-is and re-append it into a sourceFormatHint input.
    AVURLAsset *asset =
        [AVURLAsset assetWithURL:fileUrlFor(tracks.sourcePath)];
    if (asset == nil) {
      return Error::make(ErrorCode::SourceUnavailable,
                         "cannot open the audio source", "apple");
    }
    std::string loadError;
    if (!loadAssetTracks(asset, loadError)) {
      return Error::make(ErrorCode::SourceUnavailable, loadError, "apple");
    }
    AVAssetTrack *audioTrack = nil;
    for (AVAssetTrack *candidate in asset.tracks) {
      if (candidate.mediaType == AVMediaTypeAudio) {
        audioTrack = candidate;
        break;
      }
    }
    if (audioTrack == nil ||
        [audioTrack.formatDescriptions count] == 0) {
      return Error::make(ErrorCode::InvalidArgument,
                         "audio copy requested but the source has no audio "
                         "track",
                         "apple");
    }
    impl_->audioReader =
        [AVAssetReader assetReaderWithAsset:asset error:&nsError];
    if (impl_->audioReader == nil) {
      return Error::make(ErrorCode::SourceUnavailable,
                         nsError != nil
                             ? nsError.localizedDescription.UTF8String
                             : "cannot create the audio reader",
                         "apple");
    }
    // nil output settings = compressed samples, no decode.
    impl_->audioOutput = [[AVAssetReaderTrackOutput alloc]
        initWithTrack:audioTrack
        outputSettings:nil];
    if (impl_->audioOutput == nil ||
        ![impl_->audioReader canAddOutput:impl_->audioOutput]) {
      return Error::make(ErrorCode::UnsupportedFeature,
                         "cannot read the source audio track", "apple");
    }
    [impl_->audioReader addOutput:impl_->audioOutput];
    if (![impl_->audioReader startReading]) {
      return Error::make(
          ErrorCode::ExecutionFailed,
          impl_->audioReader.error != nil
              ? impl_->audioReader.error.localizedDescription.UTF8String
              : "cannot start reading the audio source",
          "apple");
    }
    CMFormatDescriptionRef format =
        (CMFormatDescriptionRef)audioTrack.formatDescriptions[0];
    impl_->audioInput =
        [[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeAudio
                                        outputSettings:nil
                                       sourceFormatHint:format];
    impl_->audioInput.expectsMediaDataInRealTime = NO;
    if (impl_->audioInput == nil ||
        ![impl_->writer canAddInput:impl_->audioInput]) {
      return Error::make(ErrorCode::UnsupportedFeature,
                         "cannot configure the audio track", "apple");
    }
    [impl_->writer addInput:impl_->audioInput];
  }
  return Error::make(ErrorCode::None, "");
}

bool AppleMuxStage::wantsGlobalHeader() const {
  // See the header's packet contract: MP4-family tracks carry the codec
  // configuration in the track's format description (rebuilt from the
  // first packet's configData), not in a stream header.
  return false;
}

Error AppleMuxStage::writeVideoPacket(const PacketView &packet) {
  if (impl_->finalized) {
    return Error::make(ErrorCode::InvalidArgument, "mux stage finalized",
                       "apple");
  }
  if (packet.data == nullptr || packet.size == 0) {
    return Error::make(ErrorCode::InvalidArgument, "empty video packet",
                       "apple");
  }

  if (!impl_->started) {
    // First packet: rebuild the video track's format description from
    // the carried Annex-B parameter sets and start the file. The writer
    // consumes the encoder's own bitstream (outputSettings nil).
    if (packet.configData == nullptr || packet.configSize == 0) {
      return Error::make(ErrorCode::ExecutionFailed,
                         "first video packet carried no codec configuration",
                         "apple");
    }
    const std::vector<std::vector<std::uint8_t>> nals =
        splitAnnexB(packet.configData, packet.configSize);
    if (nals.size() < 2) {
      return Error::make(ErrorCode::ExecutionFailed,
                         "codec configuration is not a usable parameter "
                         "set list",
                         "apple");
    }
    std::vector<const std::uint8_t *> pointers;
    std::vector<std::size_t> sizes;
    pointers.reserve(nals.size());
    sizes.reserve(nals.size());
    for (const auto &nal : nals) {
      pointers.push_back(nal.data());
      sizes.push_back(nal.size());
    }
    // -50 is paramErr (the MacTypes constant is not in this file's
    // include graph); every branch below either assigns a real status or
    // returns an explicit Error, so the initializer is purely defensive.
    OSStatus status = -50;
    if (impl_->tracks.videoCodec == VideoCodec::HEVC) {
      if (@available(macOS 10.13, iOS 11.0, *)) {
        status = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
            kCFAllocatorDefault, pointers.size(), pointers.data(),
            sizes.data(), 4, nullptr, &impl_->videoFormat);
      } else {
        return Error::make(ErrorCode::UnsupportedFeature,
                           "HEVC parameter sets need macOS 10.13/iOS 11",
                           "apple");
      }
    } else {
      status = CMVideoFormatDescriptionCreateFromH264ParameterSets(
          kCFAllocatorDefault, pointers.size(), pointers.data(),
          sizes.data(), 4, &impl_->videoFormat);
    }
    if (status != noErr || impl_->videoFormat == nullptr) {
      return Error::make(ErrorCode::ExecutionFailed,
                         "cannot rebuild the video format description",
                         "apple");
    }
    impl_->videoInput = [[AVAssetWriterInput alloc]
        initWithMediaType:AVMediaTypeVideo
        outputSettings:nil
        sourceFormatHint:impl_->videoFormat];
    impl_->videoInput.expectsMediaDataInRealTime = NO;
    if (impl_->videoInput == nil ||
        ![impl_->writer canAddInput:impl_->videoInput]) {
      return Error::make(ErrorCode::UnsupportedFeature,
                         "cannot configure the video track", "apple");
    }
    [impl_->writer addInput:impl_->videoInput];
    [impl_->writer startWriting];
    // Anchor the session at the first video packet's timestamp so a
    // nonzero source start does not gain a leading gap.
    CMTime sessionStart = packet.ptsUs > 0
                              ? CMTimeMake(packet.ptsUs, 1000000)
                              : kCMTimeZero;
    [impl_->writer startSessionAtSourceTime:sessionStart];
    impl_->started = true;
  }

  // Rebuild a CMSampleBuffer over the packet's AVCC bytes. The block
  // buffer references the view's memory in place (kCFAllocatorNull):
  // appendSampleBuffer is synchronous, and the view is valid until the
  // encoder's next pull by contract.
  CMBlockBufferRef block = nullptr;
  OSStatus status = CMBlockBufferCreateWithMemoryBlock(
      kCFAllocatorDefault, const_cast<std::uint8_t *>(packet.data),
      packet.size, kCFAllocatorNull, nullptr, 0, packet.size, 0, &block);
  if (status != noErr || block == nullptr) {
    return Error::make(ErrorCode::ExecutionFailed,
                       "cannot wrap the packet data", "apple");
  }
  const CMSampleTimingInfo timing = {
      packet.durationUs > 0 ? CMTimeMake(packet.durationUs, 1000000)
                            : kCMTimeInvalid,
      CMTimeMake(packet.ptsUs, 1000000), kCMTimeInvalid};
  const std::size_t sampleSize = packet.size;
  CMSampleBufferRef sample = nullptr;
  status = CMSampleBufferCreateReady(kCFAllocatorDefault, block,
                                     impl_->videoFormat, 1, 1, &timing, 1,
                                     &sampleSize, &sample);
  if (status != noErr || sample == nullptr) {
    CFRelease(block);
    return Error::make(ErrorCode::ExecutionFailed,
                       "cannot rebuild the video sample", "apple");
  }
  // Mark non-keyframes as not-sync. CMSetAttachment is the documented
  // API for sample attachments; reaching into the attachments array with
  // a mutable cast no longer compiles (CFArrayGetValueAtIndex returns
  // const void * in current SDKs).
  CMSetAttachment(sample, kCMSampleAttachmentKey_NotSync,
                  packet.keyframe ? kCFBooleanFalse : kCFBooleanTrue,
                  kCMAttachmentMode_ShouldNotPropagate);

  // Non-realtime inputs still have backpressure; wait briefly for
  // readiness so appends never buffer unbounded, but never deadlock:
  // the writer drains its own queues on internal threads.
  for (int round = 0; round < 2000 && !impl_->videoInput.isReadyForMoreMediaData;
       ++round) {
    [NSThread sleepForTimeInterval:0.005];
  }
  const BOOL appended = [impl_->videoInput appendSampleBuffer:sample];
  CFRelease(sample);
  CFRelease(block);
  if (!appended) {
    return Error::make(
        ErrorCode::ExecutionFailed,
        impl_->writer.error != nil
            ? impl_->writer.error.localizedDescription.UTF8String
            : "video append failed",
        "apple");
  }
  return drainAudioUpTo(packet.ptsUs);
}

Error AppleMuxStage::drainAudioUpTo(std::int64_t boundaryPtsUs) {
  if (impl_->audioOutput == nil) {
    return Error::make(ErrorCode::None, "");
  }
  for (;;) {
    CMSampleBufferRef sample = nullptr;
    if (impl_->pendingAudio != nullptr) {
      sample = impl_->pendingAudio;
      impl_->pendingAudio = nullptr;
    } else {
      sample = [impl_->audioOutput copyNextSampleBuffer];
      if (sample == NULL) {
        break; // audio EOF
      }
    }
    // One-sample lookahead: hold back packets that belong to a later
    // interleave window (mirrors FfmpegMuxStage::drainAudioUpTo).
    const std::int64_t ptsUs =
        cmTimeToUs(CMSampleBufferGetPresentationTimeStamp(sample));
    if (boundaryPtsUs >= 0 && ptsUs > boundaryPtsUs) {
      impl_->pendingAudio = sample; // ownership moves to the lookahead
      return Error::make(ErrorCode::None, "");
    }
    for (int round = 0;
         round < 2000 && impl_->audioInput != nil &&
         !impl_->audioInput.isReadyForMoreMediaData;
         ++round) {
      [NSThread sleepForTimeInterval:0.005];
    }
    const BOOL appended =
        impl_->audioInput != nil
            ? [impl_->audioInput appendSampleBuffer:sample]
            : NO;
    CFRelease(sample);
    if (!appended) {
      return Error::make(
          ErrorCode::ExecutionFailed,
          impl_->writer.error != nil
              ? impl_->writer.error.localizedDescription.UTF8String
              : "audio append failed",
          "apple");
    }
  }
  return Error::make(ErrorCode::None, "");
}

Error AppleMuxStage::finalize() {
  if (impl_->finalized) {
    return Error::make(ErrorCode::None, "");
  }
  if (!impl_->started) {
    return Error::make(ErrorCode::ExecutionFailed,
                       "no video packet was written; refusing to finalize "
                       "an empty hybrid output",
                       "apple");
  }
  Error err = drainAudioUpTo(-1);
  if (!err.ok()) {
    return err;
  }
  [impl_->videoInput markAsFinished];
  if (impl_->audioInput != nil) {
    [impl_->audioInput markAsFinished];
  }
  // The synchronous -finishWriting is deprecated; the completion-handler
  // form is the API present in every current SDK (same pattern as
  // apple_pipeline.mm).
  __block BOOL writeSucceeded = NO;
  dispatch_semaphore_t sema = dispatch_semaphore_create(0);
  [impl_->writer finishWritingWithCompletionHandler:^{
    writeSucceeded = (impl_->writer.status == AVAssetWriterStatusCompleted);
    dispatch_semaphore_signal(sema);
  }];
  dispatch_semaphore_wait(sema, DISPATCH_TIME_FOREVER);
  if (!writeSucceeded) {
    return Error::make(
        ErrorCode::ExecutionFailed,
        impl_->writer.error != nil
            ? impl_->writer.error.localizedDescription.UTF8String
            : "finishing the output failed",
        "apple");
  }
  impl_->finalized = true;
  return Error::make(ErrorCode::None, "");
}

// ---------------------------------------------------------------------------
// Stage backend factory
// ---------------------------------------------------------------------------

velmux::backend::IVideoStageDecoder *AppleStageBackend::createVideoDecoder() {
  return new AppleStageDecoder();
}

velmux::backend::IVideoStageEncoder *AppleStageBackend::createVideoEncoder() {
  return new AppleStageEncoder();
}

velmux::backend::IMuxStage *AppleStageBackend::createMux() {
  return new AppleMuxStage();
}

} // namespace velmux::apple::media

#endif // __APPLE__
