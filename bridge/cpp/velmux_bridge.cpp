#include "velmux_bridge.h"

#include "velmux/ffmpeg/ffmpeg_backend.h"

#if defined(VELMUX_WITH_ANDROID_BACKEND)
#include "android_backend.h"
#endif
#if defined(VELMUX_WITH_APPLE_BACKEND)
#include "apple_backend.h"
#endif

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace velmux::bridge {

using namespace velmux::core;

std::string jsonEscape(const std::string &text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (unsigned char c : text) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(char(c));
        }
    }
  }
  return out;
}

namespace {

std::string quoted(const std::string &s) {
  return "\"" + jsonEscape(s) + "\"";
}

std::string optionalQuoted(const std::optional<std::string> &s) {
  return s.has_value() ? quoted(*s) : "null";
}

} // namespace

std::string snapshotToJson(const JobSnapshot &snapshot) {
  std::string out = "{";
  out += "\"id\":" + quoted(snapshot.id);
  out += ",\"state\":" + quoted(jobStateName(snapshot.state));
  out += ",\"progress\":" + std::to_string(snapshot.progress);
  out += ",\"sourceName\":" + quoted(snapshot.sourceName);
  out += ",\"outputName\":" + optionalQuoted(snapshot.outputName);
  out += ",\"outputPath\":" + quoted(snapshot.outputPath);
  out += ",\"error\":" +
         (snapshot.error.empty() ? "null" : quoted(snapshot.error));
  out += ",\"outputSizeBytes\":" + std::to_string(snapshot.outputSizeBytes);
  out += ",\"inputSizeBytes\":" + std::to_string(snapshot.inputSizeBytes);
  out += ",\"pipelineSummary\":" +
         (snapshot.pipelineSummary.empty()
              ? "null"
              : quoted(snapshot.pipelineSummary));
  out += "}";
  return out;
}

std::string eventToJson(const JobEvent &event) {
  std::string out = "{";
  out += "\"jobId\":" + quoted(event.jobId);
  out += ",\"state\":" + quoted(jobStateName(event.state));
  out += ",\"progress\":" + std::to_string(event.progress);
  out += ",\"outputSizeBytes\":" + std::to_string(event.outputSizeBytes);
  out += ",\"error\":" +
         (event.error.empty() ? "null" : quoted(event.error));
  out += "}";
  return out;
}

std::string mediaInfoToJson(const MediaInfo &info) {
  std::string out = "{";
  out += "\"durationSec\":" + std::to_string(info.durationSec);
  out += ",\"container\":" + quoted(containerName(info.container));
  out += ",\"sizeBytes\":" + std::to_string(info.sizeBytes);
  out += ",\"video\":[";
  for (std::size_t i = 0; i < info.video.size(); ++i) {
    const VideoStream &v = info.video[i];
    if (i > 0) {
      out += ",";
    }
    out += "{\"codec\":" + quoted(codecName(v.codec));
    out += ",\"width\":" + std::to_string(v.width);
    out += ",\"height\":" + std::to_string(v.height);
    out += ",\"frameRate\":" + std::to_string(v.frameRate);
    out += ",\"bitRate\":" + std::to_string(v.bitRate);
    out += ",\"pixelFormat\":" + quoted(v.pixelFormat);
    out += ",\"hdr\":" + std::string(v.hdr ? "true" : "false");
    out += "}";
  }
  out += "],\"audio\":[";
  for (std::size_t i = 0; i < info.audio.size(); ++i) {
    const AudioStream &a = info.audio[i];
    if (i > 0) {
      out += ",";
    }
    out += "{\"codec\":" + quoted(codecName(a.codec));
    out += ",\"channels\":" + std::to_string(a.channels);
    out += ",\"sampleRate\":" + std::to_string(a.sampleRate);
    out += ",\"bitRate\":" + std::to_string(a.bitRate);
    out += ",\"language\":" +
           (a.language.empty() ? "null" : quoted(a.language));
    out += "}";
  }
  out += "],\"subtitles\":[";
  for (std::size_t i = 0; i < info.subtitles.size(); ++i) {
    const SubtitleStream &s = info.subtitles[i];
    if (i > 0) {
      out += ",";
    }
    out += "{\"format\":" + quoted(subtitleFormatName(s.format));
    out += ",\"language\":" +
           (s.language.empty() ? "null" : quoted(s.language));
    out += ",\"isDefault\":" + std::string(s.isDefault ? "true" : "false");
    out += ",\"isForced\":" + std::string(s.isForced ? "true" : "false");
    out += "}";
  }
  out += "]}";
  return out;
}

} // namespace velmux::bridge

// ---------------------------------------------------------------------------
// C facade
// ---------------------------------------------------------------------------

struct VelmuxEngineHandle {
  std::unique_ptr<velmux::core::VelmuxEngine> engine;
};

using namespace velmux::core;
namespace bridgeNS = velmux::bridge;

extern "C" {

VelmuxEngineHandle *velmux_engine_create(const char *dataDir) {
  if (dataDir == nullptr) {
    return nullptr;
  }
  EngineConfig config;
  config.dataDir = dataDir;
  std::vector<std::unique_ptr<velmux::backend::IBackend>> backends;
  // The FFmpeg backend is always available where the engine links it.
  backends.push_back(std::make_unique<velmux::ffmpeg::FfmpegBackend>());
  // Platform backends register honestly (unavailable on non-matching hosts).
#if defined(VELMUX_WITH_ANDROID_BACKEND)
  backends.push_back(std::make_unique<velmux::android::AndroidBackend>());
#endif
#if defined(VELMUX_WITH_APPLE_BACKEND)
  backends.push_back(std::make_unique<velmux::apple::AppleBackend>());
#endif
  auto engine = VelmuxEngine::create(config, std::move(backends));
  if (engine == nullptr) {
    return nullptr;
  }
  auto *handle = new (std::nothrow) VelmuxEngineHandle();
  if (handle == nullptr) {
    return nullptr;
  }
  handle->engine = std::move(engine);
  return handle;
}

void velmux_engine_destroy(VelmuxEngineHandle *handle) { delete handle; }

char *allocString(const std::string &s) {
  char *buf = static_cast<char *>(std::malloc(s.size() + 1));
  if (buf != nullptr) {
    std::memcpy(buf, s.c_str(), s.size() + 1);
  }
  return buf;
}

char *velmux_inspect(VelmuxEngineHandle *handle, const char *uri,
                     const char *displayName) {
  if (handle == nullptr || uri == nullptr) {
    return nullptr;
  }
  MediaSource source{uri, displayName != nullptr ? displayName : ""};
  const Result<MediaInfo> info = handle->engine->inspect(source);
  if (!info.ok()) {
    return allocString("{\"error\":" + bridgeNS::quoted(info.error().message) +
                       "}");
  }
  return allocString(bridgeNS::mediaInfoToJson(info.value()));
}

char *velmux_list_jobs(VelmuxEngineHandle *handle) {
  if (handle == nullptr) {
    return nullptr;
  }
  std::string out = "[";
  const auto jobs = handle->engine->listJobs();
  for (std::size_t i = 0; i < jobs.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    out += bridgeNS::snapshotToJson(jobs[i]);
  }
  out += "]";
  return allocString(out);
}

char *velmux_get_job(VelmuxEngineHandle *handle, const char *jobId) {
  if (handle == nullptr || jobId == nullptr) {
    return nullptr;
  }
  const auto snapshot = handle->engine->getJob(jobId);
  if (!snapshot.has_value()) {
    return allocString("null");
  }
  return allocString(bridgeNS::snapshotToJson(*snapshot));
}

char *velmux_describe_backends(VelmuxEngineHandle *handle) {
  if (handle == nullptr) {
    return nullptr;
  }
  std::string out = "[";
  const auto descriptions = handle->engine->describeBackends();
  for (std::size_t i = 0; i < descriptions.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    out += bridgeNS::quoted(descriptions[i]);
  }
  out += "]";
  return allocString(out);
}

char *velmux_start_job(VelmuxEngineHandle *handle, const char *requestJson) {
  if (handle == nullptr || requestJson == nullptr) {
    return nullptr;
  }
  // Full JSON request parsing lands with the React Native TurboModule
  // integration (the TS side builds structured requests; the C++ module
  // maps them through this facade). Until that integration the facade
  // reports the gap honestly rather than fabricating success.
  (void)requestJson;
  return allocString("{\"error\":\"job submission via C facade requires the "
                     "React Native bridge integration (not yet wired)\"}");
}

int velmux_cancel_job(VelmuxEngineHandle *handle, const char *jobId) {
  if (handle == nullptr || jobId == nullptr) {
    return -1;
  }
  const Result<bool> result = handle->engine->cancelJob(jobId);
  return result.ok() ? (result.value() ? 1 : 0) : -1;
}

int velmux_pause_job(VelmuxEngineHandle *handle, const char *jobId) {
  if (handle == nullptr || jobId == nullptr) {
    return -1;
  }
  const Result<bool> result = handle->engine->pauseJob(jobId);
  return result.ok() ? (result.value() ? 1 : 0) : -1;
}

int velmux_resume_job(VelmuxEngineHandle *handle, const char *jobId) {
  if (handle == nullptr || jobId == nullptr) {
    return -1;
  }
  const Result<bool> result = handle->engine->resumeJob(jobId);
  return result.ok() ? (result.value() ? 1 : 0) : -1;
}

void velmux_free_string(char *str) { std::free(str); }

} // extern "C"
