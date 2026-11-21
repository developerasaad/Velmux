// Android native backend — MediaCodec / AMediaExtractor / AMediaMuxer
// implementation of the Velmux backend contracts.
//
// STATUS (honest): This is the platform skeleton. The real codec execution
// (Milestone 1) is implemented in the Android build (CMake toolchain +
// JNI), which compiles this file with the NDK. On non-Android hosts the
// factory below reports the backend as unavailable instead of pretending.
//
// Ownership (docs/BACKEND_POLICY.md): this backend executes native codec
// work; it NEVER decides fallbacks. It reports capabilities and errors; the
// C++ core decides.
#pragma once

#include "velmux/backend/contracts.h"

namespace velmux::android {

// True when the process is actually running on Android with the media APIs
// linkable (determined at runtime via dlopen so the core library stays
// loadable everywhere for development).
bool androidMediaAvailable();

class AndroidInspector final : public velmux::backend::IInspector {
public:
  velmux::core::Error inspect(const velmux::core::MediaSource &source,
                              velmux::core::MediaInfo &out) override;
};

class AndroidExecutor final : public velmux::backend::IPipelineExecutor {
public:
  bool canExecute(const velmux::core::Pipeline &pipeline,
                  const velmux::core::ConversionRequest &request) const override;
  velmux::backend::ExecuteResult
  execute(const velmux::backend::ExecuteParams &params) override;
};

class AndroidBackend final : public velmux::backend::IBackend {
public:
  velmux::core::BackendId id() const override {
    return velmux::core::BackendId::Android;
  }
  std::string name() const override { return "Android MediaCodec"; }

  velmux::core::Error initialize() override;
  velmux::core::BackendCapabilities capabilities() const override;

  velmux::backend::IInspector *inspector() override { return &inspector_; }
  velmux::backend::IPipelineExecutor *executor() override { return &executor_; }

private:
  AndroidInspector inspector_;
  AndroidExecutor executor_;
  bool mediaAvailable_ = false;
  // Codec lists discovered during initialize() (MediaCodecList queries).
  std::string discoveredSummary_;
};

} // namespace velmux::android
