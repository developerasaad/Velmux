// Velmux core — compression intelligence.
//
// Goal-oriented parameter derivation: turns a compression intent
// (CompressionPreset: HighQuality / Balanced / SmallerFile / TargetSize /
// Custom, mandate §20) into concrete bitrate budgets and quality
// parameters. No misleading "compression percentage": real arithmetic over
// duration, audio allocation and container overhead.
#pragma once

#include <velmux/configuration.h>
#include <velmux/jobs.h>
#include <velmux/media.h>

#include <optional>
#include <string>

namespace velmux::core {

struct Pipeline; // processing/pipeline.h

// Container overhead factor estimate (muxing overhead as fraction of payload
// bitrate). Conservative values; validated against real outputs over time.
double containerOverheadFactor(Container container);

// Result of a budget computation.
struct BitrateBudget {
  std::int64_t videoBitrate = 0;  // bits per second, 0 = lossless/copy
  std::int64_t audioBitrate = 0;
  bool audioStreamCopy = false;   // audio kept as-is when it already fits
  bool feasible = true;
  std::string message; // human-readable feasibility explanation
};

// The request-derived inputs a budget/estimate needs, independent of the
// operation payload that carried them (VideoConvert / AudioConvert /
// ExtractAudio all reduce to this).
struct BudgetInputs {
  Container outputContainer = Container::Mp4;
  bool audioEnabled = true;
  bool videoEnabled = true;
  AudioSettings audio;               // requested settings (fields optional)
  std::optional<VideoCodec> videoCodec;
  std::optional<std::int32_t> resolutionHeight;
  std::optional<double> frameRate;
  // Effective timeline scaling (playbackSpeed): output duration is
  // duration / speed. Nullopt = 1.0.
  std::optional<double> playbackSpeed;
};

// Resolved rate-control policy derived from a CompressionIntent (mandate
// §20): presets map to the user quality scale (85/60/35), TargetSize
// computes a real budget, Custom honors explicit values, and a disabled
// intent on a forced transcode falls back to high-fidelity defaults
// (CRF ~18 per codec family).
struct ResolvedRatePolicy {
  enum class Mode : std::uint8_t { Quality, Bitrate };
  Mode mode = Mode::Quality;
  std::int32_t quality = 65;  // 0..100 user quality scale (Quality mode)
  std::int64_t bitRate = 0;   // bits per second (Bitrate mode)
};

// Compute a bitrate budget that lands under targetSizeBytes for the given
// duration. Audio allocation respects the requested audio settings when
// present. Returns a budget with feasible=false and an explanation when the
// target cannot be met (e.g. duration too short, audio alone exceeds it).
BitrateBudget computeBitrateBudget(const MediaInfo &source,
                                   const BudgetInputs &inputs,
                                   std::int64_t targetSizeBytes);

// Resolve the concrete rate policy for a compression intent. When the intent
// is disabled (or the preset resolves to quality mode without an explicit
// Custom quality) the returned policy is quality-mode with the preset's
// scale; TargetSize produces bitrate mode via a real budget when feasible.
ResolvedRatePolicy resolveRatePolicy(const MediaInfo &source,
                                     const CompressionIntent &compression,
                                     const BudgetInputs &inputs);

// Quality factor per preset (0..1 scale driving encoder quality params).
// Balanced trades size and quality; SmallerFile trades quality for size;
// HighQuality preserves quality.
double qualityFactorForPreset(CompressionPreset preset);

// Estimate output size (bytes) for a planned pipeline. Returns nullopt when
// honest estimation is not possible (e.g. unknown duration).
std::optional<std::int64_t>
estimateOutputBytes(const MediaInfo &source, const Pipeline &pipeline,
                    const BudgetInputs &inputs,
                    const ResolvedRatePolicy &rate);

// Resolution ladder: canonical heights offered by the UI (Original, 2160p,
// ... 360p, Custom). Widths derive from aspect ratio.
bool isCanonicalResolutionHeight(std::int32_t height);
// Pick a sensible output height for a target bitrate budget (may lower
// resolution when the bitrate cannot sustain the source resolution).
std::int32_t
resolutionHeightForBudget(std::int32_t sourceHeight,
                          const std::optional<std::int32_t> &requestedHeight,
                          std::int64_t videoBitrate);

} // namespace velmux::core
