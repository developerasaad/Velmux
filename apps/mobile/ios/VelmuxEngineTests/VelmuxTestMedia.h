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
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <cstdint>

NS_ASSUME_NONNULL_BEGIN

/// A generated fixture: a real MP4 (H.264 video + AAC audio) on disk.
@interface VLMXGeneratedMedia : NSObject

@property (nonatomic, copy, readonly) NSString *path;
@property (nonatomic, assign, readonly) std::int64_t durationUs;
@property (nonatomic, assign, readonly) int width;
@property (nonatomic, assign, readonly) int height;
@property (nonatomic, assign, readonly) int frameCount;
@property (nonatomic, assign, readonly) double frameRate;

- (instancetype)initWithPath:(NSString *)path
                 durationUs:(std::int64_t)durationUs
                       width:(int)width
                      height:(int)height
                  frameCount:(int)frameCount
                   frameRate:(double)frameRate;

@end

/// Independent AVAsset probe of a produced output file. Fields are nil/0
/// when the file does not carry that track (an honest answer, never a
/// guess).
@interface VLMXMediaProbe : NSObject

@property (nullable, nonatomic, copy, readonly) NSString *videoCodec; // e.g. "avc1"
@property (nullable, nonatomic, copy, readonly) NSString *audioCodec; // e.g. "aac "
@property (nonatomic, assign, readonly) std::int64_t durationUs;
@property (nonatomic, assign, readonly) int width;
@property (nonatomic, assign, readonly) int height;

@end

@interface VLMXTestMedia : NSObject

/// Generates a real H.264+AAC MP4 at `path` (overwritten): `frameCount`
/// synthetic gradient frames at `frameRate` plus a stereo 44.1 kHz AAC
/// sine-tone bed spanning the same duration. Returns nil and fills `error`
/// on writer failure.
+ (nullable VLMXGeneratedMedia *)
    generateH264AacMp4AtPath:(NSString *)path
                      width:(int)width
                     height:(int)height
                  frameRate:(double)frameRate
                 frameCount:(int)frameCount
                      error:(NSError **)error;

/// Probes a media file through AVURLAsset (independent of the producer).
+ (nullable VLMXMediaProbe *)probeAtPath:(NSString *)path
                                   error:(NSError **)error;

/// Decodes up to `maxFrames` video frames through a fresh AVAssetReader
/// (independent decode path). Returns the number of frames actually
/// decoded, or -1 when the file cannot be opened for decoding.
+ (int)decodeVideoFramesAtPath:(NSString *)path maxFrames:(int)maxFrames;

/// A unique writable directory under the test's temp space.
+ (NSString *)freshWorkDirectory:(NSString *)label;

/// File size in bytes, or 0 when missing.
+ (long long)fileSizeAtPath:(NSString *)path;

@end

NS_ASSUME_NONNULL_END
