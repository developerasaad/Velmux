# Testing Velmux

Testing is part of implementation; every suite below runs in CI through
the repository scripts, and the same commands work locally.

```bash
./scripts/test.sh                            # host suite (default)
./scripts/test.sh --ui                       # shared Kotlin/Compose (JVM target)
./scripts/test.sh --android-emulator --api 29
./scripts/test.sh --android-emulator --api 34
./scripts/test.sh --ios-sim                  # macOS + Xcode
./scripts/test.sh --ios-device               # macOS + connected signed iPhone
./scripts/test.sh --android-device           # connected Android device
```

## Host suite (Linux/macOS)

`scripts/test.sh` resolves the pinned host FFmpeg (identity-checked reuse
or build), configures the full C++ tree against it, builds every
registered suite and runs ctest. Coverage:

- **Core unit suites** (doctest): capability and feature registries,
  compression policies, engine lifecycle, job store persistence, planner
  routing, recovery, scheduler, configuration sessions.
- **FFmpeg backend integration** (`core/tests/ffmpeg`): 60 real-media test
  cases over generated fixtures — conversions, cut/split/merge, frame
  extraction, subtitle extraction — with independent output probing, not
  producer self-checks.
- **C ABI bridge tests** (`core/tests/abi`): the JSON facade exercised
  exactly like a platform host (request validation, job lifecycle,
  structured errors), against the real engine and FFmpeg backend.
- A capability manifest is generated from the ACTUAL built libraries
  (`velmux-ffmpeg-probe`) and validated against the Velmux feature
  requirements, so a build manifest can never masquerade as a capability.

## Shared Kotlin UI (`--ui`)

Compiles the shared Compose Multiplatform code and runs the JVM test
target: the engine-client contract (event coalescing, snapshot
reconciliation, stale-revision handling, submission idempotency), the
JSON payload codecs round-tripping the C++ wire format, and jobs state
mapping — including the error-envelope discrimination tests (a job
snapshot's legitimate `"error": null` is data, not an error envelope).

The Android target compiles through the product build; the iOS targets
through the macOS framework build (this JVM suite is the host
verification target).

## Android instrumented suites (`--android-emulator`)

Real emulators (API 29 and API 34, x86_64 google_apis images) run the
instrumented suites in `platform/android/tests` against the app the
release path builds:

- **NativeConversionTest**: real MediaCodec H.264+AAC fixtures generated
  on-device, converted through the same JNI facade the app uses, outputs
  validated with an independent MediaExtractor probe.
- **HybridConversionTest**: the three hybrid execution categories
  (FFmpeg/Android stage compositions) through the real core
  HybridExecutor with real media.
- **FileAccessTest**: FileProvider output sharing (exact bytes, MIME
  types, no access outside the managed tree).

The suite also verifies the cold launch (the engine-init log marker —
the process must reach a working engine, not merely a live UI) and a
relaunch after termination.

## iOS simulator suite (`--ios-sim`)

On a macOS runner: the Release simulator app build through the Xcode
project (whose prepare phase resolves/builds FFmpeg, the engine archives
and the Kotlin framework), simulator boot, cold launch with the
engine-init marker check, the `VelmuxEngineTests` XCTest suite (real
whole-pipeline transcodes, stage-composed hybrid pipelines over
VideoToolbox/AVFoundation, lifecycle honesty), and a relaunch check.

The simulator is a development/integration environment. It is NOT
physical-hardware codec validation; see `--ios-device` for that.

## Device suites

`--ios-device` and `--android-device` run the same engine suites on
connected physical hardware (signing required) and never fake hardware
validation: with no device attached they fail with an explicit message.

## Sanitizers

`./scripts/test.sh --sanitize` reconfigures a separate build tree
(`build/host-sanitize`) with AddressSanitizer + UndefinedBehaviorSanitizer
instrumentation and runs the full host suite under it (abort on first
finding). The prebuilt FFmpeg libraries stay uninstrumented — the mode
exists to find Velmux-code memory and undefined-behavior defects; leak
detection is disabled because the uninstrumented dependency's one-time
global allocations would otherwise dominate exit-time reports. CI runs
this as a dedicated job next to the regular host suite.
