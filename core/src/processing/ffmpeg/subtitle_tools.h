// Velmux FFmpeg backend — text subtitle timestamp shifting.
//
// Applies a user-requested timing offset (spec §14: "adjust subtitle
// timing") to text subtitle files: SubRip (.srt), Advanced SubStation
// Alpha (.ass/.ssa) and WebVTT (.vtt). Burn-in pipelines materialize a
// shifted copy before handing the file to the libass-backed `subtitles`
// filter; preserve pipelines shift packet timestamps directly instead.
//
// Parsing is deliberately conservative: a file whose timestamps cannot be
// parsed is reported as unsupported (false), never silently mangled.
#pragma once

#include <cstdint>
#include <string>

namespace velmux::ffmpeg::subtitles {

// Shifts every cue timestamp of the text subtitle file at `srcPath` by
// `offsetMs` (positive = later) and writes the result to `dstPath`.
// Timestamps that would become negative are clamped to zero (the cue
// survives; its tail is trimmed by the same amount).
// Returns false without writing when the format is unsupported or the
// file cannot be parsed; `reason` describes why.
bool shiftSubtitleFile(const std::string &srcPath, const std::string &dstPath,
                       std::int32_t offsetMs, std::string &reason);

} // namespace velmux::ffmpeg::subtitles
