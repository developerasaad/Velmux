// FFmpeg backend — inspection + execution using the FFmpeg libraries
// DIRECTLY (libavformat/libavcodec/libavutil/libswscale/libswresample).
// No ffmpeg CLI subprocess is ever spawned (spec §29).
//
// The executor implements two real paths:
//   1. Remux / stream copy: demux -> (bitstream filter when required) -> mux.
//   2. Transcode: demux -> decode -> scale/resample -> encode -> mux.
//
// Execution model: execute() normalizes the typed JobRequest payload into
// an ExecutionIntent (execution_intent.h) at the top — the remux/transcode
// paths consume the intent, never the payload.
//
// Subtitle handling: preserve/remove ride through as remux-time stream
// operations; burn-in goes through libavfilter's subtitles filter (libass)
// for text and a pre-decoded overlay graph for bitmap cues; extraction is
// its own sidecar operation.
#pragma once

#include "processing/contracts/contracts.h"
#include <velmux/types.h>

#include "processing/ffmpeg/execution_intent.h"
#include "processing/ffmpeg/ffmpeg_stages.h"

#include <string>

namespace velmux::ffmpeg {

// Resolve a MediaSource URI to a local filesystem path when it is one.
// Content URIs (Android SAF) are resolved by the platform integration
// layer before reaching this backend; on host they are plain paths.
std::optional<std::string> localPathFor(const velmux::core::MediaSource &source);

class FfmpegInspector final : public velmux::backend::IInspector {
public:
  velmux::core::Error inspect(const velmux::core::MediaSource &source,
                              velmux::core::MediaInfo &out) override;
  bool representativeWindow(const velmux::core::MediaSource &source,
                            std::int32_t seconds, std::int64_t &startUs,
                            std::int64_t &endUs) override;
};

class FfmpegExecutor final : public velmux::backend::IPipelineExecutor {
public:
  bool canExecute(const velmux::core::Pipeline &pipeline,
                  const velmux::core::JobRequest &request) const override;
  velmux::backend::ExecuteResult
  execute(const velmux::backend::ExecuteParams &params) override;

private:
  velmux::backend::ExecuteResult
  executeRemux(const velmux::backend::ExecuteParams &params,
               const ExecutionIntent &intent);
  // Subtitle-extraction job: selected text subtitle track(s) to sidecar
  // files (stream copy for native text formats, decode+re-encode for
  // container-internal text like MP4 mov_text).
  velmux::backend::ExecuteResult
  executeSubtitleExtract(const velmux::backend::ExecuteParams &params);
  velmux::backend::ExecuteResult
  executeTranscode(const velmux::backend::ExecuteParams &params,
                   const ExecutionIntent &intent);
  // Single pass over [rangeStartUs, rangeEndUs) of the source; used directly
  // for whole-file transcodes and once per segment by the segmented path.
  velmux::backend::ExecuteResult
  executeTranscodeRange(const velmux::backend::ExecuteParams &params,
                        const ExecutionIntent &intent,
                        std::int64_t rangeStartUs, std::int64_t rangeEndUs,
                        bool rangeActive, bool isSegmentRun = false);
  // Long-job path: transcode in independently finalized, validated and
  // committed segments, then assemble. Resumes from committed artifacts.
  velmux::backend::ExecuteResult
  executeTranscodeSegmented(const velmux::backend::ExecuteParams &params,
                            const ExecutionIntent &intent);
  // Timeline operations (mandate §27/§29).
  // Multi-range Cut: retained intervals as keyframe-safe copy windows or
  // exact transcode windows, assembled into ONE continuous output.
  velmux::backend::ExecuteResult
  executeCut(const velmux::backend::ExecuteParams &params,
             const ExecutionIntent &intent);
  // Split: six boundary strategies, per-part artifacts with checkpoints.
  velmux::backend::ExecuteResult
  executeSplit(const velmux::backend::ExecuteParams &params,
               const ExecutionIntent &intent);
  // Merge: sequential (compatibility-verified concat / normalization) and
  // spatial (side-by-side / top-bottom composition).
  velmux::backend::ExecuteResult
  executeMerge(const velmux::backend::ExecuteParams &params,
               const ExecutionIntent &intent);
  // Spatial composition (mandate §27.5): exactly two sources.
  velmux::backend::ExecuteResult
  executeMergeSpatial(const velmux::backend::ExecuteParams &params,
                      const ExecutionIntent &intent);
  // Extract Frames: eight selection modes, JPEG/PNG/WebP images + manifest.
  velmux::backend::ExecuteResult
  executeExtractFrames(const velmux::backend::ExecuteParams &params,
                       const ExecutionIntent &intent);
};

class FfmpegBackend final : public velmux::backend::IBackend {
public:
  velmux::core::BackendId id() const override {
    return velmux::core::BackendId::Ffmpeg;
  }
  std::string name() const override { return "FFmpeg"; }

  velmux::core::Error initialize() override;
  velmux::core::BackendCapabilities capabilities() const override;

  velmux::backend::IInspector *inspector() override { return &inspector_; }
  velmux::backend::IPipelineExecutor *executor() override { return &executor_; }
  // Milestone 4: every FFmpeg stage participates in hybrid pipelines.
  velmux::backend::IStageBackend *stages() override { return &stageBackend_; }

private:
  FfmpegInspector inspector_;
  FfmpegExecutor executor_;
  velmux::ffmpeg::FfmpegStageBackend stageBackend_;
  std::string implementationLabel_;
  // Probed at initialize() from the real avfilter registry (see
  // initialize()); capabilities().hdrToneMapping reports it.
  bool hdrToneMappingAvailable_ = false;
};

} // namespace velmux::ffmpeg
