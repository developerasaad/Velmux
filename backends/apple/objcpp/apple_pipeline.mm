// Apple native pipeline implementation. See apple_pipeline.h.
#import "apple_pipeline.h"

#ifdef __APPLE__

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>

#import <dispatch/dispatch.h>

#import <algorithm>
#import <cmath>
#import <vector>

namespace velmux::apple::media {

using namespace velmux::core;

// ---------------------------------------------------------------------------
// Codec mapping (CMFormatDescription / AVFoundation fourccs)
// ---------------------------------------------------------------------------

namespace {

std::optional<VideoCodec> videoCodecFromFourCC(FourCharCode code) {
  switch (code) {
  case 'avc1':
  case 'avc3':
    return VideoCodec::H264;
  case 'hvc1':
  case 'hev1':
    return VideoCodec::HEVC;
  case 'vp09':
    return VideoCodec::VP9;
  case 'vp08':
    return VideoCodec::VP8;
  case 'av01':
    return VideoCodec::AV1;
  case 'mp4v':
    return VideoCodec::Mpeg4Part2;
  case 'apcn':
  case 'apch':
  case 'apcs':
  case 'apco':
  case 'ap4h':
  case 'ap4x':
    return VideoCodec::ProRes;
  case 'dvc ':
  case 'dvcp':
    return VideoCodec::DvcPro;
  case 'mjpa':
  case 'mjpb':
    return VideoCodec::Mjpeg;
  default:
    return std::nullopt;
  }
}

std::optional<AudioCodec> audioCodecFromFourCC(FourCharCode code) {
  switch (code) {
  case 'aac ':
    return AudioCodec::Aac;
  case 'aach':
    return AudioCodec::HeAac;
  case '.mp3':
  case 'MPG3':
    return AudioCodec::Mp3;
  case 'opus':
    return AudioCodec::Opus;
  case 'alac':
    return AudioCodec::Alac;
  case 'flac':
    return AudioCodec::Flac;
  case 'ac-3':
    return AudioCodec::Ac3;
  case 'ec-3':
    return AudioCodec::Eac3;
  case 'lpcm':
    return AudioCodec::Pcm;
  case 'samr':
    return AudioCodec::AmrNb;
  case 'sawb':
    return AudioCodec::AmrWb;
  default:
    return std::nullopt;
  }
}

const char *videoCodecCode(VideoCodec codec) {
  switch (codec) {
  case VideoCodec::H264:
    return AVVideoCodecH264; // "avc1"
  case VideoCodec::HEVC:
    return AVVideoCodecHEVC; // "hvc1"
  default:
    return nullptr; // VideoToolbox encodes H.264 and HEVC (plus ProRes on
                    // capable hardware, not exposed through AVAssetWriter
                    // settings portably).
  }
}

FourCharCode audioFormatCode(AudioCodec codec) {
  switch (codec) {
  case AudioCodec::Aac:
  case AudioCodec::HeAac:
    return kAudioFormatMPEG4AAC;
  case AudioCodec::Alac:
    return kAudioFormatAppleLossless;
  case AudioCodec::Flac:
    return kAudioFormatFLAC;
  case AudioCodec::Pcm:
    return kAudioFormatLinearPCM;
  default:
    return 0; // AVAssetWriter supports these; MP3/Opus/Vorbis encode are
              // not AudioToolbox encoders.
  }
}

std::string fourCCString(FourCharCode code) {
  std::string out;
  for (int shift = 24; shift >= 0; shift -= 8) {
    out += char((code >> shift) & 0xFF);
  }
  return out;
}

bool cancelled(const RunContext &ctx) {
  return ctx.shouldContinue && !ctx.shouldContinue();
}

void reportProgress(const RunContext &ctx, double fraction) {
  if (ctx.onProgress) {
    ctx.onProgress(fraction);
  }
}

std::optional<std::string> localPathForUri(const std::string &uri) {
  const std::string prefix = "file://";
  std::string path = uri;
  if (path.rfind(prefix, 0) == 0) {
    path = path.substr(prefix.size());
  }
  if (path.rfind("content://", 0) == 0 || path.rfind("http://", 0) == 0 ||
      path.rfind("https://", 0) == 0) {
    return std::nullopt;
  }
  if (path.empty() || path.front() != '/') {
    return std::nullopt;
  }
  return path;
}

} // namespace

// ---------------------------------------------------------------------------
// Capability probing
// ---------------------------------------------------------------------------

bool enumerateSupport(NativeSupport &out) {
#if TARGET_OS_OSX || TARGET_OS_IOS || TARGET_OS_TV || TARGET_OS_WATCH
  const auto addVideo = [&out](VideoCodec codec, bool decode, bool encode,
                               bool hardware) {
    for (auto &[c, entry] : out.video) {
      if (c == codec) {
        entry.decode = entry.decode || decode;
        entry.encode = entry.encode || encode;
        entry.hardware = entry.hardware || hardware;
        return;
      }
    }
    out.video.push_back({codec, CodecEntry{decode, encode, hardware}});
  };
  const auto addAudio = [&out](AudioCodec codec, bool decode, bool encode) {
    for (auto &[c, entry] : out.audio) {
      if (c == codec) {
        entry.decode = entry.decode || decode;
        entry.encode = entry.encode || encode;
        return;
      }
    }
    out.audio.push_back({codec, CodecEntry{decode, encode, false}});
  };

  // VideoToolbox hardware queries. Software fallbacks exist for H.264 and
  // HEVC on all supported OS versions, so encode support is always true
  // for those; the hardware bit is what the probe answers.
  const bool hw264 =
      VTIsHardwareEncodeSupported(kCMVideoCodecType_H264);
  const bool hw265 =
      VTIsHardwareEncodeSupported(kCMVideoCodecType_HEVC);
  addVideo(VideoCodec::H264, true, true, hw264);
  addVideo(VideoCodec::HEVC, true, true, hw265);
  const bool hwProRes =
      VTIsHardwareEncodeSupported(kCMVideoCodecType_ProRes422);
  addVideo(VideoCodec::ProRes, true, true, hwProRes);
  if (@available(macOS 11.0, iOS 14.0, *)) {
    addVideo(VideoCodec::VP9,
             VTIsHardwareDecodeSupported(kCMVideoCodecType_VP9), false,
             false);
  }
  // Audio: AudioToolbox software encoders, available everywhere.
  addAudio(AudioCodec::Aac, true, true);
  addAudio(AudioCodec::HeAac, true, false);
  addAudio(AudioCodec::Alac, true, true);
  addAudio(AudioCodec::Flac, true, true);
  addAudio(AudioCodec::Pcm, true, true);
  addAudio(AudioCodec::Mp3, true, false);
  if (@available(macOS 10.13, iOS 11.0, *)) {
    addAudio(AudioCodec::Opus, true, false); // CoreAudio Opus decode
  }
  addAudio(AudioCodec::Ac3, true, false);
  addAudio(AudioCodec::Eac3, true, false);
  addAudio(AudioCodec::AmrNb, true, false);
  addAudio(AudioCodec::AmrWb, true, false);

  int hwCount = hw264 + hw265 + hwProRes;
  out.summary = std::string("VideoToolbox (hw: ") +
                (hw264 ? "h264 " : "") + (hw265 ? "hevc " : "") +
                (hwProRes ? "prores" : "") + ")";
  return true;
#else
  (void)out;
  return false;
#endif
}

// ---------------------------------------------------------------------------
// Inspection
// ---------------------------------------------------------------------------

bool inspectFile(const std::string &path, MediaInfo &out,
                 std::string &error) {
  const auto local = localPathForUri(path);
  if (!local.has_value()) {
    error = "the native inspector needs a local file";
    return false;
  }
  NSURL *url = [NSURL fileURLWithPath:
                         [NSString stringWithUTF8String:local->c_str()]];
  AVURLAsset *asset = [AVURLAsset assetWithURL:url];
  if (asset == nil) {
    error = "cannot open the source";
    return false;
  }
  // Load the track list synchronously (bounded by a semaphore; local files
  // resolve quickly).
  __block BOOL tracksLoaded = NO;
  dispatch_semaphore_t sema = dispatch_semaphore_create(0);
  [asset loadValuesAsynchronouslyForKeys:@[ @"tracks", @"duration" ]
                         completionHandler:^{
                           tracksLoaded = YES;
                           dispatch_semaphore_signal(sema);
                         }];
  dispatch_semaphore_wait(sema,
                          dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));
  if (!tracksLoaded ||
      [asset statusOfValueForKey:@"tracks" error:nil] ==
          AVKeyValueStatusFailed) {
    error = "cannot read the source metadata";
    return false;
  }
  MediaInfo info;
  const CMTime duration = asset.duration;
  if (CMTIME_IS_NUMERIC(duration)) {
    info.durationSec = CMTimeGetSeconds(duration);
  }
  NSDictionary *attrs =
      [[NSFileManager defaultManager] attributesOfItemAtPath:url.path
                                                       error:nil];
  if (attrs != nil) {
    info.sizeBytes = [attrs fileSize];
  }
  info.container = Container::Mov; // AVFoundation reads the QuickTime/mp4
                                   // family; refined by extension below.
  const auto dot = path.rfind('.');
  if (dot != std::string::npos) {
    const std::string ext = path.substr(dot + 1);
    if (ext == "mp4") {
      info.container = Container::Mp4;
    } else if (ext == "m4v") {
      info.container = Container::M4v;
    } else if (ext == "m4a") {
      info.container = Container::M4a;
    } else if (ext == "mov") {
      info.container = Container::Mov;
    } else if (ext == "3gp" || ext == "3g2") {
      info.container = Container::ThreeGp;
    } else if (ext == "mp3") {
      info.container = Container::Mp3;
    } else if (ext == "wav") {
      info.container = Container::Wav;
    } else if (ext == "flac") {
      info.container = Container::Flac;
    }
  }
  for (AVAssetTrack *track in asset.tracks) {
    if (track.mediaType == AVMediaTypeVideo &&
        [track.formatDescriptions count] > 0) {
      VideoStream stream;
      CMFormatDescriptionRef desc =
          (CMFormatDescriptionRef)track.formatDescriptions[0];
      const auto codec =
          videoCodecFromFourCC(CMFormatDescriptionGetMediaSubType(desc));
      if (!codec.has_value()) {
        continue;
      }
      stream.codec = *codec;
      stream.width = int32_t(track.naturalSize.width);
      stream.height = int32_t(track.naturalSize.height);
      stream.frameRate = double(track.nominalFrameRate);
      stream.bitRate = int64_t(track.estimatedDataRate);
      info.video.push_back(stream);
    } else if (track.mediaType == AVMediaTypeAudio &&
               [track.formatDescriptions count] > 0) {
      AudioStream stream;
      CMFormatDescriptionRef desc =
          (CMFormatDescriptionRef)track.formatDescriptions[0];
      const auto codec =
          audioCodecFromFourCC(CMFormatDescriptionGetMediaSubType(desc));
      if (!codec.has_value()) {
        continue;
      }
      stream.codec = *codec;
      stream.bitRate = int64_t(track.estimatedDataRate);
      const AudioStreamBasicDescription *asbd =
          (const AudioStreamBasicDescription *)CMAudioFormatDescriptionGetStreamBasicDescription(
              desc);
      if (asbd != nullptr) {
        stream.sampleRate = int32_t(asbd->mSampleRate);
        stream.channels = int32_t(asbd->mChannelsPerFrame);
      }
      info.audio.push_back(stream);
    }
  }
  if (!info.hasVideo() && !info.hasAudio()) {
    error = "no recognizable media track";
    return false;
  }
  out = info;
  return true;
}

// ---------------------------------------------------------------------------
// Stream copy (AVAssetExportSession passthrough)
// ---------------------------------------------------------------------------

RunResult remuxFile(const std::string &inPath, const std::string &outPath,
                    bool wantVideo, bool wantAudio, const RunContext &ctx) {
  RunResult result;
  const auto local = localPathForUri(inPath);
  if (!local.has_value()) {
    result.error = "the native path needs a local file";
    return result;
  }
  NSURL *inUrl =
      [NSURL fileURLWithPath:[NSString stringWithUTF8String:local->c_str()]];
  NSURL *outUrl =
      [NSURL fileURLWithPath:[NSString stringWithUTF8String:outPath.c_str()]];
  AVURLAsset *asset = [AVURLAsset assetWithURL:inUrl];
  if (asset == nil) {
    result.error = "cannot open the source";
    return result;
  }
  // Passthrough export cannot drop tracks; guard honestly: only run when
  // both enabled sets match a full copy. (Partial copies are refused by
  // the backend's canExecute.)
  (void)wantVideo;
  (void)wantAudio;
  AVAssetExportSession *session =
      [AVAssetExportSession exportSessionWithAsset:asset
                                     presetName:AVAssetExportPresetPassthrough];
  if (session == nil) {
    result.error = "passthrough export is unavailable";
    return result;
  }
  session.outputURL = outUrl;
  session.outputFileType = AVFileTypeQuickTimeMovie;
  const bool endsWith = [&outPath](const char *suffix) {
    const std::string s(suffix);
    return outPath.size() >= s.size() &&
           outPath.compare(outPath.size() - s.size(), s.size(), s) == 0;
  };
  if (endsWith(".mp4") || endsWith(".m4a")) {
    session.outputFileType = AVFileTypeAppleM4A;
  } else if (endsWith(".m4v")) {
    session.outputFileType = AVFileTypeAppleM4V;
  }
  __block AVAssetExportSessionStatus finalStatus = AVAssetExportSessionStatusUnknown;
  dispatch_semaphore_t sema = dispatch_semaphore_create(0);
  [session exportAsynchronouslyWithCompletionHandler:^{
    finalStatus = session.status;
    dispatch_semaphore_signal(sema);
  }];
  for (;;) {
    const long waited = dispatch_semaphore_wait(
        sema, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC));
    if (waited == 0) {
      break;
    }
    if (cancelled(ctx)) {
      [session cancelExport];
      dispatch_semaphore_wait(sema, DISPATCH_TIME_FOREVER);
      result.error = "canceled";
      return result;
    }
    reportProgress(ctx, session.progress);
  }
  reportProgress(ctx, 1.0);
  if (finalStatus != AVAssetExportSessionStatusCompleted) {
    result.error = session.error != nil
                       ? session.error.localizedDescription.UTF8String
                       : "export did not complete";
    return result;
  }
  result.ok = true;
  result.framesProcessed = 1;
  return result;
}

// ---------------------------------------------------------------------------
// Transcode (AVAssetReader -> AVAssetWriter)
// ---------------------------------------------------------------------------

RunResult transcodeFile(const std::string &inPath, const std::string &outPath,
                        const Pipeline &pipeline,
                        const ConversionRequest &request,
                        const RunContext &ctx) {
  RunResult result;
  const auto local = localPathForUri(inPath);
  if (!local.has_value()) {
    result.error = "the native path needs a local file";
    return result;
  }
  NSURL *inUrl =
      [NSURL fileURLWithPath:[NSString stringWithUTF8String:local->c_str()]];
  NSURL *outUrl =
      [NSURL fileURLWithPath:[NSString stringWithUTF8String:outPath.c_str()]];

  NSError *nsError = nil;
  AVURLAsset *asset = [AVURLAsset assetWithURL:inUrl];
  AVAssetReader *reader = [AVAssetReader assetReaderWithAsset:asset
                                                        error:&nsError];
  if (reader == nil) {
    result.error = nsError != nil
                       ? nsError.localizedDescription.UTF8String
                       : "cannot create the reader";
    return result;
  }
  AVAssetWriter *writer = [AVAssetWriter assetWriterWithURL:outUrl
                                                   fileType:AVFileTypeQuickTimeMovie
                                                      error:&nsError];
  if (writer == nil) {
    result.error = nsError != nil
                       ? nsError.localizedDescription.UTF8String
                       : "cannot create the writer";
    return result;
  }

  // ---- Track selection and outputs -------------------------------------------
  AVAssetTrack *videoTrack = nil;
  AVAssetTrack *audioTrack = nil;
  for (AVAssetTrack *track in asset.tracks) {
    if (videoTrack == nil && track.mediaType == AVMediaTypeVideo &&
        request.video.enabled) {
      videoTrack = track;
    } else if (audioTrack == nil && track.mediaType == AVMediaTypeAudio &&
               request.audio.enabled) {
      audioTrack = track;
    }
  }
  const bool wantVideo = request.video.enabled && videoTrack != nil;
  const bool wantAudio = request.audio.enabled && audioTrack != nil;
  if (!wantVideo && !wantAudio) {
    result.error = "no track to process";
    return result;
  }
  if (request.video.enabled && videoTrack == nil) {
    result.error = "the source has no video track";
    return result;
  }

  AVAssetReaderTrackOutput *videoOutput = nil;
  AVAssetWriterInput *videoInput = nil;
  AVAssetWriterInputPixelBufferAdaptor *videoAdaptor = nil;
  AVAssetReaderTrackOutput *audioOutput = nil;
  AVAssetWriterInput *audioInput = nil;
  double totalDurationSec = CMTimeGetSeconds(asset.duration);
  if (totalDurationSec <= 0.0) {
    totalDurationSec = 1.0;
  }

  if (wantVideo) {
    // Decode to uncompressed frames (nil settings = native decode).
    videoOutput = [AVAssetReaderTrackOutput
        trackOutputWithTrack:videoTrack
               outputSettings:nil];
    if (![reader canAddOutput:videoOutput]) {
      result.error = "cannot decode the video track";
      return result;
    }
    [reader addOutput:videoOutput];

    const char *codecCode = videoCodecCode(pipeline.videoCodec);
    if (codecCode == nullptr) {
      result.error = "VideoToolbox cannot encode this output codec";
      return result;
    }
    const double frameRate =
        videoTrack.nominalFrameRate > 0 ? videoTrack.nominalFrameRate : 30.0;
    std::int64_t bitRate = 0;
    if (request.video.rateControl == RateControl::Bitrate &&
        request.video.bitRate.has_value()) {
      bitRate = *request.video.bitRate;
    } else {
      const double quality = request.video.quality.has_value()
                                 ? double(*request.video.quality) / 100.0
                                 : 0.6;
      const double area = double(videoTrack.naturalSize.width) *
                          double(videoTrack.naturalSize.height);
      bitRate = std::int64_t(area * frameRate * (0.04 + 0.12 * quality));
      bitRate = std::clamp<std::int64_t>(bitRate, 300000, 40000000);
    }
    NSString *codec = [NSString stringWithUTF8String:codecCode];
    NSDictionary *settings = @{
      AVVideoCodecKey : codec,
      AVVideoWidthKey : [NSNumber numberWithDouble:videoTrack.naturalSize.width],
      AVVideoHeightKey : [NSNumber numberWithDouble:videoTrack.naturalSize.height],
      AVVideoCompressionPropertiesKey : @{
        AVVideoAverageBitRateKey : [NSNumber numberWithLongLong:bitRate],
        AVVideoExpectedSourceFrameRateKey : @(int(frameRate)),
        AVVideoMaxKeyFrameIntervalKey : @(int(frameRate)),
      },
    };
    videoInput = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
                                                    outputSettings:settings];
    videoInput.expectsMediaDataInRealTime = NO;
    // Preserve the source orientation.
    videoInput.transform = videoTrack.preferredTransform;
    videoAdaptor = [AVAssetWriterInputPixelBufferAdaptor
        assetWriterInputPixelBufferAdaptorWithAssetWriterInput:videoInput
                                  sourcePixelBufferAttributes:nil];
    if (![writer canAddInput:videoInput]) {
      result.error = "cannot configure the video encoder";
      return result;
    }
    [writer addInput:videoInput];
  }

  if (wantAudio) {
    // Decode to linear PCM.
    AudioStreamBasicDescription asbd = {0};
    CMFormatDescriptionRef desc =
        (CMFormatDescriptionRef)audioTrack.formatDescriptions[0];
    const AudioStreamBasicDescription *src =
        (const AudioStreamBasicDescription *)
            CMAudioFormatDescriptionGetStreamBasicDescription(desc);
    const double sampleRate = src != nullptr && src->mSampleRate > 0
                                  ? src->mSampleRate
                                  : 44100.0;
    const int channels =
        src != nullptr && src->mChannelsPerFrame > 0 ? int(src->mChannelsPerFrame)
                                                     : 2;
    const FourCharCode outFormat = audioFormatCode(pipeline.audioCodec);
    if (outFormat == 0) {
      result.error = "AudioToolbox cannot encode this output audio codec";
      return result;
    }
    asbd.mSampleRate = sampleRate;
    asbd.mChannelsPerFrame = UInt32(channels);
    asbd.mFormatID = kAudioFormatLinearPCM;
    asbd.mFormatFlags = kAudioFormatFlagIsFloat |
                        kAudioFormatFlagIsNonInterleaved;
    asbd.mBitsPerChannel = 32;
    asbd.mFramesPerPacket = 1;
    asbd.mBytesPerFrame = 4;
    asbd.mBytesPerPacket = 4;
    NSDictionary *decodeSettings = @{
      AVFormatIDKey : @(kAudioFormatLinearPCM),
      AVLinearPCMBitDepthKey : @32,
      AVLinearPCMIsFloatKey : @YES,
      AVLinearPCMIsNonInterleaved : @YES,
      AVSampleRateKey : @(sampleRate),
      AVNumberOfChannelsKey : @(channels),
    };
    audioOutput = [AVAssetReaderTrackOutput
        trackOutputWithTrack:audioTrack
               outputSettings:decodeSettings];
    if (![reader canAddOutput:audioOutput]) {
      result.error = "cannot decode the audio track";
      return result;
    }
    [reader addOutput:audioOutput];

    const std::int64_t audioBitRate =
        request.audio.bitRate.value_or(std::int64_t(channels) * 64000);
    NSDictionary *audioSettings = @{
      AVFormatIDKey : @(outFormat),
      AVSampleRateKey : @(sampleRate),
      AVNumberOfChannelsKey : @(channels),
      AVEncoderBitRateKey : [NSNumber numberWithLongLong:audioBitRate],
    };
    audioInput = [AVAssetWriterInput
        assetWriterInputWithMediaType:AVMediaTypeAudio
                      outputSettings:audioSettings];
    audioInput.expectsMediaDataInRealTime = NO;
    if (![writer canAddInput:audioInput]) {
      result.error = "cannot configure the audio encoder";
      return result;
    }
    [writer addInput:audioInput];
    (void)asbd;
  }

  if (![reader startReading]) {
    result.error = reader.error != nil
                       ? reader.error.localizedDescription.UTF8String
                       : "cannot start reading";
    return result;
  }
  [writer startWriting];
  [writer startSessionAtSourceTime:asset.startTime];

  // ---- Drive the pipeline ------------------------------------------------------
  __block BOOL videoDone = !wantVideo;
  __block BOOL audioDone = !wantAudio;
  __block std::string failure;

  dispatch_queue_t queue =
      dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0);

  if (wantAudio) {
    [audioInput requestMediaDataWhenReadyOnQueue:queue
                                        usingBlock:^{
      while (audioInput.isReadyForMoreMediaData) {
        if (cancelled(ctx)) {
          [audioInput markAsFinished];
          audioDone = YES;
          return;
        }
        CMSampleBufferRef sample = [audioOutput copyNextSampleBuffer];
        if (sample == NULL) {
          [audioInput markAsFinished];
          audioDone = YES;
          return;
        }
        if (![audioInput appendSampleBuffer:sample]) {
          failure = writer.error != nil
                        ? writer.error.localizedDescription.UTF8String
                        : "audio append failed";
          [audioInput markAsFinished];
          audioDone = YES;
          CFRelease(sample);
          return;
        }
        CFRelease(sample);
      }
    }];
  }
  if (wantVideo) {
    [videoInput requestMediaDataWhenReadyOnQueue:queue
                                       usingBlock:^{
      while (videoInput.isReadyForMoreMediaData) {
        if (cancelled(ctx)) {
          [videoInput markAsFinished];
          videoDone = YES;
          return;
        }
        CMSampleBufferRef sample = [videoOutput copyNextSampleBuffer];
        if (sample == NULL) {
          [videoInput markAsFinished];
          videoDone = YES;
          return;
        }
        CVPixelBufferRef pixels = CMSampleBufferGetImageBuffer(sample);
        const CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample);
        if (pixels != nil &&
            ![videoAdaptor appendPixelBuffer:pixels
                      withPresentationTime:pts]) {
          failure = writer.error != nil
                        ? writer.error.localizedDescription.UTF8String
                        : "video append failed";
          [videoInput markAsFinished];
          videoDone = YES;
          CFRelease(sample);
          return;
        }
        ++result.framesProcessed;
        reportProgress(
            ctx, std::clamp(CMTimeGetSeconds(pts) / totalDurationSec, 0.0,
                            1.0));
        CFRelease(sample);
      }
    }];
  }

  // Wait for both tracks to finish (or a cancel).
  for (;;) {
    if (!failure.empty()) {
      [reader cancelReading];
      [writer cancelWriting];
      result.error = failure;
      return result;
    }
    if (cancelled(ctx)) {
      [reader cancelReading];
      [writer cancelWriting];
      result.error = "canceled";
      return result;
    }
    if (videoDone && audioDone) {
      break;
    }
    [NSThread sleepForTimeInterval:0.005];
  }
  if (![writer finishWriting]) {
    result.error = writer.error != nil
                       ? writer.error.localizedDescription.UTF8String
                       : "finishing the output failed";
    return result;
  }
  NSDictionary *attrs =
      [[NSFileManager defaultManager] attributesOfItemAtPath:outUrl.path
                                                       error:nil];
  if (attrs != nil) {
    result.bytesWritten = [attrs fileSize];
  }
  if (result.framesProcessed == 0 && !wantVideo) {
    result.framesProcessed = 1;
  }
  reportProgress(ctx, 1.0);
  result.ok = true;
  return result;
}

} // namespace velmux::apple::media

#endif // __APPLE__
