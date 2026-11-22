// FFmpeg backend — inspection + execution using the FFmpeg libraries
// DIRECTLY (libavformat/libavcodec/libavutil/libswscale/libswresample).
// No ffmpeg CLI subprocess is ever spawned (spec §29).
//
// The executor implements two real paths:
//   1. Remux / stream copy: demux -> (bitstream filter when required) -> mux.
//   2. Transcode: demux -> decode -> scale/resample -> encode -> mux.
//
// Subtitle handling: preserve/remove/extract are remux-time stream
// operations; burn-in goes through libavfilter's subtitles filter (libass)
// and is exercised once the mobile artifacts land — the host path implements
// it through the filter graph when the input provides subtitles.
#pragma once

#include "velmux/backend/contracts.h"
#include "velmux/core/types.h"

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
};

class FfmpegExecutor final : public velmux::backend::IPipelineExecutor {
public:
  bool canExecute(const velmux::core::Pipeline &pipeline,
                  const velmux::core::ConversionRequest &request) const override;
  velmux::backend::ExecuteResult
  execute(const velmux::backend::ExecuteParams &params) override;

private:
  velmux::backend::ExecuteResult executeRemux(const velmux::backend::ExecuteParams &params);
  velmux::backend::ExecuteResult
  executeTranscode(const velmux::backend::ExecuteParams &params);
  // Single pass over [rangeStartUs, rangeEndUs) of the source; used directly
  // for whole-file transcodes and once per segment by the segmented path.
  velmux::backend::ExecuteResult
  executeTranscodeRange(const velmux::backend::ExecuteParams &params,
                        std::int64_t rangeStartUs, std::int64_t rangeEndUs,
                        bool rangeActive);
  // Long-job path: transcode in independently finalized, validated and
  // committed segments, then assemble. Resumes from committed artifacts.
  velmux::backend::ExecuteResult
  executeTranscodeSegmented(const velmux::backend::ExecuteParams &params);
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

private:
  FfmpegInspector inspector_;
  FfmpegExecutor executor_;
  std::string implementationLabel_;
};

} // namespace velmux::ffmpeg
