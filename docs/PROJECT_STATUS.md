# Velmux — Project Status

Honest, current state. Update at the end of each work session. Code and tests are
the ultimate truth.

## Milestone overview

| # | Milestone | State |
|---|---|---|
| 0 | Foundation (monorepo, RN app, C++ engine, bridge, backend skeletons, FFmpeg builder foundation, docs, CI) | In progress |
| 1 | First real Android conversion (MP4/H.264/AAC via native APIs) | Not started |
| 2 | Capability system and planner | In progress (core-level, host-validated) |
| 3 | FFmpeg toolchain (Android/iOS artifacts + manifest) | Not started (host build works) |
| 4 | Hybrid pipeline proof (N→N, F→N, N→F, F→F) | Not started |
| 5 | Complete V1 media support | Not started |
| 6 | Compression intelligence | Partial (budget math implemented + unit tested) |
| 7 | Persistence/recovery | Partial (SQLite job store + checkpoint model; fault-injection pending) |
| 8 | Background and notifications | Not started |
| 9 | Batch scheduler | Partial (resource-aware scheduler implemented + unit tested) |
| 10 | Premium UX polish | In progress (design system + screens scaffolded) |
| 11 | Performance measurement | Partial (planning micro-benchmark + telemetry) |
| 12 | Hardening | Not started |

## What actually works today

- C++ core builds on Linux with CMake/Ninja; unit tests pass (feature registry,
  capability registry, planner, compression budgets, scheduler, job state machine).
- FFmpeg backend performs REAL conversions through libav* libraries directly
  (demux → decode → process → encode → mux) on host, with output validation via
  the core validator and an independent ffprobe cross-check.
- Pinned host FFmpeg builds through `tools/ffmpeg-builder/build-host.sh`.
- React Native app source with Home/Convert/Queue/History/Settings surfaces.
- CI builds core+tests on Linux (fast signal).

## Known limitations (do not pretend otherwise)

- Android/iOS backends are skeletons: real interfaces and capability reporting,
  but no working device conversion yet (M1).
- Hybrid stage-composed execution interfaces exist; only whole-pipeline FFmpeg
  execution is implemented (M4 pending).
- No background hosts/notifications yet (M8).
- FFmpeg mobile artifacts not yet built (M3; scripts exist, CI pending).
- Recovery fault-injection test suite not yet implemented (M7 completion).

## Environment notes

- Sandbox: Linux, no Android SDK/NDK installed locally yet; CI is authoritative
  for Android/iOS builds.
- Host FFmpeg dev packages unavailable; backend builds against the pinned
  FFmpeg produced by `tools/ffmpeg-builder` (correct per spec §28).
