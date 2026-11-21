#include "velmux_bridge.h"

#if defined(VELMUX_WITH_FFMPEG_BACKEND)
#include "velmux/ffmpeg/ffmpeg_backend.h"
#endif

#if defined(VELMUX_WITH_ANDROID_BACKEND)
#include "android_backend.h"
#endif
#if defined(VELMUX_WITH_APPLE_BACKEND)
#include "apple_backend.h"
#endif

#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <optional>
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

// ---------------------------------------------------------------------------
// Minimal JSON reader for the bridge request contract.
//
// Scope: objects, arrays, strings, integers, doubles, booleans and null —
// exactly what the bridge contract needs, with defensive limits (input is
// untrusted host input). Surrogate pairs are decoded for \u escapes.
// ---------------------------------------------------------------------------

struct JsonError {
  std::string message;
};

// Minimal variant value for the bridge JSON contract.
struct JsonValue {
  enum class Kind { Null, Bool, Number, Int, String, Array, Object } kind =
      Kind::Null;
  bool boolValue = false;
  double number = 0.0;
  std::int64_t integer = 0;
  std::string text;
  std::vector<JsonValue> items;
  std::vector<std::pair<std::string, JsonValue>> fields;

  const JsonValue *find(const char *key) const {
    for (const auto &field : fields) {
      if (field.first == key) {
        return &field.second;
      }
    }
    return nullptr;
  }
  bool isNumberLike() const {
    return kind == Kind::Number || kind == Kind::Int;
  }
  double asDouble() const { return kind == Kind::Int ? double(integer)
                                                       : number; }
  std::int64_t asInt() const {
    return kind == Kind::Int ? integer : std::int64_t(number);
  }
};

class JsonReader {
public:
  explicit JsonReader(const std::string &text) : text_(text) {}

  // Parses one value; returns empty string on success, error text otherwise.
  std::string parse(JsonValue &out) {
    depth_ = 0;
    skipWs();
    std::string error = parseValue(out);
    if (error.empty()) {
      skipWs();
      if (pos_ != text_.size()) {
        return "trailing characters after JSON value";
      }
    }
    return error;
  }

private:

  void skipWs() {
    while (pos_ < text_.size()) {
      char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  bool consume(char c) {
    if (pos_ < text_.size() && text_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  std::string parseValue(JsonValue &out) {
    if (depth_ > 32) {
      return "nesting too deep";
    }
    skipWs();
    if (pos_ >= text_.size()) {
      return "unexpected end of input";
    }
    const char c = text_[pos_];
    if (c == '{') {
      return parseObject(out);
    }
    if (c == '[') {
      return parseArray(out);
    }
    if (c == '"') {
      out.kind = JsonValue::Kind::String;
      return parseString(out.text);
    }
    if (c == 't' || c == 'f') {
      return parseBool(out);
    }
    if (c == 'n') {
      if (text_.compare(pos_, 4, "null") == 0) {
        pos_ += 4;
        out.kind = JsonValue::Kind::Null;
        return "";
      }
      return "invalid literal";
    }
    return parseNumber(out);
  }

  std::string parseObject(JsonValue &out) {
    out.kind = JsonValue::Kind::Object;
    ++pos_; // '{'
    ++depth_;
    skipWs();
    if (consume('}')) {
      --depth_;
      return "";
    }
    for (;;) {
      skipWs();
      JsonValue key;
      key.kind = JsonValue::Kind::String;
      std::string error = parseString(key.text);
      if (!error.empty()) {
        return error;
      }
      skipWs();
      if (!consume(':')) {
        return "expected ':'";
      }
      JsonValue value;
      error = parseValue(value);
      if (!error.empty()) {
        return error;
      }
      out.fields.emplace_back(std::move(key.text), std::move(value));
      skipWs();
      if (consume(',')) {
        continue;
      }
      if (consume('}')) {
        --depth_;
        return "";
      }
      return "expected ',' or '}'";
    }
  }

  std::string parseArray(JsonValue &out) {
    out.kind = JsonValue::Kind::Array;
    ++pos_; // '['
    ++depth_;
    skipWs();
    if (consume(']')) {
      --depth_;
      return "";
    }
    for (;;) {
      JsonValue value;
      std::string error = parseValue(value);
      if (!error.empty()) {
        return error;
      }
      out.items.push_back(std::move(value));
      skipWs();
      if (consume(',')) {
        continue;
      }
      if (consume(']')) {
        --depth_;
        return "";
      }
      return "expected ',' or ']'";
    }
  }

  std::string parseString(std::string &out) {
    if (!consume('"')) {
      return "expected string";
    }
    out.clear();
    while (pos_ < text_.size()) {
      const char c = text_[pos_++];
      if (c == '"') {
        return "";
      }
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos_ >= text_.size()) {
        return "unterminated escape";
      }
      const char esc = text_[pos_++];
      switch (esc) {
        case '"':
        case '\\':
        case '/':
          out.push_back(esc);
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u': {
          if (pos_ + 4 > text_.size()) {
            return "truncated \\u escape";
          }
          unsigned cp = 0;
          for (int i = 0; i < 4; ++i) {
            const char h = text_[pos_++];
            cp <<= 4;
            if (h >= '0' && h <= '9') {
              cp |= unsigned(h - '0');
            } else if (h >= 'a' && h <= 'f') {
              cp |= unsigned(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
              cp |= unsigned(h - 'A' + 10);
            } else {
              return "invalid \\u escape";
            }
          }
          // Surrogate pair handling.
          if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= text_.size() &&
              text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
            unsigned lo = 0;
            size_t back = pos_;
            pos_ += 2;
            bool ok = true;
            for (int i = 0; i < 4; ++i) {
              const char h = text_[pos_++];
              lo <<= 4;
              if (h >= '0' && h <= '9') {
                lo |= unsigned(h - '0');
              } else if (h >= 'a' && h <= 'f') {
                lo |= unsigned(h - 'a' + 10);
              } else if (h >= 'A' && h <= 'F') {
                lo |= unsigned(h - 'A' + 10);
              } else {
                ok = false;
                break;
              }
            }
            if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else {
              pos_ = back; // lone surrogate; keep replacement
              cp = 0xFFFD;
            }
          } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD;
          }
          appendUtf8(out, cp);
          break;
        }
        default:
          return "invalid escape";
      }
    }
    return "unterminated string";
  }

  static void appendUtf8(std::string &out, unsigned cp) {
    if (cp < 0x80) {
      out.push_back(char(cp));
    } else if (cp < 0x800) {
      out.push_back(char(0xC0 | (cp >> 6)));
      out.push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(char(0xE0 | (cp >> 12)));
      out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(char(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(char(0xF0 | (cp >> 18)));
      out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(char(0x80 | (cp & 0x3F)));
    }
  }

  std::string parseBool(JsonValue &out) {
    if (text_.compare(pos_, 4, "true") == 0) {
      pos_ += 4;
      out.kind = JsonValue::Kind::Bool;
      out.boolValue = true;
      return "";
    }
    if (text_.compare(pos_, 5, "false") == 0) {
      pos_ += 5;
      out.kind = JsonValue::Kind::Bool;
      out.boolValue = false;
      return "";
    }
    return "invalid literal";
  }

  std::string parseNumber(JsonValue &out) {
    const std::size_t start = pos_;
    if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) {
      ++pos_;
    }
    bool isDouble = false;
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c >= '0' && c <= '9') {
        ++pos_;
      } else if (c == '.' || c == 'e' || c == 'E' || c == '-' || c == '+') {
        isDouble = isDouble || c == '.' || c == 'e' || c == 'E';
        ++pos_;
      } else {
        break;
      }
    }
    if (pos_ == start) {
      return "expected number";
    }
    const std::string token = text_.substr(start, pos_ - start);
    if (!isDouble) {
      errno = 0;
      char *end = nullptr;
      const long long v = std::strtoll(token.c_str(), &end, 10);
      if (errno == 0 && end != nullptr && *end == '\0') {
        out.kind = JsonValue::Kind::Int;
        out.integer = v;
        return "";
      }
    }
    errno = 0;
    char *end = nullptr;
    const double d = std::strtod(token.c_str(), &end);
    if (errno != 0 || end == nullptr || *end != '\0') {
      return "invalid number";
    }
    out.kind = JsonValue::Kind::Number;
    out.number = d;
    return "";
  }

  const std::string &text_;
  std::size_t pos_ = 0;
  int depth_ = 0;
};

// -- typed extraction helpers ------------------------------------------------

std::string getString(const JsonValue &v, const char *key,
                      std::string &error) {
  const JsonValue *field = v.find(key);
  if (field == nullptr) {
    error = std::string("missing field: ") + key;
    return "";
  }
  if (field->kind != JsonValue::Kind::String) {
    error = std::string("field is not a string: ") + key;
    return "";
  }
  return field->text;
}

std::optional<std::string> getOptString(const JsonValue &v, const char *key) {
  const JsonValue *field = v.find(key);
  if (field == nullptr || field->kind == JsonValue::Kind::Null) {
    return std::nullopt;
  }
  if (field->kind != JsonValue::Kind::String) {
    return std::nullopt;
  }
  return field->text;
}

bool getBool(const JsonValue &v, const char *key, bool fallback,
             std::string &error) {
  const JsonValue *field = v.find(key);
  if (field == nullptr || field->kind == JsonValue::Kind::Null) {
    return fallback;
  }
  if (field->kind != JsonValue::Kind::Bool) {
    error = std::string("field is not a boolean: ") + key;
    return fallback;
  }
  return field->boolValue;
}

std::optional<std::int64_t> getOptInt(const JsonValue &v, const char *key) {
  const JsonValue *field = v.find(key);
  if (field == nullptr || field->kind == JsonValue::Kind::Null) {
    return std::nullopt;
  }
  if (!field->isNumberLike()) {
    return std::nullopt;
  }
  return field->asInt();
}

std::optional<double> getOptDouble(const JsonValue &v, const char *key) {
  const JsonValue *field = v.find(key);
  if (field == nullptr || field->kind == JsonValue::Kind::Null) {
    return std::nullopt;
  }
  if (!field->isNumberLike()) {
    return std::nullopt;
  }
  return field->asDouble();
}

} // namespace

std::optional<ConversionRequest> requestFromJson(const std::string &json,
                                                 std::string &error) {
  error.clear();
  JsonValue root;
  JsonReader reader(json);
  const std::string parseError = reader.parse(root);
  if (!parseError.empty()) {
    error = "invalid request JSON: " + parseError;
    return std::nullopt;
  }
  if (root.kind != JsonValue::Kind::Object) {
    error = "request must be a JSON object";
    return std::nullopt;
  }

  ConversionRequest request;

  // source { uri, displayName }
  const JsonValue *source = root.find("source");
  if (source == nullptr || source->kind != JsonValue::Kind::Object) {
    error = "missing source object";
    return std::nullopt;
  }
  request.source.uri = getString(*source, "uri", error);
  if (!error.empty()) {
    return std::nullopt;
  }
  if (request.source.uri.empty()) {
    error = "source.uri is empty";
    return std::nullopt;
  }
  request.source.displayName = getOptString(*source, "displayName")
                                   .value_or("Untitled media");

  request.outputName = getOptString(root, "outputName");
  if (const auto container = getOptString(root, "outputContainer")) {
    request.outputContainer = containerFromName(*container);
    if (!request.outputContainer.has_value()) {
      error = "unknown outputContainer: " + *container;
      return std::nullopt;
    }
  }

  // goal { kind, targetSizeBytes? }
  if (const JsonValue *goal = root.find("goal");
      goal != nullptr && goal->kind == JsonValue::Kind::Object) {
    const std::string kind = getOptString(*goal, "kind").value_or("recommended");
    if (kind == "recommended") {
      request.goal.kind = GoalKind::Recommended;
    } else if (kind == "smallerFile") {
      request.goal.kind = GoalKind::SmallerFile;
    } else if (kind == "highQuality") {
      request.goal.kind = GoalKind::HighQuality;
    } else if (kind == "fitUnderSize") {
      request.goal.kind = GoalKind::FitUnderSize;
      request.goal.targetSizeBytes = getOptInt(*goal, "targetSizeBytes").value_or(0);
      if (request.goal.targetSizeBytes <= 0) {
        error = "fitUnderSize requires a positive targetSizeBytes";
        return std::nullopt;
      }
    } else if (kind == "custom") {
      request.goal.kind = GoalKind::Custom;
    } else {
      error = "unknown goal kind: " + kind;
      return std::nullopt;
    }
  }

  // video { enabled, codec?, resolutionHeight?, frameRate?, rateControl,
  //         bitRate?, quality? }
  if (const JsonValue *video = root.find("video");
      video != nullptr && video->kind == JsonValue::Kind::Object) {
    request.video.enabled = getBool(*video, "enabled", true, error);
    if (!error.empty()) {
      return std::nullopt;
    }
    if (const auto codec = getOptString(*video, "codec")) {
      request.video.codec = videoCodecFromName(*codec);
      if (!request.video.codec.has_value()) {
        error = "unknown video codec: " + *codec;
        return std::nullopt;
      }
    }
    request.video.resolutionHeight =
        getOptInt(*video, "resolutionHeight").has_value()
            ? std::optional<std::int32_t>(
                  std::int32_t(*getOptInt(*video, "resolutionHeight")))
            : std::nullopt;
    if (auto rate = getOptDouble(*video, "frameRate")) {
      if (*rate <= 0.0) {
        error = "frameRate must be positive";
        return std::nullopt;
      }
      request.video.frameRate = *rate;
    }
    const std::string rateControl =
        getOptString(*video, "rateControl").value_or("auto");
    if (rateControl == "auto") {
      request.video.rateControl = RateControl::Auto;
    } else if (rateControl == "bitrate") {
      request.video.rateControl = RateControl::Bitrate;
    } else if (rateControl == "quality") {
      request.video.rateControl = RateControl::Quality;
    } else {
      error = "unknown rateControl: " + rateControl;
      return std::nullopt;
    }
    if (auto bitRate = getOptInt(*video, "bitRate")) {
      if (*bitRate <= 0) {
        error = "bitRate must be positive";
        return std::nullopt;
      }
      request.video.bitRate = *bitRate;
    }
    if (auto quality = getOptInt(*video, "quality")) {
      if (*quality < 0 || *quality > 100) {
        error = "quality must be within 0..100";
        return std::nullopt;
      }
      request.video.quality = std::int32_t(*quality);
    }
  }

  // audio { enabled, codec?, bitRate?, sampleRate?, channels? }
  if (const JsonValue *audio = root.find("audio");
      audio != nullptr && audio->kind == JsonValue::Kind::Object) {
    request.audio.enabled = getBool(*audio, "enabled", true, error);
    if (!error.empty()) {
      return std::nullopt;
    }
    if (const auto codec = getOptString(*audio, "codec")) {
      request.audio.codec = audioCodecFromName(*codec);
      if (!request.audio.codec.has_value()) {
        error = "unknown audio codec: " + *codec;
        return std::nullopt;
      }
    }
    if (auto bitRate = getOptInt(*audio, "bitRate")) {
      if (*bitRate <= 0) {
        error = "bitRate must be positive";
        return std::nullopt;
      }
      request.audio.bitRate = *bitRate;
    }
    if (auto sampleRate = getOptInt(*audio, "sampleRate")) {
      if (*sampleRate <= 0) {
        error = "sampleRate must be positive";
        return std::nullopt;
      }
      request.audio.sampleRate = std::int32_t(*sampleRate);
    }
    if (auto channels = getOptInt(*audio, "channels")) {
      if (*channels <= 0 || *channels > 16) {
        error = "channels out of range";
        return std::nullopt;
      }
      request.audio.channels = std::int32_t(*channels);
    }
  }

  // subtitles { policy, externalUri?, timingOffsetMs }
  if (const JsonValue *subs = root.find("subtitles");
      subs != nullptr && subs->kind == JsonValue::Kind::Object) {
    const std::string policy = getOptString(*subs, "policy").value_or("preserve");
    if (policy == "preserve") {
      request.subtitles.policy = SubtitlePolicy::Preserve;
    } else if (policy == "remove") {
      request.subtitles.policy = SubtitlePolicy::Remove;
    } else if (policy == "extract") {
      request.subtitles.policy = SubtitlePolicy::Extract;
    } else if (policy == "burnIn") {
      request.subtitles.policy = SubtitlePolicy::BurnIn;
    } else {
      error = "unknown subtitle policy: " + policy;
      return std::nullopt;
    }
    request.subtitles.externalUri = getOptString(*subs, "externalUri");
    request.subtitles.timingOffsetMs =
        std::int32_t(getOptInt(*subs, "timingOffsetMs").value_or(0));
  }

  // processingMode
  const std::string mode =
      getOptString(root, "processingMode").value_or("auto");
  if (mode == "auto") {
    request.processingMode = ProcessingMode::Auto;
  } else if (mode == "preferNative") {
    request.processingMode = ProcessingMode::PreferNative;
  } else if (mode == "preferSoftware") {
    request.processingMode = ProcessingMode::PreferSoftware;
  } else if (mode == "balanced") {
    request.processingMode = ProcessingMode::Balanced;
  } else {
    error = "unknown processingMode: " + mode;
    return std::nullopt;
  }

  return request;
}

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

  // Event tap: queues engine events for host polling (bounded).
  static constexpr std::size_t kMaxQueuedEvents = 1024;
  std::mutex eventsMutex;
  std::deque<velmux::core::JobEvent> eventQueue;

  struct EventTap final : public velmux::core::IJobEventSink {
    VelmuxEngineHandle *owner;
    void onJobEvent(const velmux::core::JobEvent &event) override {
      std::lock_guard<std::mutex> lock(owner->eventsMutex);
      if (owner->eventQueue.size() >= kMaxQueuedEvents) {
        owner->eventQueue.pop_front(); // drop oldest (progress spam)
      }
      owner->eventQueue.push_back(event);
    }
  };
  std::unique_ptr<EventTap> tap;
};

using namespace velmux::core;
namespace bridgeNS = velmux::bridge;

extern "C" {

VelmuxEngineHandle *velmux_engine_create(const char *dataDir) {
  if (dataDir == nullptr || dataDir[0] == '\0') {
    return nullptr;
  }
  EngineConfig config;
  config.dataDir = dataDir;
  std::vector<std::unique_ptr<velmux::backend::IBackend>> backends;
  // Backends compiled into this engine register themselves; the engine
  // runs with whatever honestly initialized (failure isolation).
#if defined(VELMUX_WITH_FFMPEG_BACKEND)
  backends.push_back(std::make_unique<velmux::ffmpeg::FfmpegBackend>());
#endif
#if defined(VELMUX_WITH_ANDROID_BACKEND)
  backends.push_back(std::make_unique<velmux::android::AndroidBackend>());
#endif
#if defined(VELMUX_WITH_APPLE_BACKEND)
  backends.push_back(std::make_unique<velmux::apple::AppleBackend>());
#endif
  if (backends.empty()) {
    // No execution backend at all: refuse to create an engine that could
    // only ever fail. Hosts render an honest setup error.
    return nullptr;
  }
  auto engine = VelmuxEngine::create(config, std::move(backends));
  if (engine == nullptr) {
    return nullptr;
  }
  auto *handle = new (std::nothrow) VelmuxEngineHandle();
  if (handle == nullptr) {
    return nullptr;
  }
  handle->engine = std::move(engine);
  handle->tap = std::make_unique<VelmuxEngineHandle::EventTap>();
  handle->tap->owner = handle;
  handle->engine->events().subscribe(handle->tap.get());
  return handle;
}

void velmux_engine_destroy(VelmuxEngineHandle *handle) {
  if (handle != nullptr) {
    if (handle->engine != nullptr && handle->tap != nullptr) {
      handle->engine->events().unsubscribe(handle->tap.get());
    }
    delete handle;
  }
}

static char *allocString(const std::string &s) {
  char *buf = static_cast<char *>(std::malloc(s.size() + 1));
  if (buf != nullptr) {
    std::memcpy(buf, s.c_str(), s.size() + 1);
  }
  return buf;
}

static std::string errorJson(const std::string &message) {
  return "{\"error\":" + bridgeNS::quoted(message) + "}";
}

char *velmux_inspect(VelmuxEngineHandle *handle, const char *uri,
                     const char *displayName) {
  if (handle == nullptr || uri == nullptr) {
    return allocString(errorJson("engine or uri is null"));
  }
  MediaSource source{uri, displayName != nullptr ? displayName : ""};
  const Result<MediaInfo> info = handle->engine->inspect(source);
  if (!info.ok()) {
    return allocString(errorJson(info.error().message));
  }
  return allocString(bridgeNS::mediaInfoToJson(info.value()));
}

char *velmux_plan(VelmuxEngineHandle *handle, const char *requestJson) {
  if (handle == nullptr || requestJson == nullptr) {
    return allocString(errorJson("engine or request is null"));
  }
  std::string parseError;
  const auto request =
      bridgeNS::requestFromJson(requestJson, parseError);
  if (!request.has_value()) {
    return allocString(errorJson(parseError));
  }
  const Result<PlanResult> plan =
      handle->engine->plan(request->source, *request);
  if (!plan.ok()) {
    return allocString(errorJson(plan.error().message));
  }
  std::string out = "{";
  out += "\"feasible\":" +
         std::string(plan.value().feasible ? "true" : "false");
  out += ",\"message\":" +
         (plan.value().message.empty()
              ? "null"
              : bridgeNS::quoted(plan.value().message));
  if (plan.value().best.has_value()) {
    const PlannedPipeline &best = *plan.value().best;
    if (best.estimatedOutputBytes.has_value()) {
      out += ",\"estimatedOutputBytes\":" +
             std::to_string(*best.estimatedOutputBytes);
    } else {
      out += ",\"estimatedOutputBytes\":null";
    }
    out += ",\"pipelineSummary\":" + bridgeNS::quoted(best.pipeline.summary);
  } else {
    out += ",\"estimatedOutputBytes\":null,\"pipelineSummary\":\"\"";
  }
  out += "}";
  return allocString(out);
}

char *velmux_start_job(VelmuxEngineHandle *handle, const char *requestJson) {
  if (handle == nullptr || requestJson == nullptr) {
    return allocString(errorJson("engine or request is null"));
  }
  std::string parseError;
  const auto request =
      bridgeNS::requestFromJson(requestJson, parseError);
  if (!request.has_value()) {
    return allocString(errorJson(parseError));
  }
  const Result<std::string> jobId = handle->engine->startJob(*request);
  if (!jobId.ok()) {
    return allocString(errorJson(jobId.error().message));
  }
  return allocString("{\"jobId\":" + bridgeNS::quoted(jobId.value()) + "}");
}

char *velmux_list_jobs(VelmuxEngineHandle *handle) {
  if (handle == nullptr) {
    return allocString(errorJson("engine is null"));
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
    return allocString(errorJson("engine or jobId is null"));
  }
  const auto snapshot = handle->engine->getJob(jobId);
  if (!snapshot.has_value()) {
    return allocString("null");
  }
  return allocString(bridgeNS::snapshotToJson(*snapshot));
}

char *velmux_describe_backends(VelmuxEngineHandle *handle) {
  if (handle == nullptr) {
    return allocString(errorJson("engine is null"));
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

char *velmux_poll_events(VelmuxEngineHandle *handle) {
  if (handle == nullptr) {
    return allocString(errorJson("engine is null"));
  }
  std::deque<JobEvent> drained;
  {
    std::lock_guard<std::mutex> lock(handle->eventsMutex);
    drained.swap(handle->eventQueue);
  }
  std::string out = "[";
  for (std::size_t i = 0; i < drained.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    out += bridgeNS::eventToJson(drained[i]);
  }
  out += "]";
  return allocString(out);
}

void velmux_free_string(char *str) { std::free(str); }

} // extern "C"
