#!/usr/bin/env bash
# Shared UI suite: compile and test the Kotlin Multiplatform / Compose
# Multiplatform application (common code verified on the JVM target of the
# host; platform targets compile through the normal app builds).
set -euo pipefail

SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=common.sh
. "$SCRIPTS_DIR/lib/common.sh"

[ -f "$VELMUX_ROOT/settings.gradle.kts" ] \
  || vdie "settings.gradle.kts not found: the Kotlin Multiplatform project is not present in this tree"
[ -f "$VELMUX_ROOT/gradlew" ] \
  || vdie "gradlew wrapper not found (run from a repository checkout with ui/mobile present)"

require_tool java

vlog "Gradle: shared UI compile + JVM tests (:ui:mobile)"
run_in "$VELMUX_ROOT" ./gradlew --no-daemon :ui:mobile:jvmTest

vlog "UI suite passed"
