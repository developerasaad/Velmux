// Velmux core — pipeline planner.
//
// Generates pipeline candidates from the intersection of the intentional
// feature registry and the runtime capability registry, validates whole
// pipeline compatibility, ranks candidates with a cost model and picks the
// best. Planning is pure computation over cached registries: no codec
// enumeration, no probing, no test encoding — warm-path planning stays in
// the microsecond range (benchmarked in core/tests/planner_tests.cpp).
#pragma once

#include "velmux/core/capability_registry.h"
#include "velmux/core/feature_registry.h"
#include "velmux/core/job.h"
#include "velmux/core/media_model.h"
#include "velmux/core/pipeline_fwd.h"
#include "velmux/core/telemetry_fwd.h"

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

struct PlanResult {
  bool feasible = false;
  std::string message; // infeasibility explanation or notes
  std::optional<PlannedPipeline> best;
  std::vector<PlannedPipeline> alternates; // ranked, best first
  // Planning wall time (telemetry).
  double planningMs = 0.0;
};

class Planner {
public:
  Planner(const FeatureRegistry &features, const CapabilityRegistry &caps,
          const ITelemetryQuery *telemetry);

  // Plan a conversion. Pure function of (source, request, registries,
  // telemetry); safe to call from any thread.
  PlanResult plan(const MediaInfo &source, const ConversionRequest &request);

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
