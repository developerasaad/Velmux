#!/usr/bin/env bash
# Build the Velmux iOS app.
#
# Contract:
#   scripts/ios/build.sh [--release|--debug] [--sdk device|simulator]
#
# The production target is the DEVICE build (arm64, Release for shipping).
# The simulator SDK is a development/test target used by
# scripts/test.sh --ios-sim; it stays selectable here because the test
# script builds through this one.
#
# Everything substantial is repository-owned: the Xcode project's
# "Prepare engine and framework" phase calls scripts/ios/prepare-xcode-inputs.sh
# (FFmpeg resolve + xcframework packaging, the CMake engine archives, the
# Kotlin Compose framework) — the same path a developer runs locally.
# FFmpeg is mandatory; there is no app build without it.
set -euo pipefail

SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=lib/common.sh
. "$SCRIPTS_DIR/lib/common.sh"

case "$(uname -s)" in
  Darwin) ;;
  *) vdie "iOS builds require macOS (Xcode)" ;;
esac

BUILD_TYPE="release"
SDK_KIND="device"
while [ $# -gt 0 ]; do
  case "$1" in
    --release) BUILD_TYPE="release" ;;
    --debug) BUILD_TYPE="debug" ;;
    --sdk) shift; case "${1:-}" in device|simulator) SDK_KIND="$1" ;; *) vdie "--sdk must be device or simulator" ;; esac ;;
    *) vdie "unknown option: $1 (ios/build.sh)" ;;
  esac
  shift
done
# The Xcode configurations are named Debug/Release (and the project's
# "Prepare engine and framework" phase passes $CONFIGURATION straight into
# prepare-xcode-inputs.sh, which validates the capitalized names).
case "$BUILD_TYPE" in
  release) XCODE_CONFIGURATION="Release" ;;
  debug) XCODE_CONFIGURATION="Debug" ;;
  *) vdie "internal error: unknown build type '$BUILD_TYPE'" ;;
esac

require_tool xcodebuild
require_tool cmake
require_tool java

# ---------------------------------------------------------------------------
# Xcode build. Raw output (no xcpretty): it hides compile diagnostics.
# Deterministic -derivedDataPath so test/package scripts can locate the
# products: simulator -> build/, device -> build-device/ (both gitignored).
# The in-project "Prepare engine and framework" phase stages the engine
# archives + the Kotlin framework + the FFmpeg xcframework before Swift
# compilation needs them.
# ---------------------------------------------------------------------------
XCODE_PROJECT="$VELMUX_ROOT/$VELMUX_PLATFORM_IOS_DIR/host/Velmux.xcodeproj"
[ -d "$XCODE_PROJECT" ] || vdie "Velmux.xcodeproj not found at $XCODE_PROJECT"

case "$SDK_KIND" in
  device)
    XCODE_SDK="iphoneos"
    DERIVED="build-device"
    # CI has no signing identity: CODE_SIGNING_ALLOWED=NO produces an
    # unsigned but fully linked binary (the link itself is the verification
    # target). A developer with a signing identity drops these overrides
    # through XCODEARGS.
    SIGN_ARGS=(CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGNING_IDENTITY='')
    ;;
  simulator)
    XCODE_SDK="iphonesimulator"
    DERIVED="build"
    SIGN_ARGS=(CODE_SIGNING_ALLOWED=NO)
    ;;
esac

vlog "building iOS app ($BUILD_TYPE, $SDK_KIND, arch=$VELMUX_IOS_DEVICE_ARCH)"
run_in "$VELMUX_ROOT/$VELMUX_PLATFORM_IOS_DIR/host" xcodebuild \
  -project Velmux.xcodeproj -scheme Velmux \
  -configuration "$XCODE_CONFIGURATION" -sdk "$XCODE_SDK" \
  -derivedDataPath "$DERIVED" \
  ARCHS="$VELMUX_IOS_DEVICE_ARCH" \
  MARKETING_VERSION="${VELMUX_VERSION_NAME%%-*}" \
  CURRENT_PROJECT_VERSION="$VELMUX_VERSION_CODE" \
  build "${SIGN_ARGS[@]}" ${VELMUX_XCODEARGS:-}

vlog "iOS build complete ($BUILD_TYPE, $SDK_KIND; products in $VELMUX_PLATFORM_IOS_DIR/host/$DERIVED/Build/Products)"
