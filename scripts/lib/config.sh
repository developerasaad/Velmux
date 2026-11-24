#!/usr/bin/env bash
# Canonical Velmux build policy (single source of truth).
#
# Consumed by every script under scripts/ and indirectly by CI (which only
# calls the scripts). Native build systems (Gradle/CMake/Xcode/FFmpeg
# configure) keep their implementation details but read their POLICY from
# here through the wrapper scripts.
#
# Keep this file small and factual: pins, defaults, locations.

# ---------------------------------------------------------------------------
# Repository layout (relative to the repository root)
# ---------------------------------------------------------------------------
VELMUX_APP_ANDROID_DIR="apps/mobile/android"
VELMUX_APP_IOS_DIR="apps/mobile/ios"
VELMUX_APP_JS_DIR="apps/mobile"
VELMUX_FFMPEG_BUILDER_DIR="tools/ffmpeg-builder"
VELMUX_FFMPEG_OUT_DIR="${VELMUX_FFMPEG_OUT_DIR:-tools/ffmpeg-builder/out}"
VELMUX_HOST_BUILD_DIR="build/host"

# ---------------------------------------------------------------------------
# Android
# ---------------------------------------------------------------------------
# Product floor (docs/DECISIONS.md D-003).
VELMUX_ANDROID_MIN_API="29"
# The default ABI for product and release builds (spec: Android ARM64 by
# default). The other ABIs remain supported through --abi.
VELMUX_ANDROID_DEFAULT_ABI="arm64-v8a"
# Every ABI the engine supports (kept buildable; not built by default).
VELMUX_ANDROID_ALL_ABIS="arm64-v8a armeabi-v7a x86_64"
# The emulator test job builds only the emulator's ABI.
VELMUX_ANDROID_EMULATOR_ABI="x86_64"
# NDK used by BOTH the app build (gradle ext.ndkVersion) and the FFmpeg
# cross-build. Must match apps/mobile/android/build.gradle.
VELMUX_ANDROID_NDK_VERSION="27.1.12297006"
# Emulator system images used by the automated test job (google_apis, not
# "default": the API 29 default image hangs during headless boot).
VELMUX_ANDROID_EMULATOR_APIS="29 34"
VELMUX_ANDROID_EMULATOR_IMAGE_API29="system-images;android-29;google_apis;x86_64"
VELMUX_ANDROID_EMULATOR_IMAGE_API34="system-images;android-34;google_apis;x86_64"

# ---------------------------------------------------------------------------
# iOS
# ---------------------------------------------------------------------------
VELMUX_IOS_DEPLOYMENT_TARGET="15.0"
# Production target. The simulator is a development/test target only
# (scripts/test.sh --ios-sim), never a normal build.sh product.
VELMUX_IOS_DEVICE_ARCH="arm64"
# Simulator destination used by the automated simulator test job.
VELMUX_IOS_SIMULATOR_DEVICE="iPhone 16"

# ---------------------------------------------------------------------------
# Versions
# ---------------------------------------------------------------------------
# FFmpeg and dependency pins live in tools/ffmpeg-builder/versions.json
# (the single source for those); this file never duplicates them.
# App version defaults (CI may override through gradle/xcodebuild properties).
VELMUX_VERSION_NAME="${VELMUX_VERSION_NAME:-0.1.0-rc.2}"
VELMUX_VERSION_CODE="${VELMUX_VERSION_CODE:-1}"

# ---------------------------------------------------------------------------
# FFmpeg artifact identity
# ---------------------------------------------------------------------------
# The identity hash covers: versions.json (all pins), the builder scripts,
# the target/ABI and the relevant policy knobs (API level, deployment
# target, feature switches). Artifacts are reused ONLY when the recorded
# identity matches the current tree; anything else rebuilds.
VELMUX_FFMPEG_IDENTITY_INPUTS_HOST="build-host.sh lib.sh versions.json"
VELMUX_FFMPEG_IDENTITY_INPUTS_ANDROID="build-android.sh lib.sh versions.json"
VELMUX_FFMPEG_IDENTITY_INPUTS_IOS="build-ios.sh lib.sh versions.json"
