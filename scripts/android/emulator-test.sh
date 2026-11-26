#!/usr/bin/env bash
# Automated Android emulator test run (development testing, not a product
# build target).
#
# Contract:
#   scripts/android/emulator-test.sh --api <29|34> [--abi x86_64]
#
# What this does, in order:
#   1. resolve the SDK/NDK and fix KVM permissions when needed;
#   2. install the pinned emulator + google_apis system image;
#   3. create a lean AVD and boot it (visible, timeout-wrapped polling —
#      `adb shell` against a half-booted device blocks forever without
#      timeouts, which once ate an entire CI job silently);
#   4. build the app for the emulator ABI ONLY (FFmpeg mandatory: the
#      x86_64 artifacts are resolved or built through the identity-checked
#      fetcher);
#   5. install, cold launch, and verify: process stays alive, activity
#      resumed, the engine init marker proves the Compose app reached a
#      working engine surface (libvelmux_jni loaded, the facade answers),
#      no fatal errors in logcat;
#   6. run the full instrumented engine suite (real conversions, hybrid
#      stage tests, checkpoint recovery) through gradle;
#   7. force-stop, relaunch, verify alive again (lifecycle/persistence
#      smoke; deep recovery is covered by the instrumented suite);
#   8. collect bounded diagnostics on any failure.
#
# Emulator codec behavior is NOT physical-hardware validation (spec §9).
set -euo pipefail

SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=lib/common.sh
. "$SCRIPTS_DIR/lib/common.sh"
# validate_api_level (and the ABI/policy helpers) live in the shared
# target library — the missing source was the CI failure behind the
# "validate_api_level: command not found" exit 127.
# shellcheck source=lib/targets.sh
. "$SCRIPTS_DIR/lib/targets.sh"

API=""
ABI="$VELMUX_ANDROID_EMULATOR_ABI"
AVD_NAME="velmux-api${API}"
# Diagnostics land in a fixed, CI-uploadable location (overridable for
# local runs). collect_diagnostics is EXIT-trapped: bounded copies only.
DIAG_DIR="${VELMUX_EMULATOR_DIAG_DIR:-/tmp/diag}"
EMU_LOG=""

usage() {
  cat <<'EOF'
Usage: scripts/android/emulator-test.sh --api <29|34> [--abi x86_64]
EOF
}
while [ $# -gt 0 ]; do
  case "$1" in
    --api) shift; API="${1:?}" ;;
    --api=*) API="${1#--api=}" ;;
    --abi) shift; ABI="${1:?}" ;;
    --abi=*) ABI="${1#--abi=}" ;;
    *) usage >&2; vdie "unknown option: $1" ;;
  esac
  shift
done
[ -n "$API" ] || { usage >&2; vdie "--api is required (tested set: $VELMUX_ANDROID_EMULATOR_APIS)"; }
validate_api_level "$API"
AVD_NAME="velmux-api${API}"

case "$API" in
  29) IMAGE="$VELMUX_ANDROID_EMULATOR_IMAGE_API29" ;;
  34) IMAGE="$VELMUX_ANDROID_EMULATOR_IMAGE_API34" ;;
  *) vdie "no pinned emulator image for API $API" ;;
esac

collect_diagnostics() {
  local rc=$?
  # Collect only on failure: a green run must not pay the bugreport tax
  # (up to 90s) or litter CI with diagnostic artifacts.
  [ "$rc" -eq 0 ] && return 0
  [ -n "$DIAG_DIR" ] || return 0
  mkdir -p "$DIAG_DIR"
  vlog "collecting diagnostics into $DIAG_DIR"
  [ -n "$EMU_LOG" ] && cp "$EMU_LOG" "$DIAG_DIR/" 2>/dev/null || true
  adb_t shell getprop > "$DIAG_DIR/getprop.txt" 2>/dev/null || true
  adb_t devices -l > "$DIAG_DIR/devices.txt" 2>/dev/null || true
  adb_t logcat -d > "$DIAG_DIR/logcat.txt" 2>/dev/null || true
  # Bounded: bugreport against a half-booted emulator can hang.
  timeout 90 adb bugreport "$DIAG_DIR/bugreport.zip" > /dev/null 2>&1 || true
  cp -R "$VELMUX_ROOT/$VELMUX_ANDROID_HOST_DIR/build/outputs/androidTest-results" \
     "$DIAG_DIR/" 2>/dev/null || true
  cp -R "$VELMUX_ROOT/$VELMUX_ANDROID_HOST_DIR/build/reports" \
     "$DIAG_DIR/" 2>/dev/null || true
}
trap collect_diagnostics EXIT

# ---------------------------------------------------------------------------
# SDK, KVM, emulator image
# ---------------------------------------------------------------------------
SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
if [ -z "$SDK" ] && [ -d /usr/local/lib/android/sdk ]; then
  SDK=/usr/local/lib/android/sdk
fi
[ -n "$SDK" ] && [ -d "$SDK" ] || vdie "Android SDK not found (set ANDROID_HOME)"
export ANDROID_HOME="$SDK"
export ANDROID_SDK_ROOT="$SDK"

SDKMANAGER="$SDK/cmdline-tools/latest/bin/sdkmanager"
if [ ! -x "$SDKMANAGER" ]; then
  SDKMANAGER="$(find "$SDK/cmdline-tools" -name sdkmanager -type f 2>/dev/null | head -1 || true)"
fi
[ -n "$SDKMANAGER" ] && [ -x "$SDKMANAGER" ] || vdie "sdkmanager not found under $SDK"

# KVM: the runner user is regularly NOT in the kvm group; the emulator
# refuses to start ("x86_64 emulation currently requires hardware
# acceleration") and adb then blocks forever on wait-for-device.
if [ -e /dev/kvm ]; then
  if [ ! -r /dev/kvm ] || [ ! -w /dev/kvm ]; then
    vlog "fixing /dev/kvm permissions (runner user lacks kvm group access)"
    if command -v sudo >/dev/null 2>&1; then
      sudo chmod 666 /dev/kvm 2>/dev/null || true
    else
      chmod 666 /dev/kvm 2>/dev/null || true
    fi
  fi
  [ -r /dev/kvm ] && [ -w /dev/kvm ] \
    || vdie "/dev/kvm exists but is not accessible; add the user to the kvm group"
else
  vdie "/dev/kvm not found: x86_64 emulator images require hardware acceleration"
fi

EMU_BIN="$SDK/emulator/emulator"
ADB="$SDK/platform-tools/adb"
if [ ! -x "$ADB" ]; then
  vlog "installing platform-tools"
  yes | "$SDKMANAGER" --licenses > /dev/null 2>&1 || true
  "$SDKMANAGER" --install "platform-tools" > /dev/null
fi
if [ ! -x "$EMU_BIN" ] || ! "$SDKMANAGER" --list_installed 2>/dev/null | grep -q "^${IMAGE//;/\\;}" \
   && [ ! -d "$SDK/system-images/android-$API" ]; then
  vlog "installing emulator and system image ($IMAGE)"
  yes | "$SDKMANAGER" --licenses > /dev/null 2>&1 || true
  "$SDKMANAGER" --install "emulator" "platform-tools" "$IMAGE" > /dev/null
fi
export PATH="$SDK/emulator:$SDK/platform-tools:$PATH"

# ---------------------------------------------------------------------------
# AVD creation (lean profile: the heavy default profile slows headless CI)
# ---------------------------------------------------------------------------
AVD_HOME="${ANDROID_AVD_HOME:-$HOME/.android/avd}"
mkdir -p "$AVD_HOME"
export ANDROID_AVD_HOME="$AVD_HOME"
if [ ! -f "$AVD_HOME/$AVD_NAME.avd/config.ini" ]; then
  vlog "creating AVD $AVD_NAME"
  echo no | "$SDK/cmdline-tools/latest/bin/avdmanager" create avd \
    --name "$AVD_NAME" --package "$IMAGE" --sdcard 2048M --force
  cat >> "$AVD_HOME/$AVD_NAME.avd/config.ini" <<EOF
hw.ramSize=3072
hw.lcd.density=240
hw.lcd.width=720
hw.lcd.height=1280
disk.dataPartition.size=2G
hw.keyboard=yes
EOF
fi

# ---------------------------------------------------------------------------
# Boot. Every adb call is timeout-wrapped; the poll prints a marker per
# iteration so a hang is diagnosable from the log alone.
# ---------------------------------------------------------------------------
EMU_LOG="$(mktemp /tmp/velmux-emulator-XXXXXX.log)"
vlog "booting emulator (log: $EMU_LOG)"
nohup emulator -avd "$AVD_NAME" \
  -no-window -no-audio -no-boot-anim -no-snapshot -no-metrics \
  -gpu swiftshader_indirect -accel on -cores 2 \
  > "$EMU_LOG" 2>&1 &

boot_completed() {
  [ "$(timeout 10 adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = "1" ]
}
# The 'emulator' launcher execs qemu-system-x86_64, so the process name
# changes almost immediately — pgrep -x emulator reports a FALSE death
# while the machine is booting happily. The command line of both the
# launcher and the qemu child always carries "-avd <name>": match that.
emulator_alive() {
  pgrep -f -- "-avd $AVD_NAME" >/dev/null 2>&1
}
BOOT_OK=0
for i in $(seq 1 48); do
  if boot_completed; then
    vlog "boot completed after ~$((i * 15))s"
    BOOT_OK=1
    break
  fi
  if ! emulator_alive; then
    vlog "emulator process died; log tail:"
    tail -50 "$EMU_LOG" >&2 || true
    vdie "emulator exited during boot"
  fi
  # Visible heartbeat with device state, so stalls are diagnosable.
  if [ $((i % 4)) -eq 0 ]; then
    vlog "still booting (~$((i * 15))s): $(timeout 10 adb devices 2>/dev/null | tail -1 | tr -d '\r')"
  fi
  sleep 15
done
if [ "$BOOT_OK" != "1" ]; then
  vlog "boot never completed; emulator log tail:"
  tail -80 "$EMU_LOG" >&2 || true
  timeout 10 adb devices -l >&2 || true
  vdie "emulator boot timeout"
fi
# Some system services settle slightly after boot_completed.
sleep 10
timeout 30 adb wait-for-device 2>/dev/null || true

# ---------------------------------------------------------------------------
# App build for the emulator ABI (FFmpeg mandatory through fetch.sh)
# ---------------------------------------------------------------------------
vlog "building the app for the emulator ($ABI)"
bash "$SCRIPTS_DIR/android/build.sh" --debug --abi "$ABI"

APK_GLOB="$VELMUX_ROOT/$VELMUX_ANDROID_HOST_DIR/build/outputs/apk/debug/*.apk"
APK="$(ls $APK_GLOB 2>/dev/null | head -1 || true)"
[ -n "$APK" ] || vdie "debug APK not found after the build"

# ---------------------------------------------------------------------------
# Install + cold launch verification
# ---------------------------------------------------------------------------
vlog "installing $APK"
adb install -r "$APK" > /dev/null

# Stable instrumentation timing.
adb_t shell settings put global window_animation_scale 0
adb_t shell settings put global transition_animation_scale 0
adb_t shell settings put global animator_duration_scale 0

verify_launch() {
  local phase="$1"
  adb logcat -c 2>/dev/null || true
  timeout 60 adb shell am start -W -n com.velmux.mobile/.MainActivity > /dev/null \
    || vdie "$phase: am start failed"
  local alive=0 pid=""
  for i in $(seq 1 12); do
    pid="$(timeout 10 adb shell pidof com.velmux.mobile 2>/dev/null | tr -d '\r' || true)"
    if [ -n "$pid" ]; then
      alive=1
      break
    fi
    sleep 2
  done
  [ "$alive" = "1" ] || vdie "$phase: the app process is not alive after launch"
  # The activity must actually be resumed (not a blank/crashed launch).
  # Android 13+: MainActivity asks for POST_NOTIFICATIONS once at startup
  # (M8 product behavior), so on a fresh install the system permission
  # dialog legitimately covers it — the resumed activity is then
  # GrantPermissionsActivity, not a launch failure. Dismiss the dialog
  # exactly like a user who refuses (BACK key = denial) and still require
  # MainActivity to become the resumed activity afterwards. That also
  # verifies the documented deny path: users who refuse keep working
  # conversions. The activity class name (com.android.permissioncontroller
  # ...GrantPermissionsActivity) is stable across the controller's package
  # renames, so the class name alone is matched.
  # The dump line differs by API level: API 29 prints
  # 'mResumedActivity: ActivityRecord{...}', API 34 prints
  # 'ResumedActivity: ActivityRecord{...}' and 'topResumedActivity=...'.
  # Matching the 'ResumedActivity' substring (which 'mResumedActivity'
  # also contains) covers every format; 'mLastPausedActivity' lines never
  # match it, so a paused-but-listed velmux activity cannot false-positive
  # (verified against captured API 29 and API 34 bugreports in both the
  # dialog-up and app-resumed states).
  local resumed dismissed=0 i
  for i in $(seq 1 10); do
    resumed="$(timeout 10 adb shell dumpsys activity activities 2>/dev/null | tr -d '\r' || true)"
    if printf '%s' "$resumed" | grep -q "ResumedActivity.*com.velmux.mobile"; then
      break
    fi
    if [ "$dismissed" -lt 2 ] && printf '%s' "$resumed" \
        | grep -q "ResumedActivity.*GrantPermissionsActivity"; then
      adb_t shell input keyevent 4  # BACK: refuse the runtime permission
      dismissed=$((dismissed + 1))
    fi
    sleep 2
  done
  printf '%s' "$resumed" | grep -q "ResumedActivity.*com.velmux.mobile" \
    || vdie "$phase: MainActivity is not the resumed activity"
  # Keep it alive for a sustained window; a late crash (JNI load failure,
  # engine startup, Compose runtime) must fail the run.
  sleep 12
  pid="$(timeout 10 adb shell pidof com.velmux.mobile 2>/dev/null | tr -d '\r' || true)"
  [ -n "$pid" ] || vdie "$phase: the app process died shortly after launch"
  # Engine init marker: VelmuxApplication probes the engine facade right
  # after the client starts and logs the result. This proves
  # System.loadLibrary("velmux_jni") mapped the library, JNI_OnLoad
  # registered the surface and the facade answers — without it a process
  # that is alive but engine-dead would pass silently.
  local marker=""
  for i in $(seq 1 15); do
    marker="$(timeout 10 adb logcat -d 2>/dev/null | tr -d '\r' \
      | grep -F 'app init:' | tail -1 || true)"
    [ -n "$marker" ] && break
    sleep 2
  done
  [ -n "$marker" ] \
    || vdie "$phase: the engine init marker never appeared (the engine surface failed to initialize)"
  case "$marker" in
    *'engine ready'*) ;;
    *) vdie "$phase: the engine surface is not available at app init: $marker" ;;
  esac
  # Fatal-error scan over everything logged since launch.
  local log
  log="$(timeout 15 adb logcat -d 2>/dev/null || true)"
  if printf '%s' "$log" | grep -qE "FATAL EXCEPTION|SIGSEGV|SIGABRT|UnsatisfiedLinkError.*velmux|Force finishing activity.*com.velmux.mobile"; then
    printf '%s\n' "$log" | grep -E -A5 "FATAL EXCEPTION|SIGSEGV|SIGABRT|UnsatisfiedLinkError.*velmux" | head -60 >&2 || true
    vdie "$phase: fatal errors in logcat after launch"
  fi
  vlog "$phase: app alive (pid $pid), engine init marker present, log clean"
}

verify_launch "cold launch"

# ---------------------------------------------------------------------------
# Instrumented engine suite (real conversions through the app's engine)
# ---------------------------------------------------------------------------
vlog "running instrumented tests"
run_in "$VELMUX_ROOT" ./gradlew :platform:android:host:connectedDebugAndroidTest \
  --no-daemon --stacktrace \
  -PvelmuxAbis="$ABI" \
  -PvelmuxFFmpegPrefix="$VELMUX_ROOT/$VELMUX_FFMPEG_OUT_DIR/android" \
  -PvelmuxVersionName="$VELMUX_VERSION_NAME" \
  -PvelmuxVersionCode="$VELMUX_VERSION_CODE"

# ---------------------------------------------------------------------------
# Terminate + relaunch (lifecycle smoke; the process must come back up)
# ---------------------------------------------------------------------------
# AGP uninstalls BOTH APKs when connectedDebugAndroidTest completes
# (PACKAGE_REMOVED / PACKAGE_FULLY_REMOVED broadcasts in the run's
# diagnostics; 'am force-stop' then logs 'Invalid packageName'). The
# relaunch smoke needs the app back on the device first.
vlog "reinstalling the app after the instrumented run"
adb install -r "$APK" > /dev/null \
  || vdie "relaunch: reinstalling $APK failed"
vlog "force-stopping and relaunching"
adb_t shell am force-stop com.velmux.mobile
sleep 3
verify_launch "relaunch"

vlog "emulator test run complete (API $API)"

# Graceful shutdown so local runs do not leave an emulator burning CPU
# (CI runners are ephemeral; this is a no-op cost there).
adb_t emu kill
sleep 3
emulator_alive && pkill -f -- "-avd $AVD_NAME" 2>/dev/null || true
