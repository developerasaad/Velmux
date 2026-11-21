#include "velmux/core/capability_registry.h"

#include <algorithm>

namespace velmux::core {

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

void CapabilityRegistry::recordFailure(const FailedConfigKey &key) {
  if (!hasFailed(key)) {
    failures_.push_back(key);
  }
}

bool CapabilityRegistry::hasFailed(const FailedConfigKey &key) const {
  return std::find(failures_.begin(), failures_.end(), key) != failures_.end();
}

void CapabilityRegistry::clearFailures(BackendId backend) {
  failures_.erase(std::remove_if(failures_.begin(), failures_.end(),
                                 [backend](const FailedConfigKey &k) {
                                   return k.backend == backend;
                                 }),
                  failures_.end());
}

std::size_t CapabilityRegistry::failureCount() const { return failures_.size(); }

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
