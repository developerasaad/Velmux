#!/usr/bin/env bash
# Public clean entry point. Removes generated build outputs while keeping
# dependency downloads (source tarballs) by default — they are expensive
# and content-verified, so reusing them across cleans is safe.
#
#   ./scripts/clean.sh            app + core build outputs, FFmpeg artifacts
#   ./scripts/clean.sh --all      also the builder state (downloads, src)
set -euo pipefail

SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
. "$SCRIPTS_DIR/lib/common.sh"

ALL="no"
while [ $# -gt 0 ]; do
  case "$1" in
    --all) ALL="yes" ;;
    -h|--help)
      cat <<'EOF'
Usage: ./scripts/clean.sh [--all]

  (default)  Remove build outputs: core host build, Android app build,
             iOS build/DerivedData, packaged FFmpeg artifacts.
  --all      Also remove the FFmpeg builder state (downloaded tarballs and
             extracted sources). The next build re-downloads everything.
EOF
      exit 0
      ;;
    *) vdie "unknown clean.sh option: $1" ;;
  esac
  shift
done

# Core host build tree.
rm -rf "$VELMUX_ROOT/$VELMUX_HOST_BUILD_DIR"

# Android app build outputs.
rm -rf "$VELMUX_ROOT/$VELMUX_APP_ANDROID_DIR/app/build" \
       "$VELMUX_ROOT/$VELMUX_APP_ANDROID_DIR/build" \
       "$VELMUX_ROOT/$VELMUX_APP_ANDROID_DIR/app/.cxx"

# iOS app build outputs and CocoaPods artifacts.
rm -rf "$VELMUX_ROOT/$VELMUX_APP_IOS_DIR/build" \
       "$VELMUX_ROOT/$VELMUX_APP_IOS_DIR/build-release" \
       "$VELMUX_ROOT/$VELMUX_APP_IOS_DIR/build-release-device" \
       "$VELMUX_ROOT/$VELMUX_APP_IOS_DIR/DerivedData" \
       "$VELMUX_ROOT/$VELMUX_APP_IOS_DIR/Pods"
rm -rf "$VELMUX_ROOT/ffmpeg-ios-packaged"

# Built FFmpeg artifacts (identity stamped; rebuilt on demand).
rm -rf "$VELMUX_ROOT/$VELMUX_FFMPEG_OUT_DIR"

if [ "$ALL" = "yes" ]; then
  rm -rf "$VELMUX_ROOT/$VELMUX_FFMPEG_BUILDER_DIR/src" \
         "$VELMUX_ROOT/$VELMUX_FFMPEG_BUILDER_DIR/downloads"
  vlog "builder state removed (downloads + sources)"
fi

vlog "clean complete"
