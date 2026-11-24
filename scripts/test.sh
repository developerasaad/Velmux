#!/usr/bin/env bash
# Public test entry point. All test logic lives in the repository; CI and
# local developers run exactly this script.
#
#   ./scripts/test.sh                    host suite (default, Linux/macOS)
#   ./scripts/test.sh --ts               TypeScript suite
#   ./scripts/test.sh --android-emulator --api 29
#   ./scripts/test.sh --ios-sim
#   ./scripts/test.sh --android-device
#   ./scripts/test.sh --ios-device
#
# The host suite is the local development default (spec §11): it resolves
# the pinned host FFmpeg artifacts (identity-checked reuse or rebuild),
# configures the C++ tree against them, builds every registered suite
# (core, FFmpeg backend, bridge) and runs ctest.
set -euo pipefail

SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
. "$SCRIPTS_DIR/lib/common.sh"
# shellcheck source=lib/targets.sh
. "$SCRIPTS_DIR/lib/targets.sh"

mapfile -t ARGS < <(parse_test_args "$@")
# parse_test_args prints nothing only for the help path (usage went to
# stderr, subshell exited 0) — propagate the clean exit.
[ "${#ARGS[@]}" -gt 0 ] || exit 0
SUITE="${ARGS[0]}"
API="${ARGS[1]#api:}"
ABI="${ARGS[2]#abi:}"

case "$SUITE" in
  host) bash "$SCRIPTS_DIR/lib/host-suite.sh" ;;
  ts) bash "$SCRIPTS_DIR/lib/ts-suite.sh" ;;
  android-emulator)
    bash "$SCRIPTS_DIR/android/emulator-test.sh" --api "$API" --abi "$ABI"
    ;;
  ios-sim) bash "$SCRIPTS_DIR/ios/simulator-test.sh" ;;
  android-device) bash "$SCRIPTS_DIR/android/device-test.sh" --abi "$ABI" ;;
  ios-device) bash "$SCRIPTS_DIR/ios/device-test.sh" ;;
  *) vdie "internal error: unknown suite '$SUITE'" ;;
esac
