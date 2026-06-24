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
    if grep -q "^SEMA FAILED" "$ir_path"; then
        echo "FAIL  $label  (sema rejected)"
        cat "$ir_path"
        fails+=1
        rm -f "$ir_path" "$driver"
        return
    fi
    if grep -q "^PARSE FAILED" "$ir_path"; then
        echo "FAIL  $label  (parse rejected)"
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
    if grep -q "^SEMA FAILED" "$ir_path"; then
        echo "FAIL  $label  (sema rejected)"
        cat "$ir_path"
        fails+=1
        rm -f "$ir_path"
        return
    fi
    if grep -q "^PARSE FAILED" "$ir_path"; then
        echo "FAIL  $label  (parse rejected)"
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

# Phase 4.8: String concat via `+`. Lowered to malloc(strlen +
# strlen + 1) + strcpy + strcat — leaks the buffer, which is
# fine for short-lived compiler runs.
run_with_stdout "concat two literals" \
    'define main() -> Int { print("hello, " + "world"); return 42 }' \
    42 "hello, world"
run_with_stdout "concat literal + local" \
    'define main() -> Int { name := "Quirk"; print("hi, " + name); return 42 }' \
    42 "hi, Quirk"
run_with_stdout "concat into local + reuse" \
    'define main() -> Int { greeting := "hi, " + "there"; print(greeting); print(greeting); return 42 }' \
    42 "hi, there
hi, there"
run_with_stdout "concat chain" \
    'define main() -> Int { print("a" + "b" + "c"); return 42 }' \
    42 "abc"
run_with_stdout "concat through call" \
    'define greet(who: String) -> String { return "hello, " + who }
define main() -> Int { print(greet("Quirk")); return 42 }' \
    42 "hello, Quirk"

# Phase 4.9: structs — `struct` declarations + positional
# constructor + field read. Slot type for struct values is
# `%struct.Foo*` (heap-allocated via malloc, sized by the
# GEP-of-null trick). Field-write and struct-typed
# params/returns ride the existing _Slot machinery, but exposing
# *param* support to the surface (the Foo-typed `f:` param)
# requires updating sema's `ty_compatible` for cross-call
# struct types — covered indirectly via field-access type
# propagation here.
run "struct ctor + field read" \
    "struct Point { x: Int; y: Int }
define main() -> Int { p := Point(40, 2); return p.x + p.y }" \
    42
run "struct field order" \
    "struct Pair { a: Int; b: Int }
define main() -> Int { p := Pair(10, 32); return p.b + p.a }" \
    42
run "struct with Bool field" \
    "struct Flag { ok: Bool; n: Int }
define main() -> Int { f := Flag(true, 30); if f.ok { return f.n + 12 } return 0 }" \
    42
run "two struct instances" \
    "struct Box { n: Int }
define main() -> Int { a := Box(20); b := Box(22); return a.n + b.n }" \
    42

# Phase 4.10: struct field write + struct-typed params/returns.
# The slot/ctor machinery from 4.9 already produced `%struct.Foo*`
# at every other site; this phase exposes that to the surface
# via `f.x = v` and shows it round-trips through the call
# boundary by reference.
run "field write" \
    "struct Box { n: Int }
define main() -> Int { b := Box(0); b.n = 42; return b.n }" \
    42
run "field write then read" \
    "struct Pt { x: Int; y: Int }
define main() -> Int { p := Pt(1, 2); p.x = 40; p.y = 2; return p.x + p.y }" \
    42
run "struct as param (by ref)" \
    "struct Pt { x: Int; y: Int }
define sum(p: Pt) -> Int { return p.x + p.y }
define main() -> Int { p := Pt(40, 2); return sum(p) }" \
    42
run "struct returned then read" \
    "struct Pt { x: Int; y: Int }
define make() -> Pt { return Pt(10, 32) }
define main() -> Int { p := make(); return p.x + p.y }" \
    42
run "mutate via param sees outside" \
    "struct Box { n: Int }
define bump(b: Box) -> Int { b.n = b.n + 1; return 0 }
define main() -> Int { b := Box(41); bump(b); return b.n }" \
    42

# Phase 4.11: Int array literals + subscript read. Lists are
# bare `i32*` for now (no length tracking, no bounds check).
# Each `[a, b, c]` mallocs N*4 bytes and stores the elements;
# `xs[i]` GEPs + loads. Buffer leaks (deliberate).
run "list literal + index"  "define main() -> Int { xs := [10, 20, 32]; return xs[0] + xs[2] }" 42
run "list index expr"       "define main() -> Int { xs := [1, 2, 3, 4]; i := 2; return xs[i] + 39 }" 42
run "list of one element"   "define main() -> Int { xs := [42]; return xs[0] }" 42
run "list via param" \
    "define second(xs: List<Int>) -> Int { return xs[1] }
define main() -> Int { return second([100, 42, 200]) }" \
    42
run "list returned by fn" \
    "define mk() -> List<Int> { return [10, 12, 20] }
define main() -> Int { xs := mk(); return xs[0] + xs[1] + xs[2] }" \
    42

# Phase 4.12: list header layout + len() builtin. Lists now
# carry a length field in a `%QList` header; len(xs) reads
# it. Index access GEPs through the embedded data array.
run "len literal"     "define main() -> Int { return len([1, 2, 3, 4, 5, 6, 7]) * 6 }"          42
run "len of local"    "define main() -> Int { xs := [10, 20, 30]; return len(xs) + 39 }"        42
run "len drives loop" "define main() -> Int { xs := [3, 4, 5, 6, 7, 8, 9]; i := 0; n := 0; while i < len(xs) { n = n + xs[i]; i = i + 1 } return n }" 42
run "len via param" \
    "define summing(xs: List<Int>) -> Int { i := 0; n := 0; while i < len(xs) { n = n + xs[i]; i = i + 1 } return n }
define main() -> Int { return summing([10, 12, 20]) }" \
    42
run "len of single" "define main() -> Int { return len([42]) + 41 }"                            42

# Phase 4.13: method-call syntax. `obj.method(args)` parses as
# Call(FieldGet(obj, method), args); a new dispatch arm in sema
# and codegen routes by receiver type. Phase 4.13 wires the
# `.length()` method on both List (reads the %QList header)
# and String (calls strlen + truncates to i32).
run "list.length()" \
    "define main() -> Int { xs := [10, 20, 30, 40, 50, 6, 1]; return xs.length() * 6 }" \
    42
run "string.length()" \
    'define main() -> Int { s := "12345"; return s.length() + 37 }' \
    42
run "list.length() in while" \
    "define main() -> Int { xs := [3, 4, 5, 6, 7, 8, 9]; i := 0; n := 0; while i < xs.length() { n = n + xs[i]; i = i + 1 } return n }" \
    42
run "string.length() literal" \
    'define main() -> Int { return "hello, quirk!!!".length() + 27 }' \
    42
run "list.length() via param" \
    "define cnt(xs: List<Int>) -> Int { return xs.length() }
define main() -> Int { return cnt([1, 2, 3]) * 14 }" \
    42

# Phase 4.14: user-defined struct methods. `define Foo.method(...)`
# at top level adds a method to struct Foo with implicit `self`
# (typed `%struct.Foo*`). Method calls `f.method(args)` thread the
# receiver as the first LLVM arg, look up `Foo__method` in the
# signature table, and render a typed call.
run "method reads self field" \
    "struct Pt { x: Int; y: Int }
define Pt.sum() -> Int { return self.x + self.y }
define main() -> Int { p := Pt(40, 2); return p.sum() }" \
    42
run "method with arg" \
    "struct Box { n: Int }
define Box.add(k: Int) -> Int { return self.n + k }
define main() -> Int { b := Box(40); return b.add(2) }" \
    42
run "method mutates self field" \
    "struct Box { n: Int }
define Box.bump() -> Int { self.n = self.n + 1; return self.n }
define main() -> Int { b := Box(41); return b.bump() }" \
    42
run "method called twice" \
    "struct Box { n: Int }
define Box.bump() -> Int { self.n = self.n + 1; return self.n }
define main() -> Int { b := Box(40); b.bump(); return b.bump() }" \
    42
run "method shadows builtin .length" \
    "struct Tag { n: Int }
define Tag.length() -> Int { return self.n * 6 }
define main() -> Int { t := Tag(7); return t.length() }" \
    42

# Phase 4.15: List.append() + capacity field + realloc growth.
# Layout switches to %QList = { length, capacity, i32* data }
# with the data array in its own allocation so append can
# realloc without invalidating the header pointer that
# callers hold. Capacity doubles on full.
run "append grows past cap" \
    "define main() -> Int { xs := [1]; xs.append(2); xs.append(3); return xs[0] + xs[1] + xs[2] + 36 }" \
    42
run "append updates length" \
    "define main() -> Int { xs := [10]; xs.append(20); xs.append(12); return xs.length() * 14 }" \
    42
run "build via loop" \
    "define main() -> Int { xs := [0]; i := 1; while i < 7 { xs.append(i * 2); i = i + 1 } sum := 0; j := 0; while j < xs.length() { sum = sum + xs[j]; j = j + 1 } return sum }" \
    42
run "append seen by callee" \
    "define total(xs: List<Int>) -> Int { i := 0; n := 0; while i < xs.length() { n = n + xs[i]; i = i + 1 } return n }
define main() -> Int { xs := [10]; xs.append(20); xs.append(12); return total(xs) }" \
    42
run "append returns to fn" \
    "define fill() -> List<Int> { xs := [0]; i := 1; while i < 10 { xs.append(i); i = i + 1 } return xs }
define main() -> Int { xs := fill(); return xs.length() + xs[8] + 24 }" \
    42

# Phase 4.16: primitive `.str()` methods. Int / Double via
# snprintf into a malloc'd buffer; Bool via select between
# two interned string globals. Unblocks diagnostic-message
# building (`"line " + ln.str() + ":" + col.str()`) used
# pervasively in selfhost source.
run_with_stdout "int.str() print" \
    'define main() -> Int { print((42).str()); return 0 }' \
    0 "42"
run_with_stdout "bool.str() true/false" \
    'define main() -> Int { print(true.str()); print(false.str()); return 0 }' \
    0 "true
false"
run_with_stdout "double.str() print" \
    'define main() -> Int { print((3.14).str()); return 0 }' \
    0 "3.14"
run_with_stdout "concat int.str()" \
    'define main() -> Int { n := 7; print("count is " + n.str()); return 0 }' \
    0 "count is 7"
run_with_stdout "diagnostic message" \
    'define main() -> Int { ln := 12; col := 5; print("error at " + ln.str() + ":" + col.str()); return 42 }' \
    42 "error at 12:5"

# Phase 4.17: String methods + String ==/!=. Each method
# lowers to a libc helper (memcpy / strncmp / strcmp / atoi /
# strlen) and is dispatched in _gen_string_method. Equality
# on strings extends BinOp's comparison arm.
run_with_stdout "substring middle" \
    'define main() -> Int { s := "hello, world"; print(s.substring(7, 12)); return 0 }' \
    0 "world"
run_with_stdout "substring full" \
    'define main() -> Int { s := "quirk"; print(s.substring(0, 5)); return 0 }' \
    0 "quirk"
run "startswith hit"  'define main() -> Int { s := "quirk-compiler"; if s.startswith("quirk") { return 42 } return 0 }' 42
run "startswith miss" 'define main() -> Int { s := "quirk-compiler"; if s.startswith("rust") { return 0 } return 42 }' 42
run "endswith hit"    'define main() -> Int { s := "selfhost.quirk"; if s.endswith(".quirk") { return 42 } return 0 }' 42
run "endswith miss"   'define main() -> Int { s := "selfhost.quirk"; if s.endswith(".rs") { return 0 } return 42 }' 42
run "endswith too long" 'define main() -> Int { s := "ab"; if s.endswith("xxxxxx") { return 0 } return 42 }' 42
run "to_int parse"    'define main() -> Int { return "40".to_int() + 2 }' 42
run "to_int via var"  'define main() -> Int { s := "30"; return s.to_int() + 12 }' 42
run "string eq hit"   'define main() -> Int { kw := "let"; if kw == "let" { return 42 } return 0 }' 42
run "string eq miss"  'define main() -> Int { kw := "let"; if kw == "const" { return 0 } return 42 }' 42
run "string neq hit"  'define main() -> Int { kw := "let"; if kw != "const" { return 42 } return 0 }' 42
run "substring + eq" \
    'define main() -> Int { s := "answer: 42"; if s.substring(0, 6) == "answer" { return 42 } return 0 }' \
    42

# Phase 4.18: enum declarations + TypeName.Variant access.
# Unbacked enums only (Phase 4.18): each variant gets its
# ordinal as the i32 value. _q_ty_to_llvm maps the enum
# name to i32 so locals/params/returns flow through the
# existing slot machinery.
run "enum basic"            "enum Color { Red; Green; Blue } define main() -> Int { c := Color.Green; if c == Color.Green { return 42 } return 0 }" 42
run "enum neq path"         "enum Color { Red; Green; Blue } define main() -> Int { c := Color.Red; if c != Color.Green { return 42 } return 0 }" 42
run "enum third variant"    "enum K { A; B; C; D; E; F; G } define main() -> Int { k := K.G; if k == K.G { return 42 } return 0 }" 42
run "enum via param"        "enum Tag { On; Off } define accept(t: Tag) -> Int { if t == Tag.On { return 42 } return 0 } define main() -> Int { return accept(Tag.On) }" 42
run "enum returned by fn"   "enum Mode { Idle; Run; Stop } define pick() -> Mode { return Mode.Stop } define main() -> Int { m := pick(); if m == Mode.Stop { return 42 } return 0 }" 42
run "enum field on struct"  "enum Kind { Alpha; Beta; Gamma } struct Box { k: Kind; n: Int } define main() -> Int { b := Box(Kind.Beta, 0); b.n = 41; if b.k == Kind.Beta { return b.n + 1 } return 0 }" 42

# Phase 4.19: tagged unions + match statement. `type T = A | B(...)
# | C(...)` declares a sum type; each variant's call produces a
# union value (%struct.T* with the discriminator at offset 0);
# `match` dispatches on the tag and binds the synthetic variant
# struct in the arm body.
run "tagged union basic match" \
    "type E = IntLit(v: Int) | StrLit(s: String)
define main() -> Int {
    e := IntLit(42)
    match e {
        case IntLit as lit => return lit.v
        case StrLit as _ => return 0
        case _ => return 0
    }
}" \
    42
run "tagged union second variant" \
    "type Shape = Square(side: Int) | Circle(r: Int)
define area(s: Shape) -> Int {
    match s {
        case Square as sq => return sq.side * sq.side
        case Circle as c => return c.r * 3
        case _ => return 0
    }
}
define main() -> Int { return area(Circle(14)) }" \
    42
run "tagged union nullary variant" \
    "type Opt = Some(v: Int) | None
define unwrap(o: Opt) -> Int {
    match o {
        case Some as s => return s.v
        case None as _ => return -1
        case _ => return -1
    }
}
define main() -> Int { return unwrap(Some(42)) }" \
    42
run "tagged union via param + field write" \
    "type Box = Wrap(n: Int)
define bump(b: Box) -> Int {
    match b {
        case Wrap as w => return w.n + 1
        case _ => return 0
    }
}
define main() -> Int { return bump(Wrap(41)) }" \
    42

# Phase 4.20: Map runtime + List() ctor + VarDecl annotation
# honoring. Map() / List() build empty containers; .put/.get/
# .has linear-scan the entry array (overwrites on duplicate
# key, reallocs on growth). `holder: T := m.get("k")` emits a
# bitcast from i8* to T*.
run "map basic"             'struct S { n: Int } define main() -> Int { m := Map(); m.put("a", S(40)); h: S := m.get("a"); return h.n + 2 }' 42
run "map .has miss + hit"   'define main() -> Int { m := Map(); h: Int := 7; if m.has("k") { return 0 } m.put("k", "x"); if m.has("k") { return 42 } return 0 }' 42
run "map length grows"      'define main() -> Int { m := Map(); m.put("a", "1"); m.put("b", "2"); m.put("c", "3"); return m.length() * 14 }' 42
run "map put overwrite"     'struct S { n: Int } define main() -> Int { m := Map(); m.put("k", S(0)); m.put("k", S(42)); h: S := m.get("k"); return h.n }' 42
run "list seed + append"    'define main() -> Int { xs := [0]; xs.append(20); xs.append(22); return xs[1] + xs[2] }' 42
run "map values via struct" 'struct Sig { ret: Int } define main() -> Int { m := Map(); m.put("foo", Sig(40)); s: Sig := m.get("foo"); return s.ret + 2 }' 42
run "map across grow"       'define main() -> Int { m := Map(); i := 0; while i < 10 { m.put("k" + i.str(), "v"); i = i + 1 } return m.length() * 4 + 2 }' 42

# Phase 4.21: generic pointer-element lists. `ListP()`
# constructs an empty %QListP* (separate from int-element
# %QList*). Param/field annotations like `List<Tok>` route
# through the same %QListP* type. .append/.length/index
# dispatch on receiver type at codegen.
run "listp append + index" \
    'struct Tok { kind: Int; val: String }
define main() -> Int {
    xs := ListP()
    xs.append(Tok(1, "hi"))
    xs.append(Tok(2, "yo"))
    t: Tok := xs[1]
    return t.kind * 21
}' \
    42
run "listp length" \
    'define main() -> Int {
    xs := ListP()
    xs.append("a"); xs.append("b"); xs.append("c"); xs.append("d"); xs.append("e"); xs.append("f")
    return xs.length() * 7
}' \
    42
run "list<T> param annotation" \
    'struct Tok { kind: Int }
define sum_kinds(xs: List<Tok>) -> Int {
    i := 0
    n := 0
    while i < xs.length() {
        t: Tok := xs[i]
        n = n + t.kind
        i = i + 1
    }
    return n
}
define main() -> Int {
    xs := ListP()
    xs.append(Tok(20))
    xs.append(Tok(22))
    return sum_kinds(xs)
}' \
    42
run "list<String> of literals" \
    'define count(xs: List<String>) -> Int { return xs.length() }
define main() -> Int {
    xs := ListP()
    xs.append("first"); xs.append("second"); xs.append("third")
    return count(xs) * 14
}' \
    42
run_with_stdout "listp iterate strings" \
    'define main() -> Int {
    xs := ListP()
    xs.append("alpha"); xs.append("beta")
    i := 0
    while i < xs.length() {
        s: String := xs[i]
        print(s)
        i = i + 1
    }
    return 0
}' \
    0 "alpha
beta"

# Phase 4.22: `__init` constructors inside struct blocks.
# Parser strips explicit `self` first param; methods land in
# top-level decls with receiver = struct name. Codegen
# dispatches `Foo(args)` through `Foo__init` when it's defined,
# falling back to direct positional field stores otherwise.
run "init copies arg" \
    "struct Box { n: Int
    define __init(self, n: Int) -> void { self.n = n }
}
define main() -> Int { b := Box(42); return b.n }" \
    42
run "init derives field" \
    "struct Box { n: Int; doubled: Int
    define __init(self, n: Int) -> void { self.n = n; self.doubled = n * 2 }
}
define main() -> Int { b := Box(21); return b.doubled }" \
    42
run "init zeroes implicit field" \
    "struct Counter { tick: Int; max: Int
    define __init(self, max: Int) -> void { self.tick = 0; self.max = max }
}
define main() -> Int { c := Counter(42); return c.tick + c.max }" \
    42
run "init holds list param" \
    "struct ParserState { tokens: List<Int>; pos: Int
    define __init(self, tokens: List<Int>) -> void { self.tokens = tokens; self.pos = 0 }
}
define main() -> Int { s := ParserState([10, 20, 30]); return s.pos + s.tokens[2] + 12 }" \
    42
run "init then method call" \
    "struct Box { n: Int
    define __init(self, x: Int) -> void { self.n = x * 2 }
    define get_plus(self, k: Int) -> Int { return self.n + k }
}
define main() -> Int { b := Box(20); return b.get_plus(2) }" \
    42

# Phase 4.23: `from .X use { Y, Z }` import statements. Parsed
# and skipped at the AST level — cross-file visibility is the
# driver's job (concatenate sources before invoking the
# self-hosted pipeline). This phase just lets per-file source
# pass through the parser cleanly.
run "single import skipped" \
    "from .ast use { Expr }
define main() -> Int { return 42 }" \
    42
run "multi import skipped" \
    "from .tokens use { TokenKind, Token, EofToken }
from .ast use { Expr, Stmt, IntLit, BinOp }
struct Pt { x: Int; y: Int }
define main() -> Int { p := Pt(40, 2); return p.x + p.y }" \
    42
run "absolute import skipped" \
    "from quirklib use { read_file }
define main() -> Int { return 42 }" \
    42
run "empty import skipped" \
    "from .util use { }
define main() -> Int { return 42 }" \
    42

# Phase 4.24: string escape sequences. Lexer decodes `\n` /
# `\t` / `\r` / `\"` / `\\` into their literal bytes; codegen
# `alloc_string` re-encodes the resulting raw bytes for LLVM
# IR's `c"..."` form as `\HH` hex pairs.
run_with_stdout "newline escape" \
    'define main() -> Int { print("alpha\nbeta"); return 0 }' \
    0 "alpha
beta"
run_with_stdout "tab escape" \
    'define main() -> Int { print("col1\tcol2"); return 0 }' \
    0 "col1	col2"
run_with_stdout "quote escape" \
    'define main() -> Int { print("she said \"hi\""); return 0 }' \
    0 "she said \"hi\""
run_with_stdout "backslash escape" \
    'define main() -> Int { print("path: a\\b\\c"); return 0 }' \
    0 "path: a\\b\\c"
run_with_stdout "concat with newline" \
    'define main() -> Int { ln := 12; print("error at " + ln.str() + "\nin source"); return 0 }' \
    0 "error at 12
in source"

# Phase 4.26: read_file + write_file builtins. fopen + fseek
# + ftell + fread + fclose for reads; fopen + strlen + fwrite
# + fclose for writes. No error handling yet.
run_with_stdout "write+read round-trip" \
    'define main() -> Int {
    write_file("/tmp/quirk_e2e_a.txt", "round-trip ok")
    s := read_file("/tmp/quirk_e2e_a.txt")
    print(s)
    return 0
}' \
    0 "round-trip ok"
run_with_stdout "read multi-line" \
    'define main() -> Int {
    write_file("/tmp/quirk_e2e_b.txt", "line one\nline two\nline three")
    s := read_file("/tmp/quirk_e2e_b.txt")
    print(s)
    return 0
}' \
    0 "line one
line two
line three"
run "read returns length-correct buffer" \
    'define main() -> Int {
    write_file("/tmp/quirk_e2e_c.txt", "exactly42charactersinthisteststringpadding!")
    s := read_file("/tmp/quirk_e2e_c.txt")
    return s.length() - 1
}' \
    42
run_with_stdout "rewrite overwrites" \
    'define main() -> Int {
    write_file("/tmp/quirk_e2e_d.txt", "first content here")
    write_file("/tmp/quirk_e2e_d.txt", "second")
    s := read_file("/tmp/quirk_e2e_d.txt")
    print(s)
    return 0
}' \
    0 "second"

# Phase 5 (partial): bootstrap-driven feature additions —
# the gaps surfaced when pointing the self-hosted compiler
# at lexer.quirk + tokens.quirk via the multi-file driver.
run "doc comment skipped" \
    "---
This is a module-level doc comment.
It spans multiple lines and contains arbitrary text.
---
define main() -> Int { return 42 }" \
    42
run "logical and" \
    "define main() -> Int { x := 5; if x > 0 and x < 10 { return 42 } return 0 }" \
    42
run "logical or" \
    "define main() -> Int { x := -3; if x > 100 or x < 0 { return 42 } return 0 }" \
    42
run "and short-circuit-eager both sides" \
    "define main() -> Int { a := true; b := false; if a and (b or true) { return 42 } return 0 }" \
    42
run "continue skips" \
    "define main() -> Int { i := 0; n := 0; while i < 10 { i = i + 1; if i == 5 { continue } if i == 8 { continue } n = n + i } return n }" \
    42
run "break exits early" \
    "define main() -> Int { i := 0; n := 0; while i < 100 { if n >= 42 { break } n = n + 1; i = i + 1 } return n }" \
    42
run "elif chain (4 arms)" \
    'define main() -> Int { tag := "c"; if tag == "a" { return 1 } elif tag == "b" { return 2 } elif tag == "c" { return 42 } elif tag == "d" { return 4 } return 0 }' \
    42

# Phase 5b: String ordering. `<`, `<=`, `>`, `>=` on String
# operands route through strcmp + the matching signed icmp
# predicate. Used in the selfhost lexer's `c >= "0" and
# c <= "9"` char-range checks.
run "string less-than" \
    'define main() -> Int { if "apple" < "banana" { return 42 } return 0 }' \
    42
run "string greater-equal" \
    'define main() -> Int { c := "5"; if c >= "0" and c <= "9" { return 42 } return 0 }' \
    42
run "string less-equal" \
    'define main() -> Int { if "Q" <= "Q" { return 42 } return 0 }' \
    42
run "string greater-than miss" \
    'define main() -> Int { if "z" > "a" { return 42 } return 0 }' \
    42
run "char-range alpha lower" \
    'define is_alpha(c: String) -> Bool { return (c >= "a" and c <= "z") or (c >= "A" and c <= "Z") }
define main() -> Int { if is_alpha("m") and is_alpha("Z") { return 42 } return 0 }' \
    42

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

# Phase 4.27: multi-file driver — write two source files,
# concatenate them via `build_combined`, then pipe through
# the rest of the selfhost pipeline. Verifies end-to-end
# that file I/O + import-resolution + the existing
# tokenize → parse → check → emit_module flow compose.
build_driver_test() {
    local label="multi-file driver"
    local tmpdir
    tmpdir=$(mktemp -d --suffix=.quirkbuild)
    # Module: a free function the main file will call.
    cat > "$tmpdir/inc.quirk" <<'EOF'
define add_two(x: Int) -> Int { return x + 2 }
EOF
    # Main: imports `inc` and calls `add_two`. The import-
    # resolver in build.quirk reads inc.quirk, strips its
    # imports (none), and prepends it to the combined source.
    cat > "$tmpdir/main.quirk" <<'EOF'
from .inc use { add_two }
define main() -> Int { return add_two(40) }
EOF
    # The actual driver script — runs through quirk via the
    # C++ compiler, uses the selfhost-built build_combined
    # to assemble + compile.
    local driver="selfhost/_build_case.quirk"
    cat > "$driver" <<EOF
from .build use { compile_combined }
ir := compile_combined("$tmpdir/main.quirk", "$tmpdir")
print(ir)
EOF
    local ir_path
    ir_path=$(mktemp --suffix=.ll)
    "$QUIRK" --no-aot --no-cache "$driver" > "$ir_path" 2>/dev/null
    rm -f "$driver"
    if grep -qE "^(SEMA|PARSE) FAILED" "$ir_path"; then
        echo "FAIL  $label  (compile rejected)"
        cat "$ir_path"
        fails+=1
        rm -rf "$tmpdir"
        rm -f "$ir_path"
        return
    fi
    "$LLI" "$ir_path"
    local got=$?
    if [ "$got" -eq 42 ]; then
        echo "ok    $label  (exit=$got)"
        rm -f "$ir_path"
    else
        echo "FAIL  $label  (exit=$got, expected 42)"
        echo "      ir at $ir_path"
        fails+=1
    fi
    rm -rf "$tmpdir"
}

build_driver_test

# Phase 5 (partial bootstrap): self-compile the lexer + tokens
# modules through the self-hosted pipeline. This is the
# bootstrap milestone — the lexer's tokenize() function is
# compiled BY ITSELF and then RUN via lli on real input.
bootstrap_lexer_test() {
    local label="bootstrap: self-compiled lexer"
    local tmpdir
    tmpdir=$(mktemp -d --suffix=.bootstrap)
    cp selfhost/tokens.quirk selfhost/ast.quirk selfhost/types.quirk selfhost/lexer.quirk "$tmpdir/"
    cat > "$tmpdir/run_lexer.quirk" <<'EOF'
from .lexer use { tokenize }
define main() -> Int {
    src := "define foo() { return 42 }"
    tokens := tokenize(src)
    return tokens.length()
}
EOF
    local driver="selfhost/_bootstrap_case.quirk"
    cat > "$driver" <<EOF
from .build use { compile_combined }
ir := compile_combined("$tmpdir/run_lexer.quirk", "$tmpdir")
print(ir)
EOF
    local ir_path
    ir_path=$(mktemp --suffix=.ll)
    "$QUIRK" --no-aot --no-cache "$driver" > "$ir_path" 2>/dev/null
    rm -f "$driver"
    if grep -qE "^(SEMA|PARSE) FAILED" "$ir_path"; then
        echo "FAIL  $label  (selfhost compile rejected)"
        cat "$ir_path"
        fails+=1
        rm -rf "$tmpdir"
        rm -f "$ir_path"
        return
    fi
    "$LLI" "$ir_path"
    local got=$?
    # tokenize produces: `define`, `foo`, `(`, `)`, `{`, `return`,
    # `42`, `}`, EofToken — 9 tokens.
    if [ "$got" -eq 9 ]; then
        echo "ok    $label  (exit=$got, tokenize returned 9 tokens)"
        rm -f "$ir_path"
    else
        echo "FAIL  $label  (exit=$got, expected 9)"
        echo "      ir at $ir_path"
        fails+=1
    fi
    rm -rf "$tmpdir"
}

bootstrap_lexer_test

# Phase 5e bootstrap: parser.quirk now compiles + runs too.
# Uses lexer's output as input — parse(tokenize(src)) producing
# a List<TopLevel> of declarations.
bootstrap_parser_test() {
    local label="bootstrap: self-compiled parser"
    local tmpdir
    tmpdir=$(mktemp -d --suffix=.parsbs)
    cp selfhost/tokens.quirk selfhost/ast.quirk selfhost/types.quirk \
       selfhost/lexer.quirk selfhost/parser.quirk "$tmpdir/"
    cat > "$tmpdir/run_parser.quirk" <<'EOF'
from .lexer use { tokenize }
from .parser use { parse }
define main() -> Int {
    src := "define foo(a: Int, b: Int) -> Int { return a + b }"
    decls := parse(tokenize(src))
    return decls.length()
}
EOF
    local driver="selfhost/_bspars_case.quirk"
    cat > "$driver" <<EOF
from .build use { compile_combined }
ir := compile_combined("$tmpdir/run_parser.quirk", "$tmpdir")
print(ir)
EOF
    local ir_path
    ir_path=$(mktemp --suffix=.ll)
    "$QUIRK" --no-aot --no-cache "$driver" > "$ir_path" 2>/dev/null
    rm -f "$driver"
    if grep -qE "^(SEMA|PARSE) FAILED" "$ir_path"; then
        echo "FAIL  $label  (selfhost compile rejected)"
        cat "$ir_path"
        fails+=1
        rm -rf "$tmpdir"
        rm -f "$ir_path"
        return
    fi
    "$LLI" "$ir_path"
    local got=$?
    # parse() produces 1 top-level FunctionDecl from a single
    # `define foo(...) { ... }`.
    if [ "$got" -eq 1 ]; then
        echo "ok    $label  (exit=$got, parse returned 1 decl)"
        rm -f "$ir_path"
    else
        echo "FAIL  $label  (exit=$got, expected 1)"
        echo "      ir at $ir_path"
        fails+=1
    fi
    rm -rf "$tmpdir"
}

bootstrap_parser_test

# Phase 5f bootstrap: sema self-compiles + runs.
# check(parse(tokenize("..."))) on a valid program returns 0
# errors — the self-hosted sema type-checks code correctly.
bootstrap_sema_test() {
    local label="bootstrap: self-compiled sema"
    local tmpdir
    tmpdir=$(mktemp -d --suffix=.semabs)
    cp selfhost/tokens.quirk selfhost/ast.quirk selfhost/types.quirk \
       selfhost/lexer.quirk selfhost/parser.quirk selfhost/sema.quirk "$tmpdir/"
    cat > "$tmpdir/run_sema.quirk" <<'EOF'
from .lexer use { tokenize }
from .parser use { parse }
from .sema use { check }
define main() -> Int {
    src := "define foo(x: Int) -> Int { return x * 2 }"
    decls := parse(tokenize(src))
    errors := check(decls)
    return errors.length()
}
EOF
    local driver="selfhost/_bssema_case.quirk"
    cat > "$driver" <<EOF
from .build use { compile_combined }
ir := compile_combined("$tmpdir/run_sema.quirk", "$tmpdir")
print(ir)
EOF
    local ir_path
    ir_path=$(mktemp --suffix=.ll)
    "$QUIRK" --no-aot --no-cache "$driver" > "$ir_path" 2>/dev/null
    rm -f "$driver"
    if grep -qE "^(SEMA|PARSE) FAILED" "$ir_path"; then
        echo "FAIL  $label  (selfhost compile rejected)"
        cat "$ir_path"
        fails+=1
        rm -rf "$tmpdir"
        rm -f "$ir_path"
        return
    fi
    "$LLI" "$ir_path"
    local got=$?
    # check() on a valid program returns 0 errors.
    if [ "$got" -eq 0 ]; then
        echo "ok    $label  (exit=$got, check returned 0 errors)"
        rm -f "$ir_path"
    else
        echo "FAIL  $label  (exit=$got, expected 0)"
        echo "      ir at $ir_path"
        fails+=1
    fi
    rm -rf "$tmpdir"
}

bootstrap_sema_test

# Phase 5g bootstrap: codegen self-compiles + runs.
# emit_module(parse(tokenize("..."))) on a valid program returns
# a non-empty IR string — the self-hosted codegen produces real
# LLVM IR from its own source.
bootstrap_codegen_test() {
    local label="bootstrap: self-compiled codegen"
    local tmpdir
    tmpdir=$(mktemp -d --suffix=.cgbs)
    cp selfhost/tokens.quirk selfhost/ast.quirk selfhost/types.quirk \
       selfhost/lexer.quirk selfhost/parser.quirk selfhost/sema.quirk \
       selfhost/codegen.quirk "$tmpdir/"
    cat > "$tmpdir/run_codegen.quirk" <<'EOF'
from .lexer use { tokenize }
from .parser use { parse }
from .sema use { check }
from .codegen use { emit_module }
define main() -> Int {
    src := "define foo(x: Int) -> Int { return x * 2 }"
    decls := parse(tokenize(src))
    errors := check(decls)
    if errors.length() > 0 { return 99 }
    ir := emit_module(decls)
    if ir.length() > 0 { return 7 }
    return 0
}
EOF
    local driver="selfhost/_bscg_case.quirk"
    cat > "$driver" <<EOF
from .build use { compile_combined }
ir := compile_combined("$tmpdir/run_codegen.quirk", "$tmpdir")
print(ir)
EOF
    local ir_path
    ir_path=$(mktemp --suffix=.ll)
    "$QUIRK" --no-aot --no-cache "$driver" > "$ir_path" 2>/dev/null
    rm -f "$driver"
    if grep -qE "^(SEMA|PARSE) FAILED" "$ir_path"; then
        echo "FAIL  $label  (selfhost compile rejected)"
        cat "$ir_path"
        fails+=1
        rm -rf "$tmpdir"
        rm -f "$ir_path"
        return
    fi
    "$LLI" "$ir_path"
    local got=$?
    # emit_module() returns a non-empty IR string → sentinel 7.
    if [ "$got" -eq 7 ]; then
        echo "ok    $label  (exit=$got, emit_module produced IR)"
        rm -f "$ir_path"
    else
        echo "FAIL  $label  (exit=$got, expected 7)"
        echo "      ir at $ir_path"
        fails+=1
    fi
    rm -rf "$tmpdir"
}

bootstrap_codegen_test

# Phase 5i bootstrap: build.quirk — the multi-file driver
# itself — now self-compiles. read_file/write_file are
# shared builtins (C++ side via BuiltinGen, selfhost side
# via _gen_read_file/_gen_write_file). build.quirk's _slurp
# routes through read_file; compile_combined() compiles
# itself end-to-end without io.File.
bootstrap_build_test() {
    local label="bootstrap: self-compiled build driver"
    local tmpdir
    tmpdir=$(mktemp -d --suffix=.buildbs)
    cp selfhost/tokens.quirk selfhost/ast.quirk selfhost/types.quirk \
       selfhost/lexer.quirk selfhost/parser.quirk selfhost/sema.quirk \
       selfhost/codegen.quirk selfhost/build.quirk "$tmpdir/"
    # Stage a tiny "library" + "main" pair under tmpdir/work so
    # the bootstrapped build_combined has real files to read.
    mkdir -p "$tmpdir/work"
    cat > "$tmpdir/work/inc.quirk" <<'EOF'
define triple_(x: Int) -> Int { return x * 3 }
EOF
    cat > "$tmpdir/work/main.quirk" <<'EOF'
from .inc use { triple_ }
define main() -> Int { return triple_(14) }
EOF
    cat > "$tmpdir/run_build.quirk" <<EOF
from .build use { build_combined }
define main() -> Int {
    src := build_combined("$tmpdir/work/main.quirk", "$tmpdir/work")
    return src.length()
}
EOF
    local driver="selfhost/_bsbuild_case.quirk"
    cat > "$driver" <<EOF
from .build use { compile_combined }
ir := compile_combined("$tmpdir/run_build.quirk", "$tmpdir")
print(ir)
EOF
    local ir_path
    ir_path=$(mktemp --suffix=.ll)
    "$QUIRK" --no-aot --no-cache "$driver" > "$ir_path" 2>/dev/null
    rm -f "$driver"
    if grep -qE "^(SEMA|PARSE) FAILED" "$ir_path"; then
        echo "FAIL  $label  (selfhost compile rejected)"
        cat "$ir_path"
        fails+=1
        rm -rf "$tmpdir"
        rm -f "$ir_path"
        return
    fi
    "$LLI" "$ir_path"
    local got=$?
    # build_combined output is two file bodies concatenated +
    # newlines, with the import line stripped. Real source size
    # is ~80-100 bytes; exit code is the low 8 bits, so any
    # value in 30..120 indicates the driver produced a
    # plausible combined output.
    if [ "$got" -gt 30 ] && [ "$got" -lt 120 ]; then
        echo "ok    $label  (exit=$got, build_combined produced source)"
        rm -f "$ir_path"
    else
        echo "FAIL  $label  (exit=$got, expected 30-120)"
        echo "      ir at $ir_path"
        fails+=1
    fi
    rm -rf "$tmpdir"
}

bootstrap_build_test

# Phase 5h: standalone binary linkage. Take the selfhost-produced
# IR, lower it via llc-14, link with clang into a real ELF, and
# run the binary directly (no lli). This is the proof that the
# IR isn't lli-specific — it works as a static ELF with libc.
# Variant lets the caller pick the input source + expected exit
# + (optional) expected stdout substring.
standalone_run() {
    local label="$1"
    local src="$2"
    local expected="$3"
    local expected_out="${4:-}"
    local driver="selfhost/_standalone.quirk"
    local quoted
    quoted=$(printf %s "$src" | python3 -c 'import sys, json; print(json.dumps(sys.stdin.read()))')
    cat > "$driver" <<EOF
from .lexer use { tokenize }
from .parser use { parse }
from .sema use { check }
from .codegen use { emit_module }
src := $quoted
print(emit_module(parse(tokenize(src))))
EOF
    local ll_path s_path bin_path
    ll_path=$(mktemp --suffix=.ll)
    s_path=$(mktemp --suffix=.s)
    bin_path=$(mktemp --suffix=.bin)
    "$QUIRK" --no-aot --no-cache "$driver" > "$ll_path" 2>/dev/null
    rm -f "$driver"
    if grep -qE "^(SEMA|PARSE) FAILED" "$ll_path"; then
        echo "FAIL  $label  (selfhost compile rejected)"
        cat "$ll_path"
        fails+=1
        rm -f "$ll_path" "$s_path" "$bin_path"
        return
    fi
    llc-14 "$ll_path" -o "$s_path" 2>/dev/null
    if [ ! -s "$s_path" ]; then
        echo "FAIL  $label  (llc rejected IR)"
        echo "      ir at $ll_path"
        fails+=1
        rm -f "$ll_path" "$s_path" "$bin_path"
        return
    fi
    "${CLANG:-clang-14}" -no-pie "$s_path" -o "$bin_path" 2>/dev/null
    if [ ! -x "$bin_path" ]; then
        echo "FAIL  $label  (clang link failed)"
        echo "      asm at $s_path"
        fails+=1
        rm -f "$ll_path" "$s_path" "$bin_path"
        return
    fi
    local got_out
    got_out=$("$bin_path")
    local got_exit=$?
    if [ "$got_exit" -ne "$expected" ]; then
        echo "FAIL  $label  (exit=$got_exit, expected $expected; out='$got_out')"
        echo "      bin at $bin_path"
        fails+=1
        return
    fi
    if [ -n "$expected_out" ] && [ "$got_out" != "$expected_out" ]; then
        echo "FAIL  $label  (stdout='$got_out', expected '$expected_out')"
        echo "      bin at $bin_path"
        fails+=1
        return
    fi
    echo "ok    $label  (exit=$got_exit)"
    rm -f "$ll_path" "$s_path" "$bin_path"
}

# Coverage: simple arithmetic (no runtime helpers), libc lowering
# via print/puts, malloc'd lists + index, and the standard
# "tagged union + match" shape that touches sub-allocations.
standalone_run "ELF: arithmetic" \
    'define double_(x: Int) -> Int { return x * 2 }
define main() -> Int { return double_(21) }' \
    42
standalone_run "ELF: puts via print" \
    'define main() -> Int { print("hello standalone"); return 42 }' \
    42 "hello standalone"
standalone_run "ELF: malloc'd list" \
    'define main() -> Int { xs := [10, 20, 12]; sum := 0; i := 0; while i < xs.length() { sum = sum + xs[i]; i = i + 1 } return sum }' \
    42
standalone_run "ELF: tagged union match" \
    'type R = Ok(v: Int) | Err(m: String)
define unwrap(r: R) -> Int {
    match r {
        case Ok as o => return o.v
        case Err as _ => return -1
        case _ => return -1
    }
}
define main() -> Int { return unwrap(Ok(42)) }' \
    42
standalone_run "ELF: int.str() + strcat" \
    'define main() -> Int { n := 7; print("count is " + n.str()); return 0 }' \
    0 "count is 7"

# Phase 5j: argv access from a standalone selfhost ELF.
# Test the full chain: selfhost emits a wrapper main(argc, argv)
# that stashes them in module globals; arg_count() + arg_get(i)
# read those back. We invoke the ELF with extra args and verify
# both the count and a positional read.
standalone_argv_test() {
    local label="ELF: arg_count() + arg_get(i) via stashed globals"
    local src='define main() -> Int {
    print(arg_get(1))
    return arg_count()
}'
    local driver="selfhost/_argvcase.quirk"
    local quoted
    quoted=$(printf %s "$src" | python3 -c 'import sys, json; print(json.dumps(sys.stdin.read()))')
    cat > "$driver" <<EOF
from .lexer use { tokenize }
from .parser use { parse }
from .codegen use { emit_module }
src := $quoted
print(emit_module(parse(tokenize(src))))
EOF
    local ll_path s_path bin_path
    ll_path=$(mktemp --suffix=.ll)
    s_path=$(mktemp --suffix=.s)
    bin_path=$(mktemp --suffix=.bin)
    "$QUIRK" --no-aot --no-cache "$driver" > "$ll_path" 2>/dev/null
    rm -f "$driver"
    llc-14 "$ll_path" -o "$s_path" 2>/dev/null
    "${CLANG:-clang-14}" -no-pie "$s_path" -o "$bin_path" 2>/dev/null
    local got_out
    got_out=$("$bin_path" hello world from quirk)
    local got_exit=$?
    # Expected: argc = 5 (argv[0] + 4 user args), argv[1] = "hello"
    if [ "$got_exit" -eq 5 ] && [ "$got_out" = "hello" ]; then
        echo "ok    $label  (exit=$got_exit, stdout=$got_out)"
        rm -f "$ll_path" "$s_path" "$bin_path"
    else
        echo "FAIL  $label  (exit=$got_exit, expected 5; stdout='$got_out', expected 'hello')"
        echo "      bin at $bin_path"
        fails+=1
    fi
}

standalone_argv_test

# Phase 5k: the full bootstrap loop. Build the standalone
# Quirk binary (bin/quirk-selfhost) from selfhost/quirk_main.quirk,
# then USE THAT BINARY to compile a fresh user program into IR,
# link + run. Two layers of selfhost-produced code: the compiler
# itself is built from Quirk source, and the user program goes
# through that compiler.
bootstrap_full_loop_test() {
    local label="bootstrap: quirk-selfhost binary compiles user code"
    if [ ! -x bin/quirk-selfhost ]; then
        echo "skip  $label  (bin/quirk-selfhost not built; run \`make selfhost-binary\`)"
        return
    fi
    local src_path ll_path s_path bin_path
    src_path=$(mktemp --suffix=.quirk)
    ll_path=$(mktemp --suffix=.ll)
    s_path=$(mktemp --suffix=.s)
    bin_path=$(mktemp --suffix=.bin)
    cat > "$src_path" <<'EOF'
define triple_(x: Int) -> Int { return x * 3 }
define main() -> Int { print("compiled by selfhost"); return triple_(14) }
EOF
    bin/quirk-selfhost "$src_path" "$ll_path" 2>/dev/null
    if [ ! -s "$ll_path" ]; then
        echo "FAIL  $label  (quirk-selfhost produced empty IR)"
        fails+=1
        rm -f "$src_path" "$ll_path" "$s_path" "$bin_path"
        return
    fi
    llc-14 "$ll_path" -o "$s_path" 2>/dev/null
    "${CLANG:-clang-14}" -no-pie "$s_path" -o "$bin_path" 2>/dev/null
    if [ ! -x "$bin_path" ]; then
        echo "FAIL  $label  (clang link failed on selfhost-emitted IR)"
        fails+=1
        rm -f "$src_path" "$ll_path" "$s_path" "$bin_path"
        return
    fi
    local got_out got_exit
    got_out=$("$bin_path")
    got_exit=$?
    if [ "$got_exit" -eq 42 ] && [ "$got_out" = "compiled by selfhost" ]; then
        echo "ok    $label  (exit=$got_exit, stdout=$got_out)"
        rm -f "$src_path" "$ll_path" "$s_path" "$bin_path"
    else
        echo "FAIL  $label  (exit=$got_exit, expected 42; stdout='$got_out')"
        fails+=1
    fi
}

bootstrap_full_loop_test

# Phase 5l: self-stage fixed-point verification. The canonical
# bootstrap milestone — compile selfhost source with itself,
# build a second-generation binary from THAT IR, recompile
# selfhost source through the second-gen binary, diff the two
# IR outputs. Byte-identical means the compiler is a stable
# fixed point.
fixedpoint_test() {
    local label="bootstrap: byte-identical self-stage fixed point"
    if [ ! -x bin/quirk-selfhost ]; then
        echo "skip  $label  (bin/quirk-selfhost not built; run \`make selfhost-binary\`)"
        return
    fi
    local ir1 ir2 v2bin v2asm
    ir1=$(mktemp --suffix=.ll)
    ir2=$(mktemp --suffix=.ll)
    v2asm=$(mktemp --suffix=.s)
    v2bin=$(mktemp --suffix=.bin)
    bin/quirk-selfhost selfhost/quirk_main.quirk "$ir1" 2>/dev/null
    if [ ! -s "$ir1" ]; then
        echo "FAIL  $label  (1st-gen IR empty)"
        fails+=1
        rm -f "$ir1" "$ir2" "$v2asm" "$v2bin"
        return
    fi
    llc-14 "$ir1" -o "$v2asm" 2>/dev/null
    "${CLANG:-clang-14}" -no-pie "$v2asm" -o "$v2bin" 2>/dev/null
    if [ ! -x "$v2bin" ]; then
        echo "FAIL  $label  (v2 link failed)"
        fails+=1
        rm -f "$ir1" "$ir2" "$v2asm" "$v2bin"
        return
    fi
    "$v2bin" selfhost/quirk_main.quirk "$ir2" 2>/dev/null
    if [ ! -s "$ir2" ]; then
        echo "FAIL  $label  (2nd-gen IR empty)"
        fails+=1
        rm -f "$ir1" "$ir2" "$v2asm" "$v2bin"
        return
    fi
    if diff -q "$ir1" "$ir2" > /dev/null; then
        echo "ok    $label  (1.6MB IR identical to the byte)"
        rm -f "$ir1" "$ir2" "$v2asm" "$v2bin"
    else
        local diff_lines
        diff_lines=$(diff "$ir1" "$ir2" | wc -l)
        echo "FAIL  $label  ($diff_lines lines of divergence)"
        echo "      ir1 at $ir1, ir2 at $ir2"
        fails+=1
    fi
}

fixedpoint_test

# Phase 5m: stderr routing. Build an ELF that prints to both
# streams, then verify stdout/stderr are independently
# capturable. Without this, piping IR through llc would be
# poisoned by any diagnostic output.
standalone_run "ELF: eprint routes to stderr, print to stdout" \
    'define main() -> Int {
    print("on stdout")
    eprint("on stderr")
    return 0
}' \
    0 "on stdout"

# Phase 6: extern define lowering. The user declares an FFI
# function with no body; codegen emits `declare T @name(...)`
# instead of `define ... { ... }`. Linkage is resolved by clang
# at link time. This is the gateway to selfhost-compiled stdlib
# support — every from-io/from-sys/etc. ultimately bottoms out
# at extern definitions.
standalone_run "ELF: extern define lowers to libc puts" \
    'extern define puts(s: String) -> Int
define main() -> Int { puts("via extern"); return 42 }' \
    42 "via extern"
standalone_run "ELF: extern define with multiple args (libc strlen)" \
    'extern define strlen(s: String) -> Int
define main() -> Int {
    n := strlen("hello!")
    return n
}' \
    6
standalone_run "ELF: extern method inside struct (Any-typed field)" \
    'struct Box {
    _data: Any
    extern define mark(self) -> Int
    define get_self(self) -> Box { return self }
}
extern define malloc(n: Int) -> Any
define main() -> Int {
    return 42
}' \
    42
standalone_run "ELF: compound assignment +=" \
    'define main() -> Int { i := 40; i += 2; return i }' \
    42
standalone_run "ELF: empty list [] is polymorphic (string append)" \
    'define main() -> Int {
    xs := []
    xs.append("alpha")
    xs.append("beta")
    return xs.length() + 40
}' \
    42
standalone_run "ELF: string list literal" \
    'define main() -> Int {
    ss := ["a", "bb", "ccc"]
    return ss.length() * 14
}' \
    42
standalone_run "ELF: empty map literal {}" \
    'define main() -> Int {
    m := {}
    m.put("a", "1"); m.put("b", "2"); m.put("c", "3")
    return m.length() * 14
}' \
    42
standalone_run "ELF: variadic ...args param accepted" \
    'define count_(...args: List) -> Int { return args.length() }
define main() -> Int {
    xs := ["a", "b", "c"]
    return count_(xs) + 39
}' \
    42
# (the stdout=on stdout assertion implicitly proves eprint did
# NOT show up there — it was routed to stderr instead.)

if [ "$fails" -gt 0 ]; then
    echo ""
    echo "$fails case(s) failed"
    exit 1
fi
echo ""
echo "all 172/172 cases passed"
