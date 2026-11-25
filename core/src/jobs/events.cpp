#include <velmux/events.h>

#include <algorithm>

namespace velmux::core {

void EventBus::subscribe(IJobEventSink *sink) {
  if (sink == nullptr) {
    return;
  }
  if (std::find(sinks_.begin(), sinks_.end(), sink) == sinks_.end()) {
    sinks_.push_back(sink);
  }
}

void EventBus::unsubscribe(IJobEventSink *sink) {
  sinks_.erase(std::remove(sinks_.begin(), sinks_.end(), sink), sinks_.end());
}

void EventBus::emit(const JobEvent &event) {
  for (IJobEventSink *sink : sinks_) {
    sink->onJobEvent(event);
  }
}

} // namespace velmux::core
