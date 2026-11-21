// Velmux backend contracts — the stable C++ interfaces execution backends
// implement. The core owns policy; backends own execution. A backend must
// never decide fallbacks or product behavior — it reports capabilities and
// errors, the core decides.
//
// Buffer model (spec §10): media data crosses stage boundaries in one of
// three explicit representations — compressed packet, CPU frame, native/
// hardware frame — with explicit ownership, lifetime, memory domain and
// timestamps. Whole-pipeline executors keep buffers entirely inside the
// backend; stage-composed hybrid execution (Milestone 4) moves BufferHandles
// between backends under the checkpoint coordinator's control.
#pragma once

#include "velmux/core/capability_registry.h"
#include "velmux/core/job.h"
#include "velmux/core/media_model.h"
#include "velmux/core/pipeline_fwd.h"
#include "velmux/core/types.h"

#include <atomic>
#include <functional>
#include <memory>

namespace velmux::backend {

using velmux::core::MediaKind;

// ---------------------------------------------------------------------------
// Buffer model
// ---------------------------------------------------------------------------

enum class BufferKind : std::uint8_t {
  CompressedPacket, // demux / stream copy / decoder input / encoder output / mux
  CpuFrame,         // software decode/encode, CPU filters
  HardwareFrame,    // hardware decode, GPU processing, hardware encode
};

enum class BufferDomain : std::uint8_t {
  CpuMemory,
  AndroidSurface,  // ImageReader/Surface/Texture
  CoreVideoPixelBuffer,
  MetalTexture,
};

struct BufferDescriptor {
  BufferKind kind = BufferKind::CompressedPacket;
  BufferDomain domain = BufferDomain::CpuMemory;
  MediaKind media = MediaKind::Video;
  // Presentation timestamp in stream timebase microseconds; INT64_MIN when
  // not applicable (e.g. muxed compressed packet handled with its own ts).
  std::int64_t ptsUs = 0;
  std::int64_t durationUs = 0;
  // Format description owned by the producing backend (e.g. pixel format,
  // sample format). Consumers must not free it; lifetime is bound to the
  // buffer handle.
  const void *format = nullptr;
};

// Opaque buffer handle. Ownership is explicit: a handle owns its reference
// and releases it on destruction. Moving transfers ownership.
class IBufferHandle {
public:
  virtual ~IBufferHandle() = default;
  virtual const BufferDescriptor &descriptor() const = 0;
};

using BufferPtr = std::unique_ptr<IBufferHandle>;

// ---------------------------------------------------------------------------
// Execution control
// ---------------------------------------------------------------------------

// Cooperative cancellation token shared across a job's execution.
class CancelToken {
public:
  void cancel() { cancelled_.store(true, std::memory_order_relaxed); }
  bool isCancelled() const {
    return cancelled_.load(std::memory_order_relaxed);
  }

private:
  std::atomic<bool> cancelled_{false};
};

// Progress callback. fraction is 0..1 or negative when unknown (never fake
// precision). progressBytes/durationBytes carry the durable, measurable
// quantities when known (0 = unknown).
struct ProgressReport {
  double fraction = -1.0;
  std::int64_t processedBytes = 0;
  std::int64_t totalBytes = 0;
  std::int64_t processedDurationUs = 0;
  std::int64_t totalDurationUs = 0;
};

using ProgressCallback = std::function<void(const ProgressReport &)>;

// A committed checkpoint as seen by the executor. The recovery system
// validates artifacts before trusting them; the executor only reports.
struct CheckpointInfo {
  core::StageId stage = core::StageId::Demux;
  // Position in the durable coordinate system of the stage: byte offset for
  // stream copy, packet index for remux, presentation timestamp for
  // transcode segments.
  std::int64_t position = 0;
  // Durable artifact (finalized+validated segment) committed by this
  // checkpoint, empty when the checkpoint carries no artifact.
  std::string artifactPath;
};

// Interface the executor uses to persist checkpoints during execution. The
// coordinator implements the crash-safe protocol (write temp -> finalize ->
// validate -> flush -> promote -> record); executors call commit() only after
// a segment is finalized and safely on disk.
class ICheckpointCoordinator {
public:
  virtual ~ICheckpointCoordinator() = default;
  // Returns false when the commit itself failed (job should stop with an
  // error) or when the job was cancelled.
  virtual bool commit(const CheckpointInfo &info) = 0;
};

// ---------------------------------------------------------------------------
// Inspection
// ---------------------------------------------------------------------------

class IInspector {
public:
  virtual ~IInspector() = default;
  // Inspect a source file. Fills `out` with the normalized media model.
  // Returns an Error on failure. Must not decode; metadata/demux level only.
  virtual core::Error inspect(const core::MediaSource &source,
                              core::MediaInfo &out) = 0;
};

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

struct ExecuteParams {
  core::Pipeline pipeline;       // complete stage assignment (policy decided)
  core::ConversionRequest request; // what the user asked for
  std::string outputPath;        // final output location (core-owned)
  std::shared_ptr<CancelToken> cancel;
  ProgressCallback onProgress;
  // May be null for short jobs where checkpointing is not warranted.
  ICheckpointCoordinator *checkpoints = nullptr;
};

struct ExecuteResult {
  bool success = false;
  core::Error error;
  // Real performance observations; fed into telemetry by the core.
  double wallClockSec = 0.0;
  std::int64_t framesProcessed = 0;
  std::int64_t bytesWritten = 0;
};

// Whole-pipeline executor: runs every stage of `pipeline` that is assigned
// to THIS backend. When a pipeline assigns stages to multiple backends the
// core either finds a single backend able to serve all of them (v1) or
// composes execution stage-by-step (Milestone 4 target).
class IPipelineExecutor {
public:
  virtual ~IPipelineExecutor() = default;
  // Can this backend execute every stage assigned to it in `pipeline` for
  // `request`? Must be cheap (no probing, no test encodes).
  virtual bool canExecute(const core::Pipeline &pipeline,
                          const core::ConversionRequest &request) const = 0;
  virtual ExecuteResult execute(const ExecuteParams &params) = 0;
};

// ---------------------------------------------------------------------------
// Backend entry point
// ---------------------------------------------------------------------------

class IBackend {
public:
  virtual ~IBackend() = default;

  virtual core::BackendId id() const = 0;
  virtual std::string name() const = 0;

  // One-time initialization. Expensive work (loading shared libraries,
  // enumerating codecs) happens here, at engine startup — never on the
  // user-visible conversion path.
  virtual core::Error initialize() = 0;

  // Capability report derived from the real implementation (e.g. the actual
  // compiled FFmpeg codec set, the actual MediaCodec queries).
  virtual core::BackendCapabilities capabilities() const = 0;

  virtual IInspector *inspector() = 0;
  virtual IPipelineExecutor *executor() = 0;
};

} // namespace velmux::backend
