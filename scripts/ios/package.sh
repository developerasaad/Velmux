#!/usr/bin/env bash
# Collect the iOS release artifacts (simulator .app and the unsigned
# archive-capable device .app) plus checksums into a staging directory.
#
# Contract:
#   scripts/ios/package.sh <staging-dir>   (after build.sh --release)
#
# Store distribution requires an Apple Developer certificate and
# re-signing; the device artifact is honestly labeled unsigned.
set -euo pipefail

SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=lib/common.sh
. "$SCRIPTS_DIR/lib/common.sh"

[ $# -eq 1 ] || vdie "usage: scripts/ios/package.sh <staging-dir>"
STAGING="$1"
mkdir -p "$STAGING"

IOS_DIR="$VELMUX_ROOT/$VELMUX_APP_IOS_DIR"
# Deterministic derived-data locations written by scripts/ios/build.sh.
SIM_APP="$IOS_DIR/build/Build/Products/Release-iphonesimulator/Velmux.app"
DEV_APP="$IOS_DIR/build-device/Build/Products/Release-iphoneos/Velmux.app"
[ -d "$SIM_APP" ] || vdie "simulator Release .app not found; run scripts/ios/build.sh --release --sdk simulator first"
[ -d "$DEV_APP" ] || vdie "device Release .app not found; run scripts/ios/build.sh --release --sdk device first"

(
  cd "$(dirname "$SIM_APP")"
  zip -qry "$STAGING/Velmux-Release-ios-simulator.app.zip" "$(basename "$SIM_APP")"
)
(
  cd "$(dirname "$DEV_APP")"
  zip -qry "$STAGING/Velmux-Release-ios-device-unsigned.app.zip" "$(basename "$DEV_APP")"
)

(
  cd "$STAGING"
  find . -maxdepth 1 -type f ! -name 'SHA256SUMS*' -exec sha256sum {} + \
    > SHA256SUMS-ios.txt
)

vlog "packaged iOS artifacts into $STAGING:"
ls -la "$STAGING" >&2
