// Android native backend implementation (host-safe skeleton).
//
// On Android, initialize() queries MediaCodecList through JNI (wired in the
// Android app build; see apps/mobile/android). On other hosts the backend
// reports itself unavailable — the engine then continues with the backends
// that ARE available. No fake capability is ever reported.
#include "android_backend.h"

#include <dlfcn.h>

namespace velmux::android {

using namespace velmux::core;
using velmux::backend::ExecuteParams;
using velmux::backend::ExecuteResult;

bool androidMediaAvailable() {
  // libmedia / libmediandk are Android system libraries. On host builds the
  // symbols are absent; dlopen tells the truth.
  void *handle = ::dlopen("libmediandk.so", RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    return false;
  }
  ::dlclose(handle);
  return true;
}

Error AndroidBackend::initialize() {
  mediaAvailable_ = androidMediaAvailable();
  if (!mediaAvailable_) {
    // Not an error: the engine simply does not register this backend.
    return Error::make(ErrorCode::None,
                       "Android media APIs not available in this process");
  }
  // Real initialization (JNI env attach, MediaCodecList query) runs in the
  // Android app process; milestone 1 wires it. Until then the backend is
  // not registered (see capabilities() consumers).
  discoveredSummary_ = "MediaCodec (pending device query)";
  return Error::make(ErrorCode::None, "");
}

BackendCapabilities AndroidBackend::capabilities() const {
  BackendCapabilities caps;
  caps.backend = BackendId::Android;
  caps.implementation = mediaAvailable_ ? discoveredSummary_
                                        : "unavailable";
  caps.canInspect = false; // honest: MediaExtractor inspection not wired yet
  // Codec capability lists are populated from MediaCodecList at Milestone 1.
  // Until then this backend reports nothing so the planner never routes to
  // an executor that cannot run.
  return caps;
}

bool AndroidExecutor::canExecute(const Pipeline &pipeline,
                                 const ConversionRequest &) const {
  (void)pipeline;
  return false; // honest: no execution path compiled yet
}

ExecuteResult AndroidExecutor::execute(const ExecuteParams &params) {
  ExecuteResult result;
  result.error = Error::make(ErrorCode::UnsupportedFeature,
                             "Android execution path is not implemented in "
                             "this build (Milestone 1)",
                             "android");
  return result;
}

Error AndroidInspector::inspect(const MediaSource &source, MediaInfo &) {
  return Error::make(ErrorCode::InspectionFailed,
                     "Android inspection is not implemented in this build "
                     "(source: " + source.uri + ")",
                     "android");
}

} // namespace velmux::android
