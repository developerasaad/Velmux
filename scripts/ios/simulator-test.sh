#!/usr/bin/env bash
# Automated iOS simulator test run (development testing; the simulator is
# NOT a product target — spec §8).
#
# Contract:
#   scripts/ios/simulator-test.sh
#
# What this does, in order:
#   1. resolve the pinned iOS FFmpeg artifacts (identity-checked reuse or
#      build — both device and simulator slices);
#   2. build the Velmux app for the simulator in RELEASE configuration
#      (Release embeds the JS bundle, so cold-launch verification exercises
#      the real React Native initialization without a metro server);
#   3. select or create the pinned simulator and boot it (bootstatus-wrapped
#      polling);
#   4. install, cold launch, and verify: the process stays alive for a
#      sustained window, the RN init marker appears in the unified log
#      ("[Velmux] app init: native bridge available" proves JS runtime +
#      bridge + engine registration), and no fatal errors are logged;
#   5. run the VelmuxEngineTests XCTest suite through the SAME workspace
#      (real engine execution on-simulator: whole-pipeline transcode,
#      stage-composed hybrid pipelines, lifecycle honesty);
#   6. terminate, relaunch, verify the process and the init marker again
#      (lifecycle/persistence smoke);
#   7. collect bounded diagnostics on any failure.
#
# Simulator codec behavior is NOT physical-hardware validation (spec §10).
set -euo pipefail

SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=lib/common.sh
. "$SCRIPTS_DIR/lib/common.sh"

case "$(uname -s)" in
  Darwin) ;;
  *) vdie "iOS simulator tests require macOS (Xcode + CoreSimulator)" ;;
esac

require_tool xcodebuild
require_tool pod || vdie "CocoaPods not found (sudo gem install cocoapods)"
require_tool npm

BUNDLE_ID="com.velmux.mobile"
IOS_DIR="$(repo_dir "$VELMUX_APP_IOS_DIR")"
DEVICE_NAME="$VELMUX_IOS_SIMULATOR_DEVICE"
DIAG_DIR="$IOS_DIR/sim-test-diagnostics"
rm -rf "$DIAG_DIR"; mkdir -p "$DIAG_DIR"

collect_diagnostics() {
  vlog "collecting simulator diagnostics into $DIAG_DIR"
  # ${UDID:-booted}: the ERR trap can fire before a simulator is ever
  # selected (e.g. the FFmpeg build failed); "booted" is simctl's
  # placeholder and degrades gracefully when nothing is running.
  xcrun simctl spawn "${UDID:-booted}" log show --info --last 10m \
    --style compact --predicate 'process == "Velmux"' \
    > "$DIAG_DIR/velmux-process.log" 2>/dev/null || true
  xcrun simctl diagnose "${UDID:-booted}" --archive "$DIAG_DIR/simdiag" \
    > "$DIAG_DIR/simctl-diagnose.log" 2>&1 || true
  tail -80 "$DIAG_DIR/velmux-process.log" >&2 || true
}
trap 'collect_diagnostics' ERR

# ---------------------------------------------------------------------------
# 1+2. FFmpeg artifacts + simulator app (Release: embedded JS bundle)
# ---------------------------------------------------------------------------
bash "$SCRIPTS_DIR/ios/build.sh" --release --sdk simulator
APP_DIR="$IOS_DIR/build/Build/Products/Release-iphonesimulator"
APP="$APP_DIR/Velmux.app"
[ -d "$APP" ] || vdie "simulator Release .app not found at $APP"

# ---------------------------------------------------------------------------
# 3. Select/create + boot the pinned simulator
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
# 4. Install + cold launch verification
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

  # Sustained window: a late crash (JS bundle load, bridge setup, engine
  # startup) must fail the run.
  sleep 15
  if ! ps -p "$pid" > /dev/null 2>&1; then
    vdie "$phase: the app process died shortly after launch (pid $pid)"
  fi

  # Unified log for the window, read from the PERSISTED log after the
  # fact. The previous live 'log stream' capture was fragile (a background
  # process killed after the window; one run captured zero entries —
  # buffered output lost on kill). --info is REQUIRED: the RN init marker
  # is console.info -> os_log(OS_LOG_TYPE_INFO), and both 'log stream' and
  # 'log show' hide info-level entries without it (the AppDelegate raises
  # RN's release log threshold so the marker reaches os_log at all).
  local log="$DIAG_DIR/${phase}.log"
  xcrun simctl spawn "$UDID" log show --info --last 2m --style compact \
    --predicate 'process == "Velmux"' > "$log" 2>/dev/null || true

  # RN initialization marker: proves the JS runtime executed app init and
  # found the native bridge + engine.
  grep -q "\[Velmux\] app init: native bridge available" "$log" \
    || vdie "$phase: the RN init/bridge marker is missing from the log"

  if grep -qE "Fatal error|SIGABRT|SIGSEGV|SIGBUS|dyld.*error|Uncaught exception" "$log"; then
    grep -E -B2 -A8 "Fatal error|SIGABRT|SIGSEGV|SIGBUS|dyld.*error|Uncaught exception" "$log" \
      | head -60 >&2 || true
    vdie "$phase: fatal errors in the app log after launch"
  fi
  vlog "$phase: app alive (pid $pid), RN init marker present, log clean"
}

verify_launch "cold-launch"

# ---------------------------------------------------------------------------
# 5. Engine XCTest suite (real engine execution on the simulator)
# ---------------------------------------------------------------------------
vlog "running VelmuxEngineTests"
run_in "$IOS_DIR" xcodebuild test \
  -workspace Velmux.xcworkspace -scheme Velmux \
  -configuration Debug \
  -destination "platform=iOS Simulator,id=$UDID" \
  -derivedDataPath build \
  CODE_SIGNING_ALLOWED=NO \
  | tee "$DIAG_DIR/xctest.log"
grep -q "Test Suite 'All tests' passed\|TEST SUCCEEDED" "$DIAG_DIR/xctest.log" \
  || vdie "the VelmuxEngineTests suite failed"

# ---------------------------------------------------------------------------
# 6. Terminate + relaunch (lifecycle smoke)
# ---------------------------------------------------------------------------
vlog "terminating and relaunching"
xcrun simctl terminate "$UDID" "$BUNDLE_ID" 2>/dev/null || true
sleep 3
verify_launch "relaunch"

xcrun simctl shutdown "$UDID" 2>/dev/null || true
trap - ERR
vlog "simulator test run complete ($DEVICE_NAME)"
