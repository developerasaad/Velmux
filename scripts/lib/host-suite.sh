#!/usr/bin/env bash
# Host test suite: the local development default (mandate §46).
#
# Steps:
#   1. resolve the pinned host FFmpeg artifacts (identity-checked reuse or
#      rebuild — same path every app build takes);
#   2. configure the C++ tree against them (Debug, assertions on);
#   3. build and run every registered suite through ctest
#      (core, FFmpeg backend, bridge integration).
#
# Sanitizer mode (--sanitize): reconfigures a SEPARATE build tree with
# AddressSanitizer + UndefinedBehaviorSanitizer instrumentation and runs
# the same suites. The prebuilt FFmpeg libraries stay uninstrumented
# (interceptors still catch libc-level misuse from any code); findings in
# Velmux code are what this mode exists for.
#
# Linux is not a shipping Velmux platform: this suite exists to exercise the
# real engine + FFmpeg backend, recovery and hardening logic cheaply.
set -euo pipefail

SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=common.sh
. "$SCRIPTS_DIR/lib/common.sh"

require_tool cmake
require_tool pkg-config

SANITIZE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --sanitize) SANITIZE=1 ;;
    *) vdie "unknown option: $1 (host-suite.sh)" ;;
  esac
  shift
done

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
if [ "$SANITIZE" = "1" ]; then
  BUILD_DIR="$VELMUX_ROOT/$VELMUX_HOST_BUILD_DIR-sanitize"
  SANITIZE_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
  vlog "configuring sanitized host build ($GENERATOR, ASan+UBSan)"
  cmake -S "$VELMUX_ROOT" -B "$BUILD_DIR" -G "$GENERATOR" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DVELMUX_BUILD_TESTS=ON \
    -DVELMUX_FFMPEG_PREFIX="$FFMPEG_PREFIX" \
    -DCMAKE_C_FLAGS="$SANITIZE_FLAGS" \
    -DCMAKE_CXX_FLAGS="$SANITIZE_FLAGS" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
  # ASan's memory-corruption detection is the goal here. Leak detection is
  # off: the uninstrumented FFmpeg dependency allocates one-time global
  # state that is intentionally never freed, and exit-time leak reports
  # from it drown the signal. (Velmux-owned resource leaks surface as
  # cross-suite heap growth and as corruption findings regardless.)
  export ASAN_OPTIONS="detect_leaks=0:abort_on_error=1"
  export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"
else
  BUILD_DIR="$VELMUX_ROOT/$VELMUX_HOST_BUILD_DIR"
  vlog "configuring host build ($GENERATOR, FFmpeg: $FFMPEG_PREFIX)"
  cmake -S "$VELMUX_ROOT" -B "$BUILD_DIR" -G "$GENERATOR" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DVELMUX_BUILD_TESTS=ON \
    -DVELMUX_FFMPEG_PREFIX="$FFMPEG_PREFIX"
fi

# 3. Build + test.
vlog "building test binaries"
cmake --build "$BUILD_DIR" --parallel "${VELMUX_FFMPEG_JOBS:-$(nproc 2>/dev/null || echo 2)}"

vlog "running ctest"
ctest --test-dir "$BUILD_DIR" --output-on-failure

vlog "host suite passed$([ "$SANITIZE" = "1" ] && echo ' (sanitized)')"
