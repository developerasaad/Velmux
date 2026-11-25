// Core-owned hybrid (stage-composed) executor — Milestone 4.
//
// Composes a pipeline whose stages are assigned to DIFFERENT backends:
// the decode backend produces CPU frames in the negotiated interchange
// format; the encode/mux backend consumes them. Compressed data never
// crosses backends (the encoder's packets go straight into its own
// muxer), so bitstream incompatibilities cannot arise (spec §10/§31).
//
// v1 scope, enforced here and by the planner:
//   - video transcode only (no video stream copy; remux stays
//     whole-pipeline);
//   - no geometry or frame-rate change;
//   - audio is either absent or stream-copied by the muxing backend;
//   - subtitles: Remove only (Preserve/BurnIn need whole-pipeline
//     backends — refusing here is what keeps "never silently dropped"
//     true);
//   - no checkpointing: a hybrid run restarts from zero on retry. The
//     whole-pipeline executors keep the segmented-recovery duty; hybrid
//     execution exists for pipelines that would otherwise have no valid
//     route at all.
#pragma once

#include "processing/contracts/contracts.h"

#include <vector>

namespace velmux::core {

class HybridExecutor {
public:
  explicit HybridExecutor(std::vector<backend::IBackend *> backends);

  // Cheap structural check: stage shape, request shape, and stage-factory
  // availability. No probing, no test encodes (spec §4.2).
  bool canExecute(const Pipeline &pipeline,
                  const ConversionRequest &request) const;

  backend::ExecuteResult execute(const backend::ExecuteParams &params);

private:
  backend::IBackend *decodeBackendFor(const Pipeline &pipeline) const;
  backend::IBackend *muxBackendFor(const Pipeline &pipeline) const;

  std::vector<backend::IBackend *> backends_;
};

} // namespace velmux::core
