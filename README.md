# Velmux

Velmux is a fully offline, cross-platform video and audio converter,
compressor and focused media-processing application.

The user specifies the desired result. An authoritative C++ core determines
which configurations are supported for each source, validates every request,
and executes the accepted operation — assembling FFmpeg-based and native
OS codec stages under one deterministic planner. A shared Compose
Multiplatform UI renders the core's authoritative state and forwards user
intentions; native Android and iOS hosts provide the operating-system
services (file access, foreground execution, notifications, hardware
codecs).

## Architecture (three layers)

| Layer | Location | Responsibility |
|---|---|---|
| Shared UI (Kotlin/Compose Multiplatform) | `ui/mobile` | Navigation, presentation, gestures, display of authoritative core state |
| C++ core | `core` | Media models, capability catalog, configuration sessions, planning, processing, jobs, persistence, recovery, validation, structured errors |
| Native platform integration | `platform/android`, `platform/ios` | App hosts, JNI/C-interop bridges, file access, foreground services, MediaCodec/VideoToolbox stages |

The integration contract between the shared UI and the C++ core is a frozen,
versioned C ABI (`core/include/velmux/abi.h`) consumed through JNI on
Android and Kotlin/Native cinterop on iOS. The core is independently
testable on the host (Linux/macOS) without Compose or a mobile app.

## Repository layout

```
ui/mobile/            Shared Kotlin Multiplatform / Compose Multiplatform UI
core/                 The authoritative C++ engine (include/, src/, tests/)
platform/android/     Android host, interop (JNI + CMake), services, native stages, tests
platform/ios/         iOS host (Xcode project), interop (cinterop + ObjC++), native stages, tests
third_party/          Vendored SQLite amalgamation
scripts/              The repository-owned build/test system (build.sh, test.sh, ffmpeg-builder, ...)
docs/                 Documentation
testdata/             Test fixture generation
.github/workflows/    CI (thin orchestration over the repository scripts)
```

## Building and testing

All substantial build logic lives in repository-owned scripts; CI only
orchestrates them. The same commands work locally (macOS for Apple/iOS
work, Linux or macOS for the host and Android builds).

```bash
# Host engine + FFmpeg + the full ctest suite (Linux/macOS)
./scripts/test.sh

# Shared Kotlin/Compose compile + JVM tests (host verification target)
./scripts/test.sh --ui

# Android release build (mandatory FFmpeg resolution, per-ABI)
./scripts/build.sh --android --release
./scripts/build.sh --android --arm64-v8a --release

# Android instrumented suites on real emulators
./scripts/test.sh --android-emulator --api 29
./scripts/test.sh --android-emulator --api 34

# iOS (requires macOS + Xcode)
./scripts/build.sh --ios --release
./scripts/test.sh --ios-sim
./scripts/test.sh --ios-device        # requires a connected, signed device
```

See `docs/BUILD.md` for prerequisites, dependency construction, and
artifact locations; `docs/TESTING.md` for what each suite verifies.

## FFmpeg

The pinned, feature-selected FFmpeg and its dependencies (x264, x265,
SVT-AV1, dav1d, libvpx, opus, vorbis, mp3lame, libass, harfbuzz, fribidi,
freetype, zimg, libwebp) are built by `scripts/ffmpeg-builder` for the host,
Android ABIs, and Apple device/simulator slices. Artifacts are reused only
when a recorded build identity matches the current tree; FFmpeg is
mandatory for every app build (there is intentionally no "without FFmpeg"
product).

## Verification status

The authoritative, continuously verified state is maintained by GitHub
Actions (Linux runners for host/Android work, macOS runners for Apple
work): host engine tests including real-media processing, Android release
builds, Android API 29/34 emulator instrumented suites, and the iOS
simulator suite + device build. See `docs/TESTING.md` and
`docs/FEATURE_MATRIX.md` for what is implemented and verified, and for the
honest device-conditional limitations (physical-device codec behavior is
only validated on physical devices).

## License

See `LICENSE`, `docs/RELEASE.md` and the third-party notices there. The
linked dependency set (GPL-enabled x264/x265) imposes corresponding
obligations on distributed binaries; nothing nonfree is enabled.
