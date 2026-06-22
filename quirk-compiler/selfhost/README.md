# Self-Hosting (Quirk 4.0)

The compiler — currently a C++17 program built against LLVM 14 —
re-implemented in Quirk itself. This directory is where that work
lives in-progress; nothing here is yet built or invoked by the
default `make` target.

## Why

Self-hosting is the single biggest credibility move a language
can make. "Quirk written in Quirk" demonstrates the language is
expressive enough to build itself, surfaces friction points
neither the test suite nor the stdlib hit, and removes the C++
toolchain dependency from anyone wanting to hack on the compiler.

## Roadmap

| Phase | Component | Status |
| ----- | --------- | ------ |
|  1    | Lexer in Quirk — tokenize `.quirk` source | landed (v4.0.0-alpha.1) |
|  2    | Parser in Quirk — AST construction        | landed (v4.0.0-alpha.2) |
|  3    | Sema in Quirk — type-check + transforms   | landed (v4.0.0-alpha.3) |
|  4    | Codegen — emit LLVM IR as text + run via `lli` | landed (v4.0.0-alpha.4) |
|  4.1  | Codegen — comparison ops + `if`/`else`/`while` + alloca locals | landed (v4.0.0-alpha.5) |
|  4.2  | Unary `-` / `not` + block-termination tracker | landed (v4.0.0-alpha.6) |
|  4.3  | String literals + `print()` lowered to `puts` | landed (v4.0.0-alpha.7) |
|  4.4  | Bool as a first-class binding type (`i1` slots) | landed (v4.0.0-alpha.8) |
|  4.5  | Bool at the call boundary (params + returns + forward refs) | landed (v4.0.0-alpha.9) |
|  4.6  | Double scalar — float literals, arithmetic, cmp, locals, call boundary | landed (v4.0.0-alpha.10) |
|  4.7  | String at the call boundary — locals, params, returns, generic print | landed (v4.0.0-alpha.11) |
|  4.8  | String concat via `+` — malloc + strcpy + strcat lowering | landed (v4.0.0-alpha.12) |
|  4.9  | Structs — decls + positional ctor + field read | landed (v4.0.0-alpha.13) |
|  4.10 | Field write + struct-typed params / returns (by ref) | landed (v4.0.0-alpha.14) |
|  4.11 | List literals + subscript read (Int elems, no length) | landed (v4.0.0-alpha.15) |
|  4.12 | List header layout `{ length, data }` + `len()` builtin | landed (v4.0.0-alpha.16) |
|  4.13 | Method-call syntax — `xs.length()`, `s.length()` | landed (v4.0.0-alpha.17) |
|  5    | Bootstrap — Quirk compiler compiles itself, byte-identical | not started |

The Codegen phase deliberately targets *text-form* LLVM IR (`.ll`)
rather than FFI'ing to LLVM's C API. Quirk doesn't have C-binding
machinery for the LLVM types, and writing the bindings would be
larger than the Codegen pass itself. Emitting text and shelling
out to `llc` is the same shape the standard Rust / Swift compilers
took in their early bootstraps.

## Phase 1 scope

This is the file you're reading because the lexer just landed.
[`tokens.quirk`](tokens.quirk) defines the token taxonomy as a
Quirk enum + struct. [`lexer.quirk`](lexer.quirk) walks a source
string and produces `List<Token>`. [`lexer_test.quirk`](lexer_test.quirk)
runs the lexer against a small corpus and checks the output
against expected token sequences.

The lexer currently handles the subset needed to tokenise itself:
identifiers, keywords, punctuation, integer + float + string
literals, single-line `//` and block `/* */` comments, basic
operators. Triple-quoted strings, raw strings, escape sequences,
and string interpolation are deferred to a Phase 1.1 expansion
once Phases 2-3 confirm the shape is right.
