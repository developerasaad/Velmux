// Apple native backend implementation (host-safe skeleton).
// The real VideoToolbox/AVFoundation paths compile in the Apple target.
#include "apple_backend.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace velmux::apple {

using namespace velmux::core;
using velmux::backend::ExecuteParams;
using velmux::backend::ExecuteResult;

bool appleMediaAvailable() {
#if defined(__APPLE__)
  return true; // VideoToolbox is present on macOS 10.8+ / iOS 8+
#else
  return false;
#endif
}

Error AppleBackend::initialize() {
  mediaAvailable_ = appleMediaAvailable();
  if (!mediaAvailable_) {
    return Error::make(ErrorCode::None,
                       "Apple media APIs not available in this process");
  }
  return Error::make(ErrorCode::None, "");
}

BackendCapabilities AppleBackend::capabilities() const {
  BackendCapabilities caps;
  caps.backend = BackendId::Apple;
  caps.implementation = mediaAvailable_ ? "VideoToolbox (pending wiring)"
                                        : "unavailable";
  caps.canInspect = false; // honest: AVAsset inspection not wired yet
  // Populated from VTCompressionSession/VTDecompressionSession capability
  // queries when the iOS target lands (Milestone 1+). Until then the
  // planner never routes here.
  return caps;
}

bool AppleExecutor::canExecute(const Pipeline &pipeline,
                               const ConversionRequest &) const {
  (void)pipeline;
  return false; // honest: no execution path compiled yet
}

ExecuteResult AppleExecutor::execute(const ExecuteParams &params) {
  ExecuteResult result;
  result.error = Error::make(ErrorCode::UnsupportedFeature,
                             "Apple execution path is not implemented in "
                             "this build",
                             "apple");
  return result;
}

Error AppleInspector::inspect(const MediaSource &source, MediaInfo &) {
  return Error::make(ErrorCode::InspectionFailed,
                     "Apple inspection is not implemented in this build "
                     "(source: " + source.uri + ")",
                     "apple");
}

} // namespace velmux::apple
