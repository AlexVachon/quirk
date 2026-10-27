#!/bin/bash
# Run every tests/probes/p*.quirk and fail on anything but a clean pass.
#
# A probe fails when it
#   - crashes (ICE, malformed IR, SIGSEGV / SIGABRT) or times out,
#   - reports a compile error (`[... ERROR]`, "Compilation failed"),
#   - prints a line starting with `FAIL`.
# Probes that exist to lock a diagnostic declare it with a comment:
#     // expect-error: Q0203
# and then fail only if that text is *missing* from the output.
#
# Usage (from quirk-compiler/):  tools/check_probes.sh [path/to/quirk-cpp]
# Set QUIRK_HOME to this checkout so the dev runtime.so is used. With
# QUIRK_CHECK_HEADERS=1 (as in CI) a probe also fails if any untyped value
# it touches lacks the runtime type header (see QuirkHeader in types.h).

QUIRK=${1:-./bin/quirk-cpp}
fails=0
total=0
for p in tests/probes/p*.quirk; do
    total=$((total + 1))
    out=$(timeout 10 "$QUIRK" run --no-aot --no-cache "$p" 2>&1 < /dev/null)
    rc=$?
    plain=$(printf '%s\n' "$out" | sed 's/\x1b\[[0-9;]*m//g')
    expect=$(grep -m1 -o '// expect-error: .*' "$p" | sed 's#// expect-error: ##')

    reason=""
    if printf '%s\n' "$plain" | grep -qE "Internal compiler error|malformed IR|core dumped|Aborted|Segmentation fault"; then
        reason="crashed"
    elif [ $rc -eq 124 ]; then
        reason="timed out"
    elif printf '%s\n' "$plain" | grep -q "^\[quirk-headers\] no header:"; then
        # Only reported with QUIRK_CHECK_HEADERS=1 (CI sets it): a value
        # object was allocated without the runtime type header.
        reason="object without type header"
    elif [ -n "$expect" ]; then
        printf '%s\n' "$plain" | grep -qF -- "$expect" || reason="expected error '$expect' was not reported"
    elif printf '%s\n' "$plain" | grep -qE "ERROR\]|^Compilation failed"; then
        reason="compile error"
    elif printf '%s\n' "$plain" | grep -qE "^FAIL"; then
        reason="reported FAIL"
    fi

    if [ -n "$reason" ]; then
        fails=$((fails + 1))
        if [ -n "$GITHUB_ACTIONS" ]; then
            echo "::error file=$p::probe $reason: $p"
        else
            echo "FAIL $p — $reason"
        fi
        printf '%s\n' "$plain" | head -6 | sed 's/^/    /'
    fi
done
echo "probes: $((total - fails))/$total passed"
exit $(( fails > 0 ? 1 : 0 ))
