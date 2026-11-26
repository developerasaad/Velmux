#!/usr/bin/env bash
# Stage the Xcode app's native inputs: the C++ engine archives (CMake,
# iOS toolchain), the Kotlin Compose framework (Gradle link task) and the
# FFmpeg dependency closure for the current platform slice.
#
# Contract:
#   scripts/ios/prepare-xcode-inputs.sh --configuration <Debug|Release>
#                                       --platform <iphoneos|iphonesimulator>
#
# Invoked by the Velmux.xcodeproj "Prepare engine and framework" build
# phase (with the target's CONFIGURATION and PLATFORM_NAME), and usable
# directly. Everything lands under a STABLE layout the project references:
#
#   build/ios/stage/<platform>/<configuration>/engine/libvelmux_*.a
#   build/ios/stage/<platform>/<configuration>/framework/Velmux.framework
#   build/ios/stage/<platform>/<configuration>/ffmpeg/libffmpeg_deps.a
#   build/ios/stage/test-config/VelmuxEngineTestConfig.h
#
# The stable stage keeps the .xcodeproj free of platform-specific path
# branching: ONE reference set serves device and simulator builds. The
# FFmpeg closure is a single libtool-merged archive per slice (no
# cross-archive symbol-ordering hazards at the app link); the app links
# it as a plain static archive — an xcframework would be validated at
# build-description time, BEFORE this phase could produce it.
set -euo pipefail

SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=lib/common.sh
. "$SCRIPTS_DIR/lib/common.sh"

CONFIGURATION=""
PLATFORM=""
while [ $# -gt 0 ]; do
  case "$1" in
    --configuration) shift; CONFIGURATION="${1:?}" ;;
    --configuration=*) CONFIGURATION="${1#--configuration=}" ;;
    --platform) shift; PLATFORM="${1:?}" ;;
    --platform=*) PLATFORM="${1#--platform=}" ;;
    *) vdie "unknown option: $1 (ios/prepare-xcode-inputs.sh)" ;;
  esac
  shift
done
case "$CONFIGURATION" in Debug|Release) ;; *) vdie "--configuration must be Debug or Release" ;; esac
case "$PLATFORM" in
  iphoneos) FFMPEG_SLICE=iphoneos; KOTLIN_TARGET=IosArm64 ;;
  iphonesimulator) FFMPEG_SLICE=iphonesimulator; KOTLIN_TARGET=IosSimulatorArm64 ;;
  *) vdie "--platform must be iphoneos or iphonesimulator" ;;
esac

case "$(uname -s)" in
  Darwin) ;;
  *) vdie "iOS preparation requires macOS (Xcode + the iOS SDK)" ;;
esac
require_tool xcodebuild
require_tool cmake
require_tool java
require_tool libtool

# Xcode script phases export the TARGET platform's SDK and deployment
# targets (SDKROOT=iphone*, IPHONEOS_DEPLOYMENT_TARGET=15, ...). Everything
# this script builds is either a macOS-host tool build or an explicitly
# cross-configured one; inherited values make host-side toolchains target
# iOS against the macOS sysroot — the sanity binaries are unrunnable
# ("DYLD_ROOT_PATH not set for simulator program", then
# "using sysroot for 'MacOSX' but targeting 'iPhone'" → killed at load).
# Drop the contamination for the whole preparation tree.
unset SDKROOT MACOSX_DEPLOYMENT_TARGET IPHONEOS_DEPLOYMENT_TARGET \
  TVOS_DEPLOYMENT_TARGET WATCHOS_DEPLOYMENT_TARGET XROS_DEPLOYMENT_TARGET

JOBS="${VELMUX_FFMPEG_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 2)}"

# ---------------------------------------------------------------------------
# 1. Mandatory FFmpeg artifacts (identity-checked reuse or build), per
#    slice, straight from the builder's output tree.
# ---------------------------------------------------------------------------
export VELMUX_FFMPEG_JOBS="$JOBS"
bash "$SCRIPTS_DIR/ffmpeg/fetch.sh" ios
FFMPEG_IOS_OUT="$VELMUX_ROOT/$VELMUX_FFMPEG_OUT_DIR/ios"
FFMPEG_PREFIX="$FFMPEG_IOS_OUT/$FFMPEG_SLICE"

# ---------------------------------------------------------------------------
# 2. C++ engine archives per platform (CMake with the iOS toolchain; the
#    per-slice FFmpeg prefix is the same identity-checked artifact the app
#    links through the merged closure). ONE CMake build dir per platform.
# ---------------------------------------------------------------------------
ENGINE_BUILD="$VELMUX_ROOT/build/ios-engine-$PLATFORM"
if [ ! -f "$ENGINE_BUILD/CMakeCache.txt" ]; then
  vlog "configuring the engine for $PLATFORM (CMake, deployment $VELMUX_IOS_DEPLOYMENT_TARGET)"
  cmake -S "$VELMUX_ROOT" -B "$ENGINE_BUILD" -G Xcode \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT="$PLATFORM" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$VELMUX_IOS_DEPLOYMENT_TARGET" \
    -DVELMUX_BUILD_TESTS=OFF \
    -DVELMUX_EMBEDDED_SQLITE=ON \
    -DVELMUX_FFMPEG_PREFIX="$FFMPEG_PREFIX"
fi
vlog "building the engine for $PLATFORM ($CONFIGURATION)"
cmake --build "$ENGINE_BUILD" --config "$CONFIGURATION" -- -jobs "$JOBS" \
  ARCHS=arm64 ONLY_ACTIVE_ARCH=NO

# ---------------------------------------------------------------------------
# 3. Kotlin Compose framework (static): the shared UI + engine client +
#    cinterop surface. The undefined velmux_* symbols resolve at the APP
#    link against the engine archives staged in step 2.
# ---------------------------------------------------------------------------
vlog "linking the Kotlin framework ($KOTLIN_TARGET, $CONFIGURATION)"
run_in "$VELMUX_ROOT" ./gradlew --no-daemon \
  ":ui:mobile:link${CONFIGURATION}Framework${KOTLIN_TARGET}"

FRAMEWORK_SRC="$VELMUX_ROOT/ui/mobile/build/bin/$(echo "$KOTLIN_TARGET" | sed 's/^./\L&/')/$(echo "$CONFIGURATION" | tr '[:upper:]' '[:lower:]')Framework/Velmux.framework"
[ -d "$FRAMEWORK_SRC" ] || vdie "Kotlin framework not found at $FRAMEWORK_SRC after the link task"

# ---------------------------------------------------------------------------
# 4. Stage everything under the stable layout the Xcode project references.
# ---------------------------------------------------------------------------
STAGE="$VELMUX_ROOT/build/ios/stage/$PLATFORM/$CONFIGURATION"
vlog "staging into $STAGE"
rm -rf "$STAGE"
mkdir -p "$STAGE/engine" "$STAGE/framework" "$STAGE/ffmpeg"

# The Xcode generator places products under <dir>/<config>-<platform>/ (the
# SDK-suffixed convention); resolve by SEARCHING so a layout change cannot
# silently break the stage.
ENGINE_LIBS="$(find "$ENGINE_BUILD" -name 'libvelmux_*.a' -path "*$CONFIGURATION*" | sort)"
[ -n "$ENGINE_LIBS" ] || vdie "no engine archives found under $ENGINE_BUILD ($CONFIGURATION)"
for lib in $ENGINE_LIBS; do
  cp "$lib" "$STAGE/engine/"
done

cp -R "$FRAMEWORK_SRC" "$STAGE/framework/Velmux.framework"

# The FFmpeg dependency closure for THIS slice, merged into one archive
# (the app link then has a single symbol-indexed input; slices may contain
# different library sets — the simulator slice omits SVT-AV1).
vlog "merging the FFmpeg closure for $FFMPEG_SLICE"
FFMPEG_LIBS="$(ls "$FFMPEG_PREFIX/lib/"*.a | sort)"
[ -n "$FFMPEG_LIBS" ] || vdie "no FFmpeg static libraries under $FFMPEG_PREFIX/lib"
( cd "$FFMPEG_PREFIX/lib" && libtool -static -o "$STAGE/ffmpeg/libffmpeg_deps.a" \
    $(printf '%s\n' $FFMPEG_LIBS | xargs -n1 basename | sort) )

# Test configuration header: the XCTest suite compiles its FFmpeg-dependent
# scaffolding only when the engine actually carries the FFmpeg backend (the
# prepare path guarantees it — fetch.sh is mandatory above).
mkdir -p "$VELMUX_ROOT/build/ios/stage/test-config"
cat > "$VELMUX_ROOT/build/ios/stage/test-config/VelmuxEngineTestConfig.h" <<EOF
// Generated by scripts/ios/prepare-xcode-inputs.sh — do not edit.
// The engine archives staged for the app build were configured with
// VELMUX_FFMPEG_PREFIX (fetch.sh is mandatory), so the FFmpeg backend is
// compiled in and the FFmpeg-dependent test scaffolding is live.
#define VELMUX_TEST_FFMPEG_BACKEND 1
EOF

vlog "staged: $(ls "$STAGE/engine" | tr '\n' ' ') + Velmux.framework + $(basename "$STAGE/ffmpeg")/libffmpeg_deps.a"
