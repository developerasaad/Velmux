// Velmux core — full-fidelity JSON serialization for the typed request
// model. See serialization.h for the contract; this is the single codec
// shared by the durable job store (schema v4) and the C ABI v2 transport.
#include "api/serialization.h"

#include "serialization/json.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace velmux::core {

namespace {

using json::JsonValue;
using json::JsonReader;

// ---------------------------------------------------------------------------
// Writing helpers (compact canonical JSON)
// ---------------------------------------------------------------------------

std::string quoted(const std::string &text) {
  return "\"" + json::escape(text) + "\"";
}

std::string optString(const std::optional<std::string> &value) {
  return value.has_value() ? quoted(*value) : "null";
}

std::string optInt32(const std::optional<std::int32_t> &value) {
  return value.has_value() ? std::to_string(*value) : "null";
}

std::string optInt64(const std::optional<std::int64_t> &value) {
  return value.has_value() ? std::to_string(*value) : "null";
}

std::string optDouble(const std::optional<double> &value) {
  if (!value.has_value()) {
    return "null";
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.17g", *value);
  return buf;
}

std::string rationalId(const Rational &rate) {
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%lld/%lld",
                static_cast<long long>(rate.num),
                static_cast<long long>(rate.den));
  return buf;
}

std::optional<Rational> rationalFromId(const std::string &id) {
  const auto slash = id.find('/');
  if (slash == std::string::npos) {
    char *end = nullptr;
    errno = 0;
    const long long whole = std::strtoll(id.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0' || whole <= 0) {
      return std::nullopt;
    }
    return Rational{whole, 1};
  }
  char *end = nullptr;
  errno = 0;
  const long long num = std::strtoll(id.c_str(), &end, 10);
  if (errno != 0 || end == nullptr || *end != '/' || num <= 0) {
    return std::nullopt;
  }
  errno = 0;
  const long long den = std::strtoll(id.c_str() + slash + 1, &end, 10);
  if (errno != 0 || end == nullptr || *end != '\0' || den <= 0 ||
      num > 1000000LL * den) {
    return std::nullopt;
  }
  return Rational{num, den};
}

// Re-serializes a parsed JsonValue back to canonical JSON text (used when a
// structured session-update payload arrives as an object/array).
std::string jsonValueToText(const JsonValue &value) {
  switch (value.kind) {
    case JsonValue::Kind::Null:
      return "null";
    case JsonValue::Kind::Bool:
      return value.boolValue ? "true" : "false";
    case JsonValue::Kind::Int:
      return std::to_string(value.integer);
    case JsonValue::Kind::Number: {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%.17g", value.number);
      return buf;
    }
    case JsonValue::Kind::String:
      return quoted(value.text);
    case JsonValue::Kind::Array: {
      std::string out = "[";
      for (std::size_t i = 0; i < value.items.size(); ++i) {
        if (i > 0) {
          out += ",";
        }
        out += jsonValueToText(value.items[i]);
      }
      out += "]";
      return out;
    }
    case JsonValue::Kind::Object: {
      std::string out = "{";
      for (std::size_t i = 0; i < value.fields.size(); ++i) {
        if (i > 0) {
          out += ",";
        }
        out += quoted(value.fields[i].first) + ":" +
               jsonValueToText(value.fields[i].second);
      }
      out += "}";
      return out;
    }
  }
  return "null";
}

// ---------------------------------------------------------------------------
// Strict reading helpers (wrong-typed fields are REJECTED)
// ---------------------------------------------------------------------------

const JsonValue *objectField(const JsonValue &parent, const char *key,
                             bool required, std::string &error) {
  const JsonValue *field = parent.find(key);
  if (field == nullptr) {
    if (required) {
      error = std::string("missing field: ") + key;
    }
    return nullptr;
  }
  if (field->kind != JsonValue::Kind::Object) {
    error = std::string("field is not an object: ") + key;
    return nullptr;
  }
  return field;
}

const JsonValue *arrayField(const JsonValue &parent, const char *key,
                            bool required, std::string &error) {
  const JsonValue *field = parent.find(key);
  if (field == nullptr) {
    if (required) {
      error = std::string("missing field: ") + key;
    }
    return nullptr;
  }
  if (field->kind != JsonValue::Kind::Array) {
    error = std::string("field is not an array: ") + key;
    return nullptr;
  }
  return field;
}

// Reads a required non-empty string.
bool readString(const JsonValue &parent, const char *key, std::string &out,
                std::string &error) {
  const JsonValue *field = parent.find(key);
  if (field == nullptr) {
    error = std::string("missing field: ") + key;
    return false;
  }
  if (field->kind != JsonValue::Kind::String) {
    error = std::string("field is not a string: ") + key;
    return false;
  }
  out = field->text;
  return true;
}

// Reads a string-or-null; nullopt for null/absent, error for other types.
bool readOptString(const JsonValue &parent, const char *key,
                   std::optional<std::string> &out, std::string &error) {
  const JsonValue *field = parent.find(key);
  if (field == nullptr || field->kind == JsonValue::Kind::Null) {
    out = std::nullopt;
    return true;
  }
  if (field->kind != JsonValue::Kind::String) {
    error = std::string("field is not a string: ") + key;
    return false;
  }
  out = field->text;
  return true;
}

bool readBool(const JsonValue &parent, const char *key, bool &out,
              std::string &error) {
  const JsonValue *field = parent.find(key);
  if (field == nullptr) {
    error = std::string("missing field: ") + key;
    return false;
  }
  if (field->kind != JsonValue::Kind::Bool) {
    error = std::string("field is not a boolean: ") + key;
    return false;
  }
  out = field->boolValue;
  return true;
}

// Reads an integer field; JSON floats are rejected (the canonical encoding
// writes integers as numbers, so 1.5 for an int field is a wrong-typed
// payload).
bool readInt(const JsonValue &parent, const char *key, std::int64_t &out,
             std::string &error) {
  const JsonValue *field = parent.find(key);
  if (field == nullptr) {
    error = std::string("missing field: ") + key;
    return false;
  }
  if (field->kind != JsonValue::Kind::Int) {
    error = std::string("field is not an integer: ") + key;
    return false;
  }
  out = field->integer;
  return true;
}

bool readOptInt(const JsonValue &parent, const char *key,
                std::optional<std::int64_t> &out, std::string &error) {
  const JsonValue *field = parent.find(key);
  if (field == nullptr || field->kind == JsonValue::Kind::Null) {
    out = std::nullopt;
    return true;
  }
  if (field->kind != JsonValue::Kind::Int) {
    error = std::string("field is not an integer: ") + key;
    return false;
  }
  out = field->integer;
  return true;
}

bool readOptInt32(const JsonValue &parent, const char *key,
                  std::optional<std::int32_t> &out, std::string &error) {
  std::optional<std::int64_t> wide;
  if (!readOptInt(parent, key, wide, error)) {
    return false;
  }
  if (wide.has_value() &&
      (*wide < INT32_MIN || *wide > INT32_MAX)) {
    error = std::string("integer out of range: ") + key;
    return false;
  }
  out = wide.has_value() ? std::optional<std::int32_t>(
                               std::int32_t(*wide))
                         : std::nullopt;
  return true;
}

bool readOptDouble(const JsonValue &parent, const char *key,
                   std::optional<double> &out, std::string &error) {
  const JsonValue *field = parent.find(key);
  if (field == nullptr || field->kind == JsonValue::Kind::Null) {
    out = std::nullopt;
    return true;
  }
  if (!field->isNumberLike()) {
    error = std::string("field is not a number: ") + key;
    return false;
  }
  out = field->asDouble();
  return true;
}

bool readIntInRange(const JsonValue &parent, const char *key,
                    std::int64_t lower, std::int64_t upper, std::int64_t &out,
                    std::string &error) {
  if (!readInt(parent, key, out, error)) {
    return false;
  }
  if (out < lower || out > upper) {
    error = std::string("field out of range: ") + key;
    return false;
  }
  return true;
}

// Range-checked optional integer: null/absent -> nullopt.
bool readOptIntInRange(const JsonValue &parent, const char *key,
                       std::int64_t lower, std::int64_t upper,
                       std::optional<std::int32_t> &out, std::string &error) {
  std::optional<std::int64_t> wide;
  if (!readOptInt(parent, key, wide, error)) {
    return false;
  }
  if (!wide.has_value()) {
    out = std::nullopt;
    return true;
  }
  if (*wide < lower || *wide > upper) {
    error = std::string("field out of range: ") + key;
    return false;
  }
  out = std::int32_t(*wide);
  return true;
}

// ---------------------------------------------------------------------------
// Shared payload-section codecs
// ---------------------------------------------------------------------------

constexpr std::int64_t kMinBitrate = 1000;             // 1 kbps
constexpr std::int64_t kMaxVideoBitrate = 200'000'000; // 200 Mbps
constexpr std::int64_t kMaxAudioBitrate = 2'000'000;   // 2 Mbps
constexpr std::int64_t kUsPerDay = 24LL * 3600 * 1000000;

std::string mediaSourceToJson(const MediaSource &source) {
  return "{\"uri\":" + quoted(source.uri) +
         ",\"name\":" + quoted(source.displayName) + "}";
}

bool mediaSourceFromJson(const JsonValue &value, MediaSource &out,
                         std::string &error) {
  if (value.kind != JsonValue::Kind::Object) {
    error = "source must be an object";
    return false;
  }
  if (!readString(value, "uri", out.uri, error)) {
    return false;
  }
  if (out.uri.empty()) {
    error = "source.uri is empty";
    return false;
  }
  if (!readString(value, "name", out.displayName, error)) {
    return false;
  }
  return true;
}

std::string outputSpecToJson(const OutputSpec &output) {
  return "{\"name\":" + optString(output.name) +
         ",\"container\":" +
         (output.container.has_value()
              ? quoted(containerName(*output.container))
              : "null") +
         "}";
}

bool outputSpecFromJson(const JsonValue &value, OutputSpec &out,
                        std::string &error) {
  if (value.kind != JsonValue::Kind::Object) {
    error = "output must be an object";
    return false;
  }
  if (!readOptString(value, "name", out.name, error)) {
    return false;
  }
  if (out.name.has_value()) {
    if (out.name->size() > 255) {
      error = "output name too long";
      return false;
    }
    if (out.name->find('/') != std::string::npos ||
        out.name->find('\\') != std::string::npos ||
        *out.name == "." || *out.name == "..") {
      error = "invalid output name";
      return false;
    }
  }
  std::optional<std::string> container;
  if (!readOptString(value, "container", container, error)) {
    return false;
  }
  if (container.has_value()) {
    out.container = containerFromName(*container);
    if (!out.container.has_value()) {
      error = "unknown output container: " + *container;
      return false;
    }
  }
  return true;
}

std::string audioSettingsToJson(const AudioSettings &audio) {
  return "{\"codec\":" +
         (audio.codec.has_value() ? quoted(codecName(*audio.codec))
                                  : "null") +
         ",\"bitRate\":" + optInt64(audio.bitRate) +
         ",\"sampleRate\":" + optInt32(audio.sampleRate) +
         ",\"channels\":" + optInt32(audio.channels) +
         ",\"pcmBitDepth\":" + optInt32(audio.pcmBitDepth) + "}";
}

bool audioSettingsFromJson(const JsonValue &value, AudioSettings &out,
                           std::string &error) {
  if (value.kind != JsonValue::Kind::Object) {
    error = "audio settings must be an object";
    return false;
  }
  std::optional<std::string> codec;
  if (!readOptString(value, "codec", codec, error)) {
    return false;
  }
  if (codec.has_value()) {
    out.codec = audioCodecFromName(*codec);
    if (!out.codec.has_value()) {
      error = "unknown audio codec: " + *codec;
      return false;
    }
  }
  std::optional<std::int64_t> bitRate;
  if (!readOptInt(value, "bitRate", bitRate, error)) {
    return false;
  }
  if (bitRate.has_value() &&
      (*bitRate < kMinBitrate || *bitRate > kMaxAudioBitrate)) {
    error = "audio bitRate out of range";
    return false;
  }
  out.bitRate = bitRate;
  if (!readOptIntInRange(value, "sampleRate", 4000, 192000, out.sampleRate,
                         error)) {
    return false;
  }
  // Channel layouts: mono/stereo/5.1(6) (same rule the SessionManager
  // applies).
  if (const JsonValue *channels = value.find("channels");
      channels != nullptr && channels->kind != JsonValue::Kind::Null) {
    std::int64_t raw = 0;
    if (!readInt(value, "channels", raw, error)) {
      return false;
    }
    if (raw != 1 && raw != 2 && raw != 6) {
      error = "unsupported channel count";
      return false;
    }
    out.channels = std::int32_t(raw);
  }
  if (const JsonValue *depth = value.find("pcmBitDepth");
      depth != nullptr && depth->kind != JsonValue::Kind::Null) {
    std::int64_t raw = 0;
    if (!readInt(value, "pcmBitDepth", raw, error)) {
      return false;
    }
    if (raw != 16 && raw != 24 && raw != 32) {
      error = "pcmBitDepth must be one of 16, 24, 32";
      return false;
    }
    out.pcmBitDepth = std::int32_t(raw);
  }
  return true;
}

std::string compressionToJson(const CompressionIntent &compression) {
  return "{\"enabled\":" + std::string(compression.enabled ? "true" : "false") +
         ",\"preset\":" + quoted(compressionPresetName(compression.preset)) +
         ",\"targetSizeBytes\":" + std::to_string(compression.targetSizeBytes) +
         ",\"quality\":" + optInt32(compression.quality) +
         ",\"videoBitrate\":" + optInt64(compression.videoBitrate) +
         ",\"audioBitrate\":" + optInt64(compression.audioBitrate) + "}";
}

bool compressionFromJson(const JsonValue &value, CompressionIntent &out,
                         std::string &error) {
  if (value.kind != JsonValue::Kind::Object) {
    error = "compression must be an object";
    return false;
  }
  if (!readBool(value, "enabled", out.enabled, error)) {
    return false;
  }
  std::string preset;
  if (!readString(value, "preset", preset, error)) {
    return false;
  }
  const auto parsedPreset = compressionPresetFromName(preset);
  if (!parsedPreset.has_value()) {
    error = "unknown compression preset: " + preset;
    return false;
  }
  out.preset = *parsedPreset;
  std::int64_t target = 0;
  if (!readInt(value, "targetSizeBytes", target, error)) {
    return false;
  }
  if (target < 0 || target > (1LL << 40)) {
    error = "targetSizeBytes out of range";
    return false;
  }
  if (out.preset == CompressionPreset::TargetSize && target < 1024) {
    error = "target-size preset requires a target of at least 1024 bytes";
    return false;
  }
  out.targetSizeBytes = target;
  if (!readOptIntInRange(value, "quality", 1, 100, out.quality, error)) {
    return false;
  }
  std::optional<std::int64_t> videoBitrate;
  if (!readOptInt(value, "videoBitrate", videoBitrate, error)) {
    return false;
  }
  if (videoBitrate.has_value() && (*videoBitrate < kMinBitrate ||
                                   *videoBitrate > kMaxVideoBitrate)) {
    error = "videoBitrate out of range";
    return false;
  }
  out.videoBitrate = videoBitrate;
  std::optional<std::int64_t> audioBitrate;
  if (!readOptInt(value, "audioBitrate", audioBitrate, error)) {
    return false;
  }
  if (audioBitrate.has_value() && (*audioBitrate < kMinBitrate ||
                                   *audioBitrate > kMaxAudioBitrate)) {
    error = "audioBitrate out of range";
    return false;
  }
  out.audioBitrate = audioBitrate;
  return true;
}

// Metadata: Set -> string value, Clear -> null, Unchanged -> absent.
// (Full fidelity: all three states round-trip.)
std::string metadataToJson(const MetadataIntention &metadata) {
  std::string out = "{";
  const auto field = [&out](const char *key, MetadataFieldAction action,
                            const std::string &value, bool first) {
    if (action == MetadataFieldAction::Unchanged) {
      return first;
    }
    if (!first) {
      out += ",";
    }
    out += std::string("\"") + key + "\":";
    out += action == MetadataFieldAction::Set ? quoted(value) : "null";
    return false;
  };
  bool first = true;
  first = field("title", metadata.titleAction, metadata.title, first);
  first = field("artist", metadata.artistAction, metadata.artist, first);
  first = field("album", metadata.albumAction, metadata.album, first);
  first = field("genre", metadata.genreAction, metadata.genre, first);
  first = field("date", metadata.dateAction, metadata.date, first);
  first = field("description", metadata.descriptionAction,
                metadata.description, first);
  first = field("location", metadata.locationAction, metadata.location, first);
  field("artwork", metadata.artworkAction, metadata.artworkUri, first);
  out += "}";
  return out;
}

bool metadataFromJson(const JsonValue &value, MetadataIntention &out,
                      std::string &error) {
  if (value.kind != JsonValue::Kind::Object) {
    error = "metadata must be an object";
    return false;
  }
  const auto applyText = [&](const char *key, MetadataFieldAction &action,
                             std::string &target) {
    const JsonValue *field = value.find(key);
    if (field == nullptr) {
      return; // Unchanged
    }
    if (field->kind == JsonValue::Kind::Null) {
      action = MetadataFieldAction::Clear;
      target.clear();
    } else if (field->kind == JsonValue::Kind::String) {
      action = MetadataFieldAction::Set;
      target = field->text;
    } else {
      error = std::string("metadata field is not a string: ") + key;
    }
  };
  applyText("title", out.titleAction, out.title);
  applyText("artist", out.artistAction, out.artist);
  applyText("album", out.albumAction, out.album);
  applyText("genre", out.genreAction, out.genre);
  applyText("date", out.dateAction, out.date);
  applyText("description", out.descriptionAction, out.description);
  applyText("location", out.locationAction, out.location);
  applyText("artwork", out.artworkAction, out.artworkUri);
  return error.empty();
}

std::string videoAdjustmentsToJson(const VideoAdjustments &adj) {
  return "{\"rotation\":" + std::to_string(adj.rotationDegrees) +
         ",\"flipH\":" + std::string(adj.flipHorizontal ? "true" : "false") +
         ",\"flipV\":" + std::string(adj.flipVertical ? "true" : "false") +
         ",\"speed\":" + optDouble(adj.playbackSpeed) +
         ",\"reverse\":" + std::string(adj.reverse ? "true" : "false") +
         ",\"hdrToSdr\":" + std::string(adj.hdrToSdr ? "true" : "false") +
         "}";
}

bool videoAdjustmentsFromJson(const JsonValue &value, VideoAdjustments &out,
                              std::string &error) {
  if (value.kind != JsonValue::Kind::Object) {
    error = "adjustments must be an object";
    return false;
  }
  if (const JsonValue *rotation = value.find("rotation");
      rotation != nullptr && rotation->kind != JsonValue::Kind::Null) {
    std::int64_t raw = 0;
    if (!readInt(value, "rotation", raw, error)) {
      return false;
    }
    if (raw != 0 && raw != 90 && raw != 180 && raw != 270) {
      error = "invalid rotation";
      return false;
    }
    out.rotationDegrees = std::int32_t(raw);
  }
  if (const JsonValue *flipH = value.find("flipH");
      flipH != nullptr && flipH->kind != JsonValue::Kind::Null) {
    if (!readBool(value, "flipH", out.flipHorizontal, error)) {
      return false;
    }
  }
  if (const JsonValue *flipV = value.find("flipV");
      flipV != nullptr && flipV->kind != JsonValue::Kind::Null) {
    if (!readBool(value, "flipV", out.flipVertical, error)) {
      return false;
    }
  }
  if (!readOptDouble(value, "speed", out.playbackSpeed, error)) {
    return false;
  }
  if (out.playbackSpeed.has_value() &&
      (*out.playbackSpeed < 0.5 || *out.playbackSpeed > 2.0)) {
    error = "playback speed out of range (0.5..2.0)";
    return false;
  }
  if (const JsonValue *reverse = value.find("reverse");
      reverse != nullptr && reverse->kind != JsonValue::Kind::Null) {
    if (!readBool(value, "reverse", out.reverse, error)) {
      return false;
    }
  }
  if (const JsonValue *hdr = value.find("hdrToSdr");
      hdr != nullptr && hdr->kind != JsonValue::Kind::Null) {
    if (!readBool(value, "hdrToSdr", out.hdrToSdr, error)) {
      return false;
    }
  }
  return true;
}

std::string audioAdjustmentsToJson(const AudioAdjustments &adj) {
  return "{\"volume\":" + optDouble(adj.gainDb) +
         ",\"normalize\":" + std::string(adj.normalize ? "true" : "false") +
         ",\"fadeIn\":" + optInt32(adj.fadeInMs) +
         ",\"fadeOut\":" + optInt32(adj.fadeOutMs) +
         ",\"speed\":" + optDouble(adj.playbackSpeed) +
         ",\"preservePitch\":" +
         std::string(adj.preservePitch ? "true" : "false") +
         ",\"reverse\":" + std::string(adj.reverse ? "true" : "false") +
         ",\"removeSilence\":" +
         std::string(adj.removeSilence ? "true" : "false") + "}";
}

bool audioAdjustmentsFromJson(const JsonValue &value, AudioAdjustments &out,
                              std::string &error) {
  if (value.kind != JsonValue::Kind::Object) {
    error = "adjustments must be an object";
    return false;
  }
  if (!readOptDouble(value, "volume", out.gainDb, error)) {
    return false;
  }
  if (out.gainDb.has_value() &&
      (*out.gainDb < -60.0 || *out.gainDb > 60.0)) {
    error = "volume out of range (-60..60 dB)";
    return false;
  }
  if (const JsonValue *normalize = value.find("normalize");
      normalize != nullptr && normalize->kind != JsonValue::Kind::Null) {
    if (!readBool(value, "normalize", out.normalize, error)) {
      return false;
    }
  }
  if (!readOptIntInRange(value, "fadeIn", 0, 60000, out.fadeInMs, error)) {
    return false;
  }
  if (!readOptIntInRange(value, "fadeOut", 0, 60000, out.fadeOutMs, error)) {
    return false;
  }
  if (!readOptDouble(value, "speed", out.playbackSpeed, error)) {
    return false;
  }
  if (out.playbackSpeed.has_value() &&
      (*out.playbackSpeed < 0.5 || *out.playbackSpeed > 2.0)) {
    error = "playback speed out of range (0.5..2.0)";
    return false;
  }
  if (const JsonValue *pitch = value.find("preservePitch");
      pitch != nullptr && pitch->kind != JsonValue::Kind::Null) {
    if (!readBool(value, "preservePitch", out.preservePitch, error)) {
      return false;
    }
  }
  if (const JsonValue *reverse = value.find("reverse");
      reverse != nullptr && reverse->kind != JsonValue::Kind::Null) {
    if (!readBool(value, "reverse", out.reverse, error)) {
      return false;
    }
  }
  if (const JsonValue *silence = value.find("removeSilence");
      silence != nullptr && silence->kind != JsonValue::Kind::Null) {
    if (!readBool(value, "removeSilence", out.removeSilence, error)) {
      return false;
    }
  }
  return true;
}

std::string timeRangeToJson(const TimeRange &range) {
  return "{\"startUs\":" + std::to_string(range.startUs) +
         ",\"endUs\":" + std::to_string(range.endUs) + "}";
}

bool timeRangeFromJson(const JsonValue &value, TimeRange &out,
                       std::string &error) {
  if (value.kind != JsonValue::Kind::Object) {
    error = "range must be an object";
    return false;
  }
  std::int64_t start = 0;
  std::int64_t end = 0;
  if (!readInt(value, "startUs", start, error) ||
      !readInt(value, "endUs", end, error)) {
    return false;
  }
  if (!TimeRange{start, end}.valid()) {
    error = "invalid time range";
    return false;
  }
  out.startUs = start;
  out.endUs = end;
  return true;
}

std::string splitRulesToJson(const SplitRules &rules) {
  const char *mode = "equal-parts";
  switch (rules.mode) {
    case SplitMode::EqualParts:
      mode = "equal-parts";
      break;
    case SplitMode::Duration:
      mode = "duration";
      break;
    case SplitMode::MaxSize:
      mode = "max-size";
      break;
    case SplitMode::CustomTimestamps:
      mode = "custom";
      break;
    case SplitMode::Chapters:
      mode = "chapters";
      break;
    case SplitMode::Scenes:
      mode = "scenes";
      break;
  }
  std::string out = "{\"mode\":";
  out += quoted(mode);
  out += ",\"partCount\":" + std::to_string(rules.partCount);
  out += ",\"partDurationUs\":" + std::to_string(rules.partDurationUs);
  out += ",\"maxSizeBytes\":" + std::to_string(rules.maxSizeBytes);
  out += ",\"boundaries\":[";
  for (std::size_t i = 0; i < rules.boundaryUs.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    out += std::to_string(rules.boundaryUs[i]);
  }
  out += "]}";
  return out;
}

bool splitRulesFromJson(const JsonValue &value, SplitRules &out,
                        std::string &error) {
  if (value.kind != JsonValue::Kind::Object) {
    error = "split rules must be an object";
    return false;
  }
  std::string mode;
  if (!readString(value, "mode", mode, error)) {
    return false;
  }
  if (mode == "equal-parts") {
    out.mode = SplitMode::EqualParts;
  } else if (mode == "duration") {
    out.mode = SplitMode::Duration;
  } else if (mode == "max-size") {
    out.mode = SplitMode::MaxSize;
  } else if (mode == "custom") {
    out.mode = SplitMode::CustomTimestamps;
  } else if (mode == "chapters") {
    out.mode = SplitMode::Chapters;
  } else if (mode == "scenes") {
    out.mode = SplitMode::Scenes;
  } else {
    error = "unknown split mode: " + mode;
    return false;
  }
  std::int64_t count = 0;
  if (!readIntInRange(value, "partCount", 2, 100, count, error)) {
    return false;
  }
  out.partCount = std::int32_t(count);
  std::int64_t duration = 0;
  if (!readInt(value, "partDurationUs", duration, error)) {
    return false;
  }
  if (duration != 0 && (duration < 0 || duration > kUsPerDay)) {
    error = "invalid part duration";
    return false;
  }
  out.partDurationUs = duration;
  std::int64_t maxSize = 0;
  if (!readInt(value, "maxSizeBytes", maxSize, error)) {
    return false;
  }
  if (maxSize != 0 && (maxSize < 1024 || maxSize > (1LL << 42))) {
    error = "invalid maximum part size";
    return false;
  }
  out.maxSizeBytes = maxSize;
  if (const JsonValue *boundaries = arrayField(value, "boundaries", false,
                                               error);
      boundaries != nullptr) {
    std::vector<std::int64_t> stamps;
    for (const auto &item : boundaries->items) {
      if (item.kind != JsonValue::Kind::Int) {
        error = "split boundaries must be integers";
        return false;
      }
      if (item.integer <= 0 || item.integer >= kUsPerDay) {
        error = "invalid split boundary";
        return false;
      }
      stamps.push_back(item.integer);
    }
    std::sort(stamps.begin(), stamps.end());
    stamps.erase(std::unique(stamps.begin(), stamps.end()), stamps.end());
    out.boundaryUs = std::move(stamps);
  }
  return true;
}

std::string mergeRulesToJson(const MergeRules &rules) {
  const char *layout = "sequential";
  switch (rules.layout) {
    case MergeLayout::Sequential:
      layout = "sequential";
      break;
    case MergeLayout::SideBySide:
      layout = "side-by-side";
      break;
    case MergeLayout::TopBottom:
      layout = "top-bottom";
      break;
  }
  const char *audio = "first";
  switch (rules.audio) {
    case MergeAudioSelection::First:
      audio = "first";
      break;
    case MergeAudioSelection::Second:
      audio = "second";
      break;
    case MergeAudioSelection::Both:
      audio = "both";
      break;
    case MergeAudioSelection::None:
      audio = "none";
      break;
  }
  return std::string("{\"layout\":") + quoted(layout) +
         ",\"audio\":" + quoted(audio) +
         ",\"longerDuration\":" +
         std::string(rules.longerDuration ? "true" : "false") + "}";
}

bool mergeRulesFromJson(const JsonValue &value, MergeRules &out,
                        std::string &error) {
  if (value.kind != JsonValue::Kind::Object) {
    error = "merge rules must be an object";
    return false;
  }
  std::string layout;
  if (!readString(value, "layout", layout, error)) {
    return false;
  }
  if (layout == "sequential") {
    out.layout = MergeLayout::Sequential;
  } else if (layout == "side-by-side") {
    out.layout = MergeLayout::SideBySide;
  } else if (layout == "top-bottom") {
    out.layout = MergeLayout::TopBottom;
  } else {
    error = "unknown merge layout: " + layout;
    return false;
  }
  std::string audio;
  if (!readString(value, "audio", audio, error)) {
    return false;
  }
  if (audio == "first") {
    out.audio = MergeAudioSelection::First;
  } else if (audio == "second") {
    out.audio = MergeAudioSelection::Second;
  } else if (audio == "both") {
    out.audio = MergeAudioSelection::Both;
  } else if (audio == "none") {
    out.audio = MergeAudioSelection::None;
  } else {
    error = "unknown merge audio selection: " + audio;
    return false;
  }
  if (!readBool(value, "longerDuration", out.longerDuration, error)) {
    return false;
  }
  return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Typed JobRequest codec
// ---------------------------------------------------------------------------

std::string requestToJson(const JobRequest &request,
                          const std::string &submissionId) {
  std::string out = "{\"operation\":" + quoted(operationName(request.operation));
  if (!submissionId.empty()) {
    out += ",\"submissionId\":" + quoted(submissionId);
  }
  out += ",";
  const auto append = [&](const char *key, const std::string &payload) {
    out += quoted(key) + ":" + payload;
  };

  switch (request.payload.index()) {
    case 0: { // VideoConvert
      const auto &r = std::get<job::VideoConvert>(request.payload);
      std::string payload = "{\"source\":" + mediaSourceToJson(r.source) +
                             ",\"output\":" + outputSpecToJson(r.output) +
                             ",\"video\":{";
      payload += "\"codec\":" +
                 (r.video.codec.has_value() ? quoted(codecName(*r.video.codec))
                                            : "null");
      payload += ",\"resolutionHeight\":" + optInt32(r.video.resolutionHeight);
      payload += ",\"customWidth\":" + optInt32(r.video.customWidth);
      payload += ",\"customHeight\":" + optInt32(r.video.customHeight);
      payload += ",\"frameRate\":" +
                 (r.video.frameRate.has_value()
                      ? quoted(rationalId(*r.video.frameRate))
                      : "null");
      payload += "}";
      payload += ",\"audioTracks\":[";
      for (std::size_t i = 0; i < r.audioTracks.size(); ++i) {
        const AudioTrackRule &rule = r.audioTracks[i];
        if (i > 0) {
          payload += ",";
        }
        payload += "{\"index\":" + std::to_string(rule.sourceTrackIndex) +
                   ",\"include\":" +
                   std::string(rule.include ? "true" : "false") +
                   ",\"external\":" + optString(rule.externalUri) +
                   ",\"replace\":" +
                   std::string(rule.replace ? "true" : "false") + "}";
      }
      payload += "]";
      payload += ",\"audioSettings\":" + audioSettingsToJson(r.audioSettings);
      payload += ",\"subtitleTracks\":[";
      for (std::size_t i = 0; i < r.subtitleTracks.size(); ++i) {
        const SubtitleTrackRule &rule = r.subtitleTracks[i];
        if (i > 0) {
          payload += ",";
        }
        const char *policy = "preserve";
        switch (rule.policy) {
          case SubtitleTrackPolicy::Preserve:
            policy = "preserve";
            break;
          case SubtitleTrackPolicy::Remove:
            policy = "remove";
            break;
          case SubtitleTrackPolicy::Embed:
            policy = "embed";
            break;
          case SubtitleTrackPolicy::BurnIn:
            policy = "burnIn";
            break;
        }
        payload += "{\"index\":" + std::to_string(rule.sourceTrackIndex) +
                   ",\"policy\":" + quoted(policy) +
                   ",\"external\":" + optString(rule.externalUri) +
                   ",\"fontSize\":" + optInt32(rule.fontSize) +
                   ",\"color\":" + optString(rule.color) +
                   ",\"position\":" + optInt32(rule.position) + "}";
      }
      payload += "]";
      payload +=
          ",\"adjustments\":" + videoAdjustmentsToJson(r.adjustments);
      payload += ",\"compression\":" + compressionToJson(r.compression);
      payload += ",\"metadata\":" + metadataToJson(r.metadata);
      payload += "}";
      append(operationName(request.operation), payload);
      break;
    }
    case 1: { // AudioConvert
      const auto &r = std::get<job::AudioConvert>(request.payload);
      std::string payload = "{\"source\":" + mediaSourceToJson(r.source) +
                             ",\"output\":" + outputSpecToJson(r.output) +
                             ",\"audio\":" + audioSettingsToJson(r.audio) +
                             ",\"adjustments\":" +
                             audioAdjustmentsToJson(r.adjustments) +
                             ",\"compression\":" +
                             compressionToJson(r.compression) +
                             ",\"metadata\":" + metadataToJson(r.metadata) +
                             "}";
      append(operationName(request.operation), payload);
      break;
    }
    case 2: { // Crop
      const auto &r = std::get<job::Crop>(request.payload);
      std::string payload = "{\"source\":" + mediaSourceToJson(r.source) +
                             ",\"output\":" + outputSpecToJson(r.output) +
                             ",\"crop\":{" +
                             "\"x\":" + std::to_string(r.crop.x) +
                             ",\"y\":" + std::to_string(r.crop.y) +
                             ",\"width\":" + std::to_string(r.crop.width) +
                             ",\"height\":" + std::to_string(r.crop.height) +
                             "}}";
      append(operationName(request.operation), payload);
      break;
    }
    case 3:    // Trim
    case 4:    // Cut
    case 5:    // Split
    case 6: {  // Merge (Cut & Merge payloads)
      std::string mode;
      std::string payload;
      bool isAudioOp = request.operation == Operation::AudioCutMerge;
      if (request.payload.index() == 3) {
        const auto &r = std::get<job::Trim>(request.payload);
        mode = "trim";
        payload = "{\"source\":" + mediaSourceToJson(r.source) +
                  ",\"output\":" + outputSpecToJson(r.output) +
                  ",\"range\":" + timeRangeToJson(r.range) + "}";
      } else if (request.payload.index() == 4) {
        const auto &r = std::get<job::Cut>(request.payload);
        mode = "cut";
        payload = "{\"source\":" + mediaSourceToJson(r.source) +
                  ",\"output\":" + outputSpecToJson(r.output) +
                  ",\"removed\":[";
        for (std::size_t i = 0; i < r.removed.size(); ++i) {
          if (i > 0) {
            payload += ",";
          }
          payload += timeRangeToJson(r.removed[i]);
        }
        payload += "]}";
      } else if (request.payload.index() == 5) {
        const auto &r = std::get<job::Split>(request.payload);
        mode = "split";
        payload = "{\"source\":" + mediaSourceToJson(r.source) +
                  ",\"output\":" + outputSpecToJson(r.output) +
                  ",\"rules\":" + splitRulesToJson(r.rules) + "}";
      } else {
        const auto &r = std::get<job::Merge>(request.payload);
        mode = "merge";
        payload = "{\"sources\":[";
        for (std::size_t i = 0; i < r.sources.size(); ++i) {
          if (i > 0) {
            payload += ",";
          }
          payload += mediaSourceToJson(r.sources[i]);
        }
        payload += "],\"output\":" + outputSpecToJson(r.output) +
                   ",\"rules\":" + mergeRulesToJson(r.rules) + "}";
      }
      (void)isAudioOp;
      // The reader contract (and the documented schema) keeps "mode" and
      // the mode-keyed payload at the ROOT level next to "operation".
      out += "\"mode\":" + quoted(mode) + "," + quoted(mode) + ":" + payload;
      break;
    }
    case 7: { // ExtractAudio
      const auto &r = std::get<job::ExtractAudio>(request.payload);
      std::string payload = "{\"source\":" + mediaSourceToJson(r.source) +
                             ",\"output\":" + outputSpecToJson(r.output) +
                             ",\"trackIndices\":[";
      for (std::size_t i = 0; i < r.trackIndices.size(); ++i) {
        if (i > 0) {
          payload += ",";
        }
        payload += std::to_string(r.trackIndices[i]);
      }
      payload += "]";
      payload += ",\"audio\":" + audioSettingsToJson(r.audio);
      payload += ",\"range\":" +
                 (r.range.has_value() ? timeRangeToJson(*r.range) : "null");
      payload += ",\"compression\":" + compressionToJson(r.compression);
      payload += ",\"metadata\":" + metadataToJson(r.metadata);
      payload += "}";
      append(operationName(request.operation), payload);
      break;
    }
    case 8: { // ExtractFrames
      const auto &r = std::get<job::ExtractFrames>(request.payload);
      const char *mode = "all";
      switch (r.selection.mode) {
        case FrameSelectionMode::Current:
          mode = "current";
          break;
        case FrameSelectionMode::Selected:
          mode = "selected";
          break;
        case FrameSelectionMode::Interval:
          mode = "interval";
          break;
        case FrameSelectionMode::FrameRate:
          mode = "frame-rate";
          break;
        case FrameSelectionMode::Count:
          mode = "count";
          break;
        case FrameSelectionMode::AllInRange:
          mode = "all";
          break;
        case FrameSelectionMode::Keyframes:
          mode = "keyframes";
          break;
        case FrameSelectionMode::SceneChanges:
          mode = "scenes";
          break;
      }
      const char *format = "jpeg";
      switch (r.image.format) {
        case ImageOutputRules::Format::Jpeg:
          format = "jpeg";
          break;
        case ImageOutputRules::Format::Png:
          format = "png";
          break;
        case ImageOutputRules::Format::WebP:
          format = "webp";
          break;
      }
      std::string payload = "{\"source\":" + mediaSourceToJson(r.source) +
                             ",\"output\":" + outputSpecToJson(r.output) +
                             ",\"selection\":{";
      payload += std::string("\"mode\":") + quoted(mode);
      payload += ",\"intervalUs\":" + std::to_string(r.selection.intervalUs);
      payload += ",\"frameRate\":" +
                 (r.selection.frameRate.valid()
                      ? quoted(rationalId(r.selection.frameRate))
                      : "null");
      payload += ",\"count\":" + std::to_string(r.selection.count);
      payload += ",\"timestampsUs\":[";
      for (std::size_t i = 0; i < r.selection.timestampsUs.size(); ++i) {
        if (i > 0) {
          payload += ",";
        }
        payload += std::to_string(r.selection.timestampsUs[i]);
      }
      payload += "]";
      payload += ",\"range\":" + timeRangeToJson(r.selection.range);
      payload += "}";
      payload += ",\"image\":{";
      payload += std::string("\"format\":") + quoted(format);
      payload += ",\"quality\":" + optInt32(r.image.quality);
      payload += ",\"maxWidth\":" + optInt32(r.image.maxWidth);
      payload += ",\"maxHeight\":" + optInt32(r.image.maxHeight);
      payload += ",\"sequentialNaming\":" +
                 std::string(r.image.sequentialNaming ? "true" : "false");
      payload += "}}";
      append(operationName(request.operation), payload);
      break;
    }
    case 9: { // ExtractSubtitles
      const auto &r = std::get<job::ExtractSubtitles>(request.payload);
      auto targetName = [](SubtitleTextTarget target) {
        switch (target) {
          case SubtitleTextTarget::Original:
            return "original";
          case SubtitleTextTarget::Srt:
            return "srt";
          case SubtitleTextTarget::Vtt:
            return "vtt";
          case SubtitleTextTarget::Ass:
            return "ass";
        }
        return "original";
      };
      std::string payload = "{\"source\":" + mediaSourceToJson(r.source) +
                             ",\"output\":" + outputSpecToJson(r.output) +
                             ",\"trackIndices\":[";
      for (std::size_t i = 0; i < r.trackIndices.size(); ++i) {
        if (i > 0) {
          payload += ",";
        }
        payload += std::to_string(r.trackIndices[i]);
      }
      payload += "],\"targets\":[";
      for (std::size_t i = 0; i < r.targets.size(); ++i) {
        if (i > 0) {
          payload += ",";
        }
        payload += quoted(targetName(r.targets[i]));
      }
      payload += "]}";
      append(operationName(request.operation), payload);
      break;
    }
    default:
      break;
  }
  out += "}";
  return out;
}

namespace {

// -- per-payload decoders (strict) -------------------------------------------

bool videoConvertFromJson(const JsonValue &payload, job::VideoConvert &out,
                          std::string &error) {
  if (payload.kind != JsonValue::Kind::Object) {
    error = "video-convert payload must be an object";
    return false;
  }
  const JsonValue *source = nullptr;
  if ((source = objectField(payload, "source", true, error)) == nullptr ||
      !mediaSourceFromJson(*source, out.source, error)) {
    return false;
  }
  const JsonValue *output = nullptr;
  if ((output = objectField(payload, "output", true, error)) == nullptr ||
      !outputSpecFromJson(*output, out.output, error)) {
    return false;
  }
  const JsonValue *video = nullptr;
  if ((video = objectField(payload, "video", true, error)) == nullptr) {
    return false;
  }
  std::optional<std::string> codec;
  if (!readOptString(*video, "codec", codec, error)) {
    return false;
  }
  if (codec.has_value()) {
    out.video.codec = videoCodecFromName(*codec);
    if (!out.video.codec.has_value()) {
      error = "unknown video codec: " + *codec;
      return false;
    }
  }
  if (!readOptIntInRange(*video, "resolutionHeight", 0, 4320,
                         out.video.resolutionHeight, error)) {
    return false;
  }
  if (out.video.resolutionHeight == 0) {
    out.video.resolutionHeight.reset();
  }
  if (!readOptIntInRange(*video, "customWidth", 0, 8192, out.video.customWidth,
                         error) ||
      !readOptIntInRange(*video, "customHeight", 0, 8192,
                         out.video.customHeight, error)) {
    return false;
  }
  if (out.video.customWidth == 0) {
    out.video.customWidth.reset();
  }
  if (out.video.customHeight == 0) {
    out.video.customHeight.reset();
  }
  if (const JsonValue *rate = video->find("frameRate");
      rate != nullptr && rate->kind != JsonValue::Kind::Null) {
    if (rate->kind != JsonValue::Kind::String) {
      error = "field is not a string: frameRate";
      return false;
    }
    auto parsed = rationalFromId(rate->text);
    if (!parsed.has_value()) {
      error = "invalid frame rate: " + rate->text;
      return false;
    }
    out.video.frameRate = *parsed;
  }
  if (const JsonValue *tracks = arrayField(payload, "audioTracks", false,
                                           error);
      tracks != nullptr) {
    for (const auto &item : tracks->items) {
      if (item.kind != JsonValue::Kind::Object) {
        error = "audio track rule must be an object";
        return false;
      }
      AudioTrackRule rule;
      std::int64_t index = 0;
      if (!readInt(item, "index", index, error)) {
        return false;
      }
      if (index < -1 || index > 63) {
        error = "invalid audio track index";
        return false;
      }
      rule.sourceTrackIndex = std::int32_t(index);
      if (!readBool(item, "include", rule.include, error)) {
        return false;
      }
      if (!readOptString(item, "external", rule.externalUri, error)) {
        return false;
      }
      if (rule.externalUri.has_value() && rule.externalUri->empty()) {
        rule.externalUri.reset();
      }
      if (!readBool(item, "replace", rule.replace, error)) {
        return false;
      }
      out.audioTracks.push_back(std::move(rule));
    }
  }
  const JsonValue *audioSettings = nullptr;
  if ((audioSettings = objectField(payload, "audioSettings", false, error)) !=
      nullptr) {
    if (!audioSettingsFromJson(*audioSettings, out.audioSettings, error)) {
      return false;
    }
  }
  if (const JsonValue *subs = arrayField(payload, "subtitleTracks", false,
                                         error);
      subs != nullptr) {
    for (const auto &item : subs->items) {
      if (item.kind != JsonValue::Kind::Object) {
        error = "subtitle track rule must be an object";
        return false;
      }
      SubtitleTrackRule rule;
      std::int64_t index = 0;
      if (!readInt(item, "index", index, error)) {
        return false;
      }
      if (index < -1 || index > 63) {
        error = "invalid subtitle track index";
        return false;
      }
      rule.sourceTrackIndex = std::int32_t(index);
      std::string policy;
      if (!readString(item, "policy", policy, error)) {
        return false;
      }
      if (policy == "preserve") {
        rule.policy = SubtitleTrackPolicy::Preserve;
      } else if (policy == "remove") {
        rule.policy = SubtitleTrackPolicy::Remove;
      } else if (policy == "embed") {
        rule.policy = SubtitleTrackPolicy::Embed;
      } else if (policy == "burnIn") {
        rule.policy = SubtitleTrackPolicy::BurnIn;
      } else {
        error = "invalid subtitle policy: " + policy;
        return false;
      }
      if (!readOptString(item, "external", rule.externalUri, error)) {
        return false;
      }
      if (rule.externalUri.has_value() && rule.externalUri->empty()) {
        rule.externalUri.reset();
      }
      if (!readOptIntInRange(item, "fontSize", 8, 144, rule.fontSize, error)) {
        return false;
      }
      if (!readOptString(item, "color", rule.color, error)) {
        return false;
      }
      if (rule.color.has_value() &&
          (rule.color->size() != 7 || (*rule.color)[0] != '#')) {
        error = "invalid subtitle color (expected #RRGGBB)";
        return false;
      }
      if (!readOptIntInRange(item, "position", 0, 100, rule.position, error)) {
        return false;
      }
      out.subtitleTracks.push_back(std::move(rule));
    }
  }
  const JsonValue *adjustments = nullptr;
  if ((adjustments = objectField(payload, "adjustments", false, error)) !=
      nullptr) {
    if (!videoAdjustmentsFromJson(*adjustments, out.adjustments, error)) {
      return false;
    }
  }
  const JsonValue *compression = nullptr;
  if ((compression = objectField(payload, "compression", false, error)) !=
      nullptr) {
    if (!compressionFromJson(*compression, out.compression, error)) {
      return false;
    }
  }
  const JsonValue *metadata = nullptr;
  if ((metadata = objectField(payload, "metadata", false, error)) != nullptr) {
    if (!metadataFromJson(*metadata, out.metadata, error)) {
      return false;
    }
  }
  return true;
}

bool audioConvertFromJson(const JsonValue &payload, job::AudioConvert &out,
                          std::string &error) {
  if (payload.kind != JsonValue::Kind::Object) {
    error = "audio-convert payload must be an object";
    return false;
  }
  const JsonValue *source = nullptr;
  if ((source = objectField(payload, "source", true, error)) == nullptr ||
      !mediaSourceFromJson(*source, out.source, error)) {
    return false;
  }
  const JsonValue *output = nullptr;
  if ((output = objectField(payload, "output", true, error)) == nullptr ||
      !outputSpecFromJson(*output, out.output, error)) {
    return false;
  }
  const JsonValue *audio = nullptr;
  if ((audio = objectField(payload, "audio", false, error)) != nullptr &&
      !audioSettingsFromJson(*audio, out.audio, error)) {
    return false;
  }
  const JsonValue *adjustments = nullptr;
  if ((adjustments = objectField(payload, "adjustments", false, error)) !=
      nullptr) {
    if (!audioAdjustmentsFromJson(*adjustments, out.adjustments, error)) {
      return false;
    }
  }
  const JsonValue *compression = nullptr;
  if ((compression = objectField(payload, "compression", false, error)) !=
      nullptr) {
    if (!compressionFromJson(*compression, out.compression, error)) {
      return false;
    }
  }
  const JsonValue *metadata = nullptr;
  if ((metadata = objectField(payload, "metadata", false, error)) != nullptr) {
    if (!metadataFromJson(*metadata, out.metadata, error)) {
      return false;
    }
  }
  return true;
}

bool cropFromJson(const JsonValue &payload, job::Crop &out,
                  std::string &error) {
  if (payload.kind != JsonValue::Kind::Object) {
    error = "video-crop payload must be an object";
    return false;
  }
  const JsonValue *source = nullptr;
  if ((source = objectField(payload, "source", true, error)) == nullptr ||
      !mediaSourceFromJson(*source, out.source, error)) {
    return false;
  }
  const JsonValue *output = nullptr;
  if ((output = objectField(payload, "output", true, error)) == nullptr ||
      !outputSpecFromJson(*output, out.output, error)) {
    return false;
  }
  const JsonValue *crop = objectField(payload, "crop", true, error);
  if (crop == nullptr) {
    return false;
  }
  std::int64_t x = 0;
  std::int64_t y = 0;
  std::int64_t width = 0;
  std::int64_t height = 0;
  if (!readIntInRange(*crop, "x", 0, 16384, x, error) ||
      !readIntInRange(*crop, "y", 0, 16384, y, error) ||
      !readIntInRange(*crop, "width", 0, 16384, width, error) ||
      !readIntInRange(*crop, "height", 0, 16384, height, error)) {
    return false;
  }
  out.crop.x = std::int32_t(x);
  out.crop.y = std::int32_t(y);
  out.crop.width = std::int32_t(width);
  out.crop.height = std::int32_t(height);
  if (!out.crop.valid()) {
    error = "crop dimensions must be positive";
    return false;
  }
  return true;
}

bool trackIndicesFromJson(const JsonValue &payload, const char *key,
                          std::vector<std::int32_t> &out,
                          std::string &error) {
  const JsonValue *tracks = arrayField(payload, key, false, error);
  if (tracks == nullptr) {
    return error.empty();
  }
  for (const auto &item : tracks->items) {
    if (item.kind != JsonValue::Kind::Int) {
      error = "track indices must be integers";
      return false;
    }
    if (item.integer < 0 || item.integer > 63) {
      error = "invalid track index";
      return false;
    }
    out.push_back(std::int32_t(item.integer));
  }
  return true;
}

bool extractAudioFromJson(const JsonValue &payload, job::ExtractAudio &out,
                          std::string &error) {
  if (payload.kind != JsonValue::Kind::Object) {
    error = "extract-audio payload must be an object";
    return false;
  }
  const JsonValue *source = nullptr;
  if ((source = objectField(payload, "source", true, error)) == nullptr ||
      !mediaSourceFromJson(*source, out.source, error)) {
    return false;
  }
  const JsonValue *output = nullptr;
  if ((output = objectField(payload, "output", true, error)) == nullptr ||
      !outputSpecFromJson(*output, out.output, error)) {
    return false;
  }
  if (!trackIndicesFromJson(payload, "trackIndices", out.trackIndices,
                            error)) {
    return false;
  }
  const JsonValue *audio = nullptr;
  if ((audio = objectField(payload, "audio", false, error)) != nullptr &&
      !audioSettingsFromJson(*audio, out.audio, error)) {
    return false;
  }
  if (const JsonValue *range = payload.find("range");
      range != nullptr && range->kind != JsonValue::Kind::Null) {
    TimeRange parsed;
    if (!timeRangeFromJson(*range, parsed, error)) {
      return false;
    }
    out.range = parsed;
  }
  const JsonValue *compression = nullptr;
  if ((compression = objectField(payload, "compression", false, error)) !=
      nullptr) {
    if (!compressionFromJson(*compression, out.compression, error)) {
      return false;
    }
  }
  const JsonValue *metadata = nullptr;
  if ((metadata = objectField(payload, "metadata", false, error)) != nullptr) {
    if (!metadataFromJson(*metadata, out.metadata, error)) {
      return false;
    }
  }
  return true;
}

bool extractFramesFromJson(const JsonValue &payload, job::ExtractFrames &out,
                           std::string &error) {
  if (payload.kind != JsonValue::Kind::Object) {
    error = "extract-frames payload must be an object";
    return false;
  }
  const JsonValue *source = nullptr;
  if ((source = objectField(payload, "source", true, error)) == nullptr ||
      !mediaSourceFromJson(*source, out.source, error)) {
    return false;
  }
  const JsonValue *output = nullptr;
  if ((output = objectField(payload, "output", true, error)) == nullptr ||
      !outputSpecFromJson(*output, out.output, error)) {
    return false;
  }
  const JsonValue *selection = objectField(payload, "selection", true, error);
  if (selection == nullptr) {
    return false;
  }
  std::string mode;
  if (!readString(*selection, "mode", mode, error)) {
    return false;
  }
  if (mode == "current") {
    out.selection.mode = FrameSelectionMode::Current;
  } else if (mode == "selected") {
    out.selection.mode = FrameSelectionMode::Selected;
  } else if (mode == "interval") {
    out.selection.mode = FrameSelectionMode::Interval;
  } else if (mode == "frame-rate") {
    out.selection.mode = FrameSelectionMode::FrameRate;
  } else if (mode == "count") {
    out.selection.mode = FrameSelectionMode::Count;
  } else if (mode == "all") {
    out.selection.mode = FrameSelectionMode::AllInRange;
  } else if (mode == "keyframes") {
    out.selection.mode = FrameSelectionMode::Keyframes;
  } else if (mode == "scenes") {
    out.selection.mode = FrameSelectionMode::SceneChanges;
  } else {
    error = "unknown frame selection mode: " + mode;
    return false;
  }
  std::int64_t interval = 0;
  if (!readInt(*selection, "intervalUs", interval, error)) {
    return false;
  }
  if (interval != 0 && (interval < 0 || interval > kUsPerDay)) {
    error = "invalid interval";
    return false;
  }
  out.selection.intervalUs = interval;
  if (const JsonValue *rate = selection->find("frameRate");
      rate != nullptr && rate->kind != JsonValue::Kind::Null) {
    if (rate->kind != JsonValue::Kind::String) {
      error = "field is not a string: frameRate";
      return false;
    }
    auto parsed = rationalFromId(rate->text);
    if (!parsed.has_value()) {
      error = "invalid sampling rate";
      return false;
    }
    out.selection.frameRate = *parsed;
  }
  std::int64_t count = 0;
  if (!readIntInRange(*selection, "count", 0, 100000, count, error)) {
    return false;
  }
  out.selection.count = std::int32_t(count);
  if (const JsonValue *stamps = arrayField(*selection, "timestampsUs", false,
                                           error);
      stamps != nullptr) {
    for (const auto &item : stamps->items) {
      if (item.kind != JsonValue::Kind::Int) {
        error = "timestamps must be integers";
        return false;
      }
      if (item.integer < 0 || item.integer >= kUsPerDay) {
        error = "invalid frame timestamp";
        return false;
      }
      out.selection.timestampsUs.push_back(item.integer);
    }
  }
  const JsonValue *range = objectField(*selection, "range", true, error);
  if (range == nullptr || !timeRangeFromJson(*range, out.selection.range,
                                             error)) {
    return false;
  }
  const JsonValue *image = objectField(payload, "image", true, error);
  if (image == nullptr) {
    return false;
  }
  std::string format;
  if (!readString(*image, "format", format, error)) {
    return false;
  }
  if (format == "jpeg") {
    out.image.format = ImageOutputRules::Format::Jpeg;
  } else if (format == "png") {
    out.image.format = ImageOutputRules::Format::Png;
  } else if (format == "webp") {
    out.image.format = ImageOutputRules::Format::WebP;
  } else {
    error = "unknown image format: " + format;
    return false;
  }
  if (!readOptIntInRange(*image, "quality", 1, 100, out.image.quality,
                         error)) {
    return false;
  }
  if (!readOptIntInRange(*image, "maxWidth", 16, 8192, out.image.maxWidth,
                         error) ||
      !readOptIntInRange(*image, "maxHeight", 16, 8192, out.image.maxHeight,
                         error)) {
    return false;
  }
  if (!readBool(*image, "sequentialNaming", out.image.sequentialNaming,
                error)) {
    return false;
  }
  return true;
}

bool extractSubtitlesFromJson(const JsonValue &payload,
                              job::ExtractSubtitles &out, std::string &error) {
  if (payload.kind != JsonValue::Kind::Object) {
    error = "extract-subtitles payload must be an object";
    return false;
  }
  const JsonValue *source = nullptr;
  if ((source = objectField(payload, "source", true, error)) == nullptr ||
      !mediaSourceFromJson(*source, out.source, error)) {
    return false;
  }
  const JsonValue *output = nullptr;
  if ((output = objectField(payload, "output", true, error)) == nullptr ||
      !outputSpecFromJson(*output, out.output, error)) {
    return false;
  }
  if (!trackIndicesFromJson(payload, "trackIndices", out.trackIndices,
                            error)) {
    return false;
  }
  if (const JsonValue *targets = arrayField(payload, "targets", false, error);
      targets != nullptr) {
    for (const auto &item : targets->items) {
      if (item.kind != JsonValue::Kind::String) {
        error = "subtitle targets must be strings";
        return false;
      }
      if (item.text == "original") {
        out.targets.push_back(SubtitleTextTarget::Original);
      } else if (item.text == "srt") {
        out.targets.push_back(SubtitleTextTarget::Srt);
      } else if (item.text == "vtt") {
        out.targets.push_back(SubtitleTextTarget::Vtt);
      } else if (item.text == "ass") {
        out.targets.push_back(SubtitleTextTarget::Ass);
      } else {
        error = "unknown subtitle target: " + item.text;
        return false;
      }
    }
  }
  return true;
}

} // namespace

std::optional<DecodedRequest> requestFromJson(const std::string &json,
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

  DecodedRequest decoded;
  {
    std::optional<std::string> submission;
    if (!readOptString(root, "submissionId", submission, error)) {
      return std::nullopt;
    }
    if (submission.has_value()) {
      if (submission->empty()) {
        error = "submissionId must not be empty when present";
        return std::nullopt;
      }
      decoded.submissionId = *submission;
    }
  }

  std::string operationNameValue;
  if (!readString(root, "operation", operationNameValue, error)) {
    return std::nullopt;
  }
  const auto operation = operationFromName(operationNameValue);
  if (!operation.has_value()) {
    error = "unknown operation: " + operationNameValue;
    return std::nullopt;
  }
  decoded.request.operation = *operation;

  // Cut & Merge: "mode" + the mode payload object.
  if (*operation == Operation::VideoCutMerge ||
      *operation == Operation::AudioCutMerge) {
    std::string mode;
    if (!readString(root, "mode", mode, error)) {
      return std::nullopt;
    }
    const JsonValue *payload = objectField(root, mode.c_str(), true, error);
    if (payload == nullptr) {
      return std::nullopt;
    }
    if (mode == "trim") {
      job::Trim trim;
      const JsonValue *source =
          objectField(*payload, "source", true, error);
      if (source == nullptr || !mediaSourceFromJson(*source, trim.source,
                                                    error)) {
        return std::nullopt;
      }
      const JsonValue *output =
          objectField(*payload, "output", true, error);
      if (output == nullptr ||
          !outputSpecFromJson(*output, trim.output, error)) {
        return std::nullopt;
      }
      const JsonValue *range =
          objectField(*payload, "range", true, error);
      if (range == nullptr || !timeRangeFromJson(*range, trim.range, error)) {
        return std::nullopt;
      }
      decoded.request.payload = std::move(trim);
    } else if (mode == "cut") {
      job::Cut cut;
      const JsonValue *source =
          objectField(*payload, "source", true, error);
      if (source == nullptr ||
          !mediaSourceFromJson(*source, cut.source, error)) {
        return std::nullopt;
      }
      const JsonValue *output =
          objectField(*payload, "output", true, error);
      if (output == nullptr ||
          !outputSpecFromJson(*output, cut.output, error)) {
        return std::nullopt;
      }
      const JsonValue *removed =
          arrayField(*payload, "removed", true, error);
      if (removed == nullptr) {
        return std::nullopt;
      }
      std::vector<TimeRange> ranges;
      for (const auto &item : removed->items) {
        TimeRange range;
        if (!timeRangeFromJson(item, range, error)) {
          return std::nullopt;
        }
        ranges.push_back(range);
      }
      // Normalize: sort ascending, merge overlapping/adjacent (mandate
      // §27.2 — the decoder produces the same canonical form the session
      // path produces).
      std::sort(ranges.begin(), ranges.end(),
                [](const TimeRange &a, const TimeRange &b) {
                  return a.startUs < b.startUs;
                });
      std::vector<TimeRange> normalized;
      for (const auto &range : ranges) {
        if (!normalized.empty() &&
            range.startUs <= normalized.back().endUs) {
          normalized.back().endUs =
              std::max(normalized.back().endUs, range.endUs);
          continue;
        }
        normalized.push_back(range);
      }
      cut.removed = std::move(normalized);
      decoded.request.payload = std::move(cut);
    } else if (mode == "split") {
      job::Split split;
      const JsonValue *source =
          objectField(*payload, "source", true, error);
      if (source == nullptr ||
          !mediaSourceFromJson(*source, split.source, error)) {
        return std::nullopt;
      }
      const JsonValue *output =
          objectField(*payload, "output", true, error);
      if (output == nullptr ||
          !outputSpecFromJson(*output, split.output, error)) {
        return std::nullopt;
      }
      const JsonValue *rules =
          objectField(*payload, "rules", true, error);
      if (rules == nullptr || !splitRulesFromJson(*rules, split.rules, error)) {
        return std::nullopt;
      }
      decoded.request.payload = std::move(split);
    } else if (mode == "merge") {
      job::Merge merge;
      const JsonValue *sources =
          arrayField(*payload, "sources", true, error);
      if (sources == nullptr) {
        return std::nullopt;
      }
      for (const auto &item : sources->items) {
        MediaSource source;
        if (!mediaSourceFromJson(item, source, error)) {
          return std::nullopt;
        }
        merge.sources.push_back(std::move(source));
      }
      if (merge.sources.size() < 2) {
        error = "merge requires at least two sources";
        return std::nullopt;
      }
      const JsonValue *output =
          objectField(*payload, "output", true, error);
      if (output == nullptr ||
          !outputSpecFromJson(*output, merge.output, error)) {
        return std::nullopt;
      }
      const JsonValue *rules =
          objectField(*payload, "rules", true, error);
      if (rules == nullptr || !mergeRulesFromJson(*rules, merge.rules, error)) {
        return std::nullopt;
      }
      if (merge.rules.layout != MergeLayout::Sequential &&
          merge.sources.size() != 2) {
        error = "spatial merge requires exactly two sources";
        return std::nullopt;
      }
      decoded.request.payload = std::move(merge);
    } else {
      error = "unknown timeline mode: " + mode;
      return std::nullopt;
    }
    return decoded;
  }

  // Every other operation: the payload object lives under the operation's
  // canonical name.
  const JsonValue *payload =
      objectField(root, operationNameValue.c_str(), true, error);
  if (payload == nullptr) {
    return std::nullopt;
  }
  switch (*operation) {
    case Operation::VideoConvert: {
      job::VideoConvert request;
      if (!videoConvertFromJson(*payload, request, error)) {
        return std::nullopt;
      }
      decoded.request.payload = std::move(request);
      break;
    }
    case Operation::AudioConvert: {
      job::AudioConvert request;
      if (!audioConvertFromJson(*payload, request, error)) {
        return std::nullopt;
      }
      decoded.request.payload = std::move(request);
      break;
    }
    case Operation::VideoCrop: {
      job::Crop request;
      if (!cropFromJson(*payload, request, error)) {
        return std::nullopt;
      }
      decoded.request.payload = std::move(request);
      break;
    }
    case Operation::ExtractAudio: {
      job::ExtractAudio request;
      if (!extractAudioFromJson(*payload, request, error)) {
        return std::nullopt;
      }
      decoded.request.payload = std::move(request);
      break;
    }
    case Operation::ExtractFrames: {
      job::ExtractFrames request;
      if (!extractFramesFromJson(*payload, request, error)) {
        return std::nullopt;
      }
      decoded.request.payload = std::move(request);
      break;
    }
    case Operation::ExtractSubtitles: {
      job::ExtractSubtitles request;
      if (!extractSubtitlesFromJson(*payload, request, error)) {
        return std::nullopt;
      }
      decoded.request.payload = std::move(request);
      break;
    }
    default:
      error = "operation does not carry a direct payload: " +
              operationNameValue;
      return std::nullopt;
  }
  return decoded;
}

// ---------------------------------------------------------------------------
// MediaInfo codec (existing bridge shape)
// ---------------------------------------------------------------------------

std::string mediaInfoToJson(const MediaInfo &info) {
  std::string out = "{";
  out += "\"durationSec\":" + std::to_string(info.durationSec);
  out += ",\"container\":" + quoted(containerName(info.container));
  out += ",\"sizeBytes\":" + std::to_string(info.sizeBytes);
  out += ",\"attachedPictures\":" + std::to_string(info.attachedPictures);
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
    out += ",\"hasClosedCaptions\":" +
           std::string(v.hasClosedCaptions ? "true" : "false");
    out += ",\"language\":" + optString(v.language.empty()
                                            ? std::optional<std::string>()
                                            : std::optional(v.language));
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

std::optional<MediaInfo> mediaInfoFromJson(const std::string &json,
                                           std::string &error) {
  error.clear();
  JsonValue root;
  JsonReader reader(json);
  const std::string parseError = reader.parse(root);
  if (!parseError.empty()) {
    error = "invalid MediaInfo JSON: " + parseError;
    return std::nullopt;
  }
  if (root.kind != JsonValue::Kind::Object) {
    error = "MediaInfo must be a JSON object";
    return std::nullopt;
  }
  MediaInfo info;
  if (const JsonValue *duration = root.find("durationSec");
      duration != nullptr && duration->isNumberLike()) {
    info.durationSec = duration->asDouble();
  }
  if (std::optional<std::string> container;
      (container = json::getOptString(root, "container")).has_value()) {
    info.container = containerFromName(*container).value_or(Container::Mp4);
  }
  if (std::optional<std::int64_t> size;
      (size = json::getOptInt(root, "sizeBytes")).has_value()) {
    info.sizeBytes = *size;
  }
  if (std::optional<std::int64_t> pics;
      (pics = json::getOptInt(root, "attachedPictures")).has_value()) {
    info.attachedPictures = int(*pics);
  }
  if (const JsonValue *video = arrayField(root, "video", false, error);
      video != nullptr) {
    for (const auto &item : video->items) {
      if (item.kind != JsonValue::Kind::Object) {
        error = "video stream must be an object";
        return std::nullopt;
      }
      VideoStream stream;
      if (std::optional<std::string> codec =
              json::getOptString(item, "codec");
          codec.has_value()) {
        stream.codec = videoCodecFromName(*codec).value_or(VideoCodec::H264);
      }
      if (auto width = json::getOptInt(item, "width")) {
        stream.width = std::int32_t(*width);
      }
      if (auto height = json::getOptInt(item, "height")) {
        stream.height = std::int32_t(*height);
      }
      if (auto frameRate = json::getOptDouble(item, "frameRate")) {
        stream.frameRate = *frameRate;
      }
      if (auto bitRate = json::getOptInt(item, "bitRate")) {
        stream.bitRate = *bitRate;
      }
      if (auto pixelFormat = json::getOptString(item, "pixelFormat")) {
        stream.pixelFormat = *pixelFormat;
      }
      if (auto hdr = json::getOptInt(item, "hdr")) {
        stream.hdr = *hdr != 0;
      }
      if (const JsonValue *cc = item.find("hasClosedCaptions");
          cc != nullptr && cc->kind == JsonValue::Kind::Bool) {
        stream.hasClosedCaptions = cc->boolValue;
      }
      if (auto language = json::getOptString(item, "language")) {
        stream.language = *language;
      }
      info.video.push_back(std::move(stream));
    }
  }
  if (!error.empty()) {
    return std::nullopt;
  }
  if (const JsonValue *audio = arrayField(root, "audio", false, error);
      audio != nullptr) {
    for (const auto &item : audio->items) {
      if (item.kind != JsonValue::Kind::Object) {
        error = "audio stream must be an object";
        return std::nullopt;
      }
      AudioStream stream;
      if (std::optional<std::string> codec =
              json::getOptString(item, "codec");
          codec.has_value()) {
        stream.codec = audioCodecFromName(*codec).value_or(AudioCodec::Aac);
      }
      if (auto channels = json::getOptInt(item, "channels")) {
        stream.channels = std::int32_t(*channels);
      }
      if (auto sampleRate = json::getOptInt(item, "sampleRate")) {
        stream.sampleRate = std::int32_t(*sampleRate);
      }
      if (auto bitRate = json::getOptInt(item, "bitRate")) {
        stream.bitRate = *bitRate;
      }
      if (auto language = json::getOptString(item, "language")) {
        stream.language = *language;
      }
      info.audio.push_back(std::move(stream));
    }
  }
  if (!error.empty()) {
    return std::nullopt;
  }
  if (const JsonValue *subs = arrayField(root, "subtitles", false, error);
      subs != nullptr) {
    for (const auto &item : subs->items) {
      if (item.kind != JsonValue::Kind::Object) {
        error = "subtitle stream must be an object";
        return std::nullopt;
      }
      SubtitleStream stream;
      if (std::optional<std::string> format =
              json::getOptString(item, "format");
          format.has_value()) {
        stream.format =
            subtitleFormatFromName(*format).value_or(SubtitleFormat::Srt);
      }
      if (auto language = json::getOptString(item, "language")) {
        stream.language = *language;
      }
      if (const JsonValue *isDefault = item.find("isDefault");
          isDefault != nullptr && isDefault->kind == JsonValue::Kind::Bool) {
        stream.isDefault = isDefault->boolValue;
      }
      if (const JsonValue *isForced = item.find("isForced");
          isForced != nullptr && isForced->kind == JsonValue::Kind::Bool) {
        stream.isForced = isForced->boolValue;
      }
      info.subtitles.push_back(std::move(stream));
    }
  }
  if (!error.empty()) {
    return std::nullopt;
  }
  return info;
}

// ---------------------------------------------------------------------------
// Job snapshot / event codec
// ---------------------------------------------------------------------------

std::string jobSnapshotToJson(const JobSnapshot &snapshot) {
  std::string out = "{";
  out += "\"id\":" + quoted(snapshot.id);
  out += ",\"submissionId\":" +
         (snapshot.submissionId.empty() ? "null"
                                        : quoted(snapshot.submissionId));
  out += ",\"operation\":" + quoted(operationName(snapshot.operation));
  out += ",\"state\":" + quoted(jobStateName(snapshot.state));
  out += ",\"progress\":" + std::to_string(snapshot.progress);
  out += ",\"sourceName\":" + quoted(snapshot.sourceName);
  out += ",\"outputName\":" + quoted(snapshot.outputName);
  out += ",\"outputPath\":" + quoted(snapshot.outputPath);
  out += ",\"outputs\":[";
  for (std::size_t i = 0; i < snapshot.outputs.size(); ++i) {
    const JobOutput &output = snapshot.outputs[i];
    if (i > 0) {
      out += ",";
    }
    out += "{\"path\":" + quoted(output.path);
    out += ",\"sizeBytes\":" + std::to_string(output.sizeBytes);
    out += ",\"kind\":" + quoted(output.kind);
    out += "}";
  }
  out += "]";
  out += ",\"error\":" +
         (snapshot.error.empty() ? "null" : quoted(snapshot.error));
  out += ",\"errorCode\":" +
         (snapshot.errorCode.empty() ? "null" : quoted(snapshot.errorCode));
  out += ",\"outputSizeBytes\":" + std::to_string(snapshot.outputSizeBytes);
  out += ",\"inputSizeBytes\":" + std::to_string(snapshot.inputSizeBytes);
  out += ",\"pipelineSummary\":" +
         (snapshot.pipelineSummary.empty() ? "null"
                                           : quoted(snapshot.pipelineSummary));
  out += ",\"createdAtMs\":" + std::to_string(snapshot.createdAtMs);
  out += ",\"finishedAtMs\":" + std::to_string(snapshot.finishedAtMs);
  out += "}";
  return out;
}

std::string jobEventToJson(const JobEvent &event) {
  std::string out = "{";
  out += "\"jobId\":" + quoted(event.jobId);
  out += ",\"state\":" + quoted(jobStateName(event.state));
  out += ",\"progress\":" + std::to_string(event.progress);
  out += ",\"outputSizeBytes\":" + std::to_string(event.outputSizeBytes);
  out += ",\"error\":" + (event.error.empty() ? "null" : quoted(event.error));
  out += ",\"errorCode\":" +
         (event.errorCode.empty() ? "null" : quoted(event.errorCode));
  out += "}";
  return out;
}

// ---------------------------------------------------------------------------
// Session payload codecs
// ---------------------------------------------------------------------------

std::string sessionSnapshotToJson(const SessionSnapshot &snapshot) {
  std::string out = "{";
  out += "\"sessionId\":" + quoted(snapshot.sessionId);
  out += ",\"operation\":" + quoted(operationName(snapshot.operation));
  out += ",\"state\":" + quoted(sessionStateName(snapshot.state));
  out += ",\"revision\":" + std::to_string(snapshot.revision);
  out += ",\"sources\":[";
  for (std::size_t i = 0; i < snapshot.sources.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    out += mediaInfoToJson(snapshot.sources[i]);
  }
  out += "]";
  out += ",\"values\":[";
  for (std::size_t i = 0; i < snapshot.values.size(); ++i) {
    const AcceptedValue &value = snapshot.values[i];
    if (i > 0) {
      out += ",";
    }
    out += "{\"field\":" + quoted(fieldIdName(value.field));
    out += ",\"valueId\":" + quoted(value.valueId);
    out += ",\"origin\":" + quoted(valueOriginName(value.origin));
    if (value.payloadJson.empty()) {
      out += ",\"payload\":null";
    } else {
      out += ",\"payload\":" + value.payloadJson;
    }
    out += "}";
  }
  out += "]";
  out += ",\"configurationValid\":" +
         std::string(snapshot.configurationValid ? "true" : "false");
  out += ",\"validationMessage\":" +
         (snapshot.validationMessage.empty()
              ? "null"
              : quoted(snapshot.validationMessage));
  out += ",\"estimate\":{\"known\":" +
         std::string(snapshot.estimate.known ? "true" : "false") +
         ",\"estimatedBytes\":" + std::to_string(snapshot.estimate.estimatedBytes) +
         ",\"lowerBoundBytes\":" +
         std::to_string(snapshot.estimate.lowerBoundBytes) +
         ",\"upperBoundBytes\":" +
         std::to_string(snapshot.estimate.upperBoundBytes) + "}";
  if (snapshot.relatedChange.has_value()) {
    const RelatedChange &change = *snapshot.relatedChange;
    out += ",\"relatedChange\":{\"field\":" + quoted(fieldIdName(change.field));
    out += ",\"reason\":" + quoted(change.reason);
    out += ",\"currentValueId\":" + quoted(change.currentValueId);
    out += ",\"proposedValueId\":" + quoted(change.proposedValueId) + "}";
  } else {
    out += ",\"relatedChange\":null";
  }
  out += ",\"samplePath\":" + optString(snapshot.samplePath);
  out += "}";
  return out;
}

std::string fieldOptionsToJson(const FieldOptions &options) {
  std::string out = "{\"field\":" + quoted(fieldIdName(options.field));
  out += ",\"values\":[";
  for (std::size_t i = 0; i < options.values.size(); ++i) {
    const OptionValue &value = options.values[i];
    if (i > 0) {
      out += ",";
    }
    out += "{\"id\":" + quoted(value.id);
    out += ",\"attributes\":{";
    bool first = true;
    for (const auto &[key, attribute] : value.attributes) {
      if (!first) {
        out += ",";
      }
      first = false;
      out += quoted(key) + ":" + quoted(attribute);
    }
    out += "}}";
  }
  out += "]}";
  return out;
}

std::string sampleResultToJson(const SampleResult &sample) {
  return "{\"path\":" + quoted(sample.outputPath) +
         ",\"startUs\":" + std::to_string(sample.startUs) +
         ",\"endUs\":" + std::to_string(sample.endUs) +
         ",\"sizeBytes\":" + std::to_string(sample.sizeBytes) + "}";
}

std::optional<SessionUpdateParse>
sessionUpdateFromJson(const std::string &json, std::string &error) {
  error.clear();
  JsonValue root;
  JsonReader reader(json);
  const std::string parseError = reader.parse(root);
  if (!parseError.empty()) {
    error = "invalid update JSON: " + parseError;
    return std::nullopt;
  }
  if (root.kind != JsonValue::Kind::Object) {
    error = "update must be a JSON object";
    return std::nullopt;
  }
  SessionUpdateParse parsed;
  std::int64_t revision = 0;
  if (!readInt(root, "revision", revision, error)) {
    return std::nullopt;
  }
  if (revision < 0) {
    error = "revision must not be negative";
    return std::nullopt;
  }
  parsed.revision = std::uint64_t(revision);
  if (const JsonValue *accept = root.find("acceptRelatedChange");
      accept != nullptr && accept->kind != JsonValue::Kind::Null) {
    if (!readBool(root, "acceptRelatedChange", parsed.acceptRelatedChange,
                  error)) {
      return std::nullopt;
    }
  }
  const JsonValue *changes = arrayField(root, "changes", true, error);
  if (changes == nullptr) {
    return std::nullopt;
  }
  if (changes->items.empty()) {
    error = "empty change set";
    return std::nullopt;
  }
  for (const auto &item : changes->items) {
    if (item.kind != JsonValue::Kind::Object) {
      error = "change must be an object";
      return std::nullopt;
    }
    FieldChange change;
    std::string field;
    if (!readString(item, "field", field, error)) {
      return std::nullopt;
    }
    const auto fieldId = fieldIdFromName(field);
    if (!fieldId.has_value()) {
      error = "unknown field: " + field;
      return std::nullopt;
    }
    change.field = *fieldId;
    if (!readString(item, "valueId", change.valueId, error)) {
      return std::nullopt;
    }
    if (change.valueId.empty()) {
      error = "valueId must not be empty";
      return std::nullopt;
    }
    if (const JsonValue *payload = item.find("payload");
        payload != nullptr && payload->kind != JsonValue::Kind::Null) {
      if (payload->kind == JsonValue::Kind::String) {
        // A string payload is raw payload text (the payloadJson contract).
        change.payloadJson = payload->text;
      } else {
        // Structured payload: re-serialize to canonical text; the value is
        // trusted only after SessionManager validation applies it.
        change.payloadJson = jsonValueToText(*payload);
      }
    }
    parsed.changes.push_back(std::move(change));
  }
  return parsed;
}

} // namespace velmux::core

namespace velmux::core {

std::string mediaInfoArrayToJson(const std::vector<MediaInfo> &infos) {
  std::string out = "[";
  for (std::size_t i = 0; i < infos.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    out += mediaInfoToJson(infos[i]);
  }
  out += "]";
  return out;
}

std::optional<std::vector<MediaInfo>>
mediaInfoArrayFromJson(const std::string &json, std::string &error) {
  error.clear();
  JsonValue root;
  JsonReader reader(json);
  const std::string parseError = reader.parse(root);
  if (!parseError.empty()) {
    error = "invalid MediaInfo array JSON: " + parseError;
    return std::nullopt;
  }
  if (root.kind != JsonValue::Kind::Array) {
    error = "MediaInfo array must be a JSON array";
    return std::nullopt;
  }
  std::vector<MediaInfo> infos;
  for (const auto &item : root.items) {
    if (item.kind != JsonValue::Kind::Object) {
      error = "MediaInfo entry must be an object";
      return std::nullopt;
    }
    // Re-serialize each entry to canonical text and decode it through the
    // shared codec (one parse path, no duplicated field logic).
    const std::string text = jsonValueToText(item);
    if (auto info = mediaInfoFromJson(text, error)) {
      infos.push_back(std::move(*info));
    } else {
      return std::nullopt;
    }
  }
  return infos;
}

} // namespace velmux::core
