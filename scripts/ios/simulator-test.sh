#!/usr/bin/env bash
# Automated iOS simulator test run (development testing; the simulator is
# NOT a product target and NOT physical-hardware validation).
#
# Contract:
#   scripts/ios/simulator-test.sh
#
# What this does, in order:
#   1. build the Velmux app for the simulator in RELEASE configuration
#      through scripts/ios/build.sh — the Xcode project's "Prepare engine
#      and framework" phase resolves the mandatory FFmpeg artifacts
#      (identity-checked reuse or build), the CMake engine archives and
#      the Kotlin Compose framework first (the same path a developer
#      runs locally; there is no app build without FFmpeg);
#   2. select or create the pinned simulator and boot it
#      (bootstatus-wrapped polling);
#   3. install, cold launch, and verify: the process stays alive for a
#      sustained window, the engine-init marker appears in the unified
#      log ("[Velmux] app init: engine ready" — the shared process engine
#      answered its versioning probe: real engine initialization, not
#      merely a live UI), and no fatal errors are logged;
#   4. run the VelmuxEngineTests XCTest suite through the same Xcode
#      project (real engine execution on-simulator: whole-pipeline
#      transcode, stage-composed hybrid pipelines, lifecycle honesty);
#   5. terminate, relaunch, and verify the process and the marker again
#      (lifecycle/persistence smoke);
#   6. collect bounded diagnostics on any failure.
#
# Simulator codec behavior is NOT physical-hardware validation.
set -euo pipefail

SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=lib/common.sh
. "$SCRIPTS_DIR/lib/common.sh"

case "$(uname -s)" in
  Darwin) ;;
  *) vdie "iOS simulator tests require macOS (Xcode + CoreSimulator)" ;;
esac

require_tool xcodebuild
require_tool python3

BUNDLE_ID="com.velmux.mobile"
HOST_DIR="$VELMUX_ROOT/$VELMUX_PLATFORM_IOS_DIR/host"
DEVICE_NAME="$VELMUX_IOS_SIMULATOR_DEVICE"
DIAG_DIR="$HOST_DIR/sim-test-diagnostics"
rm -rf "$DIAG_DIR"; mkdir -p "$DIAG_DIR"

collect_diagnostics() {
  vlog "collecting simulator diagnostics into $DIAG_DIR"
  # ${UDID:-booted}: the ERR trap can fire before a simulator is ever
  # selected (e.g. the app build failed); "booted" is simctl's placeholder
  # and degrades gracefully when nothing is running.
  xcrun simctl spawn "${UDID:-booted}" log show --info --last 10m \
    --style compact --predicate 'process == "Velmux"' \
    > "$DIAG_DIR/velmux-process.log" 2>/dev/null || true
  tail -80 "$DIAG_DIR/velmux-process.log" >&2 || true
}
trap 'collect_diagnostics' ERR

# ---------------------------------------------------------------------------
# 1. Simulator app (Release: the production configuration, same staged
#    inputs the device product build uses)
# ---------------------------------------------------------------------------
bash "$SCRIPTS_DIR/ios/build.sh" --release --sdk simulator
APP_DIR="$HOST_DIR/build/Build/Products/Release-iphonesimulator"
APP="$APP_DIR/Velmux.app"
[ -d "$APP" ] || vdie "simulator Release .app not found at $APP"

# ---------------------------------------------------------------------------
# 2. Select/create + boot the pinned simulator
# ---------------------------------------------------------------------------
find_simulator() {
  # A created device with the pinned name that is available for the newest
  # iOS runtime. Exits 0 and prints the UDID, or exits 1.
  xcrun simctl list devices available -j \
    | python3 -c '
import json,sys
data=json.load(sys.stdin)
name="'"$DEVICE_NAME"'"
runtimes=sorted(data["runtimes"], key=lambda r: r.get("version","0"))
for rt in runtimes:
    for dev in data["devices"].get(rt["identifier"],[]):
        if dev["name"]==name and dev.get("isAvailable"):
            print(dev["udid"]); sys.exit(0)
sys.exit(1)
'
}

UDID="$(find_simulator || true)"
if [ -z "$UDID" ]; then
  vlog "no '$DEVICE_NAME' simulator found; creating one"
  RUNTIME="$(xcrun simctl list runtimes -j | python3 -c '
import json,sys
rts=json.load(sys.stdin)["runtimes"]
ios=[r for r in rts if r.get("platform")=="iOS" or "iOS" in r.get("identifier","")]
ios.sort(key=lambda r: r.get("version","0"), reverse=True)
if not ios: sys.exit(1)
print(ios[0]["identifier"])
')"
  DEVTYPE="$(xcrun simctl list devicetypes -j | python3 -c '
import json,sys
dts=json.load(sys.stdin)["devicetypes"]
for dt in dts:
    if dt["name"]=="'"$DEVICE_NAME"'":
        print(dt["identifier"]); sys.exit(0)
sys.exit(1)
' || true)"
  [ -n "$DEVTYPE" ] || vdie "device type '$DEVICE_NAME' not known to this Xcode"
  UDID="$(xcrun simctl create "VelmuxTest $DEVICE_NAME" "$DEVTYPE" "$RUNTIME")"
fi
vlog "simulator: $DEVICE_NAME ($UDID)"

xcrun simctl shutdown "$UDID" 2>/dev/null || true
vlog "booting the simulator (this can take a while)"
xcrun simctl boot "$UDID"
xcrun simctl bootstatus "$UDID" -b

# ---------------------------------------------------------------------------
# 3. Install + cold launch verification
# ---------------------------------------------------------------------------
vlog "installing $APP"
xcrun simctl install "$UDID" "$APP"

verify_launch() {
  local phase="$1"
  xcrun simctl terminate "$UDID" "$BUNDLE_ID" 2>/dev/null || true
  sleep 2
  local launch_out pid
  launch_out="$(xcrun simctl launch "$UDID" "$BUNDLE_ID")" \
    || vdie "$phase: simctl launch failed"
  pid="$(printf '%s' "$launch_out" | sed -n 's/.*: \([0-9][0-9]*\)$/\1/p')"
  [ -n "$pid" ] || vdie "$phase: could not parse the launched pid ($launch_out)"

  # Sustained window: a late crash (engine startup, framework load) must
  # fail the run.
  sleep 15
  if ! ps -p "$pid" > /dev/null 2>&1; then
    vdie "$phase: the app process died shortly after launch (pid $pid)"
  fi

  # Unified log for the window, read from the PERSISTED log after the
  # fact. --info keeps info-level entries visible (the engine-init marker
  # is NSLog, which lands at default level, but noise-level filtering
  # differs across Xcode releases; --info is the safe superset).
  local log="$DIAG_DIR/${phase}.log"
  xcrun simctl spawn "$UDID" log show --info --last 2m --style compact \
    --predicate 'process == "Velmux"' > "$log" 2>/dev/null || true

  # Engine-init marker: the shared process engine was created and answered
  # its ABI/version probe (VelmuxEngineHost.mm logs it exactly once).
  grep -q "\[Velmux\] app init: engine ready" "$log" \
    || vdie "$phase: the engine-init marker is missing from the log"

  if grep -qE "Fatal error|SIGABRT|SIGSEGV|SIGBUS|dyld.*error|Uncaught exception" "$log"; then
    grep -E -B2 -A8 "Fatal error|SIGABRT|SIGSEGV|SIGBUS|dyld.*error|Uncaught exception" "$log" \
      | head -60 >&2 || true
    vdie "$phase: fatal errors in the app log after launch"
  fi
  vlog "$phase: app alive (pid $pid), engine-init marker present, log clean"
}

verify_launch "cold-launch"

# ---------------------------------------------------------------------------
# 4. Engine XCTest suite (real engine execution on the simulator)
# ---------------------------------------------------------------------------
# Release configuration for the test run too: the host application is the
# exact product artifact the cold-launch phase verified, and the staged
# engine archives/FFmpeg xcframework are the Release ones. ARCHS=arm64
# matches the app build: without the pin the test bundle links a universal
# simulator binary and the x86_64 slice cannot resolve the FFmpeg symbols
# (the simulator artifact is single-slice arm64 by design).
vlog "running VelmuxEngineTests"
run_in "$HOST_DIR" xcodebuild test \
  -project Velmux.xcodeproj -scheme Velmux \
  -configuration Release \
  -destination "platform=iOS Simulator,id=$UDID" \
  -derivedDataPath build \
  ARCHS=arm64 \
  CODE_SIGNING_ALLOWED=NO \
  | tee "$DIAG_DIR/xctest.log"
grep -q "Test Suite 'All tests' passed\|TEST SUCCEEDED" "$DIAG_DIR/xctest.log" \
  || vdie "the VelmuxEngineTests suite failed"

# ---------------------------------------------------------------------------
# 5. Terminate + relaunch (lifecycle smoke)
# ---------------------------------------------------------------------------
vlog "terminating and relaunching"
xcrun simctl terminate "$UDID" "$BUNDLE_ID" 2>/dev/null || true
sleep 3
verify_launch "relaunch"

xcrun simctl shutdown "$UDID" 2>/dev/null || true
trap - ERR
vlog "simulator test run complete ($DEVICE_NAME)"
