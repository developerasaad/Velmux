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

const char *jobStateName(JobState state) {
  switch (state) {
    case JobState::Queued:
      return "queued";
    case JobState::Preparing:
      return "preparing";
    case JobState::Running:
      return "running";
    case JobState::Checkpointing:
      return "checkpointing";
    case JobState::Paused:
      return "paused";
    case JobState::Recovering:
      return "recovering";
    case JobState::Interrupted:
      return "interrupted";
    case JobState::Failed:
      return "failed";
    case JobState::Canceled:
      return "canceled";
    case JobState::Validating:
      return "validating";
    case JobState::Completed:
      return "completed";
  }
  return "unknown";
}

std::optional<JobState> jobStateFromName(const std::string &name) {
  for (std::uint8_t i = 0; i <= std::uint8_t(JobState::Completed); ++i) {
    const JobState state = JobState(i);
    if (name == jobStateName(state)) {
      return state;
    }
  }
  return std::nullopt;
}

} // namespace velmux::core
