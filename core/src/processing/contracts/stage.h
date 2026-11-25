// Stage-composition contracts (Milestone 4): backends that can serve
// individual pipeline stages expose workers behind these interfaces, and
// the core-owned hybrid executor composes them across backend boundaries.
//
// Interchange model (spec §10): the only object that crosses a backend
// boundary in v1 hybrid pipelines is the CPU VIDEO FRAME. Compressed data
// never crosses backends — the encoding backend's own packets go straight
// into its own muxer — so bitstream/extradata incompatibilities between
// backends cannot arise. Audio that cannot stream-copy through the muxing
// backend disqualifies hybrid candidates in the planner (single-backend
// pipelines keep serving those).
#pragma once

#include "processing/pipeline.h"
#include <velmux/types.h>

#include <cstdint>
#include <memory>

namespace velmux::backend {

using velmux::core::Container;
using velmux::core::Error;
using velmux::core::VideoCodec;

// ---------------------------------------------------------------------------
// Interchange formats
// ---------------------------------------------------------------------------

// The v1 CPU-frame interchange set. Backends negotiate the single format
// they will produce/consume; the planner only builds hybrid candidates
// where producer and consumer share one.
enum class InterchangePixel : std::uint8_t {
  Yuv420P, // planar 4:2:0, 8-bit — universal software domain
};

struct InterchangeFormat {
  InterchangePixel pixel = InterchangePixel::Yuv420P;
  int width = 0;
  int height = 0;
};

// ---------------------------------------------------------------------------
// CPU frame (cross-backend interchange object)
// ---------------------------------------------------------------------------

// A decoded video frame in CPU memory. Plane layout follows the negotiated
// InterchangePixel format: Yuv420P has 3 planes (Y, U, V), 2x2 subsampled.
// The handle owns its storage; lifetime ends when the handle is destroyed
// or (for producer-side recycling) replaced by the next pull.
class ICpuFrame {
public:
  virtual ~ICpuFrame() = default;
  virtual int planeCount() const = 0;
  // May be null only when the producer reported a plane it cannot expose;
  // consumers treat that as a hard error (never guess).
  virtual const std::uint8_t *planeData(int plane) const = 0;
  virtual int planeLinesize(int plane) const = 0;
  // Presentation timestamp on the source's microsecond timeline.
  virtual std::int64_t ptsUs() const = 0;
  virtual std::int64_t durationUs() const = 0;
};

using CpuFramePtr = std::unique_ptr<ICpuFrame>;

// ---------------------------------------------------------------------------
// Compressed packet view (same-backend only: encoder -> muxer)
// ---------------------------------------------------------------------------

struct PacketView {
  const std::uint8_t *data = nullptr;
  std::size_t size = 0;
  bool keyframe = false;
  std::int64_t ptsUs = 0;
  std::int64_t durationUs = 0;
  // Codec configuration data (e.g. H.264/HEVC parameter sets) produced by
  // the encoder before the first frame; the muxer attaches it to the track.
  const std::uint8_t *configData = nullptr;
  std::size_t configSize = 0;
};

// ---------------------------------------------------------------------------
// Stage workers
// ---------------------------------------------------------------------------

// Video decode stage: source file -> CPU frames in the negotiated format.
class IVideoStageDecoder {
public:
  virtual ~IVideoStageDecoder() = default;

  // Opens the source and prepares decoding of the first video stream.
  // `pixel` is the interchange format the consumer needs; the decoder must
  // convert (its own scaling, when needed) or refuse.
  virtual Error open(const std::string &sourcePath,
                     InterchangePixel pixel) = 0;
  virtual InterchangeFormat outputFormat() const = 0;
  // Source duration on the microsecond timeline (0 when unknown).
  virtual std::int64_t durationUs() const = 0;
  // Pulls the next decoded frame. Returns an error with kind None and a
  // null frame at end of stream.
  virtual Error next(CpuFramePtr &frame) = 0;
};

struct StageEncoderConfig {
  VideoCodec codec = VideoCodec::H264;
  int width = 0;
  int height = 0;
  double frameRate = 0.0; // 0 = derive from the stream
  std::int64_t bitRate = 0;
  // Quality-oriented mode when bitRate == 0 and quality has a value (0..100).
  std::optional<int> quality;
  bool useHardware = false;
  // Set by the executor from the mux stage: containers like MP4 need the
  // codec configuration in the header instead of in-band.
  bool globalHeader = false;
};

// Video encode stage: CPU frames -> compressed packets.
class IVideoStageEncoder {
public:
  virtual ~IVideoStageEncoder() = default;

  virtual Error open(const StageEncoderConfig &config,
                     InterchangePixel pixel) = 0;
  // Pushes one frame (must match the negotiated interchange format).
  // Encoders may buffer; callers interleave push with pull.
  virtual Error push(const ICpuFrame &frame) = 0;
  // Pulls one encoded packet. Returns an error with kind None and an empty
  // view when no packet is ready yet.
  virtual Error pull(PacketView &packet) = 0;
  // Flushes remaining packets. Call once after the last frame.
  virtual Error flush() = 0;
};

// Mux stage: owns the output container. Video packets arrive from the same
// backend's encoder; audio is stream-copied by the muxer itself from the
// source when requested (compressed audio never crosses backends either).
class IMuxStage {
public:
  virtual ~IMuxStage() = default;

  struct TrackOptions {
    VideoCodec videoCodec = VideoCodec::H264;
    Container container = Container::Mp4;
    int width = 0;                  // negotiated output geometry
    int height = 0;
    double frameRate = 0.0;         // 0 = 30
    bool copyAudio = false;         // stream-copy the source's first audio
                                    // track; impossible -> open() refuses
    std::string sourcePath;         // for the audio copy (same file)
  };

  virtual Error open(const std::string &outputPath,
                     const TrackOptions &tracks) = 0;
  // Whether the container needs codec configuration in the stream header
  // (e.g. MP4 avcC). The executor asks before opening the encoder.
  virtual bool wantsGlobalHeader() const = 0;
  virtual Error writeVideoPacket(const PacketView &packet) = 0;
  virtual Error finalize() = 0;
};

// ---------------------------------------------------------------------------
// Stage participation
// ---------------------------------------------------------------------------

// Backends implementing this participate in hybrid (stage-composed)
// pipelines. Factories return null when the backend cannot serve that
// stage at all; per-request refusals happen in open().
class IStageBackend {
public:
  virtual ~IStageBackend() = default;
  virtual IVideoStageDecoder *createVideoDecoder() = 0;
  virtual IVideoStageEncoder *createVideoEncoder() = 0;
  virtual IMuxStage *createMux() = 0;
};

} // namespace velmux::backend
