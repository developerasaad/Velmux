// Android MediaCodec pipeline: real AMediaExtractor / AMediaCodec /
// AMediaMuxer execution (Milestone 1).
//
// Everything here compiles only on Android (__ANDROID__). The rest of the
// backend keeps a host-safe surface so the core library still builds and
// tests on Linux/macOS; on those hosts the factory reports the backend as
// unavailable.
//
// Execution model (spec §32): this file is execution only. It never decides
// fallbacks or policy; failures surface as errors for the C++ core.
//
// Video transcode uses the zero-copy surface path: the decoder renders into
// the encoder's input surface (ANativeWindow), so no YUV layout wrangling
// and no intermediate copies. Audio transcodes through ByteBuffer codecs
// (PCM in memory). Stream copy routes extractor samples straight into the
// muxer.
#pragma once

#include <velmux/media.h>
#include "processing/pipeline.h"
#include <velmux/jobs.h>

#include <cstdint>
#include <functional>
#include <string>

namespace velmux::android::media {

// Capabilities enumerated from MediaCodecList at engine startup (JNI).
struct CodecEntry {
  bool decode = false;
  bool encode = false;
  bool hardware = false;
};

struct NativeSupport {
  // Keyed by the Velmux codec enums; only present when the platform knows
  // the mime type at all.
  std::vector<std::pair<core::VideoCodec, CodecEntry>> video;
  std::vector<std::pair<core::AudioCodec, CodecEntry>> audio;
  std::string summary; // human-readable implementation label
};

// JavaVM hook: the app's JNI_OnLoad registers the VM so the backend can
// attach a thread and query MediaCodecList. Lives here (not in the bridge)
// because only the Android backend needs it.
void setJavaVm(void *vm);

// Enumerate MediaCodecList (REGULAR_CODECS): decode/encode + hardware flag
// per mime, mapped onto the Velmux codec enums. Returns false when the JNI
// environment is missing or the query failed (backend then reports itself
// unavailable — never fabricated capabilities).
bool enumerateSupport(NativeSupport &out);

// Demux-level inspection of a local file through AMediaExtractor.
bool inspectFile(const std::string &path, core::MediaInfo &out,
                 std::string &error);

struct RunContext {
  // Progress fraction (0..1) reported periodically; may be null.
  std::function<void(double)> onProgress;
  // Returns true while the run must continue; checked frequently.
  std::function<bool()> shouldContinue;
};

struct RunResult {
  bool ok = false;
  std::string error;
  std::int64_t framesProcessed = 0;
  std::int64_t bytesWritten = 0;
};

// Stream copy: extractor -> muxer, no codecs. Copies the first video and
// first audio track with their formats (rotation preserved through the
// muxer orientation hint).
RunResult remuxFile(const std::string &inPath, const std::string &outPath,
                    bool wantVideo, bool wantAudio, const RunContext &ctx);

// Real transcode. Video: decode(Surface) -> encode through the encoder
// input surface. Audio: ByteBuffer PCM path. Requires the output codecs to
// be H.264-family + AAC-family containers the muxer supports; the backend's
// canExecute already gated on enumerated support, the executor verifies
// against the actual source formats.
RunResult transcodeFile(const std::string &inPath, const std::string &outPath,
                        const core::Pipeline &pipeline,
                        const core::ConversionRequest &request,
                        const RunContext &ctx);

} // namespace velmux::android::media
