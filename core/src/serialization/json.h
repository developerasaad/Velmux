// Velmux core — shared defensive JSON reader for untrusted input.
//
// Extracted from the C ABI implementation: the session payloads and the
// ABI transport use the same established parser (mandate §35.1: retain
// useful existing serialization). Scope: objects, arrays, strings,
// integers, doubles, booleans and null, with defensive limits — input is
// untrusted host/platform input. Surrogate pairs are decoded for \u
// escapes.
#pragma once

#include <cerrno>
#include <optional>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace velmux::core::json {

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
  // Owns the text: callers may pass temporaries (the reference-based
  // original dangled for non-SSO temporaries; ownership removes the
  // lifetime trap).
  explicit JsonReader(std::string text) : text_(std::move(text)) {}

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

  std::string text_;
  std::size_t pos_ = 0;
  int depth_ = 0;
};

// -- typed extraction helpers ------------------------------------------------

inline std::string getString(const JsonValue &v, const char *key,
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

inline std::optional<std::string> getOptString(const JsonValue &v,
                                               const char *key) {
  const JsonValue *field = v.find(key);
  if (field == nullptr || field->kind == JsonValue::Kind::Null) {
    return std::nullopt;
  }
  if (field->kind != JsonValue::Kind::String) {
    return std::nullopt;
  }
  return field->text;
}

inline bool getBool(const JsonValue &v, const char *key, bool fallback,
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

inline std::optional<std::int64_t> getOptInt(const JsonValue &v,
                                              const char *key) {
  const JsonValue *field = v.find(key);
  if (field == nullptr || field->kind == JsonValue::Kind::Null) {
    return std::nullopt;
  }
  if (!field->isNumberLike()) {
    return std::nullopt;
  }
  return field->asInt();
}

inline std::optional<double> getOptDouble(const JsonValue &v,
                                          const char *key) {
  const JsonValue *field = v.find(key);
  if (field == nullptr || field->kind == JsonValue::Kind::Null) {
    return std::nullopt;
  }
  if (!field->isNumberLike()) {
    return std::nullopt;
  }
  return field->asDouble();
}



// Escape a string for JSON embedding (control characters as \u00XX).
inline std::string escape(const std::string &text) {
  std::string out;
  out.reserve(text.size() + 2);
  for (unsigned char c : text) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
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
          std::snprintf(buf, sizeof(buf), "\\u%04x", unsigned(c));
          out += buf;
        } else {
          out.push_back(char(c));
        }
    }
  }
  return out;
}

} // namespace velmux::core::json
