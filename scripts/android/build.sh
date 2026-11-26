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
# settings.gradle.kts includes :platform:android:host only when an Android
# SDK is visible (ANDROID_HOME/ANDROID_SDK_ROOT). resolve_android_ndk
# exports it on the sdkmanager path only; with a bare ANDROID_NDK_HOME the
# module set would silently lack the app — fail with the cause instead.
if [ -z "${ANDROID_HOME:-}" ] && [ -z "${ANDROID_SDK_ROOT:-}" ]; then
  vdie "ANDROID_HOME or ANDROID_SDK_ROOT is not set: the Gradle build cannot include :platform:android:host"
fi

# ---------------------------------------------------------------------------
# Mandatory FFmpeg artifacts (identity-checked reuse or build).
# ---------------------------------------------------------------------------
export VELMUX_FFMPEG_JOBS="$JOBS"
# shellcheck source=../ffmpeg/fetch.sh
bash "$SCRIPTS_DIR/ffmpeg/fetch.sh" android $ABIS
FFMPEG_PREFIX="$VELMUX_ROOT/$VELMUX_FFMPEG_OUT_DIR/android"

# ---------------------------------------------------------------------------
# Gradle assembly. The multiplatform build lives at the repository root
# (settings.gradle.kts includes :platform:android:host — and the shared
# :ui:mobile androidTarget — when an Android SDK is present, which this
# script guarantees through resolve_android_ndk). -PvelmuxAbis keeps the
# engine/ABI sets identical to the FFmpeg artifacts: every ABI listed must
# have been resolved above.
# ---------------------------------------------------------------------------
HOST_TASK_PREFIX=":platform:android:host"
# Task LIST as an array: a quoted "assembleRelease bundleRelease" string
# would be passed to gradlew as ONE argument, and Gradle would abort with
# "Task 'assembleRelease bundleRelease' not found" (the first release run
# through this script failed exactly that way).
GRADLE_TASKS=("${HOST_TASK_PREFIX}:assembleDebug")
[ "$BUILD_TYPE" = "release" ] && \
  GRADLE_TASKS=("${HOST_TASK_PREFIX}:assembleRelease" "${HOST_TASK_PREFIX}:bundleRelease")

vlog "building Android app ($BUILD_TYPE, ABIs: $ABIS)"
GRADLE_ARGS=(
  -PvelmuxAbis="$(echo "$ABIS" | tr ' ' ',')"
  -PvelmuxFFmpegPrefix="$FFMPEG_PREFIX"
  -PvelmuxVersionName="$VELMUX_VERSION_NAME"
  -PvelmuxVersionCode="$VELMUX_VERSION_CODE"
)

run_in "$VELMUX_ROOT" ./gradlew "${GRADLE_TASKS[@]}" --no-daemon --stacktrace \
  "${GRADLE_ARGS[@]}" "${SIGNING[@]}"

# ---------------------------------------------------------------------------
# Product verification: every produced APK must carry the app's engine
# library for every requested ABI — libvelmux_jni.so (the JNI bridge with
# the C++ engine statically linked inside: RegisterNatives registration
# runs from JNI_OnLoad, so its absence is an instant load failure). A
# build that silently packages none, or only some ABIs, must fail here
# instead of in the field.
# ---------------------------------------------------------------------------
check_apk_native_libs() {
  local apk="$1"
  python3 - "$apk" $ABIS <<'PYEOF'
import sys, zipfile

apk, abis = sys.argv[1], sys.argv[2:]
required = {"libvelmux_jni.so"}
z = zipfile.ZipFile(apk)
names = {n.rsplit("/", 1)[-1]: n for n in z.namelist() if n.startswith("lib/")}
missing = []
for abi in abis:
    for lib in sorted(required):
        entry = f"lib/{abi}/{lib}"
        if entry not in z.namelist():
            missing.append(entry)
if missing:
    print("ERROR: native libraries missing from " + apk + ": "
          + ", ".join(missing)
          + "; packaged lib entries: " + (", ".join(sorted(names.values())) or "none"),
          file=sys.stderr)
    sys.exit(1)
print("verified " + apk + ": " + ", ".join(
    f"lib/{a}/{l}" for a in abis for l in sorted(required)))
PYEOF
}

APK_DIR="$VELMUX_ROOT/$VELMUX_ANDROID_HOST_DIR/build/outputs/apk/$BUILD_TYPE"
for apk in "$APK_DIR"/*.apk; do
  [ -f "$apk" ] || continue
  check_apk_native_libs "$apk"
done

vlog "Android build complete ($BUILD_TYPE)"
