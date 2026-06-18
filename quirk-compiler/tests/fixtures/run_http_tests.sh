#!/bin/bash
# Wrapper that runs the http_client tests against the in-repo
# httpbin_lite.py fixture instead of the public httpbin.org. CI
# uses this unconditionally; locally it's the way to get a
# reliable run without depending on the public service's mood.
#
# Usage:
#     tests/fixtures/run_http_tests.sh [extra args passed to quirk]
#
# Env:
#     QUIRK_HTTP_FIXTURE_PORT   bind port for the fixture (default 8765)
#     QUIRK_HOME                quirk's resolution root (auto-set if unset)

set -euo pipefail

PORT="${QUIRK_HTTP_FIXTURE_PORT:-8765}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FIXTURE="$SCRIPT_DIR/httpbin_lite.py"
TEST_FILE="$REPO_ROOT/tests/http_client_test.quirk"

# Default QUIRK_HOME to the compiler repo root so the stdlib
# resolves out of packages/ when invoked from anywhere.
export QUIRK_HOME="${QUIRK_HOME:-$REPO_ROOT}"

# Start the fixture in the background, capture its PID.
python3 "$FIXTURE" --port "$PORT" &
FIXTURE_PID=$!

# Always kill the fixture on exit (success, failure, ^C, signal).
cleanup() {
    if kill -0 "$FIXTURE_PID" 2>/dev/null; then
        kill "$FIXTURE_PID" 2>/dev/null || true
        wait "$FIXTURE_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

# Wait for the fixture to bind (max 5s). Curl on the /headers
# endpoint is cheap and confirms both `listen` and the handler
# code path are live.
for i in 1 2 3 4 5 6 7 8 9 10; do
    if curl -s -o /dev/null "http://127.0.0.1:$PORT/headers"; then
        break
    fi
    sleep 0.5
done

# Point the test at the local fixture and force-run regardless of
# CI=true (we have a deterministic target, no flake risk).
export QUIRK_HTTP_BASE="http://127.0.0.1:$PORT"
export QUIRK_NETWORK_TESTS=1

"$REPO_ROOT/bin/quirk" run "$TEST_FILE" "$@"
