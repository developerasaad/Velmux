# Velmux — Architecture

Three primary layers. Dependencies point downward only.

```
┌─────────────────────────────────────────────────────────────┐
│  Shared UI (ui/mobile, Kotlin Multiplatform + Compose)      │ presentation only
│  Home / workflow screens / Jobs / Settings                   │
├─────────────────────────────────────────────────────────────┤
│  Kotlin engine client (shared, ui/mobile engine/)            │ one client, one
│  snapshots, revisions, submission idempotency                │ event pump
├───────────────┬─────────────────────────────────────────────┤
│  JNI (Android)│  Kotlin/Native cinterop (iOS)                │ thin transports
├───────────────┴─────────────────────────────────────────────┤
│  Versioned C ABI (core/include/velmux/abi.h)                 │ the only bridge
├─────────────────────────────────────────────────────────────┤
│  C++ core (core/)                                            │ the brain
│  catalog/capabilities, configuration sessions, planner,      │
│  processing (FFmpeg + hybrid stages), jobs/scheduler,        │
│  persistence (SQLite), recovery, validation, events, errors  │
├─────────────────────────────────────────────────────────────┤
│  Native platform integration (platform/)                     │ execution + OS
│  android: MediaCodec stages, foreground service, SAF         │
│  apple: VideoToolbox/AVFoundation stages, BGTask, UNUser...  │
└─────────────────────────────────────────────────────────────┘
```

## Layer contracts

**UI must not decide**: decoder/encoder choice, FFmpeg vs native routing,
codec/container compatibility, compression parameters, recovery strategy,
checkpoint placement. It renders authoritative state and submits user
intent through configuration sessions.

**The C++ core is the single source of media truth.** It determines
supported options for each source, validates every request (UI path and
injected direct requests alike), plans deterministic routes, executes,
validates outputs, and owns the structured error contract. One engine
instance per application process; UI detachment never creates another.

**Platform code supplies capabilities.** The Kotlin JNI adapter and the
Kotlin/Native cinterop bindings are dumb transports (JSON in, JSON out,
explicit string ownership). The ObjC++ host (`VelmuxEngineHost`) and the
Android services (`ConversionService`, `EngineHolder`) provide OS
services — background execution, notifications, the thermal/battery/
memory resource feed — against the SAME process engine. They never
re-implement media policy.

## Integration contract

`core/include/velmux/abi.h` is the frozen, versioned C ABI: opaque
validated handles, explicit ABI/schema versions, structured errors,
sequenced events, configuration revisions, explicit string ownership
(`velmux_free_string`). Every payload crosses as UTF-8 JSON and passes
the same authoritative core validation — there is no permissive back
door. The Kotlin side binds it twice: `VelmuxJni` (RegisterNatives,
Android) and `com.velmux.cinterop` (generated from
`platform/ios/interop/velmux.def`, iOS). On iOS the host-provided
`velmux_ios_shared_engine` gives the Kotlin client and the ObjC++
services the ONE process engine handle.

## Core module map

| Module | Role |
|---|---|
| `catalog` | Intentional product feature + codec/container vocabulary (static, core-owned) |
| `capabilities` | What the current build/device can execute (runtime, per-backend) |
| `configuration` | Configuration sessions: revisions, provenance, transactional updates, related changes |
| `compression` | Goal-oriented parameter derivation (quality presets, bitrate budgets, target-size solver) |
| `planning` | Deterministic, capability-aware routing (stream copy vs processing, FFmpeg vs native stages) |
| `processing` | FFmpeg backend (direct libav*) + hybrid stage composition + operation handlers |
| `jobs` | Job state machine, lifecycle, scheduler, normalized events |
| `persistence` | Durable job store (SQLite, schema v4) + telemetry |
| `recovery` | Checkpoints, crash reconciliation, resume planning |
| `validation` | Output validation (independent structural/decoding checks; never trust pipeline success alone) |
| `api` | Serialization (typed request/response JSON), the C facade, engine entry |

Public headers stay shallow (`velmux/*.h`); private headers live beside
their implementations under `core/src/`.

## Stage model

A pipeline assigns an implementation to each stage independently:
`demux · decode · video processing · audio processing · subtitle
processing · encode · mux · bitstream transformation`. A single job may
mix stream copying, FFmpeg software stages and native hardware stages
(the hybrid executor composes them); the planner picks routes by
capability and simple established priorities, never by user selection.

## Jobs and durability

One authoritative job store; in-memory snapshots and observable state
are caches. Job states: queued, preparing, processing, pausing, paused,
canceling, interrupted, recovering, finalizing, validating, completed,
target-not-met, failed, canceled. Completion requires successful output
validation. The scheduler is resource-aware (bounded lanes for
preparation/inspection/interactive work/processing) and consumes the
platform resource feed. Recovery reuses genuinely valid completed work
(completed parts/images, staged ranges, multipass analysis) and
invalidates checkpoints when the source or accepted configuration
changes.

## Error contract

One structured core error contract with stable canonical codes
(`core/include/velmux/types.h`), carrying stage, field, conflicting
values, recoverability and the configuration revision. The same JSON
envelope `{"error":{"code","message"}}` crosses every boundary; the
Kotlin client discriminates it from legitimate `error` DATA fields
(job snapshots carry `"error":null`).
