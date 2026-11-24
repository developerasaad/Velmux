#!/usr/bin/env bash
# Build the Velmux Android app (APK; AAB additionally in release mode).
#
# Contract:
#   scripts/android/build.sh [--release|--debug] [--abi <a,b>] [--jobs N]
#     [--signing <keystore> <storepass> <alias> <keypass>]
#
# FFmpeg is mandatory: the script resolves matching artifacts first
# (reuse only on a verified build identity) and fails when they cannot be
# produced. There is no "build without the FFmpeg backend" path.
#
# Environment: VELMUX_VERSION_NAME / VELMUX_VERSION_CODE (defaults in
# scripts/lib/config.sh), ANDROID_HOME/ANDROID_NDK_HOME (auto-resolved).
set -euo pipefail

SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=lib/common.sh
. "$SCRIPTS_DIR/lib/common.sh"

BUILD_TYPE="debug"
ABIS="$VELMUX_ANDROID_DEFAULT_ABI"
JOBS="${VELMUX_FFMPEG_JOBS:-$(nproc 2>/dev/null || echo 2)}"
SIGNING=()
while [ $# -gt 0 ]; do
  case "$1" in
    --release) BUILD_TYPE="release" ;;
    --debug) BUILD_TYPE="debug" ;;
    --abi) shift; ABIS="${1:?}" ;;
    --abi=*) ABIS="${1#--abi=}" ;;
    --jobs) shift; JOBS="${1:?}" ;;
    --jobs=*) JOBS="${1#--jobs=}" ;;
    --signing)
      shift
      [ $# -ge 4 ] || vdie "--signing requires <keystore> <storepass> <alias> <keypass>"
      SIGNING=(-PvelmuxSigningKeystore="$1"
               -PvelmuxSigningStorePassword="$2"
               -PvelmuxSigningKeyAlias="$3"
               -PvelmuxSigningKeyPassword="$4")
      shift 3
      ;;
    *) vdie "unknown option: $1 (android/build.sh)" ;;
  esac
  shift
done

ABIS="$(echo "$ABIS" | tr ',' ' ')"
require_tool java
resolve_android_ndk
ensure_js_deps

# ---------------------------------------------------------------------------
# Mandatory FFmpeg artifacts (identity-checked reuse or build).
# ---------------------------------------------------------------------------
export VELMUX_FFMPEG_JOBS="$JOBS"
# shellcheck source=../ffmpeg/fetch.sh
bash "$SCRIPTS_DIR/ffmpeg/fetch.sh" android $ABIS
FFMPEG_PREFIX="$VELMUX_ROOT/$VELMUX_FFMPEG_OUT_DIR/android"

# ---------------------------------------------------------------------------
# Gradle assembly. -PvelmuxAbis keeps the engine/ABI sets identical to the
# FFmpeg artifacts; every ABI listed must have been resolved above.
# ---------------------------------------------------------------------------
ANDROID_DIR="$(repo_dir "$VELMUX_APP_ANDROID_DIR")"
# Task LIST as an array: a quoted "assembleRelease bundleRelease" string
# would be passed to gradlew as ONE argument, and Gradle would abort with
# "Task 'assembleRelease bundleRelease' not found" (the first release run
# through this script failed exactly that way).
GRADLE_TASKS=(assembleDebug)
[ "$BUILD_TYPE" = "release" ] && GRADLE_TASKS=(assembleRelease bundleRelease)

vlog "building Android app ($BUILD_TYPE, ABIs: $ABIS)"
run_in "$ANDROID_DIR" ./gradlew "${GRADLE_TASKS[@]}" --no-daemon --stacktrace \
  -PvelmuxAbis="$(echo "$ABIS" | tr ' ' ',')" \
  -PvelmuxFFmpegPrefix="$FFMPEG_PREFIX" \
  -PvelmuxVersionName="$VELMUX_VERSION_NAME" \
  -PvelmuxVersionCode="$VELMUX_VERSION_CODE" \
  "${SIGNING[@]}"

vlog "Android build complete ($BUILD_TYPE)"
