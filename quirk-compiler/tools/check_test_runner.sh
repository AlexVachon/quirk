#!/usr/bin/env bash
# `quirk test` end to end on tools/fixtures/quirk_test: test_* discovery,
# per-test pass/fail, setup/teardown, a crash, a main() file and -k.
set -uo pipefail
cd "$(dirname "$0")/.."
QUIRK="${QUIRK:-bin/quirk}"
F=tools/fixtures/quirk_test
fails=0
expect() {   # expect <output> <grep -E pattern> <what>
    if printf '%s\n' "$1" | grep -qE -- "$2"; then echo "  ok $3"; else echo "FAIL $3"; fails=$((fails + 1)); fi
}
reject() {
    if printf '%s\n' "$1" | grep -qE -- "$2"; then echo "FAIL $3"; fails=$((fails + 1)); else echo "  ok $3"; fi
}

out=$(NO_COLOR=1 "$QUIRK" test "$F" 2>&1); rc=$?
[ $rc -ne 0 ] && echo "  ok exits non-zero when a test fails" || { echo "FAIL exit code $rc"; fails=$((fails + 1)); }
expect "$out" '✓ test_passes$'                         "passing test"
expect "$out" '✗ test_assert_fails$'                   "failing assertion"
expect "$out" '✗ test_teardown_fails$'                 "teardown failure fails its test"
expect "$out" '✗ test_runtime_error$'                  "runtime error fails its test"
reject "$out" 'test_takes_args'                         "functions with parameters aren't tests"
reject "$out" 'helper'                                  "other functions aren't tests"
expect "$out" 'basics_test.qk :: test_assert_fails'    "failure details per test"
expect "$out" 'line 20, in test_runtime_error'         "failure traceback names the test's line"
reject "$out" ', in main$'                              "generated runner frame hidden"
expect "$out" '✓ test_before$'                         "tests before a crash"
expect "$out" '✗ test_crashes$'                        "the crashing test"
expect "$out" 'test_after \(not run'                   "tests after a crash are listed as not run"
expect "$out" '✓ .*plain_main_test.qk'                 "main() file runs as one test"
expect "$out" '✗ test_plain_assert$'                   "a plain assert"
expect "$out" 'assert total == 10 failed: left is 9, right is 10' "assert shows both sides"
expect "$out" 'tests: 3 passed, 5 failed / 8 total'    "exact totals"

out=$(NO_COLOR=1 "$QUIRK" test -k assert "$F" 2>&1)
expect "$out" '✗ test_assert_fails$'                   "-k selects by name"
reject "$out" 'test_passes|plain_main_test'             "-k deselects the rest"

echo "quirk test runner: $([ $fails -eq 0 ] && echo ok || echo "$fails failed")"
exit $fails
