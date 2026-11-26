#!/usr/bin/env bash
# Public test entry point. All test logic lives in the repository; CI and
# local developers run exactly this script.
#
#   ./scripts/test.sh                    host suite (default, Linux/macOS)
#   ./scripts/test.sh --sanitize         host suite under ASan + UBSan
#   ./scripts/test.sh --ui               shared Kotlin/Compose suite
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

# Read parse_test_args' line output into ARGS. while-read over process
# substitution, not mapfile: macOS runners still ship bash 3.2, where
# mapfile does not exist.
ARGS=()
while IFS= read -r line; do
  ARGS+=("$line")
done < <(parse_test_args "$@")
# parse_test_args prints nothing only for the help path (usage went to
# stderr, subshell exited 0) — propagate the clean exit.
[ "${#ARGS[@]}" -gt 0 ] || exit 0
SUITE="${ARGS[0]}"
API="${ARGS[1]#api:}"
ABI="${ARGS[2]#abi:}"
SANITIZE="${ARGS[3]#sanitize:}"

case "$SUITE" in
  host)
    if [ -n "$SANITIZE" ]; then
      bash "$SCRIPTS_DIR/lib/host-suite.sh" --sanitize
    else
      bash "$SCRIPTS_DIR/lib/host-suite.sh"
    fi
    ;;
  ui) bash "$SCRIPTS_DIR/lib/ui-suite.sh" ;;
  android-emulator)
    bash "$SCRIPTS_DIR/android/emulator-test.sh" --api "$API" --abi "$ABI"
    ;;
  ios-sim) bash "$SCRIPTS_DIR/ios/simulator-test.sh" ;;
  android-device) bash "$SCRIPTS_DIR/android/device-test.sh" --abi "$ABI" ;;
  ios-device) bash "$SCRIPTS_DIR/ios/device-test.sh" ;;
  *) vdie "internal error: unknown suite '$SUITE'" ;;
esac
