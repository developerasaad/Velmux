#!/usr/bin/env bash
# Public build entry point. All build logic lives in the repository; CI and
# local developers run exactly this script.
#
#   ./scripts/build.sh --android --release
#   ./scripts/build.sh --ios --release
#   ./scripts/build.sh --android --ios --release
#
# FFmpeg is mandatory for every app build: each target resolves matching
# artifacts first (identity-checked reuse or rebuild) and fails when they
# cannot be produced.
set -euo pipefail

SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
. "$SCRIPTS_DIR/lib/common.sh"
# shellcheck source=lib/targets.sh
. "$SCRIPTS_DIR/lib/targets.sh"

# Read parse_build_args' line output into ARGS. while-read over process
# substitution, not mapfile: macOS runners still ship bash 3.2, where
# mapfile does not exist.
ARGS=()
while IFS= read -r line; do
  ARGS+=("$line")
done < <(parse_build_args "$@")
# parse_build_args prints nothing only for the help path (usage went to
# stderr, subshell exited 0) — propagate the clean exit.
[ "${#ARGS[@]}" -gt 0 ] || exit 0
TARGETS="${ARGS[0]#targets:}"
BUILD_TYPE="${ARGS[1]}"
ABIS="${ARGS[2]#abis:}"
JOBS="${ARGS[3]#jobs:}"

FAILED=0
for target in $TARGETS; do
  case "$target" in
    android)
      vlog "== target: android ($BUILD_TYPE, ABIs: $ABIS) =="
      bash "$SCRIPTS_DIR/android/build.sh" \
        --"$BUILD_TYPE" --abi "$(echo "$ABIS" | tr ' ' ',')" --jobs "$JOBS" \
        || FAILED=1
      ;;
    ios)
      vlog "== target: ios ($BUILD_TYPE, device arm64) =="
      bash "$SCRIPTS_DIR/ios/build.sh" --"$BUILD_TYPE" || FAILED=1
      ;;
    ffmpeg-host)
      vlog "== target: ffmpeg-host =="
      VELMUX_FFMPEG_JOBS="$JOBS" bash "$SCRIPTS_DIR/ffmpeg/build-linux.sh" \
        || FAILED=1
      ;;
    ffmpeg-android)
      vlog "== target: ffmpeg-android (ABIs: $ABIS) =="
      VELMUX_FFMPEG_JOBS="$JOBS" \
        bash "$SCRIPTS_DIR/ffmpeg/build-android.sh" $ABIS || FAILED=1
      ;;
    ffmpeg-ios)
      vlog "== target: ffmpeg-ios =="
      VELMUX_FFMPEG_JOBS="$JOBS" bash "$SCRIPTS_DIR/ffmpeg/build-ios.sh" \
        || FAILED=1
      ;;
    *)
      vdie "internal error: unknown target '$target'"
      ;;
  esac
done

[ "$FAILED" = "0" ] || vdie "one or more build targets failed"
vlog "build complete (targets: $TARGETS)"
