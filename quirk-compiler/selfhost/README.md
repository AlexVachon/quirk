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
|  4.14 | User-defined struct methods — `define Foo.method(...)` with implicit `self` | landed (v4.0.0-alpha.18) |
|  4.15 | `List.append()` + capacity field + realloc growth | landed (v4.0.0-alpha.19) |
|  4.16 | Primitive `.str()` — Int / Bool / Double → String | landed (v4.0.0-alpha.20) |
|  4.17 | String methods (`.substring`, `.startswith`, `.endswith`, `.to_int`) + String `==` / `!=` | landed (v4.0.0-alpha.21) |
|  4.18 | Enums — `enum Name { A; B; C }` + `Name.Variant` (ordinal lowering) | landed (v4.0.0-alpha.22) |
|  4.19 | Tagged unions + `match` — `type T = A(...) \| B(...)` with payload binding | landed (v4.0.0-alpha.23) |
|  4.20 | Map (`.put`/`.get`/`.has`/`.length`) + `List()` ctor + VarDecl annotation bitcast | landed (v4.0.0-alpha.24) |
|  4.21 | Generic `List<T>` element types — `%QListP` for pointer lists + `ListP()` ctor | landed (v4.0.0-alpha.25) |
|  4.22 | Inside-struct methods + `__init` ctor dispatch | landed (v4.0.0-alpha.26) |
|  4.23 | `from .X use { … }` import statement parsing (concatenate-and-compile model) | landed (v4.0.0-alpha.27) |
|  4.24 | String escape sequences (`\n` / `\t` / `\"` / `\\` / `\r` / `\0`) | landed (v4.0.0-alpha.28) |
|  4.25 | parser.quirk rewritten to error-accumulation (drops `throw`) | landed (v4.0.0-alpha.29) |
|  4.26 | `read_file` + `write_file` builtins via libc fopen/fread/fwrite/fclose | landed (v4.0.0-alpha.30) |
|  4.27 | Multi-file driver — `build.quirk` resolves imports + assembles combined source | landed (v4.0.0-alpha.31) |
|  5a   | Bootstrap pass 1 — doc comments, `and`/`or`, `continue`/`break`, multi-elif | landed (v4.0.0-alpha.32) |
|  5b   | Bootstrap pass 2 — String ordering (`<`, `<=`, `>`, `>=`) via strcmp | landed (v4.0.0-alpha.33) |
|  5c   | Bootstrap pass 3 — flip bare `List` default to pointer-list | landed (v4.0.0-alpha.34) |
|  5d   | **🎉 Lexer bootstrap milestone** — selfhost compiler compiles + runs its own lexer | landed (v4.0.0-alpha.35) |
|  5e   | **🎉 Parser bootstrap** — Bool ==, .__get(), Return/Assign coerce, two-pass type reg | landed (v4.0.0-alpha.36) |
|  5    | Bootstrap — Quirk compiler compiles itself, byte-identical | in progress |

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
