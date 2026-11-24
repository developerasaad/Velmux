#!/usr/bin/env bash
# Host test suite: the local development default (spec §11).
#
# Steps:
#   1. resolve the pinned host FFmpeg artifacts (identity-checked reuse or
#      rebuild — same path every app build takes);
#   2. configure the C++ tree against them (Debug, assertions on);
#   3. build and run every registered suite through ctest
#      (core, FFmpeg backend, bridge integration).
#
# Linux is not a shipping Velmux platform: this suite exists to exercise the
# real engine + FFmpeg backend, recovery and hardening logic cheaply.
set -euo pipefail

SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=common.sh
. "$SCRIPTS_DIR/lib/common.sh"

require_tool cmake
require_tool pkg-config
GENERATOR="Unix Makefiles"
if command -v ninja >/dev/null 2>&1; then
  GENERATOR="Ninja"
fi

# 1. Mandatory FFmpeg artifacts (reuse when the identity matches).
bash "$SCRIPTS_DIR/ffmpeg/fetch.sh" host
FFMPEG_PREFIX="$VELMUX_ROOT/$VELMUX_FFMPEG_OUT_DIR/host"
[ -f "$FFMPEG_PREFIX/lib/pkgconfig/libavcodec.pc" ] \
  || vdie "host FFmpeg artifacts incomplete after fetch"

# 2. Configure. CTEST_OUTPUT_ON_FAILURE keeps failures readable on CI.
BUILD_DIR="$VELMUX_ROOT/$VELMUX_HOST_BUILD_DIR"
vlog "configuring host build ($GENERATOR, FFmpeg: $FFMPEG_PREFIX)"
cmake -S "$VELMUX_ROOT" -B "$BUILD_DIR" -G "$GENERATOR" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DVELMUX_BUILD_TESTS=ON \
  -DVELMUX_FFMPEG_PREFIX="$FFMPEG_PREFIX"

# 3. Build + test.
vlog "building test binaries"
cmake --build "$BUILD_DIR" --parallel "${VELMUX_FFMPEG_JOBS:-$(nproc 2>/dev/null || echo 2)}"

vlog "running ctest"
ctest --test-dir "$BUILD_DIR" --output-on-failure

vlog "host suite passed"
