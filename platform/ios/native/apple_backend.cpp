// Apple native backend — VideoToolbox / AVFoundation / CoreMedia
// implementation of the Velmux backend contracts.
//
// The real execution lives in apple_pipeline.mm and apple_stages.mm
// (Objective-C++, compiled on Apple platforms only). On other hosts the
// pipeline symbols resolve to the honest unavailable stubs below and the
// engine does not register this backend.
//
// Ownership (docs/BACKEND_POLICY.md): this backend executes native codec
// work; it NEVER decides fallbacks. It reports capabilities and errors; the
// C++ core decides.
#include "apple_backend.h"

#include "catalog/features.h"

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
  // Milestone 4: the Apple stage workers decode to planar Yuv420P through
  // AVAssetReader and encode/mux from CPU frames through VideoToolbox +
  // AVAssetWriter (apple_stages.mm). Reported only when the media stack
  // actually initialized — non-Apple hosts never claim participation.
  caps.stageDecodeToCpuFrames = true;
  caps.stageEncodeFromCpuFrames = true;
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

namespace {

// The request shape the native path can serve: the conversion-family
// payloads (VideoConvert / AudioConvert / ExtractAudio) reduced to the
// fields the native gates consumed. nullopt = an operation or field this
// backend refuses (the planner routes those elsewhere).
struct NativeShape {
  bool videoActive = false;
  bool audioActive = false;
  bool trimActive = false;
  bool geometryRateChange = false;
  bool audioFormatChange = false;
};

std::optional<NativeShape> nativeShapeOf(const JobRequest &request) {
  NativeShape shape;
  const auto &payload = request.payload;
  if (const auto *vc = std::get_if<job::VideoConvert>(&payload)) {
    shape.videoActive = true;
    shape.audioActive = true;
    // Every audio rule a removal (no external, no replacement) drops the
    // audio lane entirely.
    if (!vc->audioTracks.empty()) {
      const bool allRemoved = std::all_of(
          vc->audioTracks.begin(), vc->audioTracks.end(),
          [](const AudioTrackRule &rule) {
            return !rule.include &&
                   !(rule.externalUri.has_value() && !rule.externalUri->empty());
          });
      if (allRemoved) {
        shape.audioActive = false;
      }
    }
    shape.geometryRateChange =
        vc->video.resolutionHeight.has_value() ||
        vc->video.customWidth.has_value() ||
        vc->video.customHeight.has_value() || vc->video.frameRate.has_value();
    shape.audioFormatChange = vc->audioSettings.sampleRate.has_value() ||
                              vc->audioSettings.channels.has_value();
    // Subtitle work: none of it rides the native path (libass burn-in and
    // text-track passthrough are FFmpeg's). A BurnIn rule is an explicit
    // refusal.
    if (std::any_of(vc->subtitleTracks.begin(), vc->subtitleTracks.end(),
                    [](const SubtitleTrackRule &rule) {
                      return rule.policy == SubtitleTrackPolicy::BurnIn;
                    })) {
      return std::nullopt;
    }
  } else if (const auto *ac = std::get_if<job::AudioConvert>(&payload)) {
    shape.audioActive = true;
    shape.audioFormatChange =
        ac->audio.sampleRate.has_value() || ac->audio.channels.has_value();
  } else if (const auto *ea = std::get_if<job::ExtractAudio>(&payload)) {
    shape.audioActive = true;
    // The native reader keeps the first/default audio track only.
    if (!ea->trackIndices.empty() &&
        (ea->trackIndices.size() > 1 || ea->trackIndices.front() != 0)) {
      return std::nullopt;
    }
    shape.trimActive = ea->range.has_value() &&
                       (ea->range->startUs != 0 || ea->range->endUs >= 0);
    shape.audioFormatChange =
        ea->audio.sampleRate.has_value() || ea->audio.channels.has_value();
  } else {
    // Crop/Trim/Cut/Split/Merge/ExtractFrames/ExtractSubtitles and every
    // other operation: not native-path work.
    return std::nullopt;
  }
  return shape;
}

} // namespace

bool AppleExecutor::canExecute(const Pipeline &pipeline,
                               const JobRequest &request) const {
  if (!backend_->mediaAvailable_) {
    return false;
  }
  const std::optional<NativeShape> shapeOpt = nativeShapeOf(request);
  if (!shapeOpt.has_value()) {
    return false; // operation (or explicit field) this path refuses
  }
  const NativeShape &shape = *shapeOpt;
  const bool videoActive = shape.videoActive;
  const bool audioActive = shape.audioActive;
  // Pure remux or pure transcode only (mixed copy/transcode has no native
  // path in this milestone).
  if (videoActive && audioActive &&
      pipeline.videoStreamCopy != pipeline.audioStreamCopy) {
    return false;
  }
  // Geometry and rate changes need the FFmpeg path.
  if (videoActive && !pipeline.videoStreamCopy && shape.geometryRateChange) {
    return false;
  }
  // Trim is not implemented on the native path; a trimmed request must
  // never execute here — it would silently produce full-length output.
  if (shape.trimActive) {
    return false;
  }
  // Audio resampling / downmixing is FFmpeg's job in this milestone.
  if (audioActive && !pipeline.audioStreamCopy && shape.audioFormatChange) {
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
  const std::optional<NativeShape> shapeOpt =
      nativeShapeOf(params.request);
  if (!shapeOpt.has_value()) {
    result.error = Error::make(
        ErrorCode::UnsupportedOperation,
        "the native path serves conversion-family requests only", "apple");
    return result;
  }
  const NativeShape &shape = *shapeOpt;
  const MediaSource source = std::visit(
      [](const auto &payload) {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, job::Merge>) {
          return payload.sources.empty() ? MediaSource{}
                                         : payload.sources.front();
        } else {
          return payload.source;
        }
      },
      params.request.payload);
  const auto inPath = localPathForUri(source.uri);
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
  media::NativeTranscodeSpec spec;
  spec.wantVideo = shape.videoActive;
  spec.wantAudio = shape.audioActive;
  // Rate control resolved from the compression intent (mandate §20), the
  // same derivation the hybrid executor applies (no inspection data on
  // this path; TargetSize budgets stay on whole-pipeline routes).
  const CompressionIntent *compression = std::visit(
      [](const auto &payload) -> const CompressionIntent * {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, job::VideoConvert> ||
                      std::is_same_v<Payload, job::AudioConvert> ||
                      std::is_same_v<Payload, job::ExtractAudio>) {
          return &payload.compression;
        } else {
          return nullptr;
        }
      },
      params.request.payload);
  spec.videoQuality = 65; // disabled: high-fidelity default
  if (compression != nullptr && compression->enabled) {
    switch (compression->preset) {
      case CompressionPreset::HighQuality:
        spec.videoQuality = 85;
        break;
      case CompressionPreset::Balanced:
      case CompressionPreset::TargetSize:
        spec.videoQuality = 60;
        break;
      case CompressionPreset::SmallerFile:
        spec.videoQuality = 35;
        break;
      case CompressionPreset::Custom:
        if (compression->videoBitrate.has_value() &&
            *compression->videoBitrate > 0) {
          spec.videoBitRate = *compression->videoBitrate;
        } else {
          spec.videoQuality =
              std::clamp(compression->quality.value_or(60), 1, 100);
        }
        break;
    }
    if (compression->audioBitrate.has_value() &&
        *compression->audioBitrate > 0) {
      spec.audioBitRate = *compression->audioBitrate;
    }
  }
  // Explicit audio settings still win (AudioSettings.bitRate is the
  // user-facing audio bitrate surface).
  std::visit(
      [&](const auto &payload) {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, job::VideoConvert>) {
          if (payload.audioSettings.bitRate.has_value()) {
            spec.audioBitRate = *payload.audioSettings.bitRate;
          }
        } else if constexpr (std::is_same_v<Payload, job::AudioConvert> ||
                             std::is_same_v<Payload, job::ExtractAudio>) {
          if (payload.audio.bitRate.has_value()) {
            spec.audioBitRate = *payload.audio.bitRate;
          }
        }
      },
      params.request.payload);
  const bool pureCopy = params.pipeline.videoStreamCopy ||
                        params.pipeline.audioStreamCopy;
  media::RunResult run;
  if (pureCopy) {
    run = media::remuxFile(*inPath, params.outputPath, spec.wantVideo,
                           spec.wantAudio, ctx);
  } else {
    run = media::transcodeFile(*inPath, params.outputPath, params.pipeline,
                               spec, ctx);
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
                        const NativeTranscodeSpec &,
                        const RunContext &) {
  RunResult result;
  result.error = "Apple media APIs are not available in this process";
  return result;
}

// Stage workers (Milestone 4): honest null factories off Apple platforms.
// The declarations stay valid (apple_backend.h holds the IStageBackend
// member on every host); the backend never claims stage participation
// there, so the hybrid executor never asks for these.

backend::IVideoStageDecoder *AppleStageBackend::createVideoDecoder() {
  return nullptr;
}

backend::IVideoStageEncoder *AppleStageBackend::createVideoEncoder() {
  return nullptr;
}

backend::IMuxStage *AppleStageBackend::createMux() { return nullptr; }

} // namespace velmux::apple::media

#endif // !__APPLE__
