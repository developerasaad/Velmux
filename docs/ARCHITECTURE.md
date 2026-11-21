# Velmux — Architecture

Three primary layers. Dependencies point downward only.

```
┌──────────────────────────────────────────────┐
│  React Native UI (apps/mobile, TypeScript)   │  presentation only
├──────────────────────────────────────────────┤
│  Bridge (bridge/, TurboModule + JSI)         │  thin, no policy
├──────────────────────────────────────────────┤
│  Shared C++ core (core/)                     │  the brain
│  feature/capability registries, planner,     │
│  job manager, scheduler, recovery, telemetry,│
│  validation, events                          │
├──────────────────────────────────────────────┤
│  Execution backends (backends/)              │  execution only
│  ffmpeg/ (libav* directly)                   │
│  android/ (MediaCodec/MediaExtractor/…)      │
│  apple/ (VideoToolbox/AVFoundation/…)        │
└──────────────────────────────────────────────┘
```

## Layer contracts

**UI must not decide**: decoder/encoder choice, FFmpeg vs native, backend speed,
codec/container compatibility, recovery strategy, checkpoint placement. It submits
user intent and observes state.

**C++ core must not depend on React Native.** It is reusable from Kotlin, Swift,
Flutter, desktop, and CLI frontends. Public API: `core/include/velmux`.

**Backends own execution, never policy.** A backend must not independently fall back
to another backend; backends report capabilities and errors, the core decides.

## Core module map

| Module | Role |
|---|---|
| `feature_registry` | What Velmux intentionally supports (product-controlled, static) |
| `capability_registry` | What the current device/build can execute (runtime, per-backend) |
| `backend_registry` | Backend identity, registration, lifecycle |
| `media_model` | Normalized inspection result (video/audio/subtitle streams, metadata) |
| `inspector` | Inspection orchestration across backends |
| `pipeline` | Pipeline model, stages, candidates, compatibility validation |
| `planner` | Candidate generation, whole-pipeline cost model, ranking |
| `compression` | Goal-oriented parameter derivation (bitrate budgets, feasibility) |
| `job`, `job_manager` | Job state machine, lifecycle, durable persistence (SQLite) |
| `scheduler` | Resource-aware concurrency (CPU/GPU/memory/thermal/power) |
| `recovery` | Checkpoint manifests, crash reconciliation, resume planning |
| `validation` | Output validation orchestration (never trust pipeline success alone) |
| `telemetry` | Device-local performance learning for future planning |
| `events` | Normalized job events consumed by UI and native notifiers |
| `engine` | `VelmuxEngine` facade — single entry point for every frontend |

## Stage model

A pipeline assigns a backend to each stage independently:
`demux · decode · video processing · audio processing · subtitle processing ·
encode · mux · bitstream transformation`.

Hybrid pipelines (e.g. FFmpeg decode → native hardware encode) are first-class; the
buffer model (compressed packet, CPU frame, native/hardware frame) defines ownership,
lifetime, memory domain, and color metadata for data crossing backend boundaries.

## Execution strategy

- Stage-composed hybrid execution is the target architecture (Milestone 4).
- Whole-pipeline executors are legitimate when one backend serves every stage of the
  best candidate (e.g. full-FFmpeg transcode, native remux).
- Stream copy / remux is checked before any transcode (never re-encode unnecessarily).

## Job persistence (SQLite)

Durable job records: stable job ID, schema version, source identity + durable access
info, requested settings, media metadata, selected pipeline, backend identities,
encoder config, checkpoint manifest, artifacts, output state, progress, timestamps,
errors, performance observations. React Native is never the source of truth.

## FFmpeg

- FFmpeg is a first-class backend using the libraries directly (libavcodec,
  libavformat, libavutil, libavfilter, libswscale, libswresample). No CLI subprocess
  architecture for production conversion.
- Built by `tools/ffmpeg-builder` from pinned upstream sources with verified
  checksums; a machine-readable capability manifest is generated from the actual
  build and validated against the Velmux feature set.
- libavdevice is intentionally excluded in V1.

## Testing

- `core/tests` — unit tests (doctest), run on host in CI.
- `backends/ffmpeg/tests` — integration tests performing real conversions on
  generated fixtures; outputs validated both by the core validator and by an
  independent `ffprobe` cross-check.
- Fault-injection recovery tests: kill during demux/encode/mux/checkpoint/commit,
  restart, reconcile, verify no duplicate/missing timeline regions.
