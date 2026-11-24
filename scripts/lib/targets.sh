#!/usr/bin/env bash
# Target and argument resolution shared by scripts/build.sh and
# scripts/test.sh. Keeps the public command surface short:
#
#   ./scripts/build.sh --android --release
#   ./scripts/build.sh --ios --release
#   ./scripts/build.sh --android --abi armeabi-v7a,x86_64
#   ./scripts/build.sh --ffmpeg-android --all-abis
#   ./scripts/test.sh                 (host tests)
#   ./scripts/test.sh --android-emulator --api 29
#   ./scripts/test.sh --ios-sim

# ---------------------------------------------------------------------------
# ABI list handling
# ---------------------------------------------------------------------------
normalize_abi_list() {
  # comma-separated -> space-separated, trimmed, deduplicated, validated
  local input="$1" out="" abi
  local seen=""
  IFS=',' read -ra _abis <<< "$input"
  for abi in "${_abis[@]}"; do
    abi="$(printf '%s' "$abi" | tr -d '[:space:]')"
    [ -n "$abi" ] || continue
    case " $VELMUX_ANDROID_ALL_ABIS " in
      *" $abi "*) ;;
      *) vdie "unknown Android ABI '$abi' (supported: $VELMUX_ANDROID_ALL_ABIS)" ;;
    esac
    case " $seen " in
      *" $abi "*) ;;
      *) seen="$seen $abi"; out="$out $abi" ;;
    esac
  done
  printf '%s' "${out# }"
}

validate_api_level() {
  case "$1" in
    29|30|31|32|33|34|35|36) ;;
    *) vdie "unsupported API level '$1' (tested set: $VELMUX_ANDROID_EMULATOR_APIS)" ;;
  esac
}

# ---------------------------------------------------------------------------
# build.sh argument parsing
# ---------------------------------------------------------------------------
# Outputs (echo, one per line — callers read with mapfile):
#   android | ios | ffmpeg-host | ffmpeg-android | ffmpeg-ios
#   release | debug
#   abis:<space-separated>
#   jobs:<n>
parse_build_args() {
  local target="" build_type="debug" abis="$VELMUX_ANDROID_DEFAULT_ABI" \
        all_abis="no" jobs="${VELMUX_FFMPEG_JOBS:-$(nproc 2>/dev/null || echo 2)}"
  while [ $# -gt 0 ]; do
    case "$1" in
      --android|--ios|--ffmpeg|--ffmpeg-host|--ffmpeg-android|--ffmpeg-ios)
        [ -z "$target" ] || vdie "only one target may be given"
        case "$1" in
          --android) target="android" ;;
          --ios) target="ios" ;;
          --ffmpeg|--ffmpeg-host) target="ffmpeg-host" ;;
          --ffmpeg-android) target="ffmpeg-android" ;;
          --ffmpeg-ios) target="ffmpeg-ios" ;;
        esac
        ;;
      --release) build_type="release" ;;
      --debug) build_type="debug" ;;
      --abi) shift; [ $# -gt 0 ] || vdie "--abi requires a value"; abis="$1" ;;
      --abi=*) abis="${1#--abi=}" ;;
      --all-abis) all_abis="yes" ;;
      --jobs) shift; [ $# -gt 0 ] || vdie "--jobs requires a value"; jobs="$1" ;;
      --jobs=*) jobs="${1#--jobs=}" ;;
      -h|--help) build_sh_usage; exit 0 ;;
      *) vdie "unknown build.sh option: $1" ;;
    esac
    shift
  done
  [ -n "$target" ] || { build_sh_usage >&2; vdie "no target given"; }
  if [ "$all_abis" = "yes" ]; then
    [ "$target" = "ffmpeg-android" ] || [ "$target" = "android" ] \
      || vdie "--all-abis applies to Android targets only"
    abis="$VELMUX_ANDROID_ALL_ABIS"
  fi
  abis="$(normalize_abi_list "$abis")"
  case "$jobs" in ''|*[!0-9]*) vdie "--jobs must be a positive integer" ;; esac
  echo "$target"
  echo "$build_type"
  echo "abis:$abis"
  echo "jobs:$jobs"
}

build_sh_usage() {
  cat <<'EOF'
Usage: ./scripts/build.sh <target> [options]

Targets (exactly one):
  --android          Android app (APK; AAB with --release)
  --ios              iOS app (device arm64; the simulator is a test target
                     only — use ./scripts/test.sh --ios-sim)
  --ffmpeg-host      Host FFmpeg artifacts (development/testing)
  --ffmpeg-android   Android FFmpeg artifacts
  --ffmpeg-ios       iOS FFmpeg artifacts (device + simulator slices)

Options:
  --release          Release configuration (default: debug)
  --abi <list>       Comma-separated Android ABIs (default: arm64-v8a)
  --all-abis         All supported ABIs (arm64-v8a, armeabi-v7a, x86_64)
  --jobs <n>         Parallel jobs for the heavy builds

FFmpeg is mandatory for every app build: the script resolves matching
artifacts (reusing them only when their build identity matches the current
tree) or builds them, and fails the build when that is impossible.

Examples:
  ./scripts/build.sh --android --release
  ./scripts/build.sh --android --abi armeabi-v7a,x86_64
  ./scripts/build.sh --ios --release
EOF
}

# ---------------------------------------------------------------------------
# test.sh argument parsing
# ---------------------------------------------------------------------------
parse_test_args() {
  local suite="${VELMUX_TEST_SUITE:-host}" api="" abi="$VELMUX_ANDROID_EMULATOR_ABI"
  while [ $# -gt 0 ]; do
    case "$1" in
      --host) suite="host" ;;
      --ts) suite="ts" ;;
      --android-emulator) suite="android-emulator" ;;
      --ios-sim) suite="ios-sim" ;;
      --android-device) suite="android-device" ;;
      --ios-device) suite="ios-device" ;;
      --api) shift; [ $# -gt 0 ] || vdie "--api requires a value"; api="$1" ;;
      --api=*) api="${1#--api=}" ;;
      --abi) shift; [ $# -gt 0 ] || vdie "--abi requires a value"; abi="$1" ;;
      --abi=*) abi="${1#--abi=}" ;;
      -h|--help) test_sh_usage; exit 0 ;;
      *) vdie "unknown test.sh option: $1" ;;
    esac
    shift
  done
  case "$suite" in
    android-emulator)
      [ -n "$api" ] || vdie "--android-emulator requires --api (tested set: $VELMUX_ANDROID_EMULATOR_APIS)"
      validate_api_level "$api"
      abi="$(normalize_abi_list "$abi")"
      ;;
    android-device) abi="$(normalize_abi_list "$abi")" ;;
  esac
  echo "$suite"
  echo "api:${api:-}"
  echo "abi:$abi"
}

test_sh_usage() {
  cat <<'EOF'
Usage: ./scripts/test.sh [suite] [options]

Suites:
  (default)          Host tests: pinned FFmpeg build + full C++ suite
                     (core, backends, bridge) through ctest
  --ts               TypeScript: type check + lint of the mobile app
  --android-emulator Automated emulator run (needs --api): boot, install,
                     cold-launch verification, instrumented engine tests,
                     relaunch/recovery checks
  --ios-sim          iOS simulator run: boot, install, cold-launch
                     verification, engine XCTest suite, relaunch
  --android-device   Physical Android device tests (requires a device)
  --ios-device       Physical iOS device tests (requires a device)

Options:
  --api <level>      Emulator API level (29 or 34)
  --abi <abi>        Emulator ABI (default: x86_64)

The host suite is the local development default; mobile product builds run
through ./scripts/build.sh (CI executes the same scripts).
EOF
}
