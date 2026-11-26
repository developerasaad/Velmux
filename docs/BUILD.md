# Building Velmux

Everything substantial is repository-owned: one build policy
(`scripts/lib/config.sh`), one FFmpeg builder with pinned versions
(`scripts/ffmpeg-builder`), and per-target entry scripts. CI workflows
only orchestrate these scripts; the same commands work locally.

## Prerequisites

| Target | Requirements |
|---|---|
| Host engine + tests | Linux or macOS, CMake >= 3.21, Ninja, C++20 compiler, Python 3, pkg-config, meson (dav1d/fribidi), autoconf/automake (zimg autogen), SQLite3 dev headers (or the vendored amalgamation), make, curl |
| Shared Kotlin UI | JDK 21, Gradle wrapper (bundled) |
| Android app | Host requirements + Android SDK 35 + NDK 27.1.12297006 (auto-resolved via `ANDROID_HOME`/sdkmanager) |
| iOS app | macOS + Xcode 16+, JDK 21, cmake, meson/nasm/autoconf/automake available via brew when the FFmpeg artifacts are built cold (the builder builds a pinned libtool itself when the host ships no libtoolize) |

No Node.js, CocoaPods or React Native tooling is involved anywhere.

## FFmpeg dependency construction

`scripts/ffmpeg-builder/versions.json` pins every dependency version and
SHA-256. The builder compiles the full feature-selected stack per target:

- `scripts/ffmpeg-builder/build-host.sh` -> `out/host`
- `scripts/ffmpeg-builder/build-android.sh <abi>...` -> `out/android/<abi>`
- `scripts/ffmpeg-builder/build-ios.sh` -> `out/ios/{iphoneos,iphonesimulator}`

The scripts are idempotent, resumable per library, and record a
`BUILD-IDENTITY` hash (covering the builder scripts, versions, target
flags, NDK revision/SDK, deployment target). `scripts/ffmpeg/fetch.sh
<target>` is the resolver every app build uses: it reuses artifacts only
when the recorded identity matches the current tree and rebuilds
otherwise. CI caches the builder outputs under the exact identity key, so
cache reuse and artifact identity can never disagree.

FFmpeg is mandatory for every app build: a missing or stale prefix is a
fatal error, never a silent capability reduction.

## Build commands

```bash
./scripts/build.sh --android --release              # arm64-v8a APK + AAB
./scripts/build.sh --android --arm64-v8a --release  # explicit ABI
./scripts/build.sh --android --ios --release        # both platforms
./scripts/build.sh --ios --release                  # device .app (unsigned in CI)
```

Platform details:

- **Android** (`scripts/android/build.sh`): resolves FFmpeg per ABI, then
  drives Gradle `assembleRelease`/`bundleRelease` for
  `platform/android/host` (namespace `com.velmux.mobile`, minSdk 29). The
  native engine + JNI adapter build through CMake
  (`platform/android/interop/CMakeLists.txt`) against the per-ABI FFmpeg
  prefix, with `ANDROID_STL=c++_shared` matching the FFmpeg dependency
  closure (AGP packages `libc++_shared.so`). The script verifies every
  produced APK actually packages `libvelmux_jni.so` and
  `libc++_shared.so` for every requested ABI.
- **iOS** (`scripts/ios/build.sh`): drives the repository-owned Xcode
  project (`platform/ios/host/Velmux.xcodeproj`, scheme `Velmux`). The
  project's "Prepare engine and framework" phase calls
  `scripts/ios/prepare-xcode-inputs.sh` — the same path a developer runs
  locally — which resolves FFmpeg, builds the CMake engine archives for
  the target platform, links the Kotlin Compose framework, and stages
  everything under `build/ios/stage/<platform>/<configuration>/`:
  `engine/libvelmux_*.a`, `framework/VelmuxShared.framework` and the merged
  `ffmpeg/libffmpeg_deps.a` (the app links one symbol-indexed archive per
  slice; an xcframework would be validated at build-description time,
  before the phase could produce it). The phase environment is sanitized
  (SDKROOT/deployment targets dropped) so host-side tool builds in the
  dependency chain are never cross-contaminated.

## Artifact locations

| Artifact | Location |
|---|---|
| Host engine + tests | `build/host` (CMake) |
| Host FFmpeg | `scripts/ffmpeg-builder/out/host` |
| Android FFmpeg | `scripts/ffmpeg-builder/out/android/<abi>` |
| iOS FFmpeg | `scripts/ffmpeg-builder/out/ios/<slice>` |
| Android APK/AAB | `platform/android/host/build/outputs/{apk,bundle}/release` |
| iOS simulator .app | `platform/ios/host/build/Build/Products/Release-iphonesimulator` |
| iOS device .app | `platform/ios/host/build-device/Build/Products/Release-iphoneos` |
| iOS staged inputs | `build/ios/stage/<platform>/<configuration>` (generated) |

Generated source, build and download trees under
`scripts/ffmpeg-builder/{src,out,downloads}` and all `build/` trees are
gitignored and never committed.

## CI responsibilities

The workflows (`.github/workflows/`) are thin: checkout, toolchain setup,
identity-keyed caches, one script invocation per concern, artifact
uploads. `core.yml` runs the host suite; `mobile.yml` the shared Kotlin
compile + JVM tests; `android.yml` the release build; `android-emulated.yml`
the API 29/34 instrumented suites; `ios.yml` the simulator suite + device
build; `release.yml` the tag-driven gated release pipeline. Nothing
substantial is implemented in YAML.
