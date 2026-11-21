// Apple native backend — VideoToolbox / AVFoundation / CoreMedia
// implementation of the Velmux backend contracts.
//
// STATUS (honest): platform skeleton. The real implementation compiles as
// Objective-C++ in the iOS app target (Xcode/Mac hosts only; Linux CI
// compiles only the core + FFmpeg backend). This file provides the contract
// surface for the shared C++ core and reports the backend unavailable
// rather than pretending.
#pragma once

#include "velmux/backend/contracts.h"

namespace velmux::apple {

// True when running on Darwin with VideoToolbox available.
bool appleMediaAvailable();

class AppleInspector final : public velmux::backend::IInspector {
public:
  velmux::core::Error inspect(const velmux::core::MediaSource &source,
                              velmux::core::MediaInfo &out) override;
};

class AppleExecutor final : public velmux::backend::IPipelineExecutor {
public:
  bool canExecute(const velmux::core::Pipeline &pipeline,
                  const velmux::core::ConversionRequest &request) const override;
  velmux::backend::ExecuteResult
  execute(const velmux::backend::ExecuteParams &params) override;
};

class AppleBackend final : public velmux::backend::IBackend {
public:
  velmux::core::BackendId id() const override {
    return velmux::core::BackendId::Apple;
  }
  std::string name() const override { return "Apple VideoToolbox"; }

  velmux::core::Error initialize() override;
  velmux::core::BackendCapabilities capabilities() const override;

  velmux::backend::IInspector *inspector() override { return &inspector_; }
  velmux::backend::IPipelineExecutor *executor() override { return &executor_; }

private:
  AppleInspector inspector_;
  AppleExecutor executor_;
  bool mediaAvailable_ = false;
};

} // namespace velmux::apple
