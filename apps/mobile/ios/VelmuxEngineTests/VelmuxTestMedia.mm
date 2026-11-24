// VelmuxEngineTests — on-simulator media fixtures + INDEPENDENT validation.
//
// Two halves (both real media, no mocking):
//
//  1. Fixture generation: REAL H.264 + AAC MP4 files written by AVAssetWriter
//     (VideoToolbox encode through AVAssetWriterInputPixelBufferAdaptor +
//     AAC encode through AVAssetWriterInput) from synthetic gradient frames
//     and a sine-tone audio bed. The engine under test never touches these
//     helpers — they only produce input files in the test's temp space.
//
//  2. Independent probing: every produced output is validated through a
//     fresh AVURLAsset/AVAssetReader — never with the code that produced it
//     (spec §50). The probe reports track codecs (fourcc), geometry,
//     duration, and can decode a bounded number of frames.
//
// These helpers are compiled only into the VelmuxEngineTests bundle and
// deliberately duplicate nothing from the engine: the engine's Apple
// backend lives in the VelmuxEngine pod; the test bundle must stay an
// independent validator.
#import "VelmuxTestMedia.h"

#import <cmath>
#import <cstdlib>
#import <cstring>
#import <algorithm>

namespace {

// Backpressure wait: bounded spin so a wedged writer fails the test instead
// of hanging the CI run (the same "never deadlock" posture as the engine's
// Apple mux stage).
bool waitForReady(AVAssetWriterInput *input) {
  for (int round = 0; round < 6000 && !input.isReadyForMoreMediaData;
       ++round) {
    [NSThread sleepForTimeInterval:0.005];
  }
  return input.isReadyForMoreMediaData;
}

// Fills one biplanar (420YpCbCr8BiPlanarVideoRange) pixel buffer with a
// moving diagonal gradient: the stripe offset advances per frame so every
// frame differs (real motion for real encoders).
void fillGradientFrame(CVPixelBufferRef pixels, int frameIndex) {
  const int width = int(CVPixelBufferGetWidth(pixels));
  const int height = int(CVPixelBufferGetHeight(pixels));
  CVPixelBufferLockBaseAddress(pixels, 0);
  uint8_t *yBase = static_cast<uint8_t *>(
      CVPixelBufferGetBaseAddressOfPlane(pixels, 0));
  uint8_t *uvBase = static_cast<uint8_t *>(
      CVPixelBufferGetBaseAddressOfPlane(pixels, 1));
  const size_t yStride = CVPixelBufferGetBytesPerRowOfPlane(pixels, 0);
  const size_t uvStride = CVPixelBufferGetBytesPerRowOfPlane(pixels, 1);
  if (yBase != nullptr) {
    const int stripe = (frameIndex * 7) % 256;
    for (int row = 0; row < height; ++row) {
      uint8_t *dst = yBase + size_t(row) * yStride;
      for (int col = 0; col < width; ++col) {
        dst[col] = uint8_t((row + col + stripe) & 0xFF);
      }
    }
  }
  if (uvBase != nullptr) {
    const size_t chromaWidth = size_t(width) / 2;
    const size_t chromaHeight = size_t(height) / 2;
    for (size_t row = 0; row < chromaHeight; ++row) {
      uint8_t *dst = uvBase + row * uvStride;
      for (size_t col = 0; col < chromaWidth; ++col) {
        dst[2 * col] = 128; // Cb
        dst[2 * col + 1] = 128; // Cr
      }
    }
  }
  CVPixelBufferUnlockBaseAddress(pixels, 0);
}

// Builds one Linear-PCM (float32, interleaved) CMSampleBuffer covering
// audio frames [startFrame, startFrame+frames) of the sine-tone bed. The
// block buffer owns the malloc'ed sample memory (default allocator), so the
// sample buffer is self-contained. Returns NULL on failure.
CMSampleBufferRef makeAudioSampleBuffer(
    const AudioStreamBasicDescription &asbd, CMFormatDescriptionRef format,
    int64_t startFrame, int frames) {
  const size_t bytes = size_t(frames) * size_t(asbd.mBytesPerFrame);
  auto *samples = static_cast<float *>(std::malloc(bytes));
  if (samples == nullptr) {
    return nullptr;
  }
  float *cursor = samples;
  constexpr double kPi = 3.14159265358979323846;
  for (int i = 0; i < frames; ++i) {
    const int64_t frame = startFrame + i;
    const float tone = static_cast<float>(
        std::sin(double(frame) * 2.0 * kPi * 440.0 / asbd.mSampleRate) *
        0.25);
    for (UInt32 ch = 0; ch < asbd.mChannelsPerFrame; ++ch) {
      *cursor++ = tone;
    }
  }
  CMBlockBufferRef block = nullptr;
  OSStatus status = CMBlockBufferCreateWithMemoryBlock(
      kCFAllocatorDefault, samples, bytes, kCFAllocatorDefault, nullptr, 0,
      bytes, 0, &block);
  if (status != noErr || block == nullptr) {
    std::free(samples);
    return nullptr;
  }
  const CMSampleTimingInfo timing = {
      CMTimeMake(frames, int32_t(asbd.mSampleRate)),
      CMTimeMake(startFrame, int32_t(asbd.mSampleRate)), kCMTimeInvalid};
  CMSampleBufferRef sample = nullptr;
  status = CMSampleBufferCreate(kCFAllocatorDefault, block, true, format,
                                CMItemCount(frames), 1, &timing, 0, nullptr,
                                &sample);
  CFRelease(block); // the sample buffer retains its own reference
  if (status != noErr || sample == nullptr) {
    return nullptr;
  }
  return sample;
}

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

// A fourcc integer ('avc1' == 0x61766331) as the human-readable 4-character
// string, most-significant byte first.
NSString *vlmxFourCharString(FourCharCode code) {
  char fourcc[5] = {char((code >> 24) & 0xFF), char((code >> 16) & 0xFF),
                    char((code >> 8) & 0xFF), char(code & 0xFF), 0};
  return [NSString stringWithFormat:@"%s", fourcc];
}

} // namespace

@implementation VLMXGeneratedMedia

- (instancetype)initWithPath:(NSString *)path
                 durationUs:(std::int64_t)durationUs
                       width:(int)width
                      height:(int)height
                  frameCount:(int)frameCount
                   frameRate:(double)frameRate {
  self = [super init];
  if (self != nil) {
    _path = [path copy];
    _durationUs = durationUs;
    _width = width;
    _height = height;
    _frameCount = frameCount;
    _frameRate = frameRate;
  }
  return self;
}

@end

// Readwrite redeclarations for this translation unit only: the probe stays
// immutable to callers (the public header is readonly).
@interface VLMXMediaProbe ()
@property (nullable, nonatomic, copy, readwrite) NSString *videoCodec;
@property (nullable, nonatomic, copy, readwrite) NSString *audioCodec;
@property (nonatomic, assign, readwrite) std::int64_t durationUs;
@property (nonatomic, assign, readwrite) int width;
@property (nonatomic, assign, readwrite) int height;
@end

@implementation VLMXMediaProbe
@end

@implementation VLMXTestMedia

+ (nullable VLMXGeneratedMedia *)
    generateH264AacMp4AtPath:(NSString *)path
                      width:(int)width
                     height:(int)height
                  frameRate:(double)frameRate
                 frameCount:(int)frameCount
                      error:(NSError **)error {
  NSParameterAssert(width > 0 && height > 0 && (width % 2) == 0 &&
                    (height % 2) == 0);
  NSParameterAssert(frameRate > 0.0 && frameCount > 0);
  if (error != nil) {
    *error = nil;
  }
  [[NSFileManager defaultManager] removeItemAtPath:path error:nil];

  const int32_t sampleRate = 44100;
  const int channels = 2;

  NSError *writerError = nil;
  AVAssetWriter *writer = [[AVAssetWriter alloc]
      initWithURL:[NSURL fileURLWithPath:path]
           fileType:AVFileTypeMPEG4
              error:&writerError];
  if (writer == nil) {
    if (error != nil) {
      *error = writerError;
    }
    return nil;
  }

  // Video: H.264 through the pixel-buffer adaptor (the VideoToolbox-backed
  // writer path — same surface apple_pipeline.mm drives).
  NSDictionary *videoSettings = @{
    AVVideoCodecKey : AVVideoCodecTypeH264,
    AVVideoWidthKey : @(width),
    AVVideoHeightKey : @(height),
    AVVideoCompressionPropertiesKey : @{
      AVVideoAverageBitRateKey : @1500000,
      AVVideoExpectedSourceFrameRateKey : @(int(frameRate)),
      AVVideoMaxKeyFrameIntervalKey : @(int(frameRate)),
    },
  };
  AVAssetWriterInput *videoInput =
      [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
                                         outputSettings:videoSettings];
  videoInput.expectsMediaDataInRealTime = NO;
  NSDictionary *bufferAttributes = @{
    (NSString *)kCVPixelBufferPixelFormatTypeKey :
        @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
    (NSString *)kCVPixelBufferWidthKey : @(width),
    (NSString *)kCVPixelBufferHeightKey : @(height),
  };
  AVAssetWriterInputPixelBufferAdaptor *adaptor =
      [AVAssetWriterInputPixelBufferAdaptor
          assetWriterInputPixelBufferAdaptorWithAssetWriterInput:videoInput
                                    sourcePixelBufferAttributes:bufferAttributes];
  if (![writer canAddInput:videoInput]) {
    if (error != nil) {
      *error = [NSError
          errorWithDomain:@"VelmuxEngineTests"
                     code:1
                 userInfo:@{
                   NSLocalizedDescriptionKey : @"video input rejected"
                 }];
    }
    return nil;
  }
  [writer addInput:videoInput];

  // Audio: AAC encode from Linear-PCM sample buffers.
  NSDictionary *audioSettings = @{
    AVFormatIDKey : @(kAudioFormatMPEG4AAC),
    AVSampleRateKey : @(sampleRate),
    AVNumberOfChannelsKey : @(channels),
    AVEncoderBitRateKey : @96000,
  };
  AVAssetWriterInput *audioInput =
      [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio
                                         outputSettings:audioSettings];
  audioInput.expectsMediaDataInRealTime = NO;
  if (![writer canAddInput:audioInput]) {
    if (error != nil) {
      *error = [NSError
          errorWithDomain:@"VelmuxEngineTests"
                     code:2
                 userInfo:@{
                   NSLocalizedDescriptionKey : @"audio input rejected"
                 }];
    }
    return nil;
  }
  [writer addInput:audioInput];

  // PCM format description for the synthetic audio bed.
  AudioStreamBasicDescription asbd = {0};
  asbd.mSampleRate = double(sampleRate);
  asbd.mFormatID = kAudioFormatLinearPCM;
  asbd.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
  asbd.mChannelsPerFrame = UInt32(channels);
  asbd.mBitsPerChannel = 32;
  asbd.mBytesPerFrame = UInt32(channels * 4);
  asbd.mFramesPerPacket = 1;
  asbd.mBytesPerPacket = UInt32(channels * 4);
  CMFormatDescriptionRef audioFormat = nullptr;
  if (CMAudioFormatDescriptionCreate(kCFAllocatorDefault, &asbd, 0, nullptr,
                                     0, nullptr, nullptr,
                                     &audioFormat) != noErr ||
      audioFormat == nullptr) {
    if (error != nil) {
      *error = [NSError
          errorWithDomain:@"VelmuxEngineTests"
                     code:3
                 userInfo:@{
                   NSLocalizedDescriptionKey : @"cannot create PCM format"
                 }];
    }
    return nil;
  }

  [writer startWriting];
  [writer startSessionAtSourceTime:kCMTimeZero];

  bool failed = false;
  NSString *failure = @"";

  // Video pass: one gradient frame per tick, appended as produced (the
  // adaptor encodes through VideoToolbox on append).
  {
    CVPixelBufferRef pixels = nullptr;
    const OSStatus status = CVPixelBufferCreate(
        kCFAllocatorDefault, width, height,
        kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, nullptr, &pixels);
    if (status != noErr || pixels == nullptr) {
      failed = true;
      failure = @"cannot create pixel buffer";
    }
    for (int frame = 0; pixels != nullptr && frame < frameCount; ++frame) {
      @autoreleasepool {
        if (!waitForReady(videoInput)) {
          failed = true;
          failure = @"video input stalled";
          break;
        }
        fillGradientFrame(pixels, frame);
        const CMTime pts = CMTimeMake(int64_t(frame) * 1000000,
                                      int32_t(frameRate * 1000000));
        if (![adaptor appendPixelBuffer:pixels
                  withPresentationTime:pts]) {
          failed = true;
          failure = writer.error != nil
                        ? writer.error.localizedDescription
                        : @"video append failed";
          break;
        }
      }
    }
    if (pixels != nullptr) {
      CVPixelBufferRelease(pixels);
    }
    [videoInput markAsFinished];
  }

  // Audio pass: 1024-frame PCM chunks until the video duration is covered.
  if (!failed) {
    const int64_t totalAudioFrames =
        int64_t(double(frameCount) / frameRate * sampleRate);
    const int64_t chunkFrames = 1024;
    for (int64_t at = 0; at < totalAudioFrames; at += chunkFrames) {
      const int frames =
          int(std::min<int64_t>(chunkFrames, totalAudioFrames - at));
      CMSampleBufferRef sample =
          makeAudioSampleBuffer(asbd, audioFormat, at, frames);
      if (sample == nullptr) {
        failed = true;
        failure = @"cannot build audio sample";
        break;
      }
      if (!waitForReady(audioInput)) {
        CFRelease(sample);
        failed = true;
        failure = @"audio input stalled";
        break;
      }
      if (![audioInput appendSampleBuffer:sample]) {
        CFRelease(sample);
        failed = true;
        failure = writer.error != nil
                      ? writer.error.localizedDescription
                      : @"audio append failed";
        break;
      }
      CFRelease(sample);
    }
    [audioInput markAsFinished];
  }
  CFRelease(audioFormat);

  if (failed) {
    @try {
      [writer cancelWriting];
    } @catch (NSException *) {
      // Teardown must never throw.
    }
    if (error != nil) {
      *error = [NSError errorWithDomain:@"VelmuxEngineTests"
                                   code:4
                               userInfo:@{
                                 NSLocalizedDescriptionKey : failure
                               }];
    }
    return nil;
  }

  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  __block BOOL finished = NO;
  [writer finishWritingWithCompletionHandler:^{
    finished = YES;
    dispatch_semaphore_signal(done);
  }];
  dispatch_semaphore_wait(
      done, dispatch_time(DISPATCH_TIME_NOW, 120 * NSEC_PER_SEC));
  if (writer.status != AVAssetWriterStatusCompleted || !finished) {
    @try {
      [writer cancelWriting];
    } @catch (NSException *) {
      // Teardown must never throw.
    }
    if (error != nil) {
      *error = [NSError
          errorWithDomain:@"VelmuxEngineTests"
                     code:5
                 userInfo:@{
                   NSLocalizedDescriptionKey :
                       writer.error != nil
                           ? writer.error.localizedDescription
                           : @"writer did not complete"
                 }];
    }
    return nil;
  }

  const std::int64_t durationUs =
      std::llround(double(frameCount) / frameRate * 1e6);
  return [[VLMXGeneratedMedia alloc] initWithPath:path
                                       durationUs:durationUs
                                             width:width
                                            height:height
                                        frameCount:frameCount
                                         frameRate:frameRate];
}

+ (nullable VLMXMediaProbe *)probeAtPath:(NSString *)path
                                   error:(NSError **)error {
  if (error != nil) {
    *error = nil;
  }
  NSURL *url = [NSURL fileURLWithPath:path];
  AVURLAsset *asset = [AVURLAsset assetWithURL:url];
  __block BOOL loaded = NO;
  dispatch_semaphore_t sema = dispatch_semaphore_create(0);
  [asset loadValuesAsynchronouslyForKeys:@[ @"tracks" ]
                        completionHandler:^{
                          loaded = YES;
                          dispatch_semaphore_signal(sema);
                        }];
  dispatch_semaphore_wait(
      sema, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC));
  if (!loaded || [asset statusOfValueForKey:@"tracks" error:nil] ==
                     AVKeyValueStatusFailed) {
    if (error != nil) {
      *error = [NSError
          errorWithDomain:@"VelmuxEngineTests"
                     code:10
                 userInfo:@{
                   NSLocalizedDescriptionKey : @"cannot load tracks"
                 }];
    }
    return nil;
  }

  VLMXMediaProbe *probe = [[VLMXMediaProbe alloc] init];
  for (AVAssetTrack *track in asset.tracks) {
    if ([track.mediaType isEqualToString:AVMediaTypeVideo] &&
        probe.videoCodec == nil) {
      probe.width = int(track.naturalSize.width);
      probe.height = int(track.naturalSize.height);
      for (id description in track.formatDescriptions) {
        CMFormatDescriptionRef format =
            (__bridge CMFormatDescriptionRef)description;
        if (CMFormatDescriptionGetMediaType(format) == kCMMediaType_Video) {
          const FourCharCode codec =
              CMVideoFormatDescriptionGetCodecType(format);
          probe.videoCodec = vlmxFourCharString(codec);
          break;
        }
      }
    } else if ([track.mediaType isEqualToString:AVMediaTypeAudio] &&
               probe.audioCodec == nil) {
      for (id description in track.formatDescriptions) {
        CMFormatDescriptionRef format =
            (__bridge CMFormatDescriptionRef)description;
        if (CMFormatDescriptionGetMediaType(format) == kCMMediaType_Audio) {
          AudioFormatID formatID = 0;
          const AudioStreamBasicDescription *asbd =
              (const AudioStreamBasicDescription *)
                  CMAudioFormatDescriptionGetStreamBasicDescription(format);
          if (asbd != nullptr) {
            formatID = asbd->mFormatID;
          }
          probe.audioCodec = vlmxFourCharString(formatID);
          break;
        }
      }
    }
  }
  probe.durationUs = cmTimeToUs(asset.duration);
  return probe;
}

+ (int)decodeVideoFramesAtPath:(NSString *)path maxFrames:(int)maxFrames {
  AVURLAsset *asset = [AVURLAsset assetWithURL:[NSURL fileURLWithPath:path]];
  __block BOOL loaded = NO;
  dispatch_semaphore_t sema = dispatch_semaphore_create(0);
  [asset loadValuesAsynchronouslyForKeys:@[ @"tracks" ]
                        completionHandler:^{
                          loaded = YES;
                          dispatch_semaphore_signal(sema);
                        }];
  dispatch_semaphore_wait(
      sema, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC));
  if (!loaded) {
    return -1;
  }
  AVAssetTrack *videoTrack = nil;
  for (AVAssetTrack *track in asset.tracks) {
    if ([track.mediaType isEqualToString:AVMediaTypeVideo]) {
      videoTrack = track;
      break;
    }
  }
  if (videoTrack == nil) {
    return -1;
  }
  NSError *nsError = nil;
  AVAssetReader *reader = [AVAssetReader assetReaderWithAsset:asset
                                                        error:&nsError];
  if (reader == nil) {
    return -1;
  }
  NSDictionary *settings = @{
    (NSString *)kCVPixelBufferPixelFormatTypeKey :
        @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
  };
  AVAssetReaderTrackOutput *output =
      [[AVAssetReaderTrackOutput alloc] initWithTrack:videoTrack
                                        outputSettings:settings];
  if (output == nil || ![reader canAddOutput:output]) {
    return -1;
  }
  [reader addOutput:output];
  if (![reader startReading]) {
    return -1;
  }
  int decoded = 0;
  while (decoded < maxFrames) {
    CMSampleBufferRef sample = [output copyNextSampleBuffer];
    if (sample == NULL) {
      break;
    }
    CVPixelBufferRef pixels = CMSampleBufferGetImageBuffer(sample);
    if (pixels == NULL) {
      CFRelease(sample);
      return -1; // a video track that yields no decodable frames is a hard
                 // failure, not a short read
    }
    CFRelease(sample);
    ++decoded;
  }
  return decoded;
}

+ (NSString *)freshWorkDirectory:(NSString *)label {
  NSString *base = NSTemporaryDirectory();
  NSString *dir = [base
      stringByAppendingPathComponent:[NSString
          stringWithFormat:@"velmux-engine-tests-%@-%@", label,
                           [[NSUUID UUID] UUIDString]]];
  [[NSFileManager defaultManager] createDirectoryAtPath:dir
                            withIntermediateDirectories:YES
                                             attributes:nil
                                                  error:nil];
  return dir;
}

+ (long long)fileSizeAtPath:(NSString *)path {
  NSDictionary *attributes = [[NSFileManager defaultManager]
      attributesOfItemAtPath:path
                       error:nil];
  NSNumber *size = attributes[NSFileSize];
  return size != nil ? size.longLongValue : 0;
}

@end
