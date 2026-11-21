# Velmux

Velmux is a premium cross-platform video/audio/subtitle conversion and compression
application backed by a reusable media-processing framework.

> Velmux is a fast, premium, cross-platform media converter/compressor whose shared
> C++ brain constructs the best valid pipeline from native OS capabilities and a
> comprehensive feature-selected FFmpeg backend, while keeping ordinary workflows
> simple, preserving full advanced control for power users, and protecting
> long-running work as aggressively as technically possible.

## Architecture (three layers)

| Layer | Location | Responsibility |
|---|---|---|
| React Native UI | `apps/mobile` | Presentation, interaction, navigation, job state display |
| Shared C++ core | `core` | Feature registry, capability registry, pipeline planning, job/scheduler/recovery, validation |
| Execution backends | `backends/{ffmpeg,android,apple}` | Demux/decode/process/encode/mux execution behind stable C++ interfaces |

The C++ core has no React Native dependency and is reusable from Kotlin, Swift,
Flutter, desktop apps, and CLI tools via the stable `velmux::core` API
(`core/include/velmux`).

## Repository layout

```
docs/                  Canonical documents (start with docs/PROJECT_STATUS.md)
core/                  Shared C++ core (the "brain")
backends/ffmpeg/       FFmpeg backend using libav* libraries directly (no CLI subprocess)
backends/android/      Android native backend (MediaCodec/MediaExtractor/MediaMuxer, NDK)
backends/apple/        Apple backend (VideoToolbox/AVFoundation/CoreMedia)
bridge/                React Native TurboModule/JSI bridge
apps/mobile/           React Native application (TypeScript)
tools/ffmpeg-builder/  Reproducible, pinned, feature-selected FFmpeg build system
testdata/              Test fixture generation and fixtures
.github/workflows/     CI (authoritative cross-platform validation)
```

## Building and testing the C++ core (host, Linux/macOS)

Requirements: CMake >= 3.21, Ninja, C++20 compiler, SQLite3 dev package,
and a host FFmpeg build (see below).

```bash
# Build a pinned host FFmpeg through the Velmux builder (one time)
tools/ffmpeg-builder/build-host.sh

# Configure, build, and test the core + backends + tests
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DVELMUX_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## Current status

See `docs/PROJECT_STATUS.md` for the honest, up-to-date milestone state.

## Documentation

- `docs/PRODUCT.md` — product identity and mission
- `docs/ARCHITECTURE.md` — layer architecture and module map
- `docs/FEATURE_MATRIX.md` — supported codecs, containers, subtitle formats
- `docs/BACKEND_POLICY.md` — backend selection and ownership policy
- `docs/PERFORMANCE.md` — performance targets and measurements
- `docs/RECOVERY.md` — durability, checkpoints, crash reconciliation
- `docs/DESIGN_SYSTEM.md` — UI design direction
- `docs/DECISIONS.md` — decision log
- `docs/PROJECT_STATUS.md` — current state (start here)
- `docs/AGENT_PROTOCOL.md` — how engineering agents cooperate on this repo
