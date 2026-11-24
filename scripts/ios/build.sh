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
# FFmpeg is mandatory: the pinned iOS artifacts are resolved (identity-
# checked reuse or build), packaged into the xcframework, and exported as
# VELMUX_FFMPEG_ROOT for pod install. The Podfile refuses to install
# without it.
set -euo pipefail

SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=lib/common.sh
. "$SCRIPTS_DIR/lib/common.sh"

case "$(uname -s)" in
  Darwin) ;;
  *) vdie "iOS builds require macOS (Xcode)" ;;
esac

BUILD_TYPE="debug"
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

require_tool xcodebuild
require_tool pod || vdie "CocoaPods not found (sudo gem install cocoapods)"
ensure_js_deps

# ---------------------------------------------------------------------------
# Mandatory FFmpeg artifacts + xcframework packaging
# ---------------------------------------------------------------------------
export VELMUX_FFMPEG_JOBS="${VELMUX_FFMPEG_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 2)}"
# shellcheck source=../ffmpeg/fetch.sh
bash "$SCRIPTS_DIR/ffmpeg/fetch.sh" ios

BUILDER="$(repo_dir "$VELMUX_FFMPEG_BUILDER_DIR")"
FFMPEG_IOS_OUT="$VELMUX_ROOT/$VELMUX_FFMPEG_OUT_DIR/ios"
FFMPEG_PACKAGED="$VELMUX_ROOT/ffmpeg-ios-packaged"
if [ ! -d "$FFMPEG_PACKAGED/VelmuxFFmpeg.xcframework" ] \
   || [ "$FFMPEG_IOS_OUT/iphoneos/BUILD-IDENTITY" -nt "$FFMPEG_PACKAGED/.packaged-stamp" ] 2>/dev/null; then
  vlog "packaging the FFmpeg xcframework"
  rm -rf "$FFMPEG_PACKAGED"
  bash "$BUILDER/package-ios-xcframework.sh" "$FFMPEG_IOS_OUT" "$FFMPEG_PACKAGED"
  touch "$FFMPEG_PACKAGED/.packaged-stamp"
fi
export VELMUX_FFMPEG_ROOT="$FFMPEG_PACKAGED"

# ---------------------------------------------------------------------------
# Pods
# ---------------------------------------------------------------------------
IOS_DIR="$(repo_dir "$VELMUX_APP_IOS_DIR")"
if [ ! -d "$IOS_DIR/Pods" ] \
   || [ "$VELMUX_ROOT/VelmuxEngine.podspec" -nt "$IOS_DIR/Pods/Manifest.lock" ] \
   || [ "$IOS_DIR/Podfile" -nt "$IOS_DIR/Pods/Manifest.lock" ]; then
  vlog "pod install (VELMUX_FFMPEG_ROOT=$VELMUX_FFMPEG_ROOT)"
  run_in "$IOS_DIR" pod install
fi

# ---------------------------------------------------------------------------
# Xcode build. Raw output (no xcpretty): it hides compile diagnostics.
# Deterministic -derivedDataPath so test/package scripts can locate the
# products: simulator -> build/, device -> build-device/ (both gitignored).
# ---------------------------------------------------------------------------
case "$SDK_KIND" in
  device)
    XCODE_SDK="iphoneos"
    DERIVED="build-device"
    SIGN_ARGS=(CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGNING_IDENTITY='')
    XCODE_ARCHS="${VELMUX_IOS_DEVICE_ARCHS:-arm64}"
    ;;
  simulator)
    XCODE_SDK="iphonesimulator"
    DERIVED="build"
    SIGN_ARGS=(CODE_SIGNING_ALLOWED=NO)
    # The pinned FFmpeg simulator artifact is the single-arch arm64 slice
    # (D-058). Without an explicit ARCHS, xcodebuild builds a UNIVERSAL
    # simulator binary and the x86_64 link demands an FFmpeg slice the
    # artifact model intentionally does not build by default ('ld:
    # library velmux_ffmpeg not found'). Intel-Mac simulator development
    # pairs VELMUX_IOS_SIM_X86_64=1 (build the slice) with
    # VELMUX_IOS_SIM_ARCHS=x86_64 (link it).
    XCODE_ARCHS="${VELMUX_IOS_SIM_ARCHS:-arm64}"
    ;;
esac

vlog "building iOS app ($BUILD_TYPE, $SDK_KIND, arch=$XCODE_ARCHS)"
run_in "$IOS_DIR" xcodebuild -workspace Velmux.xcworkspace -scheme Velmux \
  -configuration "$BUILD_TYPE" -sdk "$XCODE_SDK" \
  -derivedDataPath "$DERIVED" \
  ARCHS="$XCODE_ARCHS" \
  MARKETING_VERSION="${VELMUX_VERSION_NAME%%-*}" \
  CURRENT_PROJECT_VERSION="$VELMUX_VERSION_CODE" \
  build "${SIGN_ARGS[@]}"

vlog "iOS build complete ($BUILD_TYPE, $SDK_KIND; products in $IOS_DIR/$DERIVED/Build/Products)"
