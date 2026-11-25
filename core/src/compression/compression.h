// Velmux core — compression intelligence.
//
// Goal-oriented parameter derivation: turns a user goal (Recommended,
// Smaller File, High Quality, Fit Under Size, Custom) into concrete bitrate
// budgets and quality parameters. No misleading "compression percentage":
// real arithmetic over duration, audio allocation and container overhead.
#pragma once

#include <velmux/jobs.h>
#include <velmux/media.h>

#include <optional>
#include <string>

namespace velmux::core {

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

// Compute a bitrate budget that lands under targetSizeBytes for the given
// duration. Audio allocation respects the requested audio settings when
// present. Returns a budget with feasible=false and an explanation when the
// target cannot be met (e.g. duration too short, audio alone exceeds it).
BitrateBudget computeBitrateBudget(const MediaInfo &source,
                                   const ConversionRequest &request,
                                   std::int64_t targetSizeBytes);

// Quality factor per goal kind (0..1 scale driving encoder quality params).
// Recommended balances size and quality; SmallerFile trades quality for
// size; HighQuality preserves quality.
double qualityFactorForGoal(GoalKind kind);

// Estimate output size (bytes) for a planned pipeline. Returns nullopt when
// honest estimation is not possible (e.g. unknown duration).
std::optional<std::int64_t>
estimateOutputBytes(const MediaInfo &source, const Pipeline &pipeline,
                    const ConversionRequest &request);

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
