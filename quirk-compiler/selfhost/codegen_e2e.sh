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

# Phase 4.4: Bool as a first-class binding type. The slot type
# now follows the RHS expression — `i1` for comparisons, `not`,
# and BoolLits; `i32` for everything else. `lli` would reject
# any mismatched store/load, so an `exit=42` here also implies
# the IR is well-typed.
run "bool literal binding"  "define main() -> Int { b := true; if b { return 42 } return 0 }"           42
run "bool literal false"    "define main() -> Int { b := false; if b { return 0 } return 42 }"          42
run "comparison binding"    "define main() -> Int { x := 5; b := x > 0; if b { return 42 } return 0 }"  42
run "not binding"           "define main() -> Int { b := not (1 == 2); if b { return 42 } return 0 }"   42
run "bool reassign"         "define main() -> Int { b := false; b = true; if b { return 42 } return 0 }" 42
run "bool in while cond"    "define main() -> Int { i := 0; cont := i < 3; while cont { i = i + 1; cont = i < 3 } return i + 39 }" 42

# Phase 4.5: Bool at the call boundary — Bool-returning helpers,
# Bool params, forward references where the callee is defined
# after the caller (the signature pre-pass must register both
# names before any body codegen runs).
run "bool return helper" \
    "define is_pos(x: Int) -> Bool { return x > 0 }
define main() -> Int { if is_pos(5) { return 42 } return 0 }" \
    42
run "bool return false path" \
    "define is_pos(x: Int) -> Bool { return x > 0 }
define main() -> Int { if is_pos(-3) { return 0 } return 42 }" \
    42
run "bool param + return" \
    "define flip(b: Bool) -> Bool { return not b }
define main() -> Int { if flip(false) { return 42 } return 0 }" \
    42
run "bool round-trip" \
    "define accept(b: Bool) -> Int { if b { return 42 } return 0 }
define main() -> Int { return accept(1 == 1) }" \
    42
run "forward reference (caller above callee)" \
    "define main() -> Int { if helper(7) { return 42 } return 0 }
define helper(n: Int) -> Bool { return n == 7 }" \
    42

# Phase 4.6: Double scalar — float literals, double arithmetic
# (fadd/fsub/fmul/fdiv), comparisons (fcmp ordered predicates),
# unary fneg, Double locals (slot type `double`), and Double at
# the call boundary.
run "double literal cmp"   "define main() -> Int { if 3.14 > 3.0 { return 42 } return 0 }"          42
run "double binding cmp"   "define main() -> Int { pi := 3.14; if pi > 3.0 { return 42 } return 0 }" 42
run "double add"           "define main() -> Int { a := 1.5; b := 2.5; if a + b == 4.0 { return 42 } return 0 }" 42
run "double sub mul div"   "define main() -> Int { x := 10.0; y := 3.0; q := x / y; if q > 3.0 { return 42 } return 0 }" 42
run "double unary minus"   "define main() -> Int { d := -2.5; if d < 0.0 { return 42 } return 0 }"  42
run "double param + return" \
    "define area(r: Double) -> Double { return r * r * 3.14 }
define main() -> Int { a := area(2.0); if a > 12.0 { return 42 } return 0 }" \
    42
run "double inequality"    "define main() -> Int { if 1.5 != 2.5 { return 42 } return 0 }"          42

# Phase 4.7: String at the call boundary — String locals,
# String params, String returns, and print() consuming any
# String-typed expression (not just inline literals).
run_with_stdout "print string local" \
    'define main() -> Int { s := "hello local"; print(s); return 42 }' \
    42 "hello local"
run_with_stdout "string param round-trip" \
    'define say(msg: String) -> Int { print(msg); return 0 }
define main() -> Int { return say("via param") + 42 }' \
    42 "via param"
run_with_stdout "string return" \
    'define greeting() -> String { return "hi from return" }
define main() -> Int { print(greeting()); return 42 }' \
    42 "hi from return"
run_with_stdout "string reassign" \
    'define main() -> Int { s := "first"; print(s); s = "second"; print(s); return 42 }' \
    42 "first
second"

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
echo "all 40/40 cases passed"
