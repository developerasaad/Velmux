// Velmux core — events.
//
// Normalized job events emitted by the core and consumed by the React
// Native bridge (JS events), Android Kotlin notification host and iOS
// Swift notification host. The core never renders notifications itself.
#pragma once

#include "velmux/core/job.h"
#include "velmux/core/types.h"

#include <functional>
#include <vector>

namespace velmux::core {

struct JobEvent {
  std::string jobId;
  JobState state = JobState::Queued;
  double progress = -1.0; // 0..1 or negative when unknown
  std::int64_t outputSizeBytes = -1;
  std::string error; // populated on failure
};

// Sinks must be cheap: the emitter does not wait. UI-level coalescing
// happens at the bridge; native notifiers apply their own rate limits.
class IJobEventSink {
public:
  virtual ~IJobEventSink() = default;
  virtual void onJobEvent(const JobEvent &event) = 0;
};

class EventBus {
public:
  void subscribe(IJobEventSink *sink);
  void unsubscribe(IJobEventSink *sink);
  void emit(const JobEvent &event);

private:
  std::vector<IJobEventSink *> sinks_;
};

} // namespace velmux::core
