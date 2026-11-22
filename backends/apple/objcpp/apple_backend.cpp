// Apple native backend — VideoToolbox / AVFoundation / CoreMedia
// implementation of the Velmux backend contracts.
//
// The real execution lives in apple_pipeline.mm (Objective-C++, compiled
// on Apple platforms only). On other hosts the pipeline symbols resolve to
// the honest unavailable stubs below and the engine does not register this
// backend.
//
// Ownership (docs/BACKEND_POLICY.md): this backend executes native codec
// work; it NEVER decides fallbacks. It reports capabilities and errors; the
// C++ core decides.
#include "apple_backend.h"

#include "velmux/core/feature_registry.h"

#include <algorithm>
#include <chrono>

namespace velmux::apple {

using namespace velmux::core;
using velmux::backend::ExecuteParams;
using velmux::backend::ExecuteResult;

// Resolves a source URI to a local filesystem path. Security-scoped
// bookmarks and file provider URLs need platform-layer resolution; the
// honest answer keeps this backend out of those pipelines until that
// integration lands.
static std::optional<std::string> localPathForUri(const std::string &uri) {
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

Error AppleBackend::initialize() {
  mediaAvailable_ = media::enumerateSupport(support_);
  if (!mediaAvailable_) {
    // Not an error: the engine simply does not register this backend.
    return Error::make(ErrorCode::None,
                       "Apple media APIs not available in this process");
  }
  return Error::make(ErrorCode::None, "");
}

BackendCapabilities AppleBackend::capabilities() const {
  BackendCapabilities caps;
  caps.backend = BackendId::Apple;
  caps.implementation = mediaAvailable_ ? support_.summary : "unavailable";
  caps.canInspect = mediaAvailable_;
  caps.videoScaling = false;    // AVAssetWriter encodes the decoded geometry
  caps.audioResampling = false; // sample rate / channels stay as decoded
  caps.subtitlePassthrough = false; // no text-track passthrough wired yet
  caps.subtitleBurnIn = false;      // libass runs in the FFmpeg backend
  if (!mediaAvailable_) {
    return caps;
  }
  // AVAssetWriter/ExportSession write the QuickTime/mp4 family (plus 3GP).
  for (const Container c :
       {Container::Mp4, Container::M4v, Container::M4a, Container::Mov,
        Container::ThreeGp}) {
    caps.writableContainers.push_back(c);
  }
  // AVURLAsset reads the QuickTime/mp4 family plus the CoreAudio formats.
  for (const Container c :
       {Container::Mp4, Container::M4v, Container::M4a, Container::Mov,
        Container::ThreeGp, Container::Mp3, Container::Wav,
        Container::Flac}) {
    caps.readableContainers.push_back(c);
  }
  const auto &features = FeatureRegistry::instance();
  for (const auto &[codec, entry] : support_.video) {
    const CodecSupport featureSupport = features.videoSupport(codec);
    VideoCapability cap;
    cap.codec = codec;
    cap.support.decode = featureSupport.decode && entry.decode;
    cap.support.encode = featureSupport.encode && entry.encode;
    cap.support.streamCopy = featureSupport.streamCopy;
    cap.hardware = entry.hardware;
    caps.video.push_back(cap);
  }
  for (const auto &[codec, entry] : support_.audio) {
    const CodecSupport featureSupport = features.audioSupport(codec);
    AudioCapability cap;
    cap.codec = codec;
    cap.support.decode = featureSupport.decode && entry.decode;
    cap.support.encode = featureSupport.encode && entry.encode;
    cap.support.streamCopy = featureSupport.streamCopy;
    caps.audio.push_back(cap);
  }
  return caps;
}

bool AppleExecutor::canExecute(const Pipeline &pipeline,
                               const ConversionRequest &request) const {
  if (!backend_->mediaAvailable_) {
    return false;
  }
  const bool videoActive = request.video.enabled;
  const bool audioActive = request.audio.enabled;
  // Pure remux or pure transcode only (mixed copy/transcode has no native
  // path in this milestone).
  if (videoActive && audioActive &&
      pipeline.videoStreamCopy != pipeline.audioStreamCopy) {
    return false;
  }
  // Geometry and rate changes need the FFmpeg path.
  if (videoActive && !pipeline.videoStreamCopy) {
    if (request.video.resolutionHeight.has_value() ||
        request.video.frameRate.has_value()) {
      return false;
    }
  }
  // Audio resampling / downmixing is FFmpeg's job in this milestone.
  if (audioActive && !pipeline.audioStreamCopy) {
    if (request.audio.sampleRate.has_value() ||
        request.audio.channels.has_value()) {
      return false;
    }
  }
  // Subtitle rendering runs through libass (FFmpeg backend); extraction is
  // not implemented anywhere yet (rejected by the planner, D-013).
  if (request.subtitles.policy == SubtitlePolicy::BurnIn ||
      request.subtitles.policy == SubtitlePolicy::Extract) {
    return false;
  }
  switch (pipeline.container) {
  case Container::Mp4:
  case Container::M4v:
  case Container::M4a:
  case Container::Mov:
  case Container::ThreeGp:
    break;
  default:
    return false;
  }
  if (videoActive && !pipeline.videoStreamCopy) {
    const auto *video = findVideo(pipeline.videoCodec);
    if (video == nullptr || !video->encode) {
      return false;
    }
  }
  if (audioActive && !pipeline.audioStreamCopy) {
    const auto *audio = findAudio(pipeline.audioCodec);
    if (audio == nullptr || !audio->encode) {
      return false;
    }
  }
  return true;
}

ExecuteResult AppleExecutor::execute(const ExecuteParams &params) {
  ExecuteResult result;
  const auto started = std::chrono::steady_clock::now();
  const auto inPath = localPathForUri(params.request.source.uri);
  if (!inPath.has_value()) {
    result.error = Error::make(
        ErrorCode::SourceUnavailable,
        "the native path needs a local file (security-scoped bookmark "
        "resolution is a platform-layer integration)",
        "apple");
    return result;
  }
  media::RunContext ctx;
  ctx.onProgress = [&params](double fraction) {
    if (params.onProgress) {
      backend::ProgressReport report;
      report.fraction = fraction;
      params.onProgress(report);
    }
  };
  ctx.shouldContinue = [&params]() {
    return !params.cancel || !params.cancel->isCancelled();
  };
  const bool pureCopy = params.pipeline.videoStreamCopy ||
                        params.pipeline.audioStreamCopy;
  media::RunResult run;
  if (pureCopy) {
    run = media::remuxFile(*inPath, params.outputPath,
                           params.request.video.enabled,
                           params.request.audio.enabled, ctx);
  } else {
    run = media::transcodeFile(*inPath, params.outputPath, params.pipeline,
                               params.request, ctx);
  }
  const auto elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                    started)
          .count();
  result.success = run.ok;
  if (!run.ok) {
    result.error =
        Error::make(run.error == "canceled" ? ErrorCode::Cancelled
                                            : ErrorCode::ExecutionFailed,
                    run.error, "apple");
  }
  result.framesProcessed = run.framesProcessed;
  result.bytesWritten = run.bytesWritten;
  result.wallClockSec = elapsed;
  return result;
}

Error AppleInspector::inspect(const MediaSource &source, MediaInfo &out) {
  const auto path = localPathForUri(source.uri);
  if (!path.has_value()) {
    return Error::make(ErrorCode::InspectionFailed,
                       "the native inspector needs a local file "
                       "(source: " +
                           source.uri + ")",
                       "apple");
  }
  std::string error;
  if (!media::inspectFile(*path, out, error)) {
    return Error::make(ErrorCode::InspectionFailed,
                       error + " (source: " + source.uri + ")", "apple");
  }
  return Error::make(ErrorCode::None, "");
}

} // namespace velmux::apple

// ---------------------------------------------------------------------------
// Host-safe pipeline stubs (non-Apple builds). The engine never registers
// the backend in this state.
// ---------------------------------------------------------------------------
#ifndef __APPLE__

namespace velmux::apple::media {

bool enumerateSupport(NativeSupport &) { return false; }

bool inspectFile(const std::string &, core::MediaInfo &, std::string &error) {
  error = "Apple media APIs are not available in this process";
  return false;
}

RunResult remuxFile(const std::string &, const std::string &, bool, bool,
                    const RunContext &) {
  RunResult result;
  result.error = "Apple media APIs are not available in this process";
  return result;
}

RunResult transcodeFile(const std::string &, const std::string &,
                        const core::Pipeline &,
                        const core::ConversionRequest &,
                        const RunContext &) {
  RunResult result;
  result.error = "Apple media APIs are not available in this process";
  return result;
}

} // namespace velmux::apple::media

#endif // !__APPLE__
