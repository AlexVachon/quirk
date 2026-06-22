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

run "return literal"   "define main() -> Int { return 42 }"                                  42
run "arithmetic"       "define main() -> Int { return 3 + 4 * 5 }"                           23
run "vars + reassign"  "define main() -> Int { n := 10; n = n * 4; return n + 2 }"           42
run "call"             "define double_(x: Int) -> Int { return x * 2 }
define main() -> Int { return double_(21) }"                                                 42

if [ "$fails" -gt 0 ]; then
    echo ""
    echo "$fails case(s) failed"
    exit 1
fi
echo ""
echo "all 4/4 cases passed"
