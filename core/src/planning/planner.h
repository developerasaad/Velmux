// Velmux core — pipeline planner.
//
// Generates pipeline candidates from the intersection of the intentional
// feature registry and the runtime capability registry, validates whole
// pipeline compatibility, ranks candidates with a cost model and picks the
// best. Planning is pure computation over cached registries: no codec
// enumeration, no probing, no test encoding — warm-path planning stays in
// the microsecond range (benchmarked in core/tests/planner_tests.cpp).
//
// Typed request mapping (mandate §15): every operation's payload is
// normalized into one internal conversion shape; the capability-gate
// architecture and cost model below are shape-driven and shared by all
// operations.
#pragma once

#include "capabilities/capabilities.h"
#include "catalog/features.h"
#include <velmux/jobs.h>
#include <velmux/media.h>
#include "compression/compression.h"
#include "processing/pipeline.h"
#include "persistence/telemetry_fwd.h"

#include <vector>

namespace velmux::core {

struct PlannedPipeline {
  Pipeline pipeline;
  // Cost model outputs (lower is better). Individual factors are kept for
  // diagnostics and telemetry.
  double cost = 0.0;
  double expectedThroughput = 0.0; // frames/sec estimate (0 = unknown)
  double setupLatencyMs = 0.0;
  double transferCostMs = 0.0;
  std::string rationale; // why this candidate ranked where it did
  std::optional<std::int64_t> estimatedOutputBytes;
};

// The narrow failure key a given (source, request, output codec) targets on
// a backend: codec + output geometry. Computed IDENTICALLY at planning
// time (to skip routes whose key has a live failure record) and at
// failure-recording time (job manager), so the two sides always agree on
// what "this configuration" means.
FailedConfigKey failedConfigKeyFor(BackendId backend, const MediaInfo &source,
                                   const JobRequest &request,
                                   VideoCodec outVideoCodec);

struct PlanResult {
  bool feasible = false;
  std::string message; // infeasibility explanation or notes
  // Stable error code for honest refusals: UnsupportedOperation for the
  // operations not yet implemented in this build (Split, spatial/sequential
  // Merge, multi-range Cut, ExtractFrames); None otherwise.
  ErrorCode errorCode = ErrorCode::None;
  std::optional<PlannedPipeline> best;
  std::vector<PlannedPipeline> alternates; // ranked, best first
  // Planning wall time (telemetry).
  double planningMs = 0.0;
};

class Planner {
public:
  Planner(const FeatureRegistry &features, const CapabilityRegistry &caps,
          const ITelemetryQuery *telemetry);

  // Plan a typed request against the inspected sources. Pure function of
  // (request, sources, registries, telemetry); safe to call from any
  // thread. The Result carries hard input errors (no usable source); the
  // PlanResult itself carries feasibility, honest refusal codes and notes.
  Result<PlanResult> plan(const JobRequest &request,
                          const std::vector<MediaInfo> &sources);

  // Cost model weights (tunable for tests/simulation; production defaults
  // live in the .cpp). A hardware stage is not assumed faster when bridging
  // to it costs more than it saves — the transfer cost term encodes this.
  struct Weights {
    double throughput = 1.0;
    double setupLatency = 0.002;   // ms of setup per unit cost
    double transferCost = 0.004;   // ms of CPU<->GPU bridging per unit cost
    double powerPenalty = 0.3;     // weight for hardware/battery usage
    double recoveryOverhead = 0.05;
    double preferenceBonus = 0.15; // processing mode alignment bonus
  };
  const Weights &weights() const { return weights_; }
  void setWeights(const Weights &w) { weights_ = w; }

private:
  Weights weights_;
  const FeatureRegistry &features_;
  const CapabilityRegistry &caps_;
  const ITelemetryQuery *telemetry_;
};

} // namespace velmux::core
