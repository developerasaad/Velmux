// Apple native backend — VideoToolbox / AVFoundation / CoreMedia
// implementation of the Velmux backend contracts.
//
// The real execution lives in apple_pipeline.mm and apple_stages.mm
// (Objective-C++, Apple platforms only). Non-Apple hosts resolve the
// pipeline symbols to honest unavailable stubs (apple_backend.cpp) and
// the engine does not register this backend.
//
// Ownership (docs/BACKEND_POLICY.md): this backend executes native codec
// work; it NEVER decides fallbacks. It reports capabilities and errors; the
// C++ core decides.
#pragma once

#include "apple_pipeline.h"
#include "apple_stages.h"
#include "processing/contracts/contracts.h"

namespace velmux::apple {

class AppleBackend;

class AppleInspector final : public velmux::backend::IInspector {
public:
  velmux::core::Error inspect(const velmux::core::MediaSource &source,
                              velmux::core::MediaInfo &out) override;
};

class AppleExecutor final : public velmux::backend::IPipelineExecutor {
public:
  explicit AppleExecutor(const AppleBackend *backend) : backend_(backend) {}

  bool canExecute(const velmux::core::Pipeline &pipeline,
                  const velmux::core::JobRequest &request) const override;
  velmux::backend::ExecuteResult
  execute(const velmux::backend::ExecuteParams &params) override;

private:
  const media::CodecEntry *findVideo(
      velmux::core::VideoCodec codec) const;
  const media::CodecEntry *findAudio(
      velmux::core::AudioCodec codec) const;

  const AppleBackend *backend_;
};

class AppleBackend final : public velmux::backend::IBackend {
public:
  AppleBackend() : executor_(this) {}

  velmux::core::BackendId id() const override {
    return velmux::core::BackendId::Apple;
  }
  std::string name() const override { return "Apple VideoToolbox"; }

  velmux::core::Error initialize() override;
  velmux::core::BackendCapabilities capabilities() const override;

  velmux::backend::IInspector *inspector() override { return &inspector_; }
  velmux::backend::IPipelineExecutor *executor() override { return &executor_; }
  // Milestone 4: stage workers for hybrid (stage-composed) pipelines.
  // Real factories on Apple platforms (apple_stages.mm); honest nulls
  // elsewhere (apple_backend.cpp) — and stage participation is only
  // reported in the capabilities when the media stack initialized.
  velmux::backend::IStageBackend *stages() override { return &stageBackend_; }

private:
  AppleInspector inspector_;
  AppleExecutor executor_;
  media::AppleStageBackend stageBackend_;
  bool mediaAvailable_ = false;
  media::NativeSupport support_;

  friend class AppleExecutor;
};

inline const media::CodecEntry *
AppleExecutor::findVideo(velmux::core::VideoCodec codec) const {
  for (const auto &[candidate, entry] : backend_->support_.video) {
    if (candidate == codec) {
      return &entry;
    }
  }
  return nullptr;
}

inline const media::CodecEntry *
AppleExecutor::findAudio(velmux::core::AudioCodec codec) const {
  for (const auto &[candidate, entry] : backend_->support_.audio) {
    if (candidate == codec) {
      return &entry;
    }
  }
  return nullptr;
}

} // namespace velmux::apple
