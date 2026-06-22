#!/bin/bash
# Phase 4 end-to-end smoke. For each case:
#   1. Write a per-case driver .quirk file inside selfhost/ that
#      bakes in the source as a string literal and emits the IR.
#      (Relative imports require the driver to live alongside
#      lexer/parser/sema/codegen; passing source via sys.argv
#      hit an unrelated bug worth tracking separately.)
#   2. Run the driver, capturing IR on stdout.
#   3. Pipe to lli-14 and compare the exit code against expected.

set -u
QUIRK="${QUIRK:-./bin/quirk}"
LLI="${LLI:-lli-14}"
export QUIRK_HOME="${QUIRK_HOME:-$(pwd)}"

declare -i fails=0

run() {
    local label="$1"
    local src="$2"
    local expected="$3"
    local driver="selfhost/_case.quirk"
    local ir_path
    ir_path=$(mktemp --suffix=.ll)

    # Re-quote the source as a Quirk string literal. We only need
    # to escape backslashes and double quotes; newlines stay
    # literal in the heredoc.
    local quoted
    quoted=$(printf %s "$src" | python3 -c '
import sys, json
print(json.dumps(sys.stdin.read()))
')

    cat > "$driver" <<EOF
from .lexer use { tokenize }
from .parser use { parse }
from .sema use { check }
from .codegen use { emit_module }

src := $quoted
tokens := tokenize(src)
decls := parse(tokens)
errors := check(decls)
if errors.length() > 0 {
    print("SEMA FAILED:")
    k := 0
    while k < errors.length() {
        e: String := errors.__get(k)
        print("  " + e)
        k = k + 1
    }
} else {
    print(emit_module(decls))
}
EOF

    "$QUIRK" --no-aot --no-cache "$driver" > "$ir_path" 2>/dev/null
    if grep -q "SEMA FAILED" "$ir_path"; then
        echo "FAIL  $label  (sema rejected)"
        cat "$ir_path"
        fails+=1
        rm -f "$ir_path" "$driver"
        return
    fi
    "$LLI" "$ir_path"
    local got=$?
    if [ "$got" -eq "$expected" ]; then
        echo "ok    $label  (exit=$got)"
        rm -f "$ir_path"
    else
        echo "FAIL  $label  (exit=$got, expected $expected)"
        echo "      ir at $ir_path"
        fails+=1
    fi
    rm -f "$driver"
}

# Variant that also checks stdout against an expected substring.
# Used by the print-via-puts cases — `lli` exit-code coverage
# alone can't see the printed text.
run_with_stdout() {
    local label="$1"
    local src="$2"
    local expected_exit="$3"
    local expected_out="$4"
    local driver="selfhost/_case.quirk"
    local ir_path
    ir_path=$(mktemp --suffix=.ll)
    local quoted
    quoted=$(printf %s "$src" | python3 -c 'import sys, json; print(json.dumps(sys.stdin.read()))')
    cat > "$driver" <<EOF
from .lexer use { tokenize }
from .parser use { parse }
from .sema use { check }
from .codegen use { emit_module }
src := $quoted
tokens := tokenize(src)
decls := parse(tokens)
errors := check(decls)
if errors.length() > 0 {
    print("SEMA FAILED:")
    k := 0
    while k < errors.length() {
        e: String := errors.__get(k)
        print("  " + e)
        k = k + 1
    }
} else {
    print(emit_module(decls))
}
EOF
    "$QUIRK" --no-aot --no-cache "$driver" > "$ir_path" 2>/dev/null
    rm -f "$driver"
    if grep -q "SEMA FAILED" "$ir_path"; then
        echo "FAIL  $label  (sema rejected)"
        cat "$ir_path"
        fails+=1
        rm -f "$ir_path"
        return
    fi
    local got_out
    got_out=$("$LLI" "$ir_path")
    local got_exit=$?
    if [ "$got_exit" -eq "$expected_exit" ] && [ "$got_out" = "$expected_out" ]; then
        echo "ok    $label  (exit=$got_exit, stdout=$got_out)"
        rm -f "$ir_path"
    else
        echo "FAIL  $label  (exit=$got_exit, expected $expected_exit; stdout='$got_out', expected '$expected_out')"
        echo "      ir at $ir_path"
        fails+=1
    fi
}

run "return literal"   "define main() -> Int { return 42 }"                                  42
run "arithmetic"       "define main() -> Int { return 3 + 4 * 5 }"                           23
run "vars + reassign"  "define main() -> Int { n := 10; n = n * 4; return n + 2 }"           42
run "call"             "define double_(x: Int) -> Int { return x * 2 }
define main() -> Int { return double_(21) }"                                                 42

# Phase 4.1: control flow + comparisons.
run "if true branch"   "define main() -> Int { x := 5; if x > 0 { return 42 } else { return 0 } }"        42
run "if false branch"  "define main() -> Int { x := -5; if x > 0 { return 0 } else { return 42 } }"     42
run "if without else"  "define main() -> Int { y := 10; if y == 10 { y = 42 } return y }"                42
run "while accumulate" "define main() -> Int { n := 0; i := 0; while i < 7 { n = n + 6; i = i + 1 } return n }"  42
run "while early exit via mutated bound" "define main() -> Int { n := 0; cap := 10; while n < cap { n = n + 1; if n == 5 { cap = n } } return n + 37 }"  42

# Phase 4.2: unary `-` and `not`.
run "unary minus literal" "define main() -> Int { return -(-42) }"                                                      42
run "unary minus in expr" "define main() -> Int { x := -10; return -x + 32 }"                                          42
run "not flips false"     "define main() -> Int { x := 5; if not (x > 100) { return 42 } return 0 }"                   42
run "not flips true"      "define main() -> Int { x := 5; if not (x > 0) { return 0 } return 42 }"                     42
run "double not"          "define main() -> Int { if not not (1 == 1) { return 42 } return 0 }"                        42

# Phase 4.3: string literals + print() via puts().
run_with_stdout "print literal" \
    'define main() -> Int { print("hello"); return 42 }' \
    42 "hello"
run_with_stdout "print twice" \
    'define main() -> Int { print("first"); print("second"); return 0 }' \
    0 "first
second"
run_with_stdout "print inside if" \
    'define main() -> Int { x := 7; if x > 0 { print("positive") } else { print("nope") } return x * 6 }' \
    42 "positive"
run_with_stdout "print in loop" \
    'define main() -> Int { i := 0; while i < 3 { print("tick"); i = i + 1 } return 0 }' \
    0 "tick
tick
tick"

if [ "$fails" -gt 0 ]; then
    echo ""
    echo "$fails case(s) failed"
    exit 1
fi
echo ""
echo "all 18/18 cases passed"
