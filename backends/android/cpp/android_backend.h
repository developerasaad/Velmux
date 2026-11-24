// Android native backend — MediaCodec / AMediaExtractor / AMediaMuxer
// implementation of the Velmux backend contracts (Milestone 1).
//
// The real codec execution lives in media_transcoder.cpp (compiled for the
// Android NDK; host builds compile to honest unavailable stubs). This file
// implements the backend contracts on top of it.
//
// Ownership (docs/BACKEND_POLICY.md): this backend executes native codec
// work; it NEVER decides fallbacks. It reports capabilities and errors; the
// C++ core decides.
#pragma once

#include "android_stages.h"
#include "media_transcoder.h"
#include "velmux/backend/contracts.h"

namespace velmux::android {

class AndroidBackend;

// Registers the process JavaVM for MediaCodecList enumeration. Called from
// the host app's JNI_OnLoad (extern "C" for direct linkage inside the
// single engine library).
extern "C" void velmux_android_set_java_vm(void *vm);

class AndroidInspector final : public velmux::backend::IInspector {
public:
  velmux::core::Error inspect(const velmux::core::MediaSource &source,
                              velmux::core::MediaInfo &out) override;
};

class AndroidExecutor final : public velmux::backend::IPipelineExecutor {
public:
  explicit AndroidExecutor(const AndroidBackend *backend) : backend_(backend) {}

  bool canExecute(const velmux::core::Pipeline &pipeline,
                  const velmux::core::ConversionRequest &request) const override;
  velmux::backend::ExecuteResult
  execute(const velmux::backend::ExecuteParams &params) override;

private:
  const media::CodecEntry *findVideo(
      velmux::core::VideoCodec codec) const;
  const media::CodecEntry *findAudio(
      velmux::core::AudioCodec codec) const;

  const AndroidBackend *backend_;
};

class AndroidBackend final : public velmux::backend::IBackend {
public:
  AndroidBackend() : executor_(this) {}

  velmux::core::BackendId id() const override {
    return velmux::core::BackendId::Android;
  }
  std::string name() const override { return "Android MediaCodec"; }

  velmux::core::Error initialize() override;
  velmux::core::BackendCapabilities capabilities() const override;

  velmux::backend::IInspector *inspector() override { return &inspector_; }
  velmux::backend::IPipelineExecutor *executor() override { return &executor_; }
  // Milestone 4: the Android stage workers (android_stages.cpp) decode to
  // Yuv420P CPU frames, encode from CPU frames and mux — the backend
  // participates in hybrid (stage-composed) pipelines through them.
  velmux::backend::IStageBackend *stages() override {
    return mediaAvailable_ ? &stageBackend_ : nullptr;
  }

private:
  AndroidInspector inspector_;
  AndroidExecutor executor_;
  media::AndroidStageBackend stageBackend_;
  bool mediaAvailable_ = false;
  media::NativeSupport support_;

  friend class AndroidExecutor;
};

inline const media::CodecEntry *
AndroidExecutor::findVideo(velmux::core::VideoCodec codec) const {
  for (const auto &[candidate, entry] : backend_->support_.video) {
    if (candidate == codec) {
      return &entry;
    }
  }
  return nullptr;
}

inline const media::CodecEntry *
AndroidExecutor::findAudio(velmux::core::AudioCodec codec) const {
  for (const auto &[candidate, entry] : backend_->support_.audio) {
    if (candidate == codec) {
      return &entry;
    }
  }
  return nullptr;
}

} // namespace velmux::android
