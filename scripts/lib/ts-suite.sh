#!/usr/bin/env bash
# TypeScript suite: type check + production lint of the mobile app.
set -euo pipefail

SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=common.sh
. "$SCRIPTS_DIR/lib/common.sh"

ensure_js_deps
JS_DIR="$(repo_dir "$VELMUX_APP_JS_DIR")"

vlog "TypeScript: type check (tsc --noEmit)"
run_in "$JS_DIR" npm run typecheck --silent

vlog "TypeScript: lint"
run_in "$JS_DIR" npm run lint --silent

vlog "TypeScript suite passed"
