#include "velmux/core/capability_registry.h"

#include <algorithm>
#include <chrono>

namespace velmux::core {

namespace {

// Failure-record cooldown: a recorded configuration failure steers the
// planner away from that route for this long; afterwards the backend gets
// another chance (transient failures must not poison a route forever).
constexpr std::int64_t kFailureCooldownMs = 10 * 60 * 1000;

std::int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

} // namespace

bool FailedConfigKey::operator==(const FailedConfigKey &other) const {
  return backend == other.backend && codec == other.codec &&
         profile == other.profile && width == other.width &&
         height == other.height && pixelFormat == other.pixelFormat;
}

void CapabilityRegistry::registerBackend(BackendCapabilities capabilities) {
  invalidate(capabilities.backend);
  backends_.push_back(std::move(capabilities));
}

void CapabilityRegistry::invalidate(BackendId backend) {
  backends_.erase(std::remove_if(backends_.begin(), backends_.end(),
                                 [backend](const BackendCapabilities &c) {
                                   return c.backend == backend;
                                 }),
                  backends_.end());
  clearFailures(backend);
}

std::vector<BackendId> CapabilityRegistry::backendsFor(VideoCodec codec,
                                                        CodecOp op) const {
  std::vector<BackendId> result;
  for (const auto &backend : backends_) {
    for (const auto &cap : backend.video) {
      if (cap.codec != codec) {
        continue;
      }
      const bool supported = op == CodecOp::Decode    ? cap.support.decode
                             : op == CodecOp::Encode  ? cap.support.encode
                                                      : cap.support.streamCopy;
      if (supported) {
        result.push_back(backend.backend);
        break;
      }
    }
  }
  return result;
}

std::vector<BackendId> CapabilityRegistry::backendsFor(AudioCodec codec,
                                                        CodecOp op) const {
  std::vector<BackendId> result;
  for (const auto &backend : backends_) {
    for (const auto &cap : backend.audio) {
      if (cap.codec != codec) {
        continue;
      }
      const bool supported = op == CodecOp::Decode    ? cap.support.decode
                             : op == CodecOp::Encode  ? cap.support.encode
                                                      : cap.support.streamCopy;
      if (supported) {
        result.push_back(backend.backend);
        break;
      }
    }
  }
  return result;
}

std::vector<BackendId>
CapabilityRegistry::backendsThatRead(Container container) const {
  std::vector<BackendId> result;
  for (const auto &backend : backends_) {
    if (std::find(backend.readableContainers.begin(),
                  backend.readableContainers.end(),
                  container) != backend.readableContainers.end()) {
      result.push_back(backend.backend);
    }
  }
  return result;
}

std::vector<BackendId>
CapabilityRegistry::backendsThatWrite(Container container) const {
  std::vector<BackendId> result;
  for (const auto &backend : backends_) {
    if (std::find(backend.writableContainers.begin(),
                  backend.writableContainers.end(),
                  container) != backend.writableContainers.end()) {
      result.push_back(backend.backend);
    }
  }
  return result;
}

std::optional<VideoCapability>
CapabilityRegistry::videoCapability(BackendId backend,
                                    VideoCodec codec) const {
  for (const auto &b : backends_) {
    if (b.backend != backend) {
      continue;
    }
    for (const auto &cap : b.video) {
      if (cap.codec == codec) {
        return cap;
      }
    }
  }
  return std::nullopt;
}

std::optional<AudioCapability>
CapabilityRegistry::audioCapability(BackendId backend,
                                    AudioCodec codec) const {
  for (const auto &b : backends_) {
    if (b.backend != backend) {
      continue;
    }
    for (const auto &cap : b.audio) {
      if (cap.codec == codec) {
        return cap;
      }
    }
  }
  return std::nullopt;
}

bool CapabilityRegistry::recordIsLive(const FailureRecord &record,
                                      std::int64_t now) {
  return now - record.recordedAtMs < kFailureCooldownMs;
}

void CapabilityRegistry::recordFailure(const FailedConfigKey &key) {
  std::lock_guard<std::mutex> lock(failuresMutex_);
  const std::int64_t now = nowMs();
  // Deduplicate by identity, not by liveness: an expired record for the
  // same key is refreshed in place (a repeat failure restarts the
  // cooldown) rather than accumulating stale rows.
  for (FailureRecord &record : failures_) {
    if (record.key == key) {
      record.recordedAtMs = now;
      return;
    }
  }
  failures_.push_back(FailureRecord{key, now});
}

bool CapabilityRegistry::hasFailed(const FailedConfigKey &key) const {
  std::lock_guard<std::mutex> lock(failuresMutex_);
  const std::int64_t now = nowMs();
  for (const FailureRecord &record : failures_) {
    if (record.key == key && recordIsLive(record, now)) {
      return true;
    }
  }
  return false;
}

void CapabilityRegistry::clearFailures(BackendId backend) {
  std::lock_guard<std::mutex> lock(failuresMutex_);
  failures_.erase(std::remove_if(failures_.begin(), failures_.end(),
                                 [backend](const FailureRecord &r) {
                                   return r.key.backend == backend;
                                 }),
                  failures_.end());
}

std::size_t CapabilityRegistry::failureCount() const {
  std::lock_guard<std::mutex> lock(failuresMutex_);
  // Count live records without mutating: const queries must not prune. The
  // bounded key space keeps the vector small; recordFailure deduplicates
  // and clearFailures/invalidate drop stale rows.
  const std::int64_t now = nowMs();
  std::size_t live = 0;
  for (const FailureRecord &record : failures_) {
    if (recordIsLive(record, now)) {
      ++live;
    }
  }
  return live;
}

std::vector<BackendId> CapabilityRegistry::registeredBackends() const {
  std::vector<BackendId> result;
  result.reserve(backends_.size());
  for (const auto &b : backends_) {
    result.push_back(b.backend);
  }
  return result;
}

const BackendCapabilities *
CapabilityRegistry::capabilitiesOf(BackendId backend) const {
  for (const auto &b : backends_) {
    if (b.backend == backend) {
      return &b;
    }
  }
  return nullptr;
}

} // namespace velmux::core
