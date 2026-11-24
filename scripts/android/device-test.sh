#!/usr/bin/env bash
# Physical Android device test run (final hardware validation).
#
# Contract:
#   scripts/android/device-test.sh [--abi <abi>]
#
# Requires a connected device with USB debugging. Builds the app for the
# device's ABI, installs it, runs the full instrumented suite, and the
# lifecycle smoke (cold launch, process death, relaunch). This is the only
# place MediaCodec/Surface behavior counts as hardware validation.
set -euo pipefail

SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=lib/common.sh
. "$SCRIPTS_DIR/lib/common.sh"

ABI=""
while [ $# -gt 0 ]; do
  case "$1" in
    --abi) shift; ABI="${1:?}" ;;
    --abi=*) ABI="${1#--abi=}" ;;
    *) vdie "unknown option: $1 (android/device-test.sh)" ;;
  esac
  shift
done

require_tool adb
DEVICES="$(adb devices 2>/dev/null | sed -n '2,$p' | grep -v '^$' | grep -w device | awk '{print $1}' || true)"
[ -n "$DEVICES" ] || vdie "no connected Android device found (adb devices shows none)"
DEVICE_COUNT="$(printf '%s\n' "$DEVICES" | wc -l)"
[ "$DEVICE_COUNT" -eq 1 ] || vdie "multiple devices connected; connect exactly one ($DEVICES)"

# Resolve the device ABI when not given.
if [ -z "$ABI" ]; then
  ABI="$(timeout 15 adb shell getprop ro.product.cpu.abi 2>/dev/null | tr -d '\r' || true)"
  case "$ABI" in
    arm64-v8a|armeabi-v7a|x86_64) ;;
    *) vdie "could not determine the device ABI (got '$ABI'); pass --abi explicitly" ;;
  esac
fi
vlog "device: $(adb devices | sed -n 2p), ABI: $ABI"

# Build + install + instrumented suite. The lifecycle smoke is included in
# scripts/android/emulator-test.sh; here the physical run additionally gets
# a real process-death pass: am crash the app, relaunch, verify recovery.
bash "$SCRIPTS_DIR/android/build.sh" --debug --abi "$ABI"

APK="$(ls "$VELMUX_ROOT/$VELMUX_APP_ANDROID_DIR"/app/build/outputs/apk/debug/*.apk 2>/dev/null | head -1 || true)"
[ -n "$APK" ] || vdie "debug APK not found"
adb install -r "$APK" > /dev/null

ANDROID_DIR="$(repo_dir "$VELMUX_APP_ANDROID_DIR")"
run_in "$ANDROID_DIR" ./gradlew connectedDebugAndroidTest --no-daemon --stacktrace \
  -PvelmuxAbis="$ABI" -PreactNativeArchitectures="$ABI" \
  -PvelmuxFFmpegPrefix="$VELMUX_ROOT/$VELMUX_FFMPEG_OUT_DIR/android" \
  -PvelmuxVersionName="$VELMUX_VERSION_NAME" \
  -PvelmuxVersionCode="$VELMUX_VERSION_CODE"

# Real process death: crash the app process and verify it relaunches.
vlog "killing the app process and relaunching (recovery smoke)"
adb_t shell am crash com.velmux.mobile 2>/dev/null || true
sleep 3
timeout 60 adb shell am start -W -n com.velmux.mobile/.MainActivity > /dev/null
sleep 12
PID="$(timeout 10 adb shell pidof com.velmux.mobile 2>/dev/null | tr -d '\r' || true)"
[ -n "$PID" ] || vdie "app did not relaunch after process death"
vlog "device test run complete (pid $PID after relaunch)"
