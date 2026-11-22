#include "subtitle_tools.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace velmux::ffmpeg::subtitles {

namespace {

enum class Format { SubRip, Ass, WebVtt, Unsupported };

// Upper bound for subtitle sidecars processed through the shifting path:
// real-world text subtitle tracks stay far below this, and the bound keeps
// a malformed "subtitle" file from dragging the whole engine into reading
// gigabytes (spec §38: bound memory allocations).
constexpr std::uintmax_t kMaxSubtitleFileBytes = 64ull * 1024 * 1024;

Format detectFormat(const std::string &path, const std::string &content) {
  const auto dot = path.rfind('.');
  const std::string ext =
      dot == std::string::npos ? "" : path.substr(dot + 1);
  std::string lower = ext;
  for (auto &c : lower) {
    c = char(std::tolower(static_cast<unsigned char>(c)));
  }
  if (lower == "ass" || lower == "ssa") {
    return Format::Ass;
  }
  if (lower == "vtt") {
    return Format::WebVtt;
  }
  if (lower == "srt") {
    return Format::SubRip;
  }
  // No (recognized) extension: sniff the content.
  if (content.rfind("WEBVTT", 0) == 0) {
    return Format::WebVtt;
  }
  if (content.find("[Script Info]") != std::string::npos ||
      content.find("[V4+ Styles]") != std::string::npos ||
      content.find("[V4 Styles]") != std::string::npos) {
    return Format::Ass;
  }
  if (content.find("-->") != std::string::npos) {
    return Format::SubRip;
  }
  return Format::Unsupported;
}

struct Timestamp {
  std::int64_t ms = -1; // -1 = parse failure
};

bool isTsChar(char c) {
  return std::isdigit(static_cast<unsigned char>(c)) != 0 || c == ':' ||
         c == ',' || c == '.';
}

std::string formatSrtTimestamp(std::int64_t ms);
std::string formatVttTimestamp(std::int64_t ms);
std::string formatAssTimestamp(std::int64_t ms);

// Parses "HH:MM:SS,mmm" (SubRip), "HH:MM:SS.mmm" / "MM:SS.mmm" (WebVTT).
// The number of colons in the timestamp region decides the shape, so VTT's
// hourless form cannot be misread as hours.
Timestamp parseSrtVttTimestamp(const std::string &s, std::size_t pos,
                               bool allowHourless) {
  Timestamp ts;
  std::size_t end = pos;
  while (end < s.size() && isTsChar(s[end])) {
    ++end;
  }
  int colons = 0;
  for (std::size_t i = pos; i < end; ++i) {
    if (s[i] == ':') {
      ++colons;
    }
  }
  std::vector<std::int64_t> parts; // hours?, minutes, seconds
  std::size_t p = pos;
  const int expectedFields = colons + 1;
  for (int f = 0; f < expectedFields; ++f) {
    std::size_t numEnd = p;
    while (numEnd < end && std::isdigit(static_cast<unsigned char>(s[numEnd]))) {
      ++numEnd;
    }
    if (numEnd == p) {
      return ts; // missing number
    }
    std::int64_t value = 0;
    for (std::size_t i = p; i < numEnd; ++i) {
      value = value * 10 + (s[i] - '0');
    }
    parts.push_back(value);
    p = numEnd;
    if (f < expectedFields - 1) {
      if (p >= end || s[p] != ':') {
        return ts;
      }
      ++p;
    }
  }
  std::int64_t h = 0, m = 0, sec = 0, frac = -1;
  if (colons == 2) {
    h = parts[0];
    m = parts[1];
    sec = parts[2];
  } else if (colons == 1 && allowHourless) {
    m = parts[0];
    sec = parts[1];
  } else {
    return ts;
  }
  if (p < end && (s[p] == ',' || s[p] == '.')) {
    ++p;
    std::size_t fracEnd = p;
    while (fracEnd < end && std::isdigit(static_cast<unsigned char>(s[fracEnd]))) {
      ++fracEnd;
    }
    if (fracEnd == p) {
      return ts;
    }
    // The fraction is ms-precision in SRT/VTT but written with a variable
    // number of digits in the wild: derive the scale from the digit count
    // ("500" -> 5*100, "50" -> 5*10, "5" -> 5).
    const std::size_t digits = fracEnd - p;
    std::int64_t scale = 1;
    for (std::size_t i = 1; i < digits; ++i) {
      scale *= 10;
    }
    frac = 0;
    for (std::size_t i = p; i < fracEnd && scale > 0; ++i) {
      frac += (s[i] - '0') * scale;
      scale /= 10;
    }
    p = fracEnd;
  } else {
    return ts; // SRT/VTT always carry a fractional part
  }
  if (p != end) {
    return ts; // trailing junk inside the region
  }
  if (m >= 60 || sec >= 60) {
    return ts;
  }
  ts.ms = ((h * 60 + m) * 60 + sec) * 1000 + (frac < 0 ? 0 : frac);
  return ts;
}

// Parses "H:MM:SS.cc" (ASS/SSA — single-digit hours, centiseconds).
Timestamp parseAssTimestamp(const std::string &s, std::size_t pos) {
  Timestamp ts;
  std::size_t end = pos;
  // ASS fields are comma-separated: the timestamp region ends at the next
  // comma (isTsChar also accepts ',' for SRT/VTT fraction separators, so
  // it must be excluded explicitly here).
  while (end < s.size() && isTsChar(s[end]) && s[end] != ',') {
    ++end;
  }
  // Manual parse (portable, no scanf format-width pitfalls):
  // H : MM : SS . CC — every field is exactly two digits except hours.
  std::int64_t h = 0, m = 0, sec = 0, cs = 0;
  std::size_t p = pos;
  bool ok = true;
  if (p < end && std::isdigit(static_cast<unsigned char>(s[p]))) {
    h = s[p] - '0';
    ++p;
  } else {
    ok = false;
  }
  auto twoDigits = [&](std::int64_t &out) {
    if (ok && p + 1 < end &&
        std::isdigit(static_cast<unsigned char>(s[p])) &&
        std::isdigit(static_cast<unsigned char>(s[p + 1]))) {
      out = (s[p] - '0') * 10 + (s[p + 1] - '0');
      p += 2;
    } else {
      ok = false;
    }
  };
  if (ok && p < end && s[p] == ':') {
    ++p;
  } else {
    ok = false;
  }
  twoDigits(m);
  if (ok && p < end && s[p] == ':') {
    ++p;
  } else {
    ok = false;
  }
  twoDigits(sec);
  if (ok && p < end && s[p] == '.') {
    ++p;
  } else {
    ok = false;
  }
  twoDigits(cs);
  if (ok && p != end) {
    ok = false; // trailing junk (e.g. a third fraction digit)
  }
  if (!ok || m >= 60 || sec >= 60) {
    return ts;
  }
  ts.ms = ((h * 60 + m) * 60 + sec) * 1000 + cs * 10;
  return ts;
}

std::string formatSrtTimestamp(std::int64_t ms) {
  ms = std::max<std::int64_t>(ms, 0);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld,%03lld",
                static_cast<long long>(ms / 3'600'000),
                static_cast<long long>((ms / 60'000) % 60),
                static_cast<long long>((ms / 1000) % 60),
                static_cast<long long>(ms % 1000));
  return buf;
}

std::string formatVttTimestamp(std::int64_t ms) {
  std::string srt = formatSrtTimestamp(ms);
  srt[srt.size() - 4] = '.'; // ",mmm" -> ".mmm"
  return srt;
}

std::string formatAssTimestamp(std::int64_t ms) {
  ms = std::max<std::int64_t>(ms, 0);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld.%02lld",
                static_cast<long long>(ms / 3'600'000),
                static_cast<long long>((ms / 60'000) % 60),
                static_cast<long long>((ms / 1000) % 60),
                static_cast<long long>((ms % 1000) / 10));
  return buf;
}

// Rewrites "ts --> ts" pairs in a line (SRT/VTT cue header).
bool shiftArrowLine(std::string &line, std::int64_t offsetMs, bool vtt) {
  const std::size_t arrow = line.find("-->");
  if (arrow == std::string::npos) {
    return false;
  }
  // Start timestamp ends at the arrow (trim spaces).
  std::size_t startEnd = arrow;
  while (startEnd > 0 &&
         (line[startEnd - 1] == ' ' || line[startEnd - 1] == '\t')) {
    --startEnd;
  }
  std::size_t startBegin = startEnd;
  while (startBegin > 0 && isTsChar(line[startBegin - 1])) {
    --startBegin;
  }
  if (startBegin == startEnd) {
    return false;
  }
  const std::size_t afterArrow = arrow + 3;
  std::size_t endBegin = afterArrow;
  while (endBegin < line.size() &&
         (line[endBegin] == ' ' || line[endBegin] == '\t')) {
    ++endBegin;
  }
  std::size_t endEnd = endBegin;
  while (endEnd < line.size() && isTsChar(line[endEnd])) {
    ++endEnd;
  }
  if (endBegin == endEnd) {
    return false;
  }
  const Timestamp start = parseSrtVttTimestamp(line, startBegin, vtt);
  const Timestamp end = parseSrtVttTimestamp(line, endBegin, vtt);
  if (start.ms < 0 || end.ms < 0) {
    return false;
  }
  const std::string newStart =
      vtt ? formatVttTimestamp(start.ms + offsetMs)
          : formatSrtTimestamp(start.ms + offsetMs);
  const std::string newEnd =
      vtt ? formatVttTimestamp(end.ms + offsetMs)
          : formatSrtTimestamp(end.ms + offsetMs);
  // Replace the end first so the start's offsets stay valid.
  line.replace(endBegin, endEnd - endBegin, newEnd);
  line.replace(startBegin, startEnd - startBegin, newStart);
  return true;
}

// Rewrites an ASS "Dialogue:" line by shifting fields 1 (start) and 2 (end).
bool shiftAssDialogueLine(std::string &line, std::int64_t offsetMs) {
  const std::size_t colon = line.find(':');
  if (colon == std::string::npos) {
    return false;
  }
  const std::size_t fieldsBegin = colon + 1;
  std::vector<std::pair<std::size_t, std::size_t>> fields; // [begin, end)
  std::size_t begin = fieldsBegin;
  int fieldCount = 0;
  for (std::size_t i = fieldsBegin; i <= line.size(); ++i) {
    if (i == line.size() || line[i] == ',') {
      fields.emplace_back(begin, i);
      ++fieldCount;
      begin = i + 1;
      if (fieldCount == 9) {
        // The 10th field is the text and may contain commas: take the rest.
        fields.emplace_back(begin, line.size());
        break;
      }
    }
  }
  if (fields.size() < 3) {
    return false;
  }
  const Timestamp start = parseAssTimestamp(line, fields[1].first);
  const Timestamp end = parseAssTimestamp(line, fields[2].first);
  if (start.ms < 0 || end.ms < 0) {
    return false;
  }
  const std::string newStart = formatAssTimestamp(start.ms + offsetMs);
  const std::string newEnd = formatAssTimestamp(end.ms + offsetMs);
  line.replace(fields[2].first, fields[2].second - fields[2].first, newEnd);
  line.replace(fields[1].first, fields[1].second - fields[1].first, newStart);
  return true;
}

} // namespace

bool shiftSubtitleFile(const std::string &srcPath, const std::string &dstPath,
                       std::int32_t offsetMs, std::string &reason) {
  reason.clear();
  std::error_code ec;
  if (!std::filesystem::exists(srcPath, ec)) {
    reason = "subtitle file not found";
    return false;
  }
  if (std::filesystem::file_size(srcPath, ec) > kMaxSubtitleFileBytes) {
    reason = "subtitle file exceeds the 64 MiB processing bound";
    return false;
  }
  std::ifstream in(srcPath, std::ios::binary);
  if (!in) {
    reason = "cannot open subtitle file";
    return false;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  const std::string content = buffer.str();

  const Format format = detectFormat(srcPath, content);
  if (format == Format::Unsupported) {
    reason = "unsupported subtitle format (text formats: srt, ass, vtt)";
    return false;
  }

  // CRLF transparency: process line by line, preserving the original
  // terminators byte-for-byte.
  std::ofstream out(dstPath, std::ios::binary | std::ios::trunc);
  if (!out) {
    reason = "cannot write shifted subtitle file";
    return false;
  }
  const std::int64_t offset = offsetMs;
  std::size_t pos = 0;
  bool shiftedAny = false;
  while (pos <= content.size()) {
    const std::size_t eol = content.find('\n', pos);
    const std::size_t lineEnd =
        eol == std::string::npos ? content.size() : eol;
    std::string line = content.substr(pos, lineEnd - pos);
    const bool hasLf = eol != std::string::npos;

    if (format == Format::Ass) {
      if (line.rfind("Dialogue:", 0) == 0) {
        shiftedAny = shiftAssDialogueLine(line, offset) || shiftedAny;
      }
      // Comment: lines render nothing; keep their timestamps untouched.
    } else {
      if (line.find("-->") != std::string::npos) {
        shiftedAny = shiftArrowLine(line, offset, format == Format::WebVtt) ||
                     shiftedAny;
      }
    }

    out << line;
    if (hasLf) {
      out << '\n';
    }
    if (eol == std::string::npos) {
      break;
    }
    pos = eol + 1;
  }

  if (!shiftedAny) {
    // No cue timestamps found: refuse instead of writing an identical file
    // and pretending the offset was applied.
    out.close();
    std::error_code removeEc;
    std::filesystem::remove(dstPath, removeEc);
    reason = "no cue timestamps found to shift";
    return false;
  }
  return true;
}

} // namespace velmux::ffmpeg::subtitles
