# Velmux — Backend Policy

Shared C++ owns policy. Backends own execution.

## Ownership

| Concern | Owner |
|---|---|
| Media inspection coordination | C++ core (native/FFmpeg inspect) |
| Codec selection | C++ core only |
| Native codec execution | platform backend |
| FFmpeg codec execution | FFmpeg backend |
| GPU video processing | platform GPU backend where beneficial |
| Software video processing | FFmpeg/software backend |
| Subtitle policy/timing | C++ core |
| ASS rendering | libass backend |
| Job lifecycle | C++ core |
| Background host | platform native |
| Notifications | platform native |
| Recovery policy | C++ core |
| Fallback decisions | C++ core only |

A backend must never independently "fall back to FFmpeg". Backends report
capabilities and errors; the C++ core decides.

## Selection model

Each stage is selected independently: demux, decode, video processing, audio
processing, subtitle processing, encode, mux, bitstream transformation.

The planner ranks complete compatible pipelines using:

- exact codec/profile support; resolution; frame rate; bit depth; pixel format
- HDR requirements; container compatibility
- native hardware acceleration availability
- observed throughput (telemetry); expected setup latency
- CPU/GPU transfer cost; CPU↔hardware frame conversion cost; memory bandwidth
- expected power usage; thermal behavior
- quality objective; requested processing mode
- recovery/checkpoint overhead

A hardware stage is not assumed faster when bridging to it costs more than it saves.

## Preference order (hard rules)

1. A valid pipeline always beats an invalid one.
2. Stream copy / remux beats re-encode when it satisfies the requested result.
3. Among re-encoding pipelines, rank by the whole-pipeline cost model, not by
   stage-local assumptions.

## Capability reporting

Backends register capabilities at init (from compiled configuration) and may refine
them at runtime (hardware probing, codec queries). Probing is never run synchronously
on user-visible paths unless cached or trivially cheap. Failure records are narrow:
specific implementation + profile + resolution + format, never whole-codec bans.
Capability/performance caches are invalidated on relevant OS/backend changes.

## Performance learning

Telemetry records decoder/encoder throughput, setup time, transfer cost, failure
patterns, unsupported configurations, thermal/concurrency observations. Used for
future planning. No benchmarking at user request time.

## FFmpeg backend rules

- Uses libavcodec/libavformat/libavutil/libavfilter/libswscale/libswresample
  directly. No `ffmpeg` CLI subprocess for production conversion.
- Built by `tools/ffmpeg-builder`: pinned upstream versions, verified checksums,
  feature-selected configuration covering the intentional catalog, per-target
  artifacts (Android/iOS/host), machine-readable capability manifest validated
  against the actual build.
- Capability reporting derives from the real compiled build (e.g. enumerating
  actual avcodec/avformat entries), not from assumptions.

## Platform minimums

- Android: API 29 (Android 10) baseline. MediaCodec/AMediaCodec,
  MediaExtractor/AMediaExtractor, MediaMuxer/AMediaMuxer, Media3, OpenGL ES,
  Vulkan where justified, NDK, Kotlin/JNI for lifecycle integration, foreground
  services, native notifications, SAF durable file access.
- Apple: frameworks VideoToolbox, AudioToolbox, AVFoundation, CoreMedia, CoreVideo,
  Metal, Objective-C++ bridging, Swift where useful, BackgroundTasks,
  UserNotifications/ActivityKit, security-scoped file access. Deployment target:
  iOS 15 (see DECISIONS.md D-004).
