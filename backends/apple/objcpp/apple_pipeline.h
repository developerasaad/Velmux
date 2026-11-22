// Apple native pipeline: AVFoundation / VideoToolbox execution.
//
// Everything here compiles only on Apple platforms (__APPLE__), as
// Objective-C++ (.mm). Non-Apple hosts get honest unavailable stubs in
// apple_backend.cpp; the engine then continues with the backends that ARE
// available.
//
// Execution model (spec §32): execution only, never policy. Errors surface
// for the C++ core to decide.
//
// Transcode uses AVAssetReader (decode) -> AVAssetWriter (VideoToolbox
// encode), the standard native Apple pipeline: one pass, real frames, no
// intermediate files. Stream copy uses AVAssetExportSession with the
// passthrough preset.
#pragma once

#include "velmux/core/media_model.h"
#include "velmux/core/pipeline_fwd.h"
#include "velmux/core/job.h"

#include <cstdint>
#include <functional>
#include <string>

namespace velmux::apple::media {

// Capabilities probed from VideoToolbox at engine startup.
struct CodecEntry {
  bool decode = false;
  bool encode = false;
  bool hardware = false;
};

struct NativeSupport {
  std::vector<std::pair<core::VideoCodec, CodecEntry>> video;
  std::vector<std::pair<core::AudioCodec, CodecEntry>> audio;
  std::string summary; // human-readable implementation label
};

// Probes VideoToolbox for real decode/encode support (VTIsHardware*).
// Returns false off Apple platforms (never fabricated).
bool enumerateSupport(NativeSupport &out);

// Demux-level inspection through AVURLAsset.
bool inspectFile(const std::string &path, core::MediaInfo &out,
                 std::string &error);

struct RunContext {
  std::function<void(double)> onProgress; // 0..1, may be null
  std::function<bool()> shouldContinue;   // false = cancel
};

struct RunResult {
  bool ok = false;
  std::string error;
  std::int64_t framesProcessed = 0;
  std::int64_t bytesWritten = 0;
};

// Stream copy through AVAssetExportSession (passthrough preset).
RunResult remuxFile(const std::string &inPath, const std::string &outPath,
                    bool wantVideo, bool wantAudio, const RunContext &ctx);

// Real transcode: AVAssetReader -> AVAssetWriter (VideoToolbox / AAC).
RunResult transcodeFile(const std::string &inPath, const std::string &outPath,
                        const core::Pipeline &pipeline,
                        const core::ConversionRequest &request,
                        const RunContext &ctx);

} // namespace velmux::apple::media
