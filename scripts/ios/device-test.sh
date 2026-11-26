#!/usr/bin/env bash
# Physical iOS device test run (final hardware validation).
#
# Contract:
#   scripts/ios/device-test.sh
#   (signing: VELMUX_SIGNING_TEAM=<Apple Development Team ID>, or the
#    Xcode GUI must already have a signing configuration for the device)
#
# Requires a connected iPhone with Developer Mode enabled. Runs the engine
# XCTest suite ON THE HARDWARE (VideoToolbox/AVFoundation native paths
# count as validated only here), plus a launch/lifecycle smoke.
#
# This script never fakes hardware validation: with no device attached or
# no signing identity it fails with an explicit message.
set -euo pipefail

SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=lib/common.sh
. "$SCRIPTS_DIR/lib/common.sh"

case "$(uname -s)" in
  Darwin) ;;
  *) vdie "iOS device tests require macOS (Xcode + a connected iPhone)" ;;
esac

require_tool xcodebuild
require_tool python3

BUNDLE_ID="com.velmux.mobile"
HOST_DIR="$VELMUX_ROOT/$VELMUX_PLATFORM_IOS_DIR/host"
DIAG_DIR="$HOST_DIR/device-test-diagnostics"
rm -rf "$DIAG_DIR"; mkdir -p "$DIAG_DIR"

# ---------------------------------------------------------------------------
# 1. Find exactly one connected iPhone (devicectl; Xcode 15+)
# ---------------------------------------------------------------------------
require_tool devicectl || vdie "xcrun devicectl not found (Xcode 15+ required)"
DEVICE_JSON="$DIAG_DIR/devices.json"
xcrun devicectl list devices --json-output "$DEVICE_JSON" >/dev/null 2>&1 \
  || vdie "devicectl failed to list devices"
UDID="$(python3 -c '
import json,sys
data=json.load(open(sys.argv[1]))
devices=[d for d in data.get("devices",[]) if d.get("connectionProperties",{}).get("tunnelState")!="unavailable" or d.get("isAvailable",True)]
if not devices:
    sys.exit(1)
if len(devices)>1:
    sys.stderr.write("multiple devices connected: "+", ".join(d.get("name","?") for d in devices)+"\n")
    sys.exit(1)
print(devices[0]["identifier"])
' "$DEVICE_JSON" || true)"
[ -n "$UDID" ] || vdie "no connected iOS device found (devicectl lists none)"
vlog "device: $UDID"

if [ -n "${VELMUX_SIGNING_TEAM:-}" ]; then
  SIGN_ARGS=(DEVELOPMENT_TEAM="$VELMUX_SIGNING_TEAM"
             -allowProvisioningUpdates)
  vlog "signing with team $VELMUX_SIGNING_TEAM"
else
  vdie "set VELMUX_SIGNING_TEAM to your Apple Development Team ID (hardware installation requires signing)"
fi

collect_diagnostics() {
  vlog "collecting device diagnostics into $DIAG_DIR"
  xcrun devicectl device info details --device "$UDID" \
    > "$DIAG_DIR/device-details.txt" 2>&1 || true
}
trap 'collect_diagnostics' ERR

# ---------------------------------------------------------------------------
# 2. Engine XCTest suite ON THE HARDWARE. The Xcode project's "Prepare
#    engine and framework" phase resolves the mandatory FFmpeg artifacts,
#    the CMake engine archives and the Kotlin Compose framework first —
#    the same path a developer runs locally. Debug configuration: the
#    device run is a development diagnostic, and Debug links faster.
# ---------------------------------------------------------------------------
vlog "running VelmuxEngineTests on the device (this builds for arm64)"
run_in "$HOST_DIR" xcodebuild test \
  -project Velmux.xcodeproj -scheme Velmux \
  -configuration Debug \
  -destination "platform=iOS,id=$UDID" \
  -derivedDataPath build-device \
  ARCHS="$VELMUX_IOS_DEVICE_ARCH" \
  "${SIGN_ARGS[@]}" \
  | tee "$DIAG_DIR/xctest.log"
grep -q "Test Suite 'All tests' passed\|TEST SUCCEEDED" "$DIAG_DIR/xctest.log" \
  || vdie "the on-device VelmuxEngineTests suite failed"

# ---------------------------------------------------------------------------
# 3. Launch + lifecycle smoke (the test run above already installed the app)
# ---------------------------------------------------------------------------
APP="$HOST_DIR/build-device/Build/Products/Debug-iphoneos/Velmux.app"
[ -d "$APP" ] || vdie "device .app not found at $APP"

vlog "installing and launching"
xcrun devicectl device install app --device "$UDID" "$APP" \
  | tee "$DIAG_DIR/install.log"
LAUNCH_OUT="$(xcrun devicectl device process launch --device "$UDID" "$BUNDLE_ID" 2>&1)" \
  || vdie "device launch failed: $LAUNCH_OUT"
printf '%s\n' "$LAUNCH_OUT" > "$DIAG_DIR/launch.log"
sleep 15
# Process-still-alive check through the device process list.
if ! xcrun devicectl device info processes --device "$UDID" 2>/dev/null \
     | grep -q "$BUNDLE_ID"; then
  vdie "the app process died shortly after launch on the device"
fi
vlog "app alive on device 15s after launch"

vlog "terminating and relaunching"
xcrun devicectl device process terminate --device "$UDID" "$BUNDLE_ID" \
  >/dev/null 2>&1 || true
sleep 3
xcrun devicectl device process launch --device "$UDID" "$BUNDLE_ID" \
  >/dev/null 2>&1 || vdie "device relaunch failed"
sleep 10
if ! xcrun devicectl device info processes --device "$UDID" 2>/dev/null \
     | grep -q "$BUNDLE_ID"; then
  vdie "the app process died shortly after relaunch on the device"
fi

trap - ERR
vlog "device test run complete"
