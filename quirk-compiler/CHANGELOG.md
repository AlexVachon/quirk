# Changelog

All notable changes to Quirk land here. The format is loosely
[Keep a Changelog](https://keepachangelog.com/) and the project follows
SemVer — minor bumps for new features, patches for fixes, major bumps
only for breaking changes.

## [3.19.0] — 2026-06-20

### Set operator overloads + member-access dispatch fix + set-runtime fixes

Three pieces shipped together because they all surfaced (or were
needed) while wiring up `s1 + s2` / `s1 - s2`:

**Set operator overloads (the user-facing feature).** Quirk-typing
v1.8.0 adds Quirk-side wrappers around the existing
union/difference methods:

  ```
  define __add(self, other: Set) -> Set { return self.union(other) }
  define __sub(self, other: Set) -> Set { return self.difference(other) }
  ```

Sema gains `Set + Set → Set` and `Set - Set → Set` typing rules.
The Codegen operator-overload dispatch picks up `__add`/`__sub`
through the per-struct method registry from v3.15.0.

**Member-access dispatch fix (a real latent bug).** The
member-access codegen had a "triple-underscore fallback":

  ```
  // tried as fallback
  TheModule->getFunction(typeName + "___" + memberName)
  ```

intended to find dunder methods like `__get`/`__init`. With
v1.8.0 adding `__add` to Set, calling the existing
`s1.add(1)` started routing to `Set___add` (which expects two
Sets — Int-as-Set crash). The fallback now only fires when
`memberName` already starts with `_`, so dunder lookups still
resolve and bare-name calls land on the right method.

**Set runtime fixes (pre-existing, surfaced now).** Two latent
issues in `src/Runtime/core/set.c`:

  - `Set_union`/`intersection`/`difference` allocated their
    result via `malloc(sizeof(Set))` instead of `GC_malloc`. The
    GC didn't track the result; later collections sometimes
    freed it out from under live code → SIGSEGV.

  - `Set_intersection` iterated `self->entries[i]` (raw hash
    slots, mostly empty) instead of `self->key_order[i]`
    (insertion-order dense view). `intersection({1,2}, {2,3})`
    returned size 0 instead of 1. Difference already did the
    iteration correctly.

`tests/probes/p78_set_ops.quirk` exercises `+`, `-`, the
original named methods (union/intersection/difference now all
correct), and the operands-untouched invariant.

## [3.18.0] — 2026-06-20

### `xs * n` List repetition

Natural follow-up to v3.17.0's `String * Int`. Same operator-
overload dispatch path (no Codegen changes — the v3.17.0 gate-
loosen and commutative swap both generalize cleanly to List).
Three pieces:

  - Runtime: `Core_Collections_List_List___mul(self, n)` returns
    a fresh List with self repeated n times. n<=0 returns the
    empty list. Element values are SHARED between repetitions
    (same shape as Python's list-repeat) — reassigning one row
    via index is fine, mutating a shared inner object would
    propagate.

  - Library: extern declaration in quirk-typing v1.7.0.

  - Sema: `List * Int → List` and `Int * List → List` typing in
    both the equality-shortcut block and the arithmetic branch.

Use cases:

  - Zero-init arrays: `zs := [0] * n`
  - Tile patterns: `print(["─"] * width)`
  - Test fixtures: `rows := [default_row] * 10` (sharing is
    fine when the default isn't mutated)

`tests/probes/p77_list_repeat.quirk` covers both directions,
zero-count, empty-base, and the commutative form.

## [3.17.0] — 2026-06-20

### `s * n` String repetition

`"-" * 40` for separators, `"  " * depth` for indentation,
`"!" * count` for emphasis — all common Python patterns that
rejected at Sema with `operator '*' incompatible types`. probe
p46 explicitly locked out `String * String` as a foot-gun
(prevents an IR-verifier crash from `mul %String*, %String*`),
but `String * Int` is a different operation that should work.

Four pieces:

  - Runtime: `Core_String_String___mul(self, n)` returns a
    fresh String of self repeated n times. n<=0 returns "".

  - Library: extern declaration on the String struct
    (quirk-typing v1.6.0).

  - Sema: `String * Int → String` and `Int * String → String`
    typing rules in both the equality-shortcut block (which
    runs before the arithmetic gate) and the arithmetic
    branch.

  - Codegen: two changes. First, the per-struct dispatch gate
    from v3.15.0 loosened — "either both same struct OR the
    method's RHS param type matches R as-is." This lets
    `__mul(self: String, n: Int)` dispatch when R is i32
    without misfiring for unrelated mixed-type ops (the type
    check before dispatch confirms compatibility). Second, a
    commutative L↔R swap when the user writes `3 * "ab"`
    (Int on the left, String on the right) — without the swap
    the dispatch looks at L (Int) and never reaches the magic
    method on the String receiver. Applies to `+`, `*`, `==`,
    `!=` (the symmetric operators).

`tests/probes/p76_string_repeat.quirk` covers both directions,
zero-count, length round-trip, and the empty-product edge
case. p46 (the explicit `String * String` rejection) still
holds — different operation entirely.

## [3.16.0] — 2026-06-20

### `m1 + m2` Map merge + Map.put_all

Natural follow-up to v3.15.0's List concat. Same operator-overload
dispatch path (no Codegen changes — the per-struct method registry
from v3.15.0 handles Map the same way it handles List). Three
pieces:

  - Runtime: `Core_Collections_Map_Map___add` (fresh result,
    right side wins on key collision — matches Python's
    `{**m1, **m2}` precedence) and
    `Core_Collections_Map_Map_put_all` (in-place merge). Pinned
    in quirk-typing v1.5.0; STDLIB_TAG_typing bumps accordingly.

  - Library: extern declarations on the Map struct in
    quirk-typing's `packages/typing/collections/map.quirk`.

  - Sema: `+` on two `Map` operands now types as `Map` (parallel
    to the `List + List → List` rule from v3.15.0).

Use cases this targets:

  - HTTP header merging: `merged := defaults + caller_headers`
  - Config layering: `final := base + env_overrides + cli_flags`
  - Builder patterns where each step contributes a few entries

`tests/probes/p75_map_merge.quirk` exercises the merge operator,
right-wins collision semantics, originals-unchanged invariant,
put_all mutation, and empty-operand edge cases.

## [3.15.0] — 2026-06-20

### `xs + ys` List concatenation + List.append_all + struct method dispatch fix

Before this release, `xs + ys` on two Lists SIGSEGV'd at runtime —
Sema typed the operator as Int via the numeric compatible-operands
fallback, and Codegen emitted raw integer add against actual List
pointers (dereferencing pointers as integers). User-side workaround
was a manual `for i in 0..ys.length() { xs.append(ys.__get(i)) }`
loop.

Three changes land:

  - Runtime gains `Core_Collections_List_List___add` (fresh
    result) and `Core_Collections_List_List_append_all` (mutate
    in place). Pinned in quirk-typing v1.4.0; STDLIB_TAG_typing
    bumps accordingly.

  - Sema: `+` on two `List` operands now types as `List` instead
    of falling through to Int. Other operand combinations
    (String+anything, numeric+numeric, generic-param erasure)
    take their existing paths unchanged.

  - Codegen gains a per-struct method registry
    (`structMethodNodes: structName → method → FunctionNode`)
    populated during Pass 3. The binary-op overload dispatch
    queries the registry first to recover the full linkage name
    (`Core_Collections_List_List___add`) from the bare `List`
    operand-struct type — without the registry, the dispatch
    was falling back to `getFunction("List___add")` (struct +
    dunder, no module prefix), which only matched user structs
    defined at module root.

The dispatch is gated to "both operands have the SAME struct
type" so mixed-type binary ops like `String + Double` keep
flowing through the legacy coercion paths instead of misfiring
into a magic method that expected matching argument shapes.

The mutating form is named `append_all`, not `extend`, because
`extend` is a reserved keyword in Quirk for `extend Foo { ... }`
declarations that bolt methods onto an existing struct. The name
is descriptive enough — `xs.append_all(ys)` reads cleanly.

`tests/probes/p74_list_concat.quirk` exercises both API shapes
plus empty-operand edge cases.

## [3.14.0] — 2026-06-20

### Type aliases work in function parameter + return types

v3.12.0 dereferenced type aliases at variable-binding time in
`checkVarDecl`, but function signatures were a separate code path.
Declarations like

```
type Headers = Map
define add_header(h: Headers, k: String, v: String) -> Headers { ... }
```

bound `h` inside the function body under the raw alias name
"Headers", downstream method dispatch fell back to Any, and the
return-type check at the `return h` statement compared "Headers"
against the inferred concrete type — frequently mismatching.

`checkFunction` now mirrors the substitution onto FunctionNode
parameters and return type at entry:

```cpp
for (auto &param : f->parameters) {
    auto aIt = typeAliases.find(baseType(param.type));
    if (aIt != typeAliases.end() && aIt->second != "Any")
        param.type = aIt->second;
}
// same for f->returnType
```

The rewrite mutates the AST in place so Codegen — which reads
`f->parameters[i].type` and `f->returnType` verbatim for LLVM
signature emission — sees the canonical type. Methods registered
in `methodRegistry` point at the same FunctionNode, so the
overload-resolution and arity-checking paths pick up the rewrite
automatically.

Limitation: function bodies that combine aliased params with
operators Sema doesn't model (e.g. `List + List` for concat —
Sema types `+` on two same-typed non-numeric operands as Int,
which fights a declared `-> List` return) still fail. That's a
separate Sema gap that pre-dates aliases.

`tests/probes/p73_alias_in_signatures.quirk` exercises `Headers`
(Map alias) and `Score` (Int alias) flowing through param +
return positions with realistic call/return shapes.

## [3.13.0] — 2026-06-20

### Function-body locals shadow module-level globals

Closes the pre-existing edge case I flagged as a known limitation in
v3.12.0's `type Name = String` work. Before this release a top-level
`n := "alex"` materialised as an LLVM GlobalVariable typed `String*`,
and then ANY stdlib method whose body did `n := self.length()` (e.g.
`List.sort` with its inner sort loop) routed the local declaration
through `updateLocalVariable` — writing an Int through the
`String*`-typed global slot. Downstream `i < n` ICmp asserted in
the LLVM verifier:

```
Both operands to ICmp instruction are not of the same type!
```

The bug only fired for short identifiers (`i`, `j`, `n`) because
those happen to appear as locals across the stdlib; longer names
like `config_name` dodged it by accident.

Root cause: the var-decl codegen path used `varGen->exists(name)`
to decide between "create a fresh local" and "update an existing
binding", but `exists()` returns true for either a local OR a
global. So inside a function body, `name := value` where `name`
existed as a global slipped into the "update existing" branch and
overwrote the global instead of allocating a fresh stack slot.

The fix adds `VariableGen::hasLocal(name)` (NamedValues-only, no
globalVars consultation) and uses it in the var-decl path: when
`!inModuleScope && vdecl->op == ":="` AND the name isn't already a
local in this function, allocate a fresh local that shadows the
global. The existing fallback branches handle the "reuse local"
and "assign-not-declare" cases unchanged.

Side benefit: `type Name = String` aliases now work end-to-end —
v3.12.0 routed them through the same broken path and produced
this exact ICE. The probe re-tests Map/List/Int aliases (still
working) plus the previously-broken String case.

`tests/probes/p72_local_shadows_global.quirk` exercises the
canonical repro: top-level `n := "alex"`, then `xs.sort(...)`
which forces `List.sort`'s body to materialise its own `n :=
self.length()` local. Both run cleanly and the global retains
its String value across the sort call.

## [3.12.0] — 2026-06-20

### Type aliases dispatch to the underlying type's methods

`type Name = T` declarations have been parsed and registered in
`Sema::typeAliases` since v2.x, but the map was only consulted for
generic-param erasure (`T → Any`). User-written aliases over
concrete types — `type Headers = Map`, `type IntList = List`,
`type Age = Int` — were silently defined and never dereferenced
at use sites:

```
type Headers = Map
h: Headers := Map()
h.put("a", "1")     // → Unknown method 'String.put'
h.length()          // → 8 (garbage read from wrong struct slot)
```

`h: Headers` landed `h` in scope under the literal type name
"Headers"; Codegen's `typeGen->getLLVMType("Headers")` fell back to
an opaque shape; method dispatch either errored at Sema or
returned wrong values at runtime.

v3.12.0 dereferences each alias at variable-binding time in
`checkVarDecl`:

  - `finalType` is replaced with the alias's target before
    `defineVariable` is called, so the scope entry carries the
    canonical type and Sema's method-lookup chain works.

  - `node->typeAnnotation` is rewritten to the same target so
    Codegen — which reads it verbatim for LLVM-type lookup and
    Any-unboxing decisions — allocates the right struct and
    dispatches through the right method table.

Both pieces are required; the Sema-side fix alone leaves Codegen
allocating the wrong shape. Skips rewriting when the target is
the special `"Any"` sentinel used by generic-param erasure so
existing `Box[T]` machinery stays intact.

Limitation: this release covers value-side aliases (Map, List,
Int, Bool, etc.). Aliases over String trip a pre-existing edge
case in Codegen's top-level VarDecl path (string-typed bindings
as the first AST node ICE the LLVM verifier) that's unrelated
to alias work and queued for a follow-up.

`tests/probes/p71_type_alias_dispatch.quirk` exercises Map, List,
and Int aliases through realistic call shapes (`.put`/`.get`/
`.length`/arithmetic).

## [3.11.0] — 2026-06-19

### Import aliases: `from X use { y as local }`

The parser used to reject `as` inside a `from X use { ... }` block
with a bare `Expected '}' (found 'as')`, even though every other
piece of the toolchain already understood the concept — the LSP's
`scanImports` records aliases (line 631 of quirk-lsp/src/server.ts
predates this release), the runtime symbol table is name-keyed, and
the whole-module form `from X as alias` already worked. This
release fills in the missing parser/Sema path.

The classic motivating case: two same-named exports from sibling
stdlib packages. `url` and `toml` both expose `parse(String)`;
without aliases the second `from … use { parse }` collides with the
first. With aliases each lands under a distinct local name:

```
from url use { parse as url_parse, URL }
from toml use { parse as toml_parse }

u: URL := url_parse("https://example.com/")    // dispatches to url.parse
m: Map := toml_parse("k = 1")                  // dispatches to toml.parse
```

Implementation:

  - `UseNode` gains a `filterAliases: vector<string>` parallel
    to `filterList`. Empty entry = no alias; non-empty = the
    local name the user wrote after `as`.

  - Parser reads optional `AS <ident>` after each filter name
    inside the `{ ... }` body. Backward-compatible: bare
    `from X use { y }` keeps working unchanged because the
    alias slot stays empty.

  - Sema's `VisibilityContext` gains `importAliases:
    map<local, source>`. `checkUse` records the mapping when
    `filterAliases[i]` is non-empty and uses the local name
    for visibility checks; the source name continues to flow
    into `visibleSymbolSources` so cross-package overload
    disambiguation (v3.6.0) keeps working.

  - `lookupTopLevel` checks `importAliases` first and
    dereferences the local name to the source before the
    rest of the resolution chain runs. The chosen
    `FunctionNode` carries its canonical linkage name, which
    Codegen reads via the same `CallNode::resolvedLinkageName`
    slot v3.6.0 already wired up. No Codegen changes needed.

Limitation: this release aliases top-level functions only.
Aliasing structs / enums / interfaces would require type-name
substitution everywhere those names appear in annotations —
larger surface, queued for a follow-up if anyone needs it.

`tests/probes/p70_import_aliases.quirk` exercises the two-package
collision case (`url.parse` + `toml.parse` aliased side-by-side).

## [3.10.1] — 2026-06-19

### Pin stdlib net to v1.2.0 (Response.json() convenience method)

Patch release: bumps `STDLIB_TAG_net` from v1.1.0 to v1.2.0 so
`make bootstrap-stdlib` picks up the new `.json()` method on the
HTTP Response struct:

```
resp := http.get("https://api.example.com/user/42")
if resp.ok {
    data: Map := resp.json()
    print(data.get("name"))
}
```

The method is a one-liner around `encoding.json.parse(self.text)`
— no runtime work on this side. v1.1.0's TLS support is
preserved unchanged.

The release exists as a patch (not minor) because no compiler
code changed; the only thing v3.10.0 → v3.10.1 carries is the
Makefile tag pin so a clean clone picks the updated net library.

## [3.10.0] — 2026-06-19

### Typo-suggestion hints for module functions and enum variants

Sema already emitted `did you mean X?` hints for undefined
identifiers, missing struct members, parent struct names, and
`from X use { y }` imports. Two common typo paths still produced
bare errors:

  - `net.gte("...")` — used to fail at *Codegen* time with
    plain `Unknown function 'gte'` (no hint, error reported at
    the wrong source location). Sema's `MODULE$X.fn(...)`
    handler used to silently return `void` when the function
    didn't resolve, deferring the error to Codegen.

  - `Color.Reed` — used to fail at Sema with `'Reed' is not a
    variant of enum 'Color'` but no suggestion.

This release adds two helpers — `suggestModuleFunctions` and
`suggestEnumVariants` — and wires them into the matching error
sites:

```
module 'net' has no function 'gte'
  hint: did you mean `get`?

'Reed' is not a variant of enum 'Color'
  hint: did you mean `Red`?
```

The Sema-time error on missing module functions also moves the
error report to the correct source location (the call site)
instead of Codegen's late diagnostic.

`editDistance` upgraded from plain Levenshtein to
Damerau-Levenshtein so single-char transpositions count as one
edit. Without this, `gte` ↔ `get` scored as distance 2 (two
substitutions) and missed the existing cutoff of 1 for short
queries — `gte` would have produced no hint despite being the
canonical keyboard-finger-swap typo.

`tests/probes/p44_typo_hints.quirk` documents the feature and
keeps the happy-path enum construction working so a future
regression in the new Sema branch surfaces in CI as an ICE.

## [3.9.0] — 2026-06-19

### Python-style format specs in f-strings

C-style specs (`.2f`, `05d`, `x`) already passed through to printf
via `append_formatted` in the runtime. Python-style alignment
(`<`, `>`, `^`), thousand separators (`,`), and bare alignment
without an explicit type (`${name:>10}` on a String) all
silently emitted raw `%>10` text into the output.

This release lands a Python→printf translation layer in
`src/Runtime/core/string.c`:

  - **Alignment** — `<N` (left), `>N` (right), `^N` (centre).
    `<` maps to printf's `-` flag; `>` is the printf default;
    `^` is post-processed by redistributing the leading
    padding to both sides. Works for ints, floats, and
    strings.

  - **Thousand separators** — `,d`, `,`, `,.2f`. The comma is
    stripped from the printf spec and inserted into the
    integer portion of the result post-hoc, walking from
    right to left. Handles negative numbers and floats with
    the comma confined to the int part.

  - **Default type suffix** — `${name:>10}` (no type char)
    used to invoke undefined behaviour in `snprintf("%10",
    arg)`. The translator now appends `d`/`g`/`s` based on
    the value's runtime type so the spec is well-formed in
    every branch.

Custom fill chars (`*>10` to pad with asterisks) and Python's
`=` alignment for signed numbers are deferred — printf can't
do either without manual post-processing and there are no
known users blocked on them today.

`tests/probes/p43_fstring_python_specs.quirk` locks the
behaviour: 15 cases covering numeric + string alignment in
all three directions, thousands on int + float, and the
canonical C-style passthroughs (which keep working).

## [3.8.0] — 2026-06-19

### Match-exhaustiveness warnings for plain enums

Tagged-union scrutinees already got a warning when a `match`
left a variant uncovered. Plain enums had no parallel check —
this:

```
enum Color { Red, Green, Blue }
match c {
    case Color.Red   => ...
    case Color.Green => ...
}
```

silently fell through on `Color.Blue` at runtime with no
diagnostic, and the silent fall-through has caused at least
one head-scratching debug session.

Sema's MatchNode handler now mirrors the tagged-union path
when the scrutinee type is in `enumRegistry`. It walks each
arm's patterns looking for `EnumName.Variant` member-access
forms, collects the covered variants, and warns once with the
full list of missing names:

```
non-exhaustive match on enum 'Color' — missing variant: Color.Blue.
Add an arm or a `_` wildcard.
```

Stays a warning, not an error — matches the tagged-union
policy. Partial matches are still legal; the runtime
fall-through (match exits silently) doesn't crash. Guarded
arms (`case Color.Red if x > 0 => …`) are treated as
non-covering because the body is conditional.

`tests/probes/p42_enum_exhaustiveness.quirk` locks the
behaviour across three shapes: fully covered (no warning),
partially covered with `_` (no warning), and partially
covered without `_` (warning fires).

## [3.7.0] — 2026-06-19

### TLS support in net.http via libssl

`net.http.get("https://...")` works. Until this release the HTTP
client could only talk to plain http:// URLs because the runtime
had no TLS path — the CI httpbin fixture from v3.5.3 was the
workaround. v3.7.0 closes the loop.

Runtime side (this repo):

  - `src/Runtime/libs/net.c` gains four C symbols:
    `Net_tls_connect`, `Net_tls_send`, `Net_tls_recv`,
    `Net_tls_close`. `tls_connect` allocates a fresh socket,
    performs TCP connect + TLS handshake (with SNI), and
    verifies the cert against the system CA bundle. Hands the
    Quirk side an Int handle that indexes into a 256-slot
    table.

  - SSL_CTX is lazily allocated process-wide; min protocol is
    TLS 1.2, peer verification is mandatory.
    `SSL_VERIFY_PEER` + `SSL_CTX_set_default_verify_paths` +
    `SSL_set1_host` means hostname mismatches and expired
    certs both fail at connect time, before any plaintext data
    moves.

  - Makefile links `-lssl` alongside the existing `-lcrypto`.
    `libssl-dev` is already in the CI base image, so no
    workflow change beyond the ubsan rebuild line.

Library side (`quirk-net` v1.1.0):

  - Four matching externs in `packages/net/index.quirk`.

  - `packages/net/http.quirk` dispatches on `u.scheme`. Default
    port is 443 for https, 80 for http. A new `_send_and_read`
    helper owns the socket / TLS lifetime + read loop so the
    two paths share one body — the rest of `_request_with_depth`
    (headers, redirect chain, chunked transfer) is unchanged.

  - Relative-Location redirect resolution now preserves the
    original scheme so an https→relative→https chain doesn't
    accidentally downgrade.

Makefile pins `STDLIB_TAG_net = v1.1.0`. Anyone running
`make bootstrap-stdlib` against v3.7.0 picks up the matching
library automatically.

`tests/probes/p41_tls.quirk` is the regression lock: when
`QUIRK_NETWORK_TESTS=1` it hits `https://example.com/`,
verifies the cert, parses HTTP/1.1, and asserts status=200 +
non-empty body. Off by default in CI (per v3.5.x policy —
public internet is not a CI dependency).

No `verify: false` opt-out exists today. If you have a
self-signed staging host that actually needs it, file an
issue and we'll add a kwarg.

## [3.6.0] — 2026-06-19

### Codegen overload disambiguation for cross-package bare-name calls

When two stdlib packages exported the same top-level function name
(e.g. `console.input` and `prompt.input`, or `url.parse` and
`toml.parse`), Sema's `lookupTopLevel` already picked the right
candidate at type-check time by matching the caller's module
against `visibleSymbolSources`. Codegen, however, kept a separate
`functionDeclarations` map keyed by bare name — last write wins —
so the LLVM call site dispatched to whichever package was loaded
last regardless of which import the caller wrote. The v3.4.0
CHANGELOG promised this would be fixed for v3.5.0; it wasn't —
this release closes that gap.

The fix routes Sema's chosen overload to Codegen explicitly:

  - `CallNode` gains a `resolvedLinkageName` field. Sema stamps
    it on every bare-name call it resolves through
    `lookupTopLevel` when the chosen function has a non-empty
    linkage name (i.e. it's package-prefixed). Single-slot calls
    still set it harmlessly — Codegen treats that as a hint, not
    a constraint.

  - Codegen's bare-name dispatch in `handleCall` checks
    `call->resolvedLinkageName` first and looks the LLVM function
    up directly by linkage name. Falls back to the historical
    `resolveFunction(name, classPrefix)` path when unset, so
    non-collision callers see no behaviour change.

`tests/probes/p40_overload_disambig.quirk` locks the regression
by importing `parse` from `url` while also pulling `toml` into
the same program (both export top-level `parse`). Before the fix,
Codegen could dispatch the bare `parse(...)` call into
`toml.parse` and either ICE on the type mismatch or land on a
Map at runtime. The probe confirms the URL-shaped result.

Note: the `html.input` ↔ `console.input` collision that
motivated this work was previously dodged by renaming
`html.input` to `input_` (shipped in html v1.0.0). The rename
stays in place — disambiguation lets us un-rename in a future
html release without breaking callers, but that's a separate
package bump.

## [3.5.3] — 2026-06-18

### Tests: in-repo httpbin fixture replaces external dependency

`tests/http_client_test.quirk` was tied to `http://httpbin.org`,
which goes through periodic 503 storms that turned the test into
a coin flip. The CI=true gate from v3.5.2 kept releases moving
but didn't fix the local-developer experience.

Three new files take the public service out of the loop:

  - `tests/fixtures/httpbin_lite.py` — ~150-line stdlib-only
    Python HTTP server with the four endpoints the test needs
    (`/headers`, `/get`, `/post`, `/redirect/N`). Mirrors
    httpbin's response shapes closely enough that the existing
    assertions work unchanged.

  - `tests/fixtures/run_http_tests.sh` — bash wrapper that
    starts the fixture, waits for readiness, runs the test
    against `http://127.0.0.1:<port>`, and cleans up on exit.

  - `tests/http_client_test.quirk` now reads `QUIRK_HTTP_BASE`
    from the env, defaulting to `http://httpbin.org` so a bare
    `quirk run tests/http_client_test.quirk` still works
    against the real service when you want that. The wrapper
    overrides it to the local fixture.

CI Test workflow gains a dedicated "Run http client tests
against in-repo fixture" step that invokes the wrapper. The
existing stdlib-tests loop still walks `http_client_test.quirk`
but it short-circuits on the `CI=true` gate (no QUIRK_HTTP_BASE
set in that step), so it's the wrapper run that exercises the
HTTP client end-to-end. No external network dependency on the
release path.

## [3.5.2] — 2026-06-18

### Tests: skip network suites only under `CI=true`

v3.5.1's `QUIRK_NETWORK_TESTS=1` gate had inverted ergonomics —
running `quirk run tests/http_client_test.quirk` locally
silently skipped, which is the opposite of what a developer
typing the command expects.

The gate now flips to `CI=true and not QUIRK_NETWORK_TESTS=1`:

  - Local `quirk run …` invocations run the test (your intent is
    clear when you type the command).
  - GitHub Actions (which sets `CI=true` automatically) skips by
    default.
  - `QUIRK_NETWORK_TESTS=1` in CI forces a real run when you want
    to verify the network path manually.

### Net: discovered limitation worth surfacing

While debugging the http_client_test failures, confirmed that
`packages/net/http.quirk` is plain-HTTP-only — its docstring
already states "`https://` URLs will fail because there is no
TLS in the runtime yet". An attempt to switch tests to
`https://httpbin.dev` returned 301 because Quirk opens port 80
regardless of the URL scheme; httpbin.dev's HTTP listener
redirects everything to HTTPS.

No fix in this patch — adding TLS is a runtime project (openssl
linkage + handshake + SNI). Filed as a real follow-up. The
tests stay on `http://httpbin.org` (gated by `CI=true`) until
TLS lands or a self-hosted CI fixture replaces the external
dependency.

## [3.5.1] — 2026-06-18

### Tests: gate network-dependent stdlib tests behind `QUIRK_NETWORK_TESTS=1`

`tests/http_client_test.quirk` and `tests/http_test.quirk` make
real HTTP calls to httpbin.org / example.com. The v3.5.0 CI
Test run failed because httpbin.org returned `503` on one call
during the run window — a real-world service hiccup that the
stdlib-test runner had no way to distinguish from a code
regression. Re-running CI passed (httpbin came back), but a
release shouldn't be blocked by an upstream's mood.

Both tests now check `sys.env("QUIRK_NETWORK_TESTS") == "1"` at
the top of `main()` and print a "skipped" line + return when
unset. The default-skip keeps CI green; local runs that want the
real-network coverage stay one env-var away:

```bash
QUIRK_NETWORK_TESTS=1 ./bin/quirk tests/http_client_test.quirk
```

## [3.5.0] — 2026-06-18

### Stdlib: new `html` package

`make bootstrap-stdlib` now fetches `quirk-html` v1.0.0
alongside the other 23 stdlib packages. `from html use { ... }`
works on a fresh clone with no extra install.

The package ships ~40 tag constructors (block, inline, heading,
document, void), an `attrs([class_("c"), id("home")])` helper +
13 one-entry attribute helpers, and `text()` / `raw()` /
`escape()` text utilities. `Element.render()` walks the tree to
a single `String` ready to drop into a `net.http.Response` body.
21-case test suite covers escape rules, void elements, attribute
escaping, nested rendering, and the raw escape hatch.

```quirk
from html use { html_, head_, body_, title_, h1, p_a, class_ }

page := html_([
    head_([title_(["Recipes"])]),
    body_([h1(["Hello"]), p_a([class_("tag")], ["Fuzzy lookup."])])
])
print(page.render())
```

See `github.com/AlexVachon/quirk-html` for the full surface.

### examples/recipe_web rewritten on the html lib

The HTTP example previously built its response with ~70 lines
of `+ "<div class=\"…\"` string concatenation and a hand-rolled
`html_escape`. It now renders through the html lib:

```quirk
return article_a([class_(cls)], [
    h3([m.recipe]),
    p_a([class_("meta")], [
        span_a([class_("book")], [m.book]),
        span_a([class_("page")], ["p." + m.page.str()]),
        span_a([class_("score")], ["distance " + m.distance.str()])
    ])
])
```

Every text node is auto-escaped by the lib — no more manual
`&` / `<` / `"` sprinkled at every interpolation. The full
page tree (doctype + html + head + body) reads top-to-bottom.
Pulls in `html v1.0.1` for the generic `attr(name, value)`
helper used for `placeholder` / `method` / `action` /
`autofocus` (everything outside the 13 named attribute
helpers).

## [3.4.0] — 2026-06-17

### `quirk init` scaffolds src/ + tests/ stubs

`quirk init` used to write only `quirk.toml` and leave you to
remember the conventional layout. It now scaffolds a working
project the first command can exercise:

```
my-project/
├── quirk.toml
├── src/
│   └── index.quirk       # define main() — `quirk run` enters here
└── tests/
    └── index_test.quirk  # one TestCase — `quirk test` runs it
```

Pass `--lib` for the library shape (no `main`, `src/index.quirk`
exposes a public function). Pass `--bin` (the default) explicitly
if you want to be loud about it. Both shapes leave the project
in a state where `quirk run` and `quirk test` both succeed
immediately on a fresh `quirk init -y` directory.

Existing `src/index.quirk` / `tests/index_test.quirk` files are
left alone — the scaffolder only writes the stub if the target
path doesn't exist yet.

### Parser: `..element` (parent-of-current) relative imports

`from ..element use { Element }` now parses — the lexer already
collapsed `..` into a `DOTDOT` token (for the range operator
`1..10`) but `parsePath` in the `use`-statement parser only
accepted `DOT` and `ELLIPSIS`. Now `DOTDOT` is folded into the
same leading-dots-string the resolver in
`Compiler.cpp:resolveImportPath` already understood — it counts
the dots and walks `parent_path()` once per dot beyond the
first. `.`/`..`/`...` all resolve consistently.

Use this for sub-package layouts where a file in
`src/foo/index.quirk` wants its sibling-of-parent:
```
src/
├── element/index.quirk
└── tag/index.quirk      # from ..element use { Element }
```

### Sema: same-name top-level functions across packages

`html.input` (HTML `<input>` tag) used to fight `console.input`
because `methodRegistry[""]` was a flat global namespace —
whichever package Pass 1 walked last won the slot, and the
loser's own internal calls then routed through the winner's
signature with confusing "expected List but got String" errors
in unrelated code.

A new `topLevelOverloads` side-table tracks every candidate
when a name collides across modules. The new `lookupTopLevel`
helper disambiguates at the call site by preferring (1) the
candidate from the caller's own module, then (2) the candidate
whose package prefix matches the user's explicit
`from PKG use { name }`, then (3) any visible candidate, then
falling back to the historical last-write-wins single slot.

Codegen still has a parallel single-slot registry, so the bare
`input` name in the html lib stays as `input_` for v3.4.x —
the rename pattern matches the existing `select_`/`main_`/
`html_` keyword-collision convention. Codegen disambiguation
is queued for v3.5.0.

## [3.3.2] — 2026-06-17

### Codegen + runtime: list literals of Double / Bool

`[1.5, 2.5]` used to ICE with "Invalid bitcast double to i8*"
and `[true, false]` rendered as `[1, 0]`.
`createListFromValues` in `StructGen.hpp` had a Double element
fall through to a raw bitcast for the i8* list slot, which LLVM
rejects on type-class mismatch — and Bool elements went through
the inline-Int path because `isIntegerTy()` matches both `i32`
and `i1`. Each non-Int element now routes through the matching
`Core_Primitives_Any_box_*` runtime helper so it carries an
`ANY_DOUBLE` / `ANY_BOOL` tag readable by every
`quirk_opaque_to_*` decoder. (Bool is checked before the generic
`isIntegerTy()` branch so `i1` doesn't silently take the
inline-int path.)

The companion `String.join` (used by `List.__repr` → print)
also needed a fix: the old fast-path inlined the tagged-Int
case and assumed every non-Int item was a String*, so heap-
tagged Doubles segfaulted on the first dereference. Each item
now routes through `quirk_opaque_to_string`, picking up
ANY_INT / ANY_DOUBLE / ANY_STRING / ANY_LIST / ANY_MAP /
ANY_TUPLE / ANY_CALLABLE / String* / raw-tagged-int safely.

Probe `p62_double_list_literal` covers Double and the mixed
`[Int, Double, Bool]` case.

### examples/recipe_web

Browser-served version of `recipe_search`. Boots an HTTP server
on `127.0.0.1:8080` with an inline-styled search form; submitting
a query returns ranked match cards with each recipe's book + page.
Unblocked by v3.3.1's `boxToVoidPtr` fix — the handler's Response*
now survives the Callable round-trip intact. Demonstrates
`net.server`, inline HTML/CSS, `url.unquote_form` for GET form
decoding, and the same `List.sort` + Option API as the TUI
companion.

## [3.3.1] — 2026-06-16

### Codegen: `boxToVoidPtr` no longer auto-stringifies user structs

A long-standing shortcut in `boxToVoidPtr` auto-called `__str`
on any user-defined struct being boxed to `i8*`, on the theory
that boxToVoidPtr was always feeding `print()`. It isn't — it's
also the canonical "send a value through a Callable return /
generic Any slot" path. The result: `Server().listen(host,
port, handler)` segfaulted on the first request because the
handler's Response* got stringified to a String* mid-flight,
the caller bitcast the String* back to Response*, and
`_format_response` read garbage offsets.

Removed the auto-str. Print and format already route through
`BuiltinGen::valueToString`, which calls `__str` at the
consumer site — no other call path relied on the silent
conversion. Probe `p61_callable_returns_user_struct` locks
both the annotated and inferred forms.

This unblocks every `net.server` example (the canonical one
in `packages/net/server.quirk` runs cleanly now) and any
higher-order code passing user structs through Callables.

## [3.3.0] — 2026-06-16

### Sema: bare function name resolves to Callable in value contexts

`apply(dbl, 5)` was rejected with "expected 'Callable' but got
'Int'": Pass 1 binds top-level functions in the global scope
with their **return type**, so a literal lookup got "Int" (dbl's
return) instead of the function value. `checkLiteral` now
detects function names that aren't shadowed by a deeper local
scope and surfaces them as `Callable` — the direct-call path
`checkCall(callee == LiteralNode)` bypasses checkLiteral and
still gets the return type, so `dbl(5)` is unchanged. Probe
`p55_bare_fn_as_callable` locks it.

### Parser: trailing comma + newline in list and map literals

Reader-friendly layout used to fail:
```
points := [
    Pt(1, 2),
    Pt(3, 4),
]
```
Set and tuple literals already had the peek-after-comma break;
list and map didn't. Same one-line check applied to both.
Probe `p56_multiline_literals` covers all four shapes.

### Codegen: emitBox preserves raw i8* opaque values

`a: Any := xs.__get(0); b: Int := a` silently produced 0. The
walrus with `Any` annotation routed val through `emitBox`,
which had no special case for raw `i8*` (the universal opaque
shape coming out of `__get` / `Map.get` / lambda returns) and
fell through to the "must be a String" fallback — re-encoding
an Int-tagged i8* as a heap `Any-of-String`. The downstream
`b: Int := a` then quirk_opaque_to_int'd the string-tagged
wrapper and hit the default case (0).

Fix: emitBox now passes raw `i8*` through unchanged. Every
`quirk_opaque_to_*` decoder already accepts `i8*` and dispatches
on the runtime tag, so the universal-opaque shape is a valid
Any payload as-is.

Probe `p57_any_int_walrus`. The fix also lets `List.sort` drop
the awkward "no annotation on `a` / `b` / `cmp`" workaround
that v1.3.0 shipped with — comparators now read the way the
docstring example claimed they did.

### Codegen: Double → opaque-ptr in variant-field stores

`Some(3.14)` (a generic-T variant constructed with a Double arg)
tripped the verifier: "Stored value type does not match pointer
operand type! store double, i8**". The init-arg coercion path
got a Double → opaque-ptr branch in v3.2.0, but the no-`__init`
fallback path in `StructGen.hpp`'s direct-field-store loop did
not — and variant structs use that fallback. Same `Any_box_double`
treatment now applied there. Probe `p58_double_field_in_variant`
locks it.

### Codegen: Any → Bool field assignment

`h.n = items.get(0)` where `h.n: Bool` and `items.get(0)`
returns Any used to fail the verifier with "Invalid bitcast
i8* to i1". The field-assign coercion chain had Any→Int and
Any→Double routes but no Any→Bool, so it fell through to a
raw bitcast LLVM rejects. Routed through `quirk_any_as_bool`
(same helper used for truthy checks on i8* values in if/while
conditions). Probe `p60_any_to_bool_field`.

### Sema: arity gate on free and module-qualified function calls

`test.assert_eq(crypto.sha256(""),)` (the new v3.3.0 trailing-
comma support let this parse as a 1-arg call) slipped past Sema
and reached Codegen, which emitted a malformed
`call void @"Test$assert_eq"(i8* %3)`. Sema's free-function path
type-checked positional args but didn't gate count; the module-
qualified path (`module.fn(...)`) didn't gate either count or
types. Both paths now reject too-few / too-many calls with a
clean message ("`assert_eq() takes between 2 and 3 arguments
but 1 was given`") and the module-path also type-checks its
matched positional args. Probe `p59_call_arity`.

## [3.2.0] — 2026-06-14

### typing v1.3.0 — Option/Result combinators + List.sort

Bumps `STDLIB_TAG_typing` from v1.1.0 to v1.3.0. New stdlib API:

- `Option[T]` gains `unwrap`, `unwrap_or_else(f)`, `map(f)`,
  `and_then(f)`, `or_else(f)`, `filter(pred)`, `ok_or(err)`.
- `Result[T, E]` gains `unwrap`, `unwrap_err`,
  `unwrap_or_else(f)`, `map(f)`, `map_err(f)`, `and_then(f)`,
  `or_else(f)`, `ok()`, `err()`.
- `List.sort(cb: Callable) -> List` — in-place insertion sort
  by comparator (negative/zero/positive Int, usual convention).

Probe `p53_option_result_combinators` locks the combinator API.

### prompt v1.1.0 — select_obj + select_index

Bumps `STDLIB_TAG_prompt` from v1.0.3 to v1.1.0:

- `select_index(message, options, default_idx)` returns the
  zero-based index of the chosen option (carried alongside the
  label inside a `_Pick`).
- `select_obj(message, options, format, default_idx)` takes a
  `format: Callable` row-renderer and returns the **original**
  option object, not the rendered label. Drops the round-trip
  through string parsing that callers used to do.

### Codegen: Int-zero/null conflation in T-erased boxing

`IntToPtr(0)` produces an i8* null pointer, indistinguishable
from the absence-of-value sentinel — so
`Option[Int].unwrap_or(0)` rendered as "null" instead of "0",
and any Int 0 routed through Any-erasure was lost. New
`src/Backend/BoxInt.hpp` factors a shared helper that routes
Int 0 (static or dynamic) through `Core_Primitives_Any_box_int`
for a real heap Any* with `ANY_INT` tag; non-zero stays on the
inline-tag fast path. Applied at four sites: `boxToVoidPtr`
(lambda env/return), the call-arg coercion in `Codegen.cpp`,
and the method-init + field-store paths in `StructGen.hpp`.
Probe `p54_int_zero_any_box` locks it.

### Codegen: lambda Int/Double params unbox heap-Any safely

`unboxLambdaParam` was using raw `PtrToInt` for Int and a
similar bit-cast for Double, which returned the pointer
address (not the contained value) when called with a heap
Any* — exactly the shape the new BoxInt helper produces for
zero, and the shape `Any_box_*` already produced for runtime-
materialized values. Routed both through `quirk_opaque_to_int`
and `quirk_opaque_to_double` so lambda params receive sane
values regardless of inline-tag vs heap-Any encoding.

Surfaced while shipping `List.sort` — comparators with `Int`
params were getting 0 / pointer addresses for `__get`-sourced
list elements.

### Codegen: Double → opaque-ptr arg coercion

`StructGen.hpp`'s init-arg coercion handled Int→ptr but not
Double→ptr. A generic-T constructor like `Some(value: T)`
called with a Double argument left the value at the call site
without boxing, producing "Stored value type does not match
pointer operand type! store double … i8** …" at verify time.
Routed through `Core_Primitives_Any_box_double` (matching the
`Codegen.cpp` call-arg path).

### examples/recipe_search

New example: an interactive fuzzy-search TUI over a hardcoded
library of cookbooks. Dogfoods the new Option API for
"best match or nothing", `List.sort` for ranking, `select_obj`
for a discoverable `/` drawer of slash commands, and ANSI
inline styling for a polished terminal feel.

## [3.1.1] — 2026-06-13

### Param defaults: Sema rejects type-mismatched defaults

`define __init(self, raw: Int = "Hello")` (Int param, String
default) used to slip past Sema and surface as an LLVM verifier
abort in Codegen — the default expression was lowered to a
`String*` and passed into the Int parameter slot, producing
"Call parameter type does not match function signature!". Fuzz
finding #1 of this patch. checkFunction now type-checks each
param's default against the declared param type and emits a
clean Sema error pointing at the parameter. Probe
`p52_param_default_typecheck` locks the regression.

### Match-arm equality: no invalid bitcast on type-mismatched scrutinee

When the match scrutinee was a struct pointer and a case pattern
was a primitive (e.g. `match n { case 3 ... }` where `n` ended up
typed as `String*` due to upstream type-mismatch from a mistyped
container annotation), `emitMatchEq` forged a `bitcast i32 to
String*`, which LLVM's verifier rejects as malformed IR. The fix:
when R is a primitive and L is a struct ptr, return constant-false
— the arm can't match at runtime anyway, and the program may still
trip a Sema error or a different runtime check, but it no longer
takes the compiler down. Probe `p51_match_eq_typemismatch` locks
the regression.

### Fuzz harness: deterministic seed-file order

`tools/fuzz.py`'s seed corpus was iterated in `Path.rglob` order,
which is filesystem-dependent. Same `--seed 1` could pick a
different seed file on overlay-fs CI vs ext4 local — and the
v3.1.0 CI run found a crash my local box wouldn't repro. Sort
seeds for stable ordering. (Sorted seeds also surfaced the match-
eq bitcast crash above on local — the fuzz finding is what drove
the fix.)

CI workflow also now uploads `tools/fuzz_findings/` as an
artifact (and inlines the crash file's contents in the log) when
the fuzz step fails — so a future flake-find is debuggable
without an extra round-trip through CI.

## [3.1.0] — 2026-06-12

### Per-instantiation Codegen monomorphization — phase 1

Last open item from the v3 roadmap. When a binding is annotated
with a concrete generic type, Sema synthesizes a specialised
struct with the type parameters substituted in the field list:

```quirk
struct Box[T] {
    value: T
    define __init(self, v: T) -> void { self.value = v }
}

define main() -> void {
    a: Box[Int]    := Box(42)        // synth: Box__Int  { value: Int }
    b: Box[String] := Box("hello")   // synth: Box__String { value: String }
    p: Pair[String, Int] := Pair("age", 27)   // synth: Pair__String__Int

    print(a.value + 1)        // 43       (Sema knows a.value: Int directly)
    print(b.value + "!")      // "hello!"
    print(a.get())            // 42       (method walks inheritance to Box)
}
```

**How it works.** A new Sema Pass 0 runs before Pass 1
registration: walk every type annotation in the AST
(`VarDeclNode.typeAnnotation`, `Parameter.type`,
`FunctionNode.returnType`, `StructField.type`), collect every
`StructName[ConcreteArgs]` reference, synthesize one specialised
`StructNode` per unique pair (with `T`-substituted field types
+ `parents = [original]` so method dispatch still resolves), and
rewrite every annotation in place to the mangled name
(`Box[Int]` → `Box__Int`). Downstream passes see only concrete
structs.

### Parser: var-decl annotations now accept the full type grammar

`b: Box[Int] := …`, `b: List[Int]?`, `b: Int | String = …` all
parse cleanly. Was a separate small bug — var-decl annotations
only read one identifier + optional `?`; param-list types and
return types had been using the richer `parseTypeString` for a
while. Bringing them into alignment was the prerequisite for
monomorphization being exercisable in idiomatic code.

### Deferred to v3.1.x

- **Constructor inference.** `b := Box(42)` (no annotation) keeps
  the erased `Box` layout. Use `b: Box[Int] := Box(42)` for
  specialisation. Inferring the type args from the constructor's
  arg types is straightforward but interacts with method
  dispatch + Sema's type-substitution logic; saving for a
  follow-up.
- **Method-body T-substitution.** Methods declared on the generic
  dispatch via inheritance — they still see `T` as the erased
  i8* layout. Field-level type safety improves; method-level
  codegen is unchanged.

Probe `p50_monomorph_struct.quirk` locks the field-substitution
path for Box[Int] / Box[String] / Pair[K, V] simultaneously, plus
method dispatch through the inheritance chain. 50/50 probes pass.

## [3.0.4] — 2026-06-12

### CI gates the fuzzer + Sema fixes its leftover hole

`make fuzz FUZZ_ITERS=300 FUZZ_SEED=1` now runs as a Test
workflow step on every push + PR. Deterministic seed so flakes
don't erode confidence; 300 iters fits a ~30s budget on the
ubuntu-20.04 container. Any unique crash promotes to a build
failure.

A first-run pass against the v3.0.3 binary surfaced one more
class the v3.0.3 fixes didn't cover: **`super().__init(complex_arg)`
bypassed Sema's arg-type checking** because the searchMethod
dispatch looked up `<Type>___init` (triple underscore — the
shape used for every *other* dunder) while `__init` is stored as
`<Type>__init` (single underscore connector swallowed by the
double-underscore method name). Without finding the method, the
arg loop never ran, and `super().__init("x" * msg)` slipped past
the Sema arith gate. Codegen then ICE'd on
`mul %String*, %String*`. Fixed: added the `<Type>__init`
fallback in `searchMethod` so dunder dispatch finds it.

### Three more example projects under `examples/`

| Directory                                                 | Shape                  | Highlights                                                       |
|-----------------------------------------------------------|------------------------|------------------------------------------------------------------|
| [`calc/`](../examples/calc/README.md)                     | Interpreter (~150 LOC) | Recursive AST tagged union, Pratt parser, match evaluator        |
| [`md2html/`](../examples/md2html/README.md)               | Text transform (~100)  | String slicing, multi-pass token replacement, functional decomp  |
| [`todo_cli/`](../examples/todo_cli/README.md) (prior)     | Data + dispatch (~140) | Tagged unions, Option/Result, match with payload narrowing       |

Each example is self-contained and walks through which language
feature each block exercises. See [`examples/README.md`](../examples/README.md)
for the overview.

## [3.0.3] — 2026-06-12

### Mutation-based fuzzer (`tools/fuzz.py` / `make fuzz`)

```
make fuzz                                  # 500 iterations
make fuzz FUZZ_ITERS=2000 FUZZ_SEED=42      # reproducible
```

Picks random files from `tests/probes` + `tests/`, applies 1-3
mutations (operator swaps, literal changes, type swaps, line
drops/dups/swaps), runs the result, and saves any catastrophic
failure (SIGSEGV, SIGABRT, "Internal compiler error", LLVM verifier
assert) under `tools/fuzz_findings/` with the seed + mutation log
embedded. Crashes are deduplicated by signature so repeats don't
flood the output.

Sema rejections (clean error + exit 1) are intentionally fine —
the bar is "no mutation, however nonsensical, should crash the
compiler."

### Fixed: 5 crash classes the fuzzer surfaced

**1. `String * String` → LLVM IR rejection.** The arithmetic
operator-typing gate short-circuited on `lType == rType`, accepting
matched-but-non-numeric operands. Codegen then emitted `mul
%String*, %String*` and LLVM rejected. Tightened the gate: `-` /
`*` / `/` / `%` now require each side to be numeric-or-unknown.
`==` / `!=` on equal types is still fine via the dedicated eq-branch.

**2. `Int → Double` widening on function return → IR mismatch.**
Sema accepts the widening via `isCompatibleTypes`, but the
return-statement Codegen had no `i32 → double` path — only
`i32 → i32` (cast width) and `i8* → i32/double` (Any unbox). Added
explicit `SIToFP` for `Int → Double` and `FPToSI` for the
explicit-`as`-Int narrow.

**3. Ordered comparison on a generic-field `i8*` vs typed
`String*` → ICmp operand mismatch.** v2.4.3 added a Codegen
bridge that bitcasts the erased side to the typed struct for
`==` / `!=`. Extended the bridge to `<` / `>` / `<=` / `>=`.
Intentionally not applied to arithmetic operators (`+` / `-` /
`*` / `/`): for those, an i8* operand against a String* is the
string-concat path and bitcasting would turn a tagged-int into a
garbage pointer.

**4. `Double → Int` field assignment → IR rejection.** Sema
accepts `self.age = doubleVal` (where `age: Int`) because both
types are numeric, but Codegen's field-assign coercion ladder had
no `double → i32` step, only `i32 → double`. Added FPToSI.

**5. `Double → Any` call-arg passing → IR rejection.** Calling
`test.assert_eq(doubleVal, ...)` where the param is `Any` (i8*)
emitted `call(double %x)` against an i8* signature. Added
`Core_Primitives_Any_box_double` wrap in the call-arg coercion
ladder.

Probes `p46_string_mul_rejected.quirk`,
`p47_int_to_double_return.quirk`,
`p48_ordered_cmp_generic_field.quirk`, and
`p49_double_to_int_field.quirk` lock the fixes.

After all five fixes, a 1000-iter fuzz run with a fresh seed
shows zero new crashes on the existing probe + test corpus.

## [3.0.2] — 2026-06-12

### "Did you mean ...?" hints on four common Sema errors

The existing `suggestNames` Levenshtein helper was wired into one
site (undefined-variable). Extended to four more high-traffic error
paths where typos are the dominant root cause:

```
member 'nme' not found in 'User'
  hint: did you mean `name`?
```

```
module 'sys' does not export symbol 'argz'
  hint: did you mean `argv`?
```

```
catch type 'ValueErorr' is not defined
  hint: did you mean `ValueError`?
```

```
struct 'Box' inherits from undefined type 'Comparbale'
  hint: did you mean `Comparable`?
```

New `suggestMembers(structName, query)` helper for the first one —
walks fields + methods on the struct and its parent chain. Re-uses
the existing edit-distance cutoff so we don't propose
"Direction" for "print".

Pure quality-of-life — no compiler behaviour change on the happy
path. 45/45 probes still pass.

## [3.0.1] — 2026-06-12

### Fix: Test CI workflow was failing on every push since v2.3.4

The Release workflow has been green throughout (binaries shipped),
but the Test workflow on `main` was failing every commit since
v2.3.4. Two distinct regressions, both in this patch.

**1. Closure-captured `count` returned the heap-Any pointer
address (decorator wrapper return path).** When v2.3.4 switched
nonlocal-cell boxing to `Any_box_*` (so Int 0 doesn't lower as
NULL), the cell now holds an Any\* heap pointer. The decorator
wrapper's `dec_int` return-unbox at
[`Codegen.cpp:904`](src/Backend/Codegen.cpp#L904) and the lambda /
function `ret_unbox_int` at [`Codegen.cpp:~2712`](src/Backend/Codegen.cpp#L2712)
both used raw `PtrToInt` — fine for legacy tagged ints, broken for
Any\* heap-boxes (returns the pointer address as the int).
Symptom: a `@call_counter`-decorated function returning a counter
gave back random heap addresses (-1155488768, 914379776, …)
instead of 1, 2, 3. Both sites now route through
`quirk_opaque_to_int`, which handles both encodings.

**2. `match x { case Int => … }` SIGSEGV on a primitive
scrutinee.** v2.4.4's direct-`__type_id` ICmp path in the match-on-
type codegen fired whenever `typeIdLookup(typeName) > 0`. But
primitive types like `Int` (whose StructNode is in the registry
but isn't actually struct-shaped at the LLVM layer) ended up
vtable-eligible and got a type-id — so `case Int` on a `42` i32
emitted `bitcast (i8*)42 to i32* ; load i32` and segfaulted reading
address 42. Added a `scrutVal` shape guard: the direct-load path
only fires when the scrutinee is actually a pointer-to-struct;
primitives keep using the `isinstance` runtime fallback (unchanged
from v2.3.x behaviour).

The CI workflow itself wasn't changed — same probes + same
stdlib smoke harness now run clean: 45/45 probes + 59 stdlib tests
pass on both linux-x86_64 and macos-arm64.

## [3.0.0] — 2026-06-10

### Milestone — the v3 type-system overhaul is complete

v3.0.0 is the marker that closes the type-system arc that ran
across v2.3.0 → v2.4.4. **No breaking changes** vs v2.4.4 — same
code compiles. The major bump is a narrative milestone, not a
break.

The v3 type system in one place:

| Feature              | Landed | Headline                                                                    |
|----------------------|--------|-----------------------------------------------------------------------------|
| Nullable primitives  | v2.3.0 | `x: Int? = null` — `Int` / `Bool` / `Double` / `Char` lower as `i8*`        |
| Tagged unions        | v2.4.0 | `type Result = Ok(...) \| Err(...)` with payloads + exhaustiveness          |
| Match narrowing      | v2.4.0 | `case Ok as o => o.value` bitcasts the bind to the variant struct           |
| Generic tagged unions| v2.4.1 | `type Option[T] = Some(value: T) \| None()`                                 |
| Generic substitution | v2.4.2 | `b: Box[Int]; b.value * 2` — Sema substitutes `T → Int` at use sites        |
| Generic method bodies| v2.4.3 | `define triple(self) -> T { return self.value * 3 }` — T treated as Any    |
| Variant methods      | v2.4.4 | `extend Ok { define is_ok(self) -> Bool { return true } }`                  |
| Canonical `Option` / `Result` | v2.4.4 | `from typing use { Option, Some, None, Result, Ok, Err }`         |

### Deferred to a future v3.x

- **Per-instantiation Codegen monomorphization.** `Box[Int]` and
  `Box[String]` still share a single LLVM struct layout with
  `value: i8*`. The full type-system correctness is shipped; the
  perf win of unboxed primitive payloads is future work.

### Regression coverage

45 probes lock the v3 surface against crash / wrong-output
regressions; 60 stdlib smoke tests cover the broader compiler. All
pass on both linux-x86_64 and macos-arm64.

See [RELEASE_NOTES_v3.0.0.md](RELEASE_NOTES_v3.0.0.md) for the full
upgrade narrative.

## [2.4.4] — 2026-06-10

### Per-variant methods via `extend VariantName { … }`

```quirk
type Result = Ok(value: Int) | Err(msg: String)

extend Ok  { define describe(self) -> String { return "Ok(${self.value})" } }
extend Err { define describe(self) -> String { return "Err: ${self.msg}"  } }
```

Two parser bugs blocked this before:

1. The `extend` block didn't strip the leading `self` parameter the
   way the in-struct parser does — so Sema's param-loop re-defined
   `self` as Any and clobbered the `self: VariantName` binding,
   making `self.value` error with "'Any' has no member 'value'".
2. Two extends defining the same raw method (`describe` on `Ok` and
   on `Err`) shared the parser-default linkage `<mp>$describe`. LLVM
   emitted only one and every dispatch routed to whichever was
   defined last. The extend parser now composes
   `<modulePrefix>_<Target>_<raw>` linkage matching the in-struct
   method shape.

### `Option[T]` / `Result[T, E]` canonical shape works end-to-end

```quirk
type Option[T] = Some(value: T) | None()

extend Option {
    define is_some(self) -> Bool {
        match self {
            case Some => return true
            case None => return false
        }
        return false
    }
    define unwrap_or(self, default_value: T) -> T {
        match self {
            case Some as s => return s.value
            case None      => return default_value
        }
        return default_value
    }
}
```

`is_some` / `is_none` / `is_ok` / `is_err` / `unwrap_or` live on
the union (not the variants) — that's the shape users actually hold
in practice; per-variant narrowing only kicks in inside `case … as v`
arms.

The canonical declarations land in the `quirk-typing` package
(`option.quirk` + `result.quirk`) so user code can write
`from typing use { Option, Some, None, Result, Ok, Err }`. Those are
in a separate repo and ship through the stdlib-bootstrap path; the
in-repo probe inlines the same shape so the compiler-level contract
is locked here regardless of the typing repo's state.

### Codegen fixes that unblocked the canonical types

- **Variant → union return upcast.** `return Some(42)` from a fn
  declared `-> Option` emitted `ret %Some* %x` against a `%Option*`
  signature and LLVM's tail-merge then phi'd two distinct variant
  types into one slot. Return path now bitcasts variant struct
  pointers to the declared union return type before `ret`.
- **`varEnumTypes` cross-function leak.** The tracker for
  `s := SomeEnum(…)` bindings (so `s.value` routes to the enum-
  accessor path) wasn't cleared between functions. A top-level
  `s := Status(404)` made `s.value` in
  `Option.unwrap_or`'s `case Some as s => return s.value` arm try
  to call `quirk_enum_value_int` on a `%Some*` → IR verifier
  rejected the call. Now cleared alongside `varGen` at every
  function-scope reset.

### Monomorphization (#1 from today's options) — deferred

Started scoping per-instantiation Codegen monomorphization
(distinct `Box__Int` / `Box__String` LLVM struct layouts so
primitive payloads pack directly instead of round-tripping through
i8*). Reverted the partial scaffolding because the surface is
genuinely 500–1000 LOC across Sema + Codegen + constructor
inference + ~70 lookup sites. Saved for a dedicated multi-session
push when a perf demand surfaces — the current uniform-
representation correctness is shipped (v2.4.3) and is what
languages like OCaml use by default.

Probes `p44_variant_methods.quirk` and `p45_typing_option_result.quirk`
lock the new behaviour. 45/45 probes + 19 stdlib tests pass.

## [2.4.3] — 2026-06-10

### Generics: method bodies can operate on `T` (v3 phase 3-c, lite)

```quirk
struct Box[T] {
    value: T
    define triple(self) -> T { return self.value * 3 }       // ✓
    define equals(self, other: T) -> Bool {
        return self.value == other                           // ✓
    }
}
```

Pre-v2.4.3 these errored at Sema with "operator '*' incompatible
types: 'T' and 'Int'". Sema now treats unbound type-params as
deferred-to-runtime in the operator-typing gates — the existing
`pushTypeParam` already aliases T as "Any" in `typeAliases`, so
`isGenericParam(T)` returns true and `isUnknown`/`isUnknownEarly`
short-circuit the type-mismatch error.

### New runtime helper: `quirk_opaque_eq`

Codegen previously hit two i8* operands on `==` with no struct-type
info and fell back to raw pointer equality — so
`b1.equals(b2)` on `Box[String]` with equal contents but distinct
heap allocations compared false. The new helper dispatches on
shape: tagged ints compare bitwise, Any\*-tagged values compare by
the .ival/.dval/.ptr appropriate to their tag, and the fall-through
treats both pointers as Strings and uses `Core_String_String___eq`.

### Caveat — true monomorphization (per-instantiation IR layouts)
remains future work

This is "uniform representation" (every generic field is `i8*` at
the IR layer, with shape-aware runtime dispatch) rather than the
C++-style "stamp out a struct per `(Type, [Args])` pair". The user-
visible behaviour is now correct; the perf win of unboxed primitive
payloads (`Box[Int]` packing the Int directly) is on the table for a
future deeper push.

Probe `p43_generic_method_bodies.quirk` locks arithmetic + equality
on T for both Int and String instantiations. 43/43 probes + 19
stdlib tests pass.

## [2.4.2] — 2026-06-10

### Generics: Sema type substitution at use sites (v3 phase 3-b)

Generic struct parameters are now properly threaded through member
access at compile time:

```quirk
struct Box[T] {
    value: T
    define __init(self, value: T) -> void { self.value = value }
}

define double_it(b: Box[Int]) -> Int {
    return b.value * 2          // ✓ now: T → Int, arithmetic works
}                               //   pre-v2.4.2: "'*' operands must be
                                //   numeric; got 'T' and 'Int'"
```

`checkMemberAccess` preserves the parameterized receiver type
(`Box[Int]`), extracts the type arguments, and substitutes against
the struct's `typeParams` before returning the field type. Works for
multi-param generics too (`Pair[K, V]` substitutes K and V
independently).

### Codegen bridges to keep the IR coherent

Two codegen tweaks ride along so the new Sema narrowing doesn't
introduce IR-type mismatches:

* **`i8*` vs typed `Struct*` equality** — the equality codegen
  now bitcasts the opaque `i8*` (generic-field read) to the typed
  struct pointer on the other side, so `w.value == "expected"`
  (where `w: Wrapper[String]`) flows through the existing `String___eq`
  dispatch path instead of asserting on mismatched ICmp operand types.
* **`i8* + i8*` defaults to numeric arithmetic** — when both
  operands are opaque, the string-concat path is suppressed and
  MathGen's `quirk_opaque_to_int` unbox handles both legacy tagged-
  int and Any* heap-box encodings. Real String literals always
  arrive as `%String*` (not `i8*`), so the string-concat branch
  still fires correctly for `"a" + "b"`.

### Caveat — still type-erased at the IR layer

The runtime layout for `Box[Int]` and `Box[String]` is still a
single shared struct (`value: i8*`). True per-instantiation
monomorphization (Phase 3-c) is future work; the Sema layer now
delivers correctness, but the perf win of unboxed primitive
payloads is on the table for a follow-up. Probe
`p42_generic_substitution.quirk` locks the Sema contract for Box,
Wrapper, and Pair.

42/42 probes + 19 stdlib tests pass.

## [2.4.1] — 2026-06-10

### Generic tagged unions (v3 phase 3-a)

```quirk
type Option[T]    = Some(value: T) | None()
type Result[T, E] = Ok(value: T)   | Err(error: E)
```

Closes the documented v2.4.0 limitation. The parser now accepts a
`[T, U, ...]` clause after the type name; the synthesised parent +
variant `StructNode`s inherit the type params, and Sema's existing
struct-generic scope-push picks up the payload type annotations
(`value: T`, `error: E`) at body-check time.

**Caveat — still type-erased at runtime.** Phase 3-a is parser
+ Sema only. The codegen continues to lower `T` as `Any*`, so
`Box(42)` / `Box("hello")` round-trip values opaquely. This is
sufficient for the common cases (construct, match, destructure
payload, propagate through function returns) but the type
parameter isn't substituted at use sites yet — `b: Box[Int]` will
still see `b.value` typed as `T` rather than `Int`. Phase 3-b
(Sema type substitution) and 3-c (per-instantiation Codegen) are
the next sessions.

Probe `p41_generic_tagged_unions.quirk` locks the construction +
match + destructure path against Option[T] and Result[T, E].
41/41 probes + 12 stdlib tests pass.

## [2.4.0] — 2026-06-09

### Feature: tagged unions (v3 phase 2)

```quirk
type Result = Ok(value: Int) | Err(msg: String)

define unwrap_or_zero(r: Result) -> Int {
    match r {
        case Ok as o => return o.value
        case Err     => return 0
    }
    return 0
}
```

Sum types with typed payloads. Each variant is a named constructor;
zero-payload variants use `()` (`Pending()`) to disambiguate from the
existing type-alias union (`type T = Int | String`).

**Design — desugar to struct + vtable.** Each `type T = A(...) | B(...)`
synthesizes one fieldless parent struct `T` and one child struct per
variant with `parents = {T}` and the declared payload fields. The
existing struct-inheritance + vtable machinery handles the rest:
- Variant constructors (`Ok(42)`) flow through Codegen's normal
  struct-constructor path — fields stored at GEP slots after `__type_id`.
- `match` dispatches via a direct `__type_id` ICmp (no `isinstance`
  runtime call needed for vtable-eligible types).
- `case Variant as v` narrows `v`'s LLVM type to the variant struct
  (bitcast at the bind site) so `v.payload` GEPs the right layout.

**New Sema rule — struct-inheritance compatibility.** `isCompatibleTypes`
now walks the parent chain: `Ok` (parent `Result`) is assignable to
anything declared `Result`. Drives both tagged-union assignability
and ordinary struct subtyping in the wild.

**Exhaustiveness check.** A match on a tagged-union scrutinee warns
when one or more variants are missing and no `_` wildcard is present.
Warning only — not an error — so partial matches keep compiling.

```
[WARNING] non-exhaustive match on 'Result' — missing variant: Err.
          Add an arm or a `_` wildcard.
```

**Known limitations (v2.4.0):**
- Generic variants (`Option<T>`) deferred to phase 3 (monomorphization).
- Variant methods deferred — methods live on the union, dispatched via match.
- Recursive variants (`type Tree = Leaf() | Node(value: Int, left: Tree, right: Tree)`)
  work via existing struct forward-declaration.

Probe `p40_tagged_unions.quirk` locks construction + match + narrow-
bind. 40/40 probes + 19 stdlib tests pass.

## [2.3.4] — 2026-06-09

### Wart cleanup — closures, tuples-in-lists, equality on Any

Three long-standing rough edges around boxing/unboxing fixed in one pass.

#### Int 0 (and Bool false) in a nonlocal cell read back as "null"

```quirk
counter := 0
nonlocal counter
bump := fn() => { nonlocal counter; counter = counter + 1 }
print(counter)      // before: "null"   after: 0
```

`boxToVoidPtr` encoded Int N as `IntToPtr(N)`, so `0` became the null
pointer. `quirk_opaque_to_string(null)` short-circuited to `"null"`.
Nonlocal-cell boxing now routes through
`Core_Primitives_Any_box_int` / `_box_double` / `_box_bool`
(new `VariableGen::boxForNonlocalCell`) so the cell pointer is never
null. Companion unboxers (MathGen's arithmetic auto-unbox + Codegen's
call-site auto-unbox) route through `quirk_opaque_to_int`, which
handles both the Any* and the legacy tagged-int encoding.

#### Closures didn't capture write-only nonlocal references

```quirk
seen := -1
nonlocal seen
set := fn() => { nonlocal seen; seen = 7 }    // before: silently no-op
set()
print(seen)         // before: -1   after: 7
```

The free-var collector walked `VarDeclNode::expression` (the RHS)
but never the LHS. So a closure that ASSIGNED to a nonlocal without
also READING it never picked up the capture, silently producing a
fresh local instead. Now walks the LHS for `op == "="`
reassignments.

#### `pairs.get(i).0` no longer needs a `t: Tuple :=` annotation

```quirk
pairs := [(1, "one"), (2, "two")]
a := pairs.get(0).0     // before: error 'Any' has no member '0'
                        // after:  a = 1
```

Sema now lets all-digit member names (`.0`, `.1`, …) through on Any
(returns Any). Codegen's tuple-`.N` path accepts i8* receivers and
bitcasts to `Tuple*` before calling
`Core_Collections_Tuple_Tuple___get`. The annotation workaround is
still valid, just no longer required.

#### `boxed == int_literal` no longer SIGSEGVs on tagged-int pointers

The "boxed-Any vs primitive Int" equality path called
`Core_Primitives_Any_to_int`, which derefs `.tag` on the receiver.
That works for Any* heap-boxes but SIGSEGV'd on tagged-int pointers
(e.g. `IntToPtr(10)`) where derefing pointer 10 is UB. Switched to
`quirk_opaque_to_int`, which transparently handles both encodings.

Probes `p38_nonlocal_int_zero.quirk` and
`p39_tuple_in_list_unbox.quirk` added to lock the regression.

## [2.3.3] — 2026-06-08

### Internal: virtual-dispatch state folded into `StructGen`

Continuing the locality refactor that landed `EnumGen` in v2.3.2.
The three pieces of vtable bookkeeping that Codegen.cpp owned —
`vtableEligible` (struct set), `structTypeIds` (name → runtime id),
and `overrideMap` (parent → method → [(child, id), …]) — all moved
into `StructGen`, where the struct-inheritance + dispatch story
already lives. The dispatch site, the field-0 `__type_id` prepend,
and the override-map build now read/write through StructGen's
`markVtableEligible` / `isVtableEligible` / `registerTypeId` /
`getTypeId` / `recordOverride` / `getOverrides` accessors.

The duplicate `structTypeIds` map dropped — StructGen's
`typeIdMap` was always the source of truth; Codegen's mirror was
just there because the pre-scan happened in Codegen.

No user-visible change. Same code paths, same generated IR; just
the inheritance/dispatch bookkeeping lives in one file now.

## [2.3.2] — 2026-06-08

### Internal: extracted `EnumGen` out of `Codegen.cpp`

All enum-related Codegen state — the variant registry, per-variable
enum-type map, and backed-enum value/blob metadata — moved into a
new `EnumGen` helper alongside the existing `StructGen` / `MathGen`
/ `TypeGen` / `VariableGen` / `ControlFlowGen` / `TypeExtensions` /
`BuiltinGen` cluster. EnumGen also owns the "pure" emission steps:
the per-enum packed value/name LLVM globals, the `__<Enum>_name`
C-string, and the `__<Enum>_str(i32) -> String*` ordinal-to-name
helper. Codegen still owns dispatch (handleCall / handleExpression
member-access branches) because those need too many Codegen
internals to extract cleanly.

No user-visible change. The bookkeeping refactor cuts ~120 lines of
inline enum machinery from Codegen.cpp and makes future enum work
(richer parse errors, custom traits, etc.) land in one focused file.

## [2.3.1] — 2026-06-08

### Breaking: enum accessors are methods, not properties

The six enum accessors added in v2.2.13 / v2.2.16 are now methods
(require parens). This aligns them with Quirk's existing convention
— every other parameterless accessor in the language uses parens
(`list.length()`, `set.size()`, `s.is_empty()`, `console.size()`).
The property form was an unconscious Python-ism that didn't match.

```quirk
//          v2.3.0          →   v2.3.1+
//          ------          →   --------
g.value         → g.value()
g.name          → g.name()
g.ordinal       → g.ordinal()
Gender.values   → Gender.values()
Gender.names    → Gender.names()
Gender.variants → Gender.variants()

// Unchanged:
Gender.parse(s)              // always was a method
Gender.Male                  // variant constant, no parens
```

#### Migration

Sema rejects the bare property form with a pointer at the new shape:

```
[ERROR] 'value' is a method on enum 'Gender' — write `obj.value()` (with parens)
  --> file.quirk:7:13
```

Manually: add `()` after every `.value`, `.name`, `.ordinal`,
`.values`, `.names`, `.variants` on an enum context. The error
message tells you exactly where each one is. Variant access
(`Gender.Male`) and struct-field access (`user.name` where name is
a String field) are unaffected — Sema only fires when the receiver
is an enum-typed value or an enum class.

#### Side fix: f-string + nested call interaction

While wiring the Sema rejection I had to be careful not to call
`resolveVariable` on receivers that aren't variable names (string
literals, numbers, etc). My first pass triggered "undefined variable
'\"...\"'" errors for every f-string interpolation that called a
method (`${s.upper()}` etc.) because the resolution happens after
the string-literal-receiver check. Guarded with an `isIdentLit`
predicate so only actual identifier names go to resolveVariable.

#### What stays as a property

`Gender.Male` (variant access) — still bare. Variants are constants
that resolve to i32 ordinals at compile time; there's nothing being
"called". Treating them as methods would mean `Gender.Male()` which
reads as "construct a Male"; that's not what's happening.

## [2.3.0] — 2026-06-06

### Nullable primitives + enums actually hold null at runtime

**Phase 1 of the v3.0.0 type-system overhaul.** Each "deferred to a
future release" note left through v2 around nullability is now
addressed.

#### Before

```quirk
x: Int? = null    // silently stored 0; `x == null` was false
g: Gender? = null // i32 slot can't hold null; behavior was UB-ish
Gender.parse(s)   // had to return Any because Gender? was useless
```

The `T?` annotation was syntax-only: TypeGen stripped the `?` and
the slot lowered to the base type (`i32` for Int, `i1` for Bool,
etc.). A nullable primitive couldn't actually hold null — it just
silently held the zero-value.

#### After

```quirk
x: Int? = null            // → real null pointer in an i8* slot
y: Int? = 42              // → heap Any-boxed 42
z: Int? = 0               // → distinguishable from null (was impossible before)

if x == null { ... }      // works as expected
y ?? -1                   // null-coalesce works for primitives now
```

For primitive (`Int`/`Bool`/`Double`/`Char`) and enum `T`, the
nullable form `T?` lowers to `i8*` (an opaque pointer). Codegen
auto-boxes a primitive value into a heap `Any` at assignment to a
`T?` slot. Existing reads still work because `match` / `==`-against-
null / `??` already operate on the pointer shape.

For struct types (`String?`, `List?`, `User?`, …) the base was
already a pointer, so nothing changes — those nullable cases have
been working all along.

#### `Enum.parse(v) -> Enum?` returns the proper type

The v2.2.16 workaround (returning `Any` because `Gender?` couldn't
hold null) is gone. `Gender.parse(s)` now has the correct signature.
Existing `match` / `??` patterns keep working unchanged.

#### Migration notes (breaking change for explicit `T? = null` users)

Old programs that relied on `Int? = null` *silently meaning zero*
will see a real null instead. Before:

```quirk
x: Int? = null
print(x + 1)     // pre-2.3.0: printed 1; now: SIGSEGV / runtime null deref
```

The fix is to handle the null case:

```quirk
x: Int? = null
print((x ?? 0) + 1)         // explicit coalesce
// or
if x != null { print(x + 1) }
```

If you find this pattern in your code, the compiler change exposes a
bug that was always there; the silent-zero behavior was the bug.

#### What's still ahead (rest of v3 theme)

* **Phase 2 — generics monomorphization** (`List<Int>.get(i)` returns
  `Int` directly, no Any unbox). 2–3 sessions of work.
* **Phase 3 — tagged unions** (`String | Int` with safe match
  discrimination). 1–2 sessions.
* When all three phases land, the next release bundles them as
  **v3.0.0**.

Probe `tests/probes/p37_nullable_primitives.quirk` covers seven
nullable-primitive cases including the previously-impossible
`Int? = 0` distinction.

## [2.2.16] — 2026-06-06

### Enum magic surface expanded + Double backing

Five additions and one new backing type, all orthogonal to each
other. Builds on the v2.2.4 backed enums + v2.2.13 `Enum.values`.

#### `instance.ordinal` — declaration-order index

`Gender.Male.ordinal` → `0`. At runtime an enum instance IS the i32,
so this is a pure passthrough at codegen — no helper, no allocation.
Works for all backings (String, Int, Double, unbacked).

#### `Enum.names` — variant identifiers as Strings

Always returns the variant *names* regardless of backing:

```quirk
enum Gender(String) { Male = "male", Female = "female" }
Gender.values  // ["male", "female"]      (backing values)
Gender.names   // ["Male", "Female"]      (variant identifiers)
```

For unbacked enums `.names` and `.values` are content-identical.

#### `Enum.variants` — List of variant ordinals

```quirk
for v in Gender.variants {
    ord: Int := v
    print(v.value)
}
```

Each element is a tagged-int Any holding the ordinal. Unbox with
`ord: Int := v.get(i)`.

#### `for v in EnumName` — iteration sugar

Equivalent to `for v in Gender.variants`. Recognised at Sema (item
type is Any) and Codegen (rewrites the iterable to a `.variants`
member access on the fly). Same runtime cost.

#### `EnumName.parse(v) -> Any` — safe lookup

Companion to the throwing `EnumName(v)` form. Returns `null` on
miss, an Any-boxed ordinal on hit:

```quirk
g := Gender.parse(input)
match g {
    case null { fallback() }
    case _    {
        ord: Int := g
        // use ord as a Gender ordinal
    }
}
```

Return type is `Any` rather than `Gender?` because Quirk's nullable
primitives lower to their base type at LLVM level — `Gender?` would
become i32, which has no null state. Returning Any keeps the
nullable semantics correct at the cost of one explicit unbox.

#### Double-backed enums

```quirk
enum Prices(Double) {
    Cheap = 9.99
    Mid = 49.99
    Expensive = 199.99
}

Prices(199.99)              // Prices.Expensive
Prices.Mid.value            // 49.99
Prices.values               // [9.99, 49.99, 199.99]  (List<Double>)
Prices.parse(0.0)           // null
```

Joins String and Int as a backing type. Exact double equality
applies on lookup (same FP-equality caveats as `==`).

#### Codegen side-effect: safer i8* → Int typed-walrus

While wiring `Enum.parse`, I found the typed-walrus `ord: Int := g`
path used a raw `PtrToInt` for any `i8* → int` coercion. That works
for tagged-int Any boxes (the common shape from `list.get(i)`) but
returns the heap address for *real* `Any*` heap allocations — which
is what `parse` returns. Routed through `emitUnboxToType` so both
shapes unbox correctly.

Probe `tests/probes/p36_enum_magic.quirk` covers all five additions
plus Double backing across String/Int/Double/unbacked enums.

## [2.2.15] — 2026-06-06

### `quirk compiler update --with-extension`

`compiler update` and `compiler install` now accept `--with-extension`
and `--no-extension`, threaded directly through to `install.sh`.
Saves the second `curl … install.sh | sh -s -- --with-extension`
invocation when you just want to bump the compiler and grab the
latest VSCode extension at the same time.

```
$ quirk compiler update --with-extension
$ quirk compiler install v2.2.14 --with-extension
$ quirk compiler update --global --with-extension     # inside a venv
```

Works in both code paths — the global install path and the v2.2.14
in-venv staged install path — because the extension install side-
effect in install.sh isn't scoped to `INSTALL_DIR`. The .vsix
always lands in `~/.vscode/extensions/` regardless of which compiler
target was chosen. Reuses install.sh's `CODE_CMD` lookup, so
`CODE_CMD=cursor quirk compiler update --with-extension` works for
Cursor / VSCodium.

## [2.2.14] — 2026-06-06

### Per-venv compiler versions (Python-style)

Venvs are now fully self-contained. `quirk venv <path>` writes a
**real copy** of the compiler binary, runtime.so, and the entire
stdlib into the venv instead of symlinks to the global location.
Each venv pins the compiler version it was created with; updates
to the global no longer leak in.

```
.venv/
├── bin/
│   ├── quirk           ← real ELF binary (not a symlink)
│   └── runtime.so      ← real, not a symlink
└── lib/quirk/
    ├── stdlib/         ← real per-package directories (was symlinks)
    └── site-packages/  ← user-installed packages
```

#### `quirk compiler update` is now venv-aware

```
$ quirk compiler update              # in venv  → updates venv only
$ quirk compiler update --global     # in venv  → updates global only
$ quirk compiler update              # outside  → updates global
```

Default behaviour mirrors `pkg install`: the active context wins.
A staging install runs in a temp dir first so a half-installed
toolchain can never overwrite a working venv. The global is left
untouched unless `--global` is passed.

Inside an active venv the output makes the scope explicit:

```
  ✓ venv updated  (23 stdlib package(s), bin/quirk + runtime.so)
      global compiler unchanged (use `quirk compiler update --global`
      to also update the global)
```

#### `quirk venv repair` syncs from the current global

The existing `venv repair` command now refreshes the venv's
compiler + stdlib content from whatever the global is right now.
The flow for "I want to bump this venv":

```
$ deactivate
$ quirk compiler update              # bump global to latest
$ source .venv/bin/activate
$ quirk venv repair .venv            # pull global into this venv
```

Also migrates any pre-2.2.14 symlinks the venv still has — repair
detects each symlink, removes it, and replaces with a copy from the
target. No manual cleanup needed when upgrading existing venvs.

#### Disk-cost trade-off

Each venv is now ~30 MB larger (binary + runtime + 23 stdlib
copies). The benefit is reproducibility: a venv created against a
specific compiler version stays on that version regardless of what
happens to the global. Same trade-off Python makes; matches the
"projects pin their toolchain" mental model.

## [2.2.13] — 2026-06-06

### `EnumName.values` — class-level accessor

Every enum now exposes a `.values` accessor that returns a `List`
of the backing values (or variant names for unbacked enums):

```quirk
enum Gender(String) { Male = "male", Female = "female", Other = "other" }
Gender.values  // → ["male", "female", "other"]   (List<String>)

enum Status(Int) { OK = 200, NotFound = 404 }
Status.values  // → [200, 404]                    (List<Int>)

enum Color { Red, Green, Blue }
Color.values   // → ["Red", "Green", "Blue"]      (List<String>, variant names)
```

The motivating use case — drop the triple `.value` repetition when
building a menu from an enum:

```quirk
// before
gender := prompt.select(
    "Gender?",
    [Gender.Male.value, Gender.Female.value, Gender.Other.value],
    Gender.Other.value
)

// after
gender := prompt.select("Gender?", Gender.values, Gender.Other.value)
```

Built lazily: each access calls a runtime helper
(`quirk_enum_values_str` / `_int`) that walks the per-enum packed
global emitted at codegen time. The List is GC-allocated; the
backing bytes are immutable. Same uniform interface across all
three enum shapes (String-backed, Int-backed, unbacked).

Probe `tests/probes/p35_enum_values.quirk` exercises all three.

## [2.2.12] — 2026-06-06

### CLI ergonomics: aliases, typo suggestions, fuller per-verb help

Four small improvements that don't touch the verb-first shape of the
CLI (which intentionally matches cargo / pip / npm / git / gh).

**New short aliases for high-frequency verbs:**

| Short | Long |
|---|---|
| `quirk r <file>` | `quirk run <file>` |
| `quirk t [<file>...]` | `quirk test` |
| `quirk h [<cmd>]` | `quirk help` |

Joins the existing `i`/`add` (install), `up` (upgrade), `rm`/`un`
(remove), `ls` (list). `-h` / `--help` / `--version` are also
normalised through the same path so they work in every position.

**Typo suggestions** at both the top level and inside `quirk pkg`:

```
$ quirk insatll
quirk: unknown command 'insatll'
    did you mean `quirk install`?

$ quirk pkg insatll
pkg: unknown subcommand 'insatll'
    did you mean `quirk pkg install`?
```

Same edit-distance cutoffs Sema uses for identifier suggestions
(1 edit for short verbs, 2 for longer ones). Genuine file paths
(anything with a `/` or `.`) bypass the suggestion path so
`quirk somefile.quirk` keeps working unchanged.

**`quirk help <verb>` now covers every verb.** Previously `help`
worked for the package verbs but silently fell back to the top-
level summary for `compiler`, `auth`, `test`, `completion`, and
`resolve`. Added per-verb help text for all five.

**Top-level help layout tidied:** the `RUN CODE` and `MISC` blocks
now list the new short aliases inline, and a small `TIPS` block at
the bottom points at `quirk help <cmd>`, typo suggestions, and the
global `--verbose`/`--quiet`/`-h` flags.

No CLI shape changes. Existing scripts and muscle memory are
unaffected.

## [2.2.11] — 2026-06-06

### Bugproofing pass: 34-probe sweep, 8 distinct bugs closed

Sat down with a structured adversarial probe suite (`tests/probes/`,
34 minimal programs) that exercises the corners where compiler /
runtime bugs typically hide: Any-laundering, division by zero,
out-of-bounds, recursion depth, unicode, closure-over-loop,
self-referential structs, comparison across types, etc. First pass
caught **8 distinct bugs across 5 categories**; all eight now
produce correct output, a clean Quirk exception, or a clean Sema
rejection.

#### Sema: cross-type `==` / `!=` / arithmetic no longer aborts LLVM

`5 == "5"` used to fall through Sema (returns `Bool` unconditionally),
hit Codegen's `ICmp(i32, String*)`, and **abort the LLVM verifier** —
no traceback, no chance to catch. Same hazard for `enum + Int` and
any other cross-type arithmetic. Added a type-compatibility gate at
the top of `checkBinaryOp`:

```
operator '==' incompatible types: 'Int' and 'String'
  --> main.quirk:3:10
```

Primitives (`Int`/`Double`/`Bool`/`Char`/`String`) that happen to be
registered as structs no longer trigger the operator-overload bypass
— the gate now probes whether the overload method actually exists
before trusting it. Enums are intentionally rejected from arithmetic
(use `.value` if you want the backing int).

#### Codegen: `i / 0` and `i % 0` throw `ZeroDivisionError`

LLVM `sdiv` / `srem` on zero is *undefined behavior* — at -O2 the
optimizer assumes a non-zero divisor and the result is arbitrary
bits. Probes saw `10 / 0` returning `-1736491003`. Codegen now emits
a guard before each integer divide: zero ⇒ `throw
ZeroDivisionError("integer division by zero")`. Doubles are
unaffected (IEEE 754 defines `x/0` cleanly).

#### Codegen: `h.n = items.get(0)` (Any → typed field) no longer ICEs

Field-assignment codegen had branches for `int→int`, `ptr→ptr`,
`int→ptr`, `int→double`, but not `i8*(Any-boxed) → int/double`. So
assigning the result of `list.get(i)` (which returns `i8*`) into a
typed field used to emit malformed IR (`store i8* %x, i32* %y`).
The new branch routes through `emitUnboxToType`, which now uses
runtime helpers (`quirk_opaque_to_int` / `_to_double`) that handle
all three opaque shapes safely instead of assuming a heap-allocated
`Any*`.

#### Codegen: Any-launder into Set / Queue / File / user struct

Twin of the v2.2.7 (String) and v2.2.10 (List/Map/Tuple/Callable)
fixes, generalized to every struct type without a dedicated
`AnyTag`. New runtime helper `quirk_opaque_check_struct_or_null`
rejects the two obviously-wrong opaque shapes (tagged int, `Any*`
heap wrap) at the arg boundary with a clean `TypeError`, instead of
the raw-bitcast → SIGSEGV-on-first-method-dispatch path. Together
with v2.2.10 this closes the Any-launder crash class **across all
struct types**, not just the four that had explicit tags.

```
TypeError: expected Set but got Int (laundered through Any-typed parameter)
TypeError: expected Box but got Int (laundered through Any-typed parameter)
```

### Package layout: Python-strict (`site-packages/`)

Aligned the venv layout with Python's convention:

```
<venv>/lib/quirk/
├── stdlib/                (symlinks → ~/.quirk/packages/, opaque to pkg)
└── site-packages/         (Python-conventional name, flat, dist-info siblings)
```

- New installs land in `site-packages/`. Old `packages/` stays
  accepted by the resolver until `quirk venv repair` migrates it
  (rename + non-stdlib entries moved across).
- `quirk pkg install <stdlib-name>` is now a **hard error**, no
  short-circuit-with-cleanup machinery. Stdlib packages are exposed
  only through the `stdlib/` symlink, and the resolver order
  (site-packages first, then stdlib) mirrors Python — third-party
  installs override the stdlib by *name choice*, not by accidental
  same-name shadowing.
- `venv repair` sweeps stale stdlib copies from both old and new
  dirs and migrates remaining user packages.

### Probe suite kept as regression tests

The 34 `tests/probes/*.quirk` programs and a `README.md` are now
part of the repo. Each one documents the bug class it protects
against. Anyone regressing the fixes above will see the
corresponding probe fail in CI.

## [2.2.10] — 2026-06-05

### Codegen: Any → List/Map/Tuple/Callable arg coercion is now type-safe

Twin of the v2.2.7 String fix, applied to the other collection types
that had the same latent crash hazard: an `Any`-laundered non-pointer
value (a tagged-int from `Int.parse("...")` flowing through an
untyped parameter, say) hitting a `: List` / `: Map` / `: Tuple` /
`: Callable` slot used to get raw-bitcast straight to the struct
pointer type and SIGSEGV at the first method dispatch.

Repro that crashed before this release:

```quirk
define take_list(lst: List) -> Int { return lst.length() }
define wrap(x)                      { return take_list(x) }
define main() -> void               { print(wrap(42)) }   // crash
```

Now produces a clean exception instead:

```
TypeError: expected List but got Int (laundered through Any-typed parameter)
```

Implementation:
- New runtime helper `quirk_opaque_unwrap_or_null(val, expected_tag,
  type_name)` — same heuristic as `quirk_opaque_to_string` but
  parameterised by `AnyTag`. Throws `TypeError` on mismatch (tagged
  int into struct slot, or `Any*` with wrong tag); returns the
  pointer unchanged on the common direct-struct-ptr path; preserves
  null so callers' `lst == null` guards keep working.
- `processCallArgs` at the `i8* → struct*` site routes through this
  helper for `List`/`Map`/`Tuple`/`Callable`. `String` stays on
  `quirk_opaque_to_string_or_null` (added in 2.2.9). Unknown struct
  types fall back to the existing bitcast.

Doesn't yet cover `Set`/`Queue`/`File` (no `ANY_*` tag) or
user-defined structs. Same hazard but hasn't been hit in practice;
fixing it would need per-struct tag assignments which is a larger
change.

## [2.2.9] — 2026-06-05

### Codegen: arg-coercion preserves null pointers

Follow-up to 2.2.7. The `i8* → String*` arg coercion routes through
`quirk_opaque_to_string` to handle Any-laundered values safely. The
helper's stringification semantics turn a literal null into the
4-char string `"null"`, which is correct for `print(null)` and
collection display, but wrong at function-argument boundaries: any
callee that defends with `s != null` then sees a non-null String
of length 4, and the guard silently passes through.

Concretely: `prompt.input("Name?", null)` (or any wrapper that
forwards a null through an `Any`-typed parameter) was rendering as
`Name? [null]: ` — the v1.0.3 `default != null and default.length()
> 0` guard in the package thought it had a real default.

Fix: added a sibling runtime helper `quirk_opaque_to_string_or_null`
that returns the null pointer for null input and otherwise behaves
identically. Codegen's arg-coercion site uses the new helper;
stringification callers (print / debug / collection display) stay
on the original.

The v2.2.2 return-path null propagation is unaffected (it already
short-circuits on `isa<ConstantPointerNull>` before reaching the
unbox helper).

## [2.2.8] — 2026-06-05

### `quirk pkg install <stdlib-name>` no longer shadows the stdlib symlink

Inside a venv the resolver order is:

```
./packages/                      # project local
<venv>/lib/quirk/packages/       # venv-installed (real copies)
<venv>/lib/quirk/stdlib/         # symlink → ~/.quirk/packages/<name>
…
```

`quirk venv` populates `stdlib/` with symlinks to the user-global
stdlib, which means `quirk compiler update` propagates to every
venv automatically. But before this release, `quirk pkg install
<stdlib-name>` (with no version pin) wrote a frozen copy under
`packages/`, which sits *above* the symlink in the resolver order
and silently kept the venv on whatever version was current at
install time — even after the global got updated. That's how the
user-reported scenario surfaced: a venv created on day 1, an old
stale `prompt` copy from a `pkg install prompt`, then days of
compiler/stdlib updates that never reached the program.

Three changes:

1. **`pkg install <stdlib-name>` short-circuits in a venv** when no
   version pin is provided. Prints "<name> is bundled with the
   stdlib", points at the symlink, and tells the user how to pin
   anyway if that's actually what they want.

2. **The short-circuit also cleans up any existing stale copy** in
   `<venv>/lib/quirk/packages/<name>`. Re-running `pkg install
   <stdlib-name>` is now the easy way to recover from a previously-
   broken venv.

3. **`quirk venv repair` sweeps stale stdlib copies** out of
   `<venv>/lib/quirk/packages/` for every name in
   `stdlib_registry()`. Reports the count alongside the new
   stdlib-link tally. User packages in the same directory are
   preserved.

Pinned installs (`quirk pkg install prompt@1.0.2`) are honored
unchanged — the user is explicitly choosing to deviate from the
bundled stdlib, the lockfile records the pin, and subsequent bare
installs go through the lockfile fast-path as before.

Bug fixed along the way: the short-circuit used to write an empty
`LockEntry`, which `cmd_install` then persisted as `[[package]]
name = ""` in `quirk.lock`. The next bare install routed through
the lockfile fast-path with the empty version, producing the
malformed `pkg@` spec and re-triggering the short-circuit by
accident. Now the short-circuit signals "no install happened" via
an empty `e.name`, and the caller filters that case before touching
the lock.

## [2.2.7] — 2026-06-05

### Codegen: `Any → String*` arg coercion no longer hands back a bogus pointer

`processCallArgs` widened the `i8* → SomeStruct*` arg coercion in
an earlier change so `Callable`/`Map`/`List` values flowing through
`Any`-typed slots wouldn't trip the LLVM verifier. The widening was
fine for those types — their `i8*` IS a real struct pointer cast
down. But for `String*`, the `i8*` may also be:

- an Any-laundered **tagged-int** (`inttoptr small_int`, used to box
  enum ordinals and small ints) — bitcasting it produces a `String*`
  pointing at the tag value as a literal memory address;
- an Any-laundered **Any\*** heap wrapper — bitcasting reinterprets
  the Any struct's first field as the String buffer pointer.

Both crash on the next `.length()` / `.buffer` access. The user-
reported repro: a helper with an untyped `default` parameter passing
`Gender.Other` (ordinal `2`) to `prompt.input(..., default: String)`
landed an invalid `String*` at address `0x2`, and the first
`default.length()` SIGSEGV'd.

Fix at [Codegen.cpp:2269](src/Backend/Codegen.cpp#L2269): when the
expected param type is `String*` specifically, route through
`quirk_opaque_to_string` — the same runtime helper that already
handles all three opaque shapes (tagged int, `Any*`, `String*`) and
hands back a real `String*` every time. Other struct targets keep
the existing bitcast; their misuse pattern is the same hazard but
needs separate per-type runtime helpers, which is out of scope here.

This doesn't validate the *semantic* correctness of e.g. passing
`Gender.Other` where a `String` is expected — it just stops the
crash. The cleanest fix on the source side is to type the helper's
`default` parameter as `String` explicitly; with the 2.2.6 arg-type
check, Sema then rejects the enum-into-string call at compile time.

## [2.2.6] — 2026-06-05

### Sema: tighten null compatibility + extend arg-type checks to function calls

Two adjacent cleanups that mirror the 2.2.3 enum-compat tighten.

#### `isCompatibleTypes("X", "Null")` is no longer a free pass

Before, `null` was compatible with *every* type:

```cpp
if (expected == "Null" || actual == "Null") return true;
```

So `User(name, age, null)` for `age: Int` flowed straight through
Sema, even with the 2.2.2 ctor type check active. Now null is gated:

- **Allowed**: nullable annotations (`T?`), `Any`, structs and other
  reference types (List/Map/Set/Queue/Tuple/Callable/File). These
  lower to pointers at the IR level and can hold null at runtime.
- **Rejected**: primitives (`Int`, `Double`, `Bool`, `Char`) — no
  null state in their representation.
- **Rejected**: enums — they lower to i32 ordinals, also no null
  state. Use `Enum?` if you genuinely need null.

```
[ERROR] argument 2 of User() expected 'Int' but got 'Null'
 --> main.quirk:10:14
```

#### Sema now type-checks positional args at literal-callee function calls

The 2.2.2 type check was wired into struct constructors and member-
method calls but not into plain `foo(a, b, c)` calls. Now it is —
same `isCompatibleTypes` rule, same error shape. Catches null →
Int/enum and any other type mismatch at the call site that would
have surfaced later as a malformed-IR crash or a runtime SIGSEGV.

```
[ERROR] argument 1 of f() expected 'Color' but got 'Null'
 --> main.quirk:8:5
```

#### What this *doesn't* catch

Values laundered through `Any` (or any untyped parameter) still pass
through Sema, because `Any` is the explicit escape hatch and tightening
it would break existing stdlib patterns. So a chain like
`confirm_input(m, f, default)` with an untyped `default` will still
let null reach `prompt.input` — that's why `quirk-prompt@v1.0.3`
added runtime null-tolerance to `prompt.input`. Tightening `Any` is
a much bigger change tracked separately.

## [2.2.5] — 2026-06-05

### Stdlib: bundle `quirk-prompt@v1.0.3`

Patch release of the bundled prompt package. `prompt.input(message,
default)` now treats a null `default` argument the same way it
treats an absent one (empty → re-prompt loop) instead of crashing
with SIGSEGV on the first `default.length()` call.

The trip-up: callers that route the default through an untyped /
Any-typed parameter chain (e.g. `confirm_input(m, f, default)` then
`prompt.input(m, default)`) silently pass null where the String
signature would suggest a real value. The compiler's existing
type-erasure rules let this through Sema (Null is "compatible" with
everything as far as Sema is concerned). The package now defends
itself; the broader Sema cleanup is tracked separately.

No compiler-side code changed in this release — just the
`STDLIB_TAG_prompt` Makefile pin moves to `v1.0.3` so the release
tarball ships the patched package.

## [2.2.4] — 2026-06-05

### Backed enums (Python-style)

Enums can now declare a backing type and per-variant values, and the
enum itself becomes callable for value→variant lookup:

```quirk
enum Gender(String) {
    Male
    Female = "F"
    Other
}

enum Status(Int) {
    OK = 200
    NotFound = 404
    ServerError = 500
}

g := Gender("F")          // → Gender.Female
g.value                   // → "F"
Gender.Other.value        // → "Other"

s := Status(404)          // → Status.NotFound
s.value                   // → 404

Gender("nope")            // → throws ValueError
```

- Defaults: a variant without `= ...` uses the variant name as-written
  for String backing, or its ordinal index for Int backing.
- `EnumName(value)` is a value→variant lookup. On miss, it raises
  `ValueError("'<value>' is not a valid <EnumName>")` — use try/catch
  for null-on-miss behaviour.
- `instance.value` returns the backing value. Works for plain bindings
  (`g.value`), chained variant access (`Gender.Other.value`), and
  struct-field access (`response.code.value`).
- Existing unbacked enums (`enum X { A, B, C }`) are unaffected — the
  parentheses are optional and the old ordinal-only shape stays as-is.

Implementation surface:
- AST: `EnumNode` gains `backingType` and a parallel `variantValues`.
- Parser: `enum Name(T) { V1, V2 = literal, ... }`.
- Sema: `EnumName(value)` is recognized at the literal-callee path
  (alongside struct constructors); `.value` is recognized at member
  access for any backed enum.
- Runtime: four helpers — `quirk_enum_lookup_str`, `_lookup_int`,
  `_value_str`, `_value_int` — walk a packed values blob emitted as
  a global per backed enum. No per-call setup at the call site.
- Codegen: one global blob + one name string per backed enum, both
  marked `unnamed_addr` so the linker can merge identical blobs
  across translation units if it wants.

Not yet: `EnumName.parse(value) -> EnumName?`. Quirk's nullable
primitives don't have a runtime null state today (`Int?` and
`Gender?` lower to the underlying primitive), so a clean null-return
parse would need a separate boxing path. Wrapping `EnumName(...)`
in try/catch covers the same shape for now.

## [2.2.3] — 2026-06-05

### Sema: enum compatibility is no longer a free pass

`isCompatibleTypes` short-circuited to **true** whenever either side
was an enum:

```cpp
if (enumRegistry.count(expected) || enumRegistry.count(actual))
    return true;
```

So `User(name, age, gender)` where `gender: Gender` got a `String`
was waved through Sema — including by the new ctor type check added
in 2.2.2 — and only blew up at the LLVM verifier:

```
Call parameter type does not match function signature!
  call void @User__init(%User*, %String*, i32, %String*)
                                                ^^^^^^^^ expected i32
```

Tightened to: an enum is compatible with itself (already covered by
the `==` short-circuit above), with `Int`/`int` (since enums lower
to `i32` and the cast goes both ways), with `Any`, and with `Null`.
Anything else against an enum is a real mismatch.

Caught now at the Sema layer:

```
[ERROR] argument 3 of User() expected 'Gender' but got 'String'
 --> main.quirk:59:17
```

The full stdlib + per-file regression sweep stays green — the loose
rule turned out to only hide bugs, not enable any legitimate use.

## [2.2.2] — 2026-06-04

### Better diagnostics + `return null` actually returns null

Three follow-on fixes after 2.2.1 — all surfaced while a user wrote a
program against the new `prompt` package.

#### Sema: type-check positional struct constructors

Calling `User(name, age, gender)` with the wrong arg types used to
fall through Sema and surface as `Internal compiler error: malformed
IR` at codegen. Now Sema reports it cleanly:

```
[ERROR] argument 2 of User() expected 'Int' but got 'String'
 --> main.quirk:37:17
```

Wired into both `ConstructorNode` (named-field form) and `CallNode`
when the literal callee resolves to a known struct. The check piggybacks
on the existing `isCompatibleTypes` helper, so the rules match
ordinary method-arg checking (Any/Null/enum widening all preserved).

#### Sema: live return-type lookup for inferred-return functions

Pass 1 freezes each function's scope binding to whatever return type
was parsed (the FunctionNode default is `"void"` when no `-> T`).
Pass 2 later infers the real return type from `return` statements
and writes it back to the FunctionNode, but the **scope binding was
never updated**. So at the call site, `name := infer_returning_fn()`
saw `name: void` even though the function actually returned `String`,
breaking the new struct-ctor type check (and quietly degrading every
other arg-type check that relied on `vtype`).

Fix in `checkCall`: when the callee is a literal naming a registered
function, read `methodRegistry[""][name]->returnType` instead of the
stale scope binding. The FunctionNode is the source of truth; the
scope binding is now just a fallback for non-function values.

#### Codegen: `return null` no longer becomes the literal string `"null"`

The 2.2.1 return-unbox fix routed `i8* → String*` returns through
`quirk_opaque_to_string`. That helper translates a null pointer to
`make_String("null")` — the 4-char string `"null"` — which is right
for stringification contexts (`print(null)`) but wrong for actual
returns: `define try() -> String? { return null }` made the caller
see a non-null `"null"` string and silently miss the `case null` arm.

Fix in the ReturnNode handler: check `isa<ConstantPointerNull>`
**before** the String-unbox branch. Null propagates as a null pointer
cast to the expected struct type; non-null `i8*` values still route
through `quirk_opaque_to_string` as before.

#### Stdlib: `prompt.input_optional`

Pairs with the null-return fix. Returns `String?` — `null` on empty
reply, `String` otherwise. Bundled as `quirk-prompt@v1.0.2`.

```quirk
name := prompt.input_optional("Your name")
match name {
    case null => print("(skipped)")
    case _    => print("hi, " + name)
}
```

## [2.2.1] — 2026-06-04

### Two boxing/scoping bugs fixed — `ask` is now `prompt`

Two warts surfaced while building the [2.2.0](#220--2026-06-04) `ask`
package made the `prompt` name unusable and forced a typed-walrus
workaround on every `select`/`multiselect` callsite. Both are now
fixed at the compiler level, the package is renamed back to its
natural name, and the workaround is gone.

#### 1. `use X` no longer shadows local parameters

Before: `use prompt` in any file made `prompt` route as a module
alias **everywhere** — including inside `console.input(prompt: ...)`,
where `prompt.length()` would then look up `prompt`-the-module
instead of `prompt`-the-parameter and die with `Unknown function
'length'`. Codegen's `handleCall` was checking module aliases before
local-variable scope.

Fix at `quirk-compiler/src/Backend/Codegen.cpp` ~line 1515: defer to
local scope first; the module-alias branch only fires when the
identifier does not name a live local.

```cpp
bool litIsLocal = varGen->exists(lit->value);
if (!litIsLocal && activeModuleAliases.count(lit->value)) { ... }
```

#### 2. `return list.get(i)` from a `-> String` slot is no longer garbled

Before: `List.get` (and any other `void*`-returning callsite) hands
back an `i8*` that may be a `String*`, an `Any*`, or a tagged int.
When that `i8*` was returned into a function whose declared return
type was `String`, codegen wrapped it as if the pointer were a raw
`char*` — wrapping the *bytes of the String struct pointer* into a
new String. Output was mojibake. The standing workaround was a
typed-walrus annotation (`chosen: String := list.get(i); return
chosen`) which routed through the same unbox helper that already
existed for var-decls.

Fix at `quirk-compiler/src/Backend/Codegen.cpp` ~line 2390: at the
ReturnNode handler, when the source is `i8*` and the destination is
`String*`, route through `quirk_opaque_to_string` (the runtime
helper that already handles all three shapes) instead of
`make_String`. String literals were never affected — they reach the
return site already typed as `String*`, so the regression surface
is empty.

#### Stdlib: `ask` → `prompt`

With (1) fixed, the natural name is usable again:

- `stdlib_registry()` swaps `ask → quirk-ask` for `prompt → quirk-prompt`.
- `Makefile`'s `STDLIB_PACKAGES` swaps `ask` for `prompt`.
- The package source drops the typed-walrus workarounds in
  `select` / `multiselect`; direct `return options.get(i)` works.

```quirk
use prompt

name := prompt.input("Your name", "Anonymous")
mode := prompt.select("Mode?", ["fast", "thorough", "debug"])
```

The `quirk-ask` v1.0.0 repo stays online as a historical artifact;
the v2.2.1 tarball ships `prompt` only.

## [2.2.0] — 2026-06-04

### Stdlib gains `ask` — interactive CLI prompts

[`AlexVachon/quirk-ask`](https://github.com/AlexVachon/quirk-ask) `v1.0.0`
joins the bundled stdlib. Pairs with `argparse` for runtime questions
the user didn't pre-answer on the command line.

```quirk
use ask

name := ask.input("Your name", "Anonymous")
pw   := ask.password("Password: ")
go   := ask.confirm("Continue?", true)
mode := ask.select("Mode?", ["fast", "thorough", "debug"])
tags := ask.multiselect("Tags?", ["urgent", "blocked", "needs-review"])
```

- Added to `stdlib_registry()` (bare-name install works).
- Added to `STDLIB_PACKAGES` in the Makefile (release tarball bundles
  it; `use ask` works on fresh `install.sh` users with no extra step).

### Why not `prompt`?

The natural name `prompt` collides with `prompt: String` parameters
inside the `console` package. Quirk's current compiler treats `use X`
as a workspace-global declaration (rather than scoping it per-file),
so `use prompt` in any file makes `prompt` look like a module
reference everywhere — including inside `console.input`'s body where
`prompt` is a local parameter. Sema then dies on `prompt.length()`.

The `ask` name avoids the collision and ships the same surface. When
the compiler's import scoping bug is fixed, both names could
co-exist, but renaming is the right call for now.

### Known caveat in `ask.select` / `multiselect`

Both use numbered menus (user types `1`, `2`, …) rather than
arrow-key navigation. Arrow keys would need a raw-mode `getch()`
primitive that the stdlib doesn't yet expose; the public signatures
will stay the same when that primitive lands and `ask` switches to
arrow keys under the hood.

## [2.1.1] — 2026-06-04

### `toml` is now bundled

Promotes `toml` from "registered, install-on-demand" to "bundled with
every release tarball" — same tier as the original 21 stdlib packages.

- Added to `STDLIB_PACKAGES` in `quirk-compiler/Makefile` so
  `make bootstrap-stdlib` (run by the release workflow) pulls
  `github.com/AlexVachon/quirk-toml@v1.0.0` into the tarball.
- The repo also gets a `v1.0.0` tag pointing at the same commit as
  `v0.1.0`, to match the stdlib tagging convention.

After the next release ships, fresh `install.sh` users will have
`use toml` working out of the box without any `pkg install` step.

## [2.1.0] — 2026-06-04

### Stdlib registry adds `toml`

[`AlexVachon/quirk-toml`](https://github.com/AlexVachon/quirk-toml) `v0.1.0`
joins the compiler-shipped registry. Bare-name `quirk pkg install toml`
now resolves to the canonical repo without `pkg registry add` first.

The package is a pure-Quirk parser covering the subset used by Quirk's
own `quirk.toml` / `quirk.lock`: top-level pairs, `[sections]`,
`[[array-of-tables]]`, basic + literal strings, integers (with `_`
separators), booleans, one-line arrays, comments. Returns the top-level
table as a `Map`; errors raise `ValueError` annotated with the offending
line number.

Why this is a minor bump (`2.1.0`) instead of a patch: adding a name
to `stdlib_registry()` extends the compiler's public surface (any script
can now write `use toml` after `quirk pkg install toml` with no user
config). Future stdlib package additions will land the same way.

## [2.0.3] — 2026-06-04

### Dead-code sweep

A focused scan turned up almost nothing — every static function is
called, no commented-out blocks, no leftover code from reverted
experiments (bit-48 boxing in particular was confirmed gone). What
did surface:

- `test_output.txt` was tracked at the repo root. The
  `sys_test.quirk` test writes a relative `test_output.txt` from
  whichever directory it runs in, so both `/test_output.txt` and
  `quirk-compiler/test_output.txt` are accidental commits. Removed
  the tracked copy; gitignore now covers both paths.
- `STDLIB.md` linked to `quirk-compiler/libs/typing/` — the v1.0.7
  layout that 1.0.8's `libs/` → `packages/` rename obsoleted.
  Updated to `quirk-compiler/packages/typing/`.

What was scanned but kept as intentional:
- `libs/` fallback paths in the compiler's import resolver
  (documented backwards-compat for pre-1.0.8 installs).
- `REPLACE_ME` OAuth client_id placeholder (tracked, waiting on the
  user to register the GitHub OAuth App).
- The bitcode-cache LRU-eviction TODO (non-blocking; old entries
  are stale-safe, just take disk space).

Compiler binary byte-identical to 2.0.2.

## [2.0.2] — 2026-06-04

### Stdlib no longer ships in the compiler repo

The 21 stdlib packages used to live in two places: the source-tree
`quirk-compiler/packages/` AND the 21 canonical
`github.com/AlexVachon/quirk-<name>` repos. As of 2.0.2 only the
GitHub repos are tracked — the source tree's copy is now a build
artifact, populated on demand and excluded from git.

### What changed

- **New `make bootstrap-stdlib` target.** Clones each of the 21
  stdlib repos at `v1.0.0` into `quirk-compiler/packages/`.
  Idempotent (skips packages that already have an `index.quirk`),
  ~30s on a typical connection.
- **`setup.sh`** now runs `make bootstrap-stdlib` before the build
  so source-build users keep working out of the box.
- **`.github/workflows/release.yml`** runs the same target on both
  Linux and macOS jobs before packaging — the release tarball is
  byte-equivalent to what 2.0.1 shipped.
- **`quirk-compiler/packages/`** is now in `.gitignore`. The
  directory is no longer in git's index.

### Why

- Single source of truth: editing argparse means pushing to
  `quirk-argparse`, not duplicating the change in two places.
- Smaller repo: ~28 stdlib files no longer track in git history.
- Cleaner mental model: "stdlib lives in repos; compiler bundles them
  for distribution" matches how every other package works.

### Compatibility

`install.sh` users see no difference — the release tarball still
ships a populated `packages/` (built by the workflow). Existing
clones with a populated `packages/` keep working; git considers it
deleted but the files stay on disk (since they're now ignored).

Fresh contributors must run `make bootstrap-stdlib` (or `setup.sh`,
which calls it) before their first build.

## [2.0.1] — 2026-06-04

### Fix: `quirk run` mistook flags for script names

`quirk run --emit-ast foo.quirk` (and similar with `--check`,
`--debug`, `-v`, …) bailed out with `script: no quirk.toml here`.
The dispatch was reading `argv[2]` as the target; when that was a
flag like `--emit-ast`, it didn't exist on disk and had no dot or
slash, so it fell into the `[scripts]` lookup branch — which then
required a `quirk.toml`.

Fix: skip leading flags in the `run` dispatcher to find the first
positional argument, and use *that* as the candidate target. Bare
`quirk run` and the `--list` shortcut keep their existing semantics.

Compiler binary is otherwise byte-identical to 2.0.0.

## [2.0.0] — 2026-06-04

### AOT execution model — `quirk build` + cached native binaries

This is the first major version bump. Quirk's default execution flow
changes: instead of JIT-compiling every invocation, the compiler now
checks for a precompiled native binary cached at
`~/.quirk/cache/build/<sha>-<arch>/quirk-bin`. When the cache hits,
`execv` replaces this process with the binary — typical warm-run
startup drops from ~80ms (JIT cold) to ~25ms (process startup +
dlopen). When the cache misses, execution falls through to the
existing JIT path (1.x behavior); nothing breaks.

### New subcommand

- **`quirk build <file>`** runs Codegen + `llc-14` + `gcc` and stashes
  the resulting binary at the cache key. Idempotent — re-running on
  an unchanged source is a no-op. The cache key shares the bitcode
  cache's content+imports hash, plus the host arch so a Linux x86_64
  binary doesn't get exec'd on macOS arm64 (or vice versa).

### New flags / env vars

- `--build` — alias for `quirk build <file>` (same flow, single CLI).
- `--no-aot` — skip the AOT cache lookup for this invocation, force
  the JIT path. Useful for `quirk run --debug` or when debugging
  Codegen.
- `QUIRK_NO_AOT=1` — same kill switch, env-var form.

### Behavior summary

| Command | Before (1.7.x) | After (2.0.0) |
|--|--|--|
| `quirk file.quirk` | JIT every time | Cache hit: exec binary. Miss: JIT (same as 1.7.x). |
| `quirk build file.quirk` | (didn't exist) | Build to cache; no run. |
| `quirk -o out file.quirk` | One-shot AOT | One-shot AOT (unchanged). |
| `quirk --debug file.quirk` | JIT with stepper | JIT with stepper (cache skipped). |

### Why a major bump

Quirk now defaults to a "compile-once, run-many" execution model. The
old "JIT each run" can still be opted into via `--no-aot`, but the
recommended workflow is `quirk build my_script.quirk` followed by
fast `quirk my_script.quirk` invocations. Scripts that pipe through
`quirk --debug` or `quirk --check` are unaffected.

### Known limitations

- The build pipeline still shells out to `llc-14` and `gcc`. Users
  without those on PATH get a clean "build failed" message; `--no-aot`
  works around it without effort.
- `quirk build` doesn't yet recurse into a project's `quirk.toml` for
  multi-entry builds; that's the next thing on the v2.x todo list.
- macOS arm64 path is theoretical (the bin is built CI-only in v1.3+);
  if the cached binary architecture mismatches the runtime arch, the
  cache key's `-arch` suffix ensures we never cross-execv.

## [1.7.3] — 2026-06-04

### `quirk-lsp` 0.18.0 — document links for imports

`textDocument/documentLink` surfaces `use X` and `from X use { … }`
lines as clickable hyperlinks. The link range covers just the module
name; the target is the resolved file (`quirk resolve X`). Resolution
goes through the per-session cache the LSP already maintains for
go-to-definition, so links don't cost a fresh compiler spawn.

Compiler binary byte-identical to 1.7.2 modulo the version constant.

## [1.7.2] — 2026-06-04

### "Did you mean … ?" diagnostics + LSP quick fixes

Compiler:
- New `Sema::suggestNames(query, N=3)` — Levenshtein over every
  in-scope name (locals, params, globals, structs, enums,
  interfaces, methods, module constants). Cutoff scales with name
  length so 3-char typos don't get 2-edit "matches".
- Undefined-identifier errors now carry top-3 candidates. Human-
  readable rendering adds a `hint: did you mean \`X\`?` line;
  `--diagnostics-json` emits a `suggestions` array on the record.

quirk-lsp 0.17.0:
- `textDocument/codeAction` returns one `QuickFix` per suggestion,
  with the closest match marked `isPreferred` (single-keystroke
  default-fix in most editors).
- Suggestions ride along on each diagnostic's `data` field, so the
  code-action handler can pull them out without re-running the
  compiler.

Round-trip verified: `print(gret("world"))` surfaces
`undefined variable or function 'gret'` with `suggestions:["greet"]`,
and the LSP exposes a `Replace with 'greet'` quick fix.

## [1.7.1] — 2026-06-04

### `quirk-lsp` 0.16.0 — call hierarchy

Three new LSP requests:

- `callHierarchy/prepareCallHierarchy` — given a cursor position,
  return the function/method represented there.
- `callHierarchy/incomingCalls` — given an item, list every caller
  (each is a `usage` record of the item's name; the caller is the
  function whose decl matches `usage.scope`).
- `callHierarchy/outgoingCalls` — given an item, list every callee
  (each is a `usage` record with `scope == item.name`).

Built on the 1.7.0 usage table — no text walks; scope-precise. The
editor's "Show Call Hierarchy" panel can now walk arbitrary depth
through Quirk code.

Compiler binary byte-identical to 1.7.0 modulo the version constant.

## [1.7.0] — 2026-06-04

### Per-usage tracking in Sema; semantic LSP references and rename

Sema now records every successful identifier resolution into a
`usages` table — one entry per `resolveVariable` call. Each entry
has `(name, scope, file, line, col)` where `scope` mirrors the
declaration-side records (`"module"`, a struct name, or an
enclosing function's demangled name). The table is exposed through
`--symbols-json` as `kind:"usage"` records that interleave with
the existing decl records.

This was the missing piece for two LSP features that previously
fell back to text-only walks:

- **`textDocument/references`** now returns Sema's exact answer.
  A parameter `x` in function A doesn't show up when listing refs
  of an unrelated local `x` in function B.
- **`textDocument/rename`** uses the same data for precise, scope-
  respecting edits — no more "rename the wrong `foo`" risk.

Both still text-search per matched line for the exact identifier
column, since Sema's usage records use the enclosing-expression
start (Parameter doesn't carry its own line/col yet). For files
Sema didn't see this session, both features gracefully fall back to
the v1.6.x text-based walker.

This is a minor bump (`1.7.0`) — every change is additive. Existing
clients of `--symbols-json` see new records they didn't before and
should drop unfamiliar `kind`s the way the spec asks.

## [1.6.13] — 2026-06-03

### `quirk-lsp` 0.14.0 — document highlights

`textDocument/documentHighlight` — cursor on an identifier highlights
every other occurrence in the current file. Decl sites render as
`Write`, other uses as `Read`. Most editors render this as a subtle
background tint.

Compiler binary byte-identical to 1.6.12 modulo the version constant.

## [1.6.12] — 2026-06-03

### `--symbols-json` learns inferred types + `quirk-lsp` 0.13 inlay hints

Compiler:
- `VarDeclNode` gains a new `inferredType` field. `Sema::checkVarDecl`
  fills it with the RHS expression's type when the user didn't write
  an explicit `typeAnnotation`. The original `typeAnnotation` stays
  untouched so the AST still reflects the source.
- `--symbols-json` prefers `typeAnnotation` when present, falls back
  to `inferredType`. Result: `variable` records now carry a `type`
  even for bare `x := value` declarations.

LSP:
- New `textDocument/inlayHint` handler. Renders `: <type>` next to
  the identifier of every `:=` binding whose record has a type. The
  hint is virtual — Source files aren't modified.

Example: `count := 0` shows in the editor as `count: Int := 0` once
the LSP has a chance to publish.

## [1.6.11] — 2026-06-03

### `quirk-lsp` 0.12.0 — folding ranges

`textDocument/foldingRange` lets the editor collapse function bodies,
struct/enum/interface blocks, `if`/`else`/`while`/`for` bodies, the
multi-line `from X use { … }` import form, `---` doc fences, and
runs of `//` line comments. Brace-balanced scan; no compiler call.

Compiler binary byte-identical to 1.6.10 modulo the version constant.

## [1.6.10] — 2026-06-03

### `quirk-lsp` 0.11.0 — scope-aware rename

`textDocument/prepareRename` returns the identifier span so the
editor's rename popup pre-fills with the current name. `textDocument/
rename` picks one of two paths based on the symbol cache:

- **Local-only rename** (parameters, local variables): touches the
  current file only. Other files can't reference a local.
- **Workspace rename** (functions, structs, enums, interfaces,
  methods, fields, module constants, enum variants): word-boundary
  rename across every opened doc + every `.quirk` file under the
  workspace folders. Same walker as find-references.

Caveat: text-based for the actual replacement. Two locals with the
same name in different functions, or a parameter shadowed by a
local, can't yet be renamed independently from the workspace path
— that needs per-usage tracking from Sema (v1.6.11+ direction).
The same-file case mitigates the worst risk by limiting scope when
the symbol cache says the name is local.

Compiler binary byte-identical to 1.6.9 modulo the version constant.

## [1.6.9] — 2026-06-03

### `quirk-lsp` 0.10.0 — workspace symbols

`workspace/symbol` searches every symbol the LSP has cached this
session. Lists top-level decls + methods + fields (skips parameters
and local variables — too noisy at the workspace level). Substring
matches the query case-insensitively; capped at 500 results so a
huge multi-file project doesn't flood the editor's picker.

Scope is "files opened this session" rather than every `.quirk` file
under the workspace folders. A full pre-indexed workspace search
would require running `--symbols-json` for every file on startup or
on a `workspace/didChangeWatchedFiles` notification — deferred until
someone hits the gap.

Compiler binary byte-identical to 1.6.8 modulo the version constant.

## [1.6.8] — 2026-06-03

### `quirk-lsp` 0.9.0 — signature help

`textDocument/signatureHelp` triggers on `(` and `,` inside a call.
Walks backward from the cursor balancing parens to find the callee
identifier and the active argument index, then pulls the function's
parameter list from the cached `--symbols-json` records. Active
parameter is highlighted; multiple matching decls (e.g. interface
method + concrete method with the same name) all show up and the
editor renders a chooser.

Round-trip verified end-to-end on a two-parameter function: cursor
between `(` and `,` highlights param `[0]`, cursor after the `,`
switches to param `[1]`.

Compiler binary is byte-identical to 1.6.7 modulo the version
constant — this release is `quirk-lsp` only.

## [1.6.7] — 2026-06-03

### `quirk --symbols-json` + scope-aware LSP completion

- **New `--symbols-json` flag.** Walks the AST after Sema and emits
  one NDJSON record per declaration: functions, methods (demangled
  from `<Struct>_<raw>` to `<raw>`), structs, fields, enums, enum
  variants, interfaces, parameters, local variables, module
  constants. Each record carries `kind`, `name`, `scope` (`"module"`
  / struct-name / enclosing-function-name), `file`, `line`, `col`,
  and `type` where Sema knows one. Implies `--check`.
- **`quirk-lsp` 0.8.0** runs `--symbols-json` on `didOpen` + every
  `didSave` and caches the records per-document. Completion now
  surfaces the parameters and local variables of the function the
  cursor is in, alongside the existing identifier / keyword /
  member-access suggestions. Detect the enclosing function by
  picking the latest `function`/`method` record whose line is at or
  above the cursor; coarse but matches Quirk's column-0
  decl convention well enough for the common case.

This unlocks two future features without further compiler work:
*signature help* (use the parameter records of the function being
called) and (with usage tracking added) *semantic rename*.

## [1.6.6] — 2026-06-03

### `quirk-lsp` 0.7.0 — completion

`textDocument/completion` adds two modes:

- **Identifier completion** (fires automatically as you type) merges:
  - Top-level declarations in the current file (functions, structs,
    enums, interfaces)
  - Names brought in via `from X use { Y, Z }` blocks
  - Quirk keywords (`define`, `struct`, `if`, `match`, …)
  - Builtin types (`String`, `Int`, `List`, `Map`, exception kinds, …)
- **Member access** (triggered by `.`) — when the LHS is a known
  imported module, the LSP reads that module's file and offers its
  top-level declarations. Typing `argparse.` after `use argparse`
  surfaces `Parser`, `flag`, `option`, etc.

Like the rest of `quirk-lsp`, the suggestions are regex/text-based
— no scope tracking, no type inference. False positives (e.g. a
parameter name surviving past its function) are sorted by the
editor's fuzzy match as the user types.

Compiler binary is byte-identical to 1.6.5 modulo the version
constant — this release is `quirk-lsp` only.

## [1.6.5] — 2026-06-03

### `quirk-lsp` 0.6.0 — hover

`textDocument/hover` returns the declaration's signature line (wrapped
in a ```quirk fence) plus the doc-comment block that precedes it. Two
doc styles supported:

- `---` block fences (Quirk's docstring convention used throughout
  the stdlib)
- consecutive `// …` line comments

Same-file declarations win first; cross-file hits walk the file's
imports + `quirk resolve` and read the target file off disk. A
cross-file hover suffixes the source basename (e.g. `*from string.quirk*`)
so the user knows which module the signature was lifted from when
multiple stdlib modules export same-named types.

Compiler binary is byte-identical to 1.6.4 modulo the version
constant — this release is `quirk-lsp` only.

## [1.6.4] — 2026-06-03

### `quirk-lsp` 0.5.0 — find references (`textDocument/references`)

Word-boundary text search for every occurrence of the identifier
under the cursor, across every `.quirk` file in the workspace
folders. Skips `packages/`, `.venv/`, `.git/`, `node_modules/`,
`build/`, `out/`, `obj/`, `target/`, `.cache/`. Caps at 5000 files
and 1 MiB per file to keep accidental walkthroughs bounded.

- Open documents are scanned via their in-memory text first, so
  unsaved edits show up.
- Files not currently open are read from disk.
- `context.includeDeclaration: false` strips the canonical
  declaration site from the result.

Coarse on purpose — finds textual matches regardless of scope, so a
parameter named `foo` in one function and a top-level `foo()` both
appear. A scope-aware version needs the compiler to expose its
symbol table; until then, the find-references panel is "good enough"
for navigation and a bad fit for fully-automatic rename.

Compiler binary is byte-identical to 1.6.3 modulo the version
constant — this release is `quirk-lsp` only.

## [1.6.3] — 2026-06-03

### `quirk resolve <name>` + cross-file LSP go-to-def

- **New `quirk resolve <name>` subcommand.** Prints the absolute path
  of the `.quirk` file that `use <name>` would load, or exits 1 on a
  miss. Accepts both bare names (`quirk resolve argparse`) and dotted
  paths (`quirk resolve typing.primitives.int`) — the dotted form
  converts `.` → `/` before the lookup so the file layout under
  `packages/typing/primitives/int.quirk` is reachable. Reuses
  `locate_module_file` so the resolution matches what `use` does at
  compile time.
- **`quirk-lsp` 0.4.0** uses this to extend go-to-definition across
  files. The LSP scans the current document's import block
  (`use X`, `from X use { Y, Z }`, including the multi-line brace
  form heavily used by `typing/`) into a `name → module` map. On
  ctrl-click, same-file declarations still win first; if the cursor
  identifier matches an imported name, the LSP runs `quirk resolve`
  and jumps to that module's matching decl. Resolved paths are
  cached per session so repeat lookups don't re-spawn the compiler.

Tested round-trip: ctrl-click on `argparse` jumps to the user-global
install at `~/.quirk/packages/argparse/index.quirk`; ctrl-click on
`Int` (imported via `from typing.primitives.int use { Int }`) jumps
to `packages/typing/primitives/int.quirk` at the `struct Int` line.

## [1.6.2] — 2026-06-03

### `quirk-lsp` 0.3.0 — go-to-definition (current file)

- **`textDocument/definition`** — ctrl-click a name to jump to its
  `define` / `struct` / `enum` / `interface` declaration. Scope is
  intentionally tight: same-file top-level + struct methods only.
  Returns multiple `Location`s when the editor would render that as
  a chooser (e.g. interface + concrete method with the same name).
- Local variables, struct fields, and parameters don't resolve yet
  — they need scope tracking that's bigger than what regex can do.
- Cross-file resolution (`use argparse` → `packages/argparse/...`)
  is also deferred. The cleanest path is a `quirk resolve <name>
  --from <file>` query on the compiler so the LSP doesn't have to
  duplicate the C++ resolver in TypeScript; that lands in a later
  1.6.x release.

Compiler binary is byte-identical to 1.6.1 modulo the version constant
— this release is `quirk-lsp` only.

## [1.6.1] — 2026-06-03

### `quirk-lsp` 0.2.0 — outline + formatting

Two more LSP features land in the standalone server; the compiler
itself is unchanged this release. Same install instructions as 1.6.0
still work — pull a fresh `quirk-lsp` from the repo + `npm run build`.

- **`textDocument/documentSymbol`** populates the editor's outline
  panel, breadcrumbs, and `@`-prefix symbol picker. Top-level
  `define` / `struct` / `enum` / `interface` show as Function /
  Struct / Enum / Interface; methods inside a struct nest under it.
  Regex-driven (no compiler invocation) so it's cheap on every keystroke.
- **`textDocument/formatting`** shells out to `quirk fmt --stdout`
  on the buffer and returns a single edit replacing the whole
  document. Same canonical output as `quirk fmt` from the CLI, so
  format-on-save in any editor stays in sync with the project's
  pre-commit hook.

The version bump in `quirk-compiler/src/PackageManager.hpp` is only
for the bundled CHANGELOG entry; the binary itself is byte-identical
to 1.6.0 modulo the version constant.

## [1.6.0] — 2026-06-03

### LSP foundation: `quirk-lsp` server

Quirk now ships with a standalone Language Server Protocol implementation
under `quirk-lsp/`. Same diagnostics in any LSP-aware editor — Neovim,
Helix, Zed, JetBrains. The VSCode extension keeps its in-process
providers for now; switching it to use the LSP is on the v1.6.x roadmap.

This release covers the foundation:

- **`quirk --check --diagnostics-json`** — new flag on the compiler.
  Emits one NDJSON record per diagnostic to stdout instead of the
  human-readable ANSI output to stderr. Both Sema and Parser route
  through it. `--check` also stays silent on success in JSON mode
  (no `: OK` banner) so the LSP can infer success from an empty
  stream + exit 0.
- **`quirk-lsp` TypeScript server** — single Node binary. Speaks
  stdio. Spawns the compiler on `didOpen` + `didSave`, translates
  NDJSON records into LSP `Diagnostic` objects. Compiler binary
  resolved via `initializationOptions.quirk.executablePath` →
  `QUIRK_BIN` env → `quirk` on `PATH`.
- **Editor configs** — `quirk-lsp/README.md` has copy-pasteable
  snippets for Neovim's built-in LSP, Helix's `languages.toml`, and
  Zed's settings. The wire format is documented for one-off tools
  (CI gates, pre-commit hooks).

What ships in later 1.6.x:
- Hover, completion, definition, references, rename, signature help,
  semantic tokens, formatter, outline (each is a port of an existing
  VSCode provider).
- VSCode extension switches to the LSP for at least diagnostics.

## [1.5.1] — 2026-06-03

### `pkg install --frozen` lockfile-name vs URL-basename mismatch

A stdlib package spec like `crypto = "github.com/AlexVachon/quirk-crypto@v1.0.0"`
produced a lockfile entry keyed by the package's manifest name (`crypto`),
but `--frozen` lookup used the URL basename (`quirk-crypto`). Result:
every `pkg install --frozen` against a clean working tree failed with
`'quirk-crypto' not in quirk.lock` even though the lockfile was correct.

Fixed: the resolver now also builds a URL → lockfile-name map before
the install loop and falls back to it whenever `preview_name(spec)`
disagrees with what's actually in the lockfile. Same code path
handles the dedup/conflict check, so two specs pointing at the same
repo with different prefixes (e.g. one `crypto` alias and one full
`github.com/.../quirk-crypto`) are treated as duplicates.

Surfaced during a full audit of the 21 stdlib packages — round-trip
install + use works for every package; the `--frozen` path was the
only crack.

## [1.5.0] — 2026-06-03

### Full stdlib split — every package now has a canonical repo

v1.4.0 shipped the *mechanism* (compiler-baked `stdlib_registry()`)
and one pilot package (`argparse`). v1.5.0 completes the rollout:
all 21 stdlib packages now resolve via the registry to their own
GitHub repo at `github.com/AlexVachon/quirk-<name>@v1.0.0`. The
bundled copies under `<QUIRK_HOME>/packages/` stay in place as the
offline fallback.

### Resolver fixes (uncovered while validating the split)

Two related issues surfaced when round-tripping `typing` end-to-end:

- **`src/` layout install flatten.** Repos for stdlib packages use
  the `src/index.quirk` convention (separate package source from
  README/LICENSE). On install, `materialize_from_cache` now copies
  the contents of `src/` (not `src/` itself) into
  `<pkgRoot>/<name>/`, so the installed layout matches what the
  bundled packages look like. Detection is presence-of-
  `src/index.quirk`; legacy packages without `src/` are unchanged.
- **Relative-import fall-through.** `from ...sys` inside the typing
  package used to rely on `sys` being a sibling directory in the
  bundled flat layout. In a project-local install of only `typing`,
  that sibling doesn't exist. The resolver now falls through to the
  absolute search when a relative walk misses, so the import lands
  on the bundled `sys` (or a separately-installed one) instead of
  hard-failing.

Together these unblock running scripts that depend on packages
installed via the new registry.

Newly registered: `console`, `crypto`, `csv`, `datetime`, `debug`,
`encoding`, `fs`, `io`, `itertools`, `math`, `net`, `random`,
`regex`, `statistics`, `sys`, `test`, `time`, `typing`, `url`, `uuid`.

Each repo carries the same source the compiler shipped, a generated
`quirk.toml` (`quirk-version = ">=1.4.0"`), a README with the
top-of-file docstring, MIT LICENSE, and a `v1.0.0` tag. Future
maintenance: bump tags in the individual repos for fixes;
compiler-baked URLs stay stable.

`quirk pkg install typing` (or any other name) now fetches the
latest tag and shadows the bundled copy at `use typing` time.
`quirk pkg upgrade typing` re-fetches if a newer tag has been
published.

### Roadmap update

The LSP rollout (was v1.5) slides to **v1.6**. Doing the full
stdlib split first was higher leverage — every stdlib fix from
this point can ship via a package tag rather than a compiler
release.

## [1.4.0] — 2026-06-03

### Stdlib packages can now live in independent repos

First step of the stdlib-decoupling roadmap. The bundled stdlib at
`<QUIRK_HOME>/packages/` still ships and stays the offline fallback,
but `quirk pkg install <stdlib-name>` now resolves to a canonical
GitHub repo and can fetch a newer version than the one the compiler
shipped with. Users no longer need a compiler bump to pick up an
argparse fix.

- **`stdlib_registry()` in `PackageManager.hpp`** is a baked-in map
  of stdlib names → canonical repo URLs. Falls in after user aliases
  (`~/.quirk/aliases.toml`) and the cached external registry, so user
  overrides win.
- **`quirk pkg registry list`** now displays the built-in entries
  alongside aliases and the cached registry. Run it to see what bare
  names resolve to.
- **First entry: `argparse → github.com/AlexVachon/quirk-argparse`.**
  A staging repo for the package is prepared locally at
  `/tmp/quirk-argparse-staging/` (with `quirk.toml`, README, LICENSE,
  `v1.0.0` tag); compiler-maintainer creates the GitHub repo and
  pushes from there. The bundled `packages/argparse/` in the compiler
  tree stays in place as the offline fallback.

Adding a new stdlib package to the registry is one entry in
`stdlib_registry()` + a compiler ship. See
[PACKAGES.md](../PACKAGES.md#stdlib-in-independent-repos-since-140)
for the full maintainer workflow.

## [1.3.0] — 2026-06-03

### First macOS support — source-buildable + CI-validated

Quirk now compiles cleanly on macOS (Apple Silicon). All Linux-specific
code paths have been replaced with portable wrappers:

- **`self_binary()`** dispatches on `__APPLE__`: uses
  `_NSGetExecutablePath` + `realpath()` on macOS, `/proc/self/exe`
  + `readlink()` on Linux. The half-dozen `readlink("/proc/self/exe", ...)`
  call sites scattered across `PackageManager.hpp` + `Compiler.cpp`
  now go through this single helper.
- **`runtime.so`** filename is kept on macOS too (Apple's `dlopen`
  doesn't care about the extension, and renaming would ripple into
  7+ install-script and resolver sites for no practical benefit).
- **Makefile** uses `?=` on `CXX`/`CC`/`LLVM_CONFIG` so macOS users
  can build with `LLVM_CONFIG=$(brew --prefix llvm@14)/bin/llvm-config make`
  without touching any source.

### CI

A new `macos-arm64` job in the release workflow builds + smoke-tests
the compiler on every tag push and uploads `quirk-X.Y.Z-darwin-arm64.tar.gz`
alongside the Linux tarball. Marked `continue-on-error: true` for now —
won't block a release if the macOS build flakes during this
first-shipping window.

### Installer

`install.sh` recognises macOS arm64 and attempts to fetch the matching
tarball. On 404 (e.g. installing a pre-1.3 tag), it prints the
build-from-source path:

```
brew install llvm@14 bdw-gc openssl@3
LLVM_CONFIG=$(brew --prefix llvm@14)/bin/llvm-config make
```

Also: `sha256sum` (Linux) vs `shasum -a 256` (macOS) is dispatched by
availability rather than hardcoded — fixes a checksum-verification
crash that would otherwise hit Mac users.

### Known caveats

- **No local validation.** This release is a blind port — built and
  tested on Linux only. The first macOS binary will materialise from
  CI; expect a v1.3.1 with the first round of "wait, that doesn't
  compile on macOS after all" fixes.
- **Apple Silicon only.** Intel-Mac (`darwin-x86_64`) and Windows are
  not yet built; both are on the roadmap.

## [1.2.0] — 2026-06-03

### REPL line editing + persistent history

`quirk repl` now does the things you'd expect from a real interactive
shell:

- **Arrow-key recall** (↑/↓) walks through previous inputs.
- **Line editing** — ←/→, ctrl-A/E, ctrl-W, backspace etc. all work.
- **Persistent history** at `~/.quirk/repl_history` (capped at 1000
  entries). Sessions merge with the file on save, so quitting and
  reopening keeps your context.
- **Non-TTY input still works** — when stdin is piped (`printf ... |
  quirk repl`), linenoise transparently falls back to plain fgets.

The REPL itself (preamble/state model, multi-line via brace balance,
`:help` / `:quit` / `:reset` / `:state` meta-commands) was already in
the codebase; this release just makes it pleasant to use.

Implementation: vendored linenoise (BSD-2-clause, antirez/linenoise)
into `src/third_party/linenoise/`. No new build dep — linenoise is a
single-file C library, compiled in alongside the existing objects.
~1500 LOC added, scoped to `quirk repl` only.

## [1.1.0] — 2026-06-03

### `quirk test` is now usable

The runner has been in the codebase for a while — walks `tests/`,
spawns `quirk run` per `*_test.quirk`, parses framework summary
lines — but in practice it deadlocked on any test that opened a
network port or waited on stdin (e.g. `server_test.quirk` would
block forever binding a socket). v1.1.0 ships the fixes that make
it actually usable as a CI / pre-tag check.

- **`--timeout <secs>`** (default 30s, `0` to disable). Per-file
  wall-clock cap, enforced by shelling out to `timeout(1)`. Tests
  over the cap fail with exit 124 and render as `(timeout)` in the
  status line instead of `(exit 124)`.
- **`--filter <substr>`** runs only files whose path contains
  `<substr>`. Useful for iterating on one suite without paying for
  the whole batch.
- **`-v` / `--verbose`** still dumps each file's full output (was
  already there; just documented now).

The runner discovers files matching `<name>_test.quirk` recursively
under `tests/` by default, or the path you pass. It already skipped
`packages/`, `.venv`, `.git`, `node_modules` — that's unchanged.

The framework contract: tests must end with a summary line matching
`N passed` (success) or `M failed, N passed (of T)` (failure). The
bundled `test` package emits both shapes; user frameworks can match
either format. Files that exit non-zero without a summary line still
count as failures with `(exit N)` in the status line.

## [1.0.12] — 2026-06-03

### `pkg remove` now keeps the lockfile consistent

`quirk pkg remove <name>` deleted `packages/<name>/` and stripped the
entry from `quirk.toml`, but left a stale `[[package]]` block in
`quirk.lock`. A subsequent `pkg install --frozen` would then either
resurrect the package or fail with a misleading "lockfile/manifest
mismatch". Fixed: `cmd_remove` now also drops matching entries from
the lockfile, and removes the file entirely when it ends up empty so
there's no header-only file polluting git diffs.

This applies to both bare-name remove (`pkg remove logger`) and the
versioned `pkg remove logger@0.1.0` path when the version being
removed is the active one.

### Audit notes (no code change)

Round-tripped a third-party path-based install end-to-end:
- `pkg install` → correct project-local install under `packages/` when
  `quirk.toml` is present; falls back to `~/.quirk/packages/` outside
  a project, as documented.
- `quirk-version = ">=X"` is enforced — packages requiring a newer
  compiler are rejected with a clear message.
- Resolver precedence is correct: project-local `./packages/X` wins
  over user-global `~/.quirk/packages/X`.

## [1.0.11] — 2026-06-03

### Cache key now walks the transitive import graph

The 1.0.10 bitcode cache hashed only the entry file. Changing a `use`d
file (a stdlib module, a project-local package) without also changing
the entry script produced a stale cache hit. Fixed: a mini-scanner
now walks `use` / `from … use` directives starting at the entry file,
following resolved imports recursively, and folds each loaded file's
bytes into the SHA-256. `typing` is seeded explicitly since it's
auto-imported by every program.

The scanner is deliberately simpler than the real parser — line-based
text scan, no AST, no Sema. Worst-case divergence (e.g. picking up a
`use ...` string inside a block comment) just adds a spurious file to
the hash, which downgrades to a cache miss on the next run. Never
produces a wrong result.

`--no-cache` still bypasses everything. Stale entries from old keys
linger in `~/.quirk/cache` until a future LRU eviction (TODO).

## [1.0.10] — 2026-06-03

### Per-invocation bitcode cache

- **`quirk <file>` now caches compiled bitcode under `~/.quirk/cache/`.**
  The cache key is `sha256(file content + compiler version + opt
  level)`. On a hit, parse + Sema + Codegen + LLVM passes are skipped
  entirely — the JIT consumes the cached IR directly. Saves ~15–25%
  per-run on real scripts that import stdlib; small wins on
  compute-heavy scripts where runtime dominates.
- **`--no-cache`** opts out, e.g. for benchmarking or when transitive
  imports have changed (see known limitation below).
- The cache is skipped automatically when `--debug` is on (the line
  stepper needs the source map that bitcode doesn't carry) and for the
  `-o`/`--check`/`--emit-*` paths.

**Known limitation:** the cache key hashes the entry file but not its
transitive `use` imports. If a `use`d file changes without the entry
also changing, the stale cache wins. Workaround: `--no-cache` or
`rm -rf ~/.quirk/cache`. A future release will walk imports during
key computation.

### Misc

- `quirk --help` now correctly documents `-O1` as the default optimization
  level (it always was — only the help text was wrong).

### Wart #2 status (Int 0 in nonlocal cells)

Investigated a fix using bit-48 tagging of nonlocal-cell pointers. The
tagged pointer survives the `ptrtoint→i32` unbox sites (truncation
discards the tag), but it breaks downstream consumers that treat the
cell value as a real `Any*` — `print(it())` crashed with a SEGV when
the tagged pointer was dereferenced. A correct fix needs coordinated
changes to both the boxing convention and every unbox site, which is
larger than this release. Tracked in `feedback_iter_lib_warts`.

## [1.0.9] — 2026-06-03

### Closures-with-state fixes

Two longstanding codegen warts that bit anyone writing stateful iterators
or filter pipelines through `Callable`. Both surfaced while building
`packages/itertools/` and have been worked around with annotations until
now.

- **Nonlocal Int values now flow through call sites correctly.** A
  `nonlocal i` cell stores `i8*`; passing it as the index to
  `List.get(i)` previously crashed with `Call parameter type does not
  match function signature` because the call-args codegen didn't insert
  a `ptrtoint`. The typed-local workaround (`idx: Int = i`) is no
  longer needed.
- **`pred(v) == false` no longer aborts the JIT.** Comparing the
  `i8*`-boxed return of a `Callable` against an `i1` literal used to
  hit `ICmpInst::AssertOK`. The BinaryOp codegen now routes the boxed
  side through `Core_Primitives_Any_to_int` (which reads `.ival` for
  `ANY_BOOL`/`ANY_INT`/`ANY_CHAR`) before the compare, so any
  `boxed-Any ==/!= primitive` pattern works.

Both fixes are pure compiler — no runtime changes — and existing tests
keep passing.

## [1.0.8] — 2026-06-02

### Stdlib lives under `packages/`

- The bundled standard library moved from `quirk-compiler/libs/` to
  `quirk-compiler/packages/`. Every stdlib module (`typing`, `console`,
  `net`, `crypto`, …) is now resolved through the same path the package
  manager uses for third-party packages.
- **Why:** a project that drops a `packages/<name>/` folder into its
  own tree can now shadow a stdlib module without reaching into the
  compiler install. Previously stdlib lived on a separate search path
  that user projects couldn't override.
- Install layout follows: tarballs now ship `<QUIRK_HOME>/packages/`
  (not `libs/`). `install.sh` wipes a stale `~/.quirk/libs/` on
  reinstall so the legacy fallback never shadows the fresh layout.
- The resolver still recognises legacy `libs/` directories — pre-1.0.8
  installs and any project that already had `./libs/` keep working
  until you reinstall.

## [1.0.7] — 2026-05-29

### Improved CLI surface
- **`quirk help` is now grouped** into RUN CODE / PROJECT / PACKAGES /
  PUBLISHING / COMPILER / MISC, with RUN FLAGS and ENVIRONMENT
  sections beneath. Replaces the alphabetical wall of 35 verbs.
- **`quirk bump-compiler`** is now also reachable as **`quirk compiler bump <part>`**
  (the old form still works; the new is documented).
- **`quirk stdlib`** is now also reachable as **`quirk compiler stdlib`**.
- **`quirk versions <pkg>`** is documented as **`quirk pkg versions <pkg>`**
  (the bare form remains an alias; the new is clearer that it lists
  *package* versions, not Quirk's).
- **`install.sh` flag renamed to `--with-extension`** (with
  `--install-extension` still working as a legacy alias, and a new
  explicit `--no-extension` opt-out). README + INSTALL.md updated.

The CLI is now consistently `quirk <command> [--flags] [args...]`,
matching standard Unix-tool conventions. No verbs were removed; only
the help / install-flag spellings changed.

## [1.0.6] — 2026-05-29

### New
- **`quirk` now notifies users when a newer release is available.**
  Cargo/npm-style: a one-line dim notice on stderr after any quirk
  subcommand finishes:

      ↑ A new Quirk release is available: 1.0.5 → 1.0.6
        run `quirk compiler update` to upgrade, or set
        QUIRK_NO_UPDATE_CHECK=1 to silence this.

  Behavior:
  - Checks GitHub at most **once per 24h** (cached at
    `~/.quirk/update-check.json`). Curl times out after 3s so a slow
    network never makes Quirk feel laggy.
  - Notice prints **at most once per 24h** even when the cache is
    refreshed sooner — tracked via `last_shown_at` in the cache file.
  - **Silent on non-TTY stdout** (pipes, redirects, CI logs).
  - **Silent on dev builds** — when the binary lives inside a source
    checkout with a `.git` directory above. Avoids nagging contributors
    working on the compiler.
  - **Silent when `QUIRK_NO_UPDATE_CHECK=1`** in the environment.

- **`quirk compiler check` — explicit on-demand version check.**
  Bypasses the 24h cache, hits GitHub right now, prints "Up to date"
  / "Update available" / "Newer than published (probably a dev build)".

### Setup note
- The first time a v1.0.5 user runs any quirk command after upgrading,
  the cache file is created. From then on the once-per-day check kicks
  in automatically. Users on older versions never see notices until they
  update once (manually or via `quirk compiler update`).

## [1.0.5] — 2026-05-29

### New
- **`quirk auth {login|status|logout}` — zero-setup publish auth.**
  Implements GitHub's OAuth Device Flow directly in the compiler so
  publishing a package doesn't need any of: SSH keys, deploy keys,
  PATs, or even the `gh` CLI. Flow:

      $ quirk auth login
      → Visit https://github.com/login/device
      → Code:  A4B7-C2D9
      [waiting for authorization...]
      ✓ Logged in as alexvachon

  Token is saved to `~/.quirk/auth.json` (chmod 600). One-time
  setup; subsequent `quirk release`/`quirk publish` Just Works.

- **`quirk release` push chain rewritten** to prefer the least-setup
  path that's actually available:
    1. Quirk's own stored token (from `quirk auth login`)
    2. `gh` CLI (from `gh auth login`)
    3. Plain `git push` against the configured remote
  When any of the first two are active, the push is automatically
  routed over HTTPS with the token applied via a scoped git
  credential helper — no manual remote-url switching, no global
  git config changes, no SSH identity routing.

### Improved
- **Updated deploy-key diagnostic** to lead with `quirk auth login`
  (zero external dependencies) as option 1, then `gh`, then the
  three SSH/PAT fallbacks.

### Setup note
- The first `quirk auth login` requires the Quirk OAuth App's
  `client_id` to be baked in (or supplied via `QUIRK_OAUTH_CLIENT_ID`).
  Build emits a clear error pointing at
  `https://github.com/settings/applications/new` if it's not
  configured. Public client IDs are safe to ship in the binary.

## [1.0.4] — 2026-05-29

### Improved
- **`quirk release` auto-routes through `gh` when available.** If the
  user has the GitHub CLI installed and is logged in (`gh auth login`,
  one-time, browser-based — no SSH keys, no PATs to manage), the
  release push automatically uses gh's stored credentials via a
  scoped git credential helper. SSH-only repos still work because
  we override the push URL to HTTPS for that single command. The
  user sees one extra line:

      using gh CLI auth (no SSH/PAT setup needed)

  …and the push just works.

  Goal: make "publish a Quirk package" cost one-time `gh auth login`
  and that's it. No deploy-key juggling, no PAT scoping, no SSH
  identity routing. The git-push fallback (with the diagnostic from
  1.0.3) still runs verbatim when `gh` isn't installed/authed.

- **Improved deploy-key diagnostic.** When `git push` fails with the
  "denied to deploy key" message, the diagnostic now leads with the
  `gh` setup (1-2 commands), then walks down through three SSH/PAT
  alternatives. Clarified that an empty deploy-keys page is fine
  (click "Add deploy key" and tick "Allow write access" on the form
  — there's no toggle on the empty list).

## [1.0.3] — 2026-05-29

### Improved
- **`quirk release` now diagnoses git-push failures** instead of just
  saying "git push failed". Pattern-matches the captured `git push`
  output for the most common reasons a publish blows up, then prints
  a targeted hint with concrete remediation steps and the relevant
  GitHub settings URL:
    - **Deploy key lacks write access** — links to
      `<repo>/settings/keys`, suggests the read-only key fix, an
      explicit `GIT_SSH_COMMAND` for a personal SSH identity, or
      switching to HTTPS + a fine-grained PAT.
    - **HTTPS auth missing / invalid** — explains PAT setup vs SSH switch.
    - **Repository not found** — flags missing repo or zero access.
    - **Network unreachable** — suggests retry.
    - **Branch is behind** — suggests `git pull --rebase && quirk release`.
  In all cases, when the commit was pushed but the tag wasn't, the
  diagnostic surfaces "the commit was pushed; only the tag is pending"
  so users know exactly which retry command to run.

  Reminder: Quirk publishes via plain `git push` — there's no central
  registry — so any auth issue that blocks `git push` blocks deploys.

## [1.0.2] — 2026-05-29

### Fixed
- **`quirk compiler` now shows up in tab-completion.** The new verb
  shipped in 1.0.1 wasn't in the hard-coded verb list inside the
  `quirk completion <shell>` output, so bash/zsh/fish tab-completion
  didn't suggest it. Added to the bash/zsh verb list plus a per-verb
  completion for the four subcommands (`version` / `list` / `install`
  / `update`), and the equivalent fish completions.

  After upgrading, refresh the completion in your shell:

  ```bash
  source <(quirk completion bash)   # or zsh / fish
  ```

  Or just open a new terminal — your rc file will source it.

## [1.0.1] — 2026-05-28

Four compiler correctness fixes, surfaced while building a real
third-party package (the in-tree `logger` lib), plus a new self-
management command so updating the compiler doesn't require touching
the install.sh URL.

### New
- **`quirk compiler <subcommand>`** — self-manage the compiler binary
  from inside the running compiler:
    - `quirk compiler version` — print the running version
    - `quirk compiler list` — list `vX.Y.Z` releases on GitHub
    - `quirk compiler install vA.B.C` — replace this binary with that version
    - `quirk compiler update` — replace this binary with the latest
  Internally delegates to `install.sh` on `main`, so there's a single
  source of truth for the install flow.

### Compiler fixes
- **Enum-typed parameters now compare to enum literals.** `define f(l: Level) -> Int { if l == Level.A {...} }` used to crash LLVM with an `ICmp` operand-type assertion because `TypeGen` fell through to `i8*` for any unrecognised name, while enum literals are codegen'd as `i32`. `TypeGen` now knows about user-declared enums and resolves them to `i32`.
- **Top-level `NAME := value` bindings are now importable across modules.** `from M use { NAME }` previously only matched struct / function / interface / enum names. A new `moduleConstRegistry` in `Sema` tracks module-level value bindings and the filter-list validator consults it.
- **Cross-module function-name collisions resolved.** Two files that both declared a function called `info` (or any common name) collided on the same LLVM symbol, and `module.info` calls could end up at the wrong implementation. Non-extern user functions now get module-prefixed linkage names (`MyMod$info`), and `moduleFunctionIndex` is keyed by both PascalCase and lowercase forms so `use console` matches the parser's `Console` prefix.
- **`list[i]` (`Any`) now passes correctly where `Callable*` (or any non-String struct pointer) is expected.** The argument-coercion site in `processCallArgs` was String-only for `i8* → struct*`; widened to all pointer-to-pointer casts, fixing the "Call parameter type does not match function signature" verifier failure that hit any code passing a `Callable` retrieved from a `List`.

### Known limitations
- Bare-name calls (`info(...)` without `module.` prefix) when two modules expose `info` still pick whichever was registered last. Module-qualified is the right pattern.
- `use .relative_path` in a sibling-file import still misses `moduleFunctionIndex`'s lookup keys; the surrounding tooling works around this for in-package imports.

## [1.0.0] — 2026-05-28

First stable release. The interactive debugger and VSCode integration are
the headline features; the rest is correctness and IDE-experience work
accumulated during that build-out.

### Interactive debugger
- **`debug.breakpoint(label)`** — tier-1 breakpoint helper that drops into an
  interactive `(qdb)` prompt with continue / quit / backtrace / skip-rest.
- **`--debug` line stepper** — tier-2 stepper that pauses at every statement.
  Commands: `c`/continue, `n`/next (step-over), `s`/step (step-into), `bt`,
  `where`, `b [file:]line` / `clear`, `skip`, `q`.
- **Locals inspection (`p <name>`, `locals`)** — Codegen registers each local
  with the runtime at its alloca site so the prompt can read live values
  for Int, Double, Bool, String, and any pointer type. Frame-scoped: stale
  entries get pruned when the owning function returns.
- **JSON event mode (`QUIRK_DBG_JSON=1`)** — qdb emits one machine-parseable
  event per stderr line (`stopped`, `stack`, `locals`, `breakpointSet`, …)
  so external tools can drive the debugger over stdio.

### VSCode integration
- **Debug Adapter Protocol bridge** — the extension ships a DAP adapter that
  spawns `quirk --debug` with `QUIRK_DBG_JSON=1` and translates DAP requests
  into qdb commands. Gutter breakpoints, F5 / F10 / F11 stepping, Call
  Stack panel, Variables panel, hover-evaluate for identifiers.
- **Inline values during debug** — Python-style `(value)` decorations next
  to variable references on paused lines, driven by a custom
  `InlineValuesProvider` that skips keywords, comments, string contents,
  and method/call chains.
- **Run/Debug dropdown** in the editor title bar with grouped sections —
  *Run File* + *Run File in Dedicated Terminal* and *Debug File* + *Debug
  using launch.json*.
- **Hover kind prefix** — every symbol hover leads with `(function)`,
  `(method)`, `(parameter)`, `(struct)`, `(enum)`, `(interface)`,
  `(type alias)`, `(constant)`, or `(variable)`, matching Pylance's
  shape. Lambda assignments (`c := fn(...)`) promote to `(function)`.
- **Ctrl+click navigation** now works on lambda parameters and variadic
  (`...args`) parameters.

### Codegen fixes
- **Variadic-lambda boxing** — passing a `Double` (or `Bool`) to a `fn(...args)`
  lambda no longer crashes with `Invalid bitcast: i8* bitcast (double … to
  i8*)`; values are now wrapped via `Core_Primitives_Any_box_*` before
  being placed into the variadic `List`. Same fix for the `is`/isinstance
  path.
- **Per-statement source line preservation** — the parser stamps `line`/`col`/
  `filePath` on every statement node so the stepper has something to show
  at each pause. The synthetic `main` shadow frame now carries the real
  source path instead of the string `"main"`.
- **Struct field inference from `__init`** — `self.X = paramName.method()`
  patterns where the receiver is a `String` parameter and the method is a
  known String-returning operation (`title`, `upper`, `lower`, `strip`, …)
  now infer to `String` instead of falling back to `Any` (which caused
  garbage memory reads on Any→String returns).

### Runtime
- **Locals registry overflow warning** — once-only `[debug] locals table
  full` message instead of silent truncation when more than 1024 locals
  are registered.
- **`strdup` routed through Boehm GC** — eliminates permanent leaks from
  libc calls that bypass the macro-no-op `free`.

### IDE extension polish
- **Variadic param syntax fixes** — `...args` no longer trips an `args is
  not defined` warning inside lambdas, and the spread operator wins the
  tokenization race against the range operator (`...` was being eaten as
  `..` + `.args`).
- **`QUIRK_HOME` auto-resolution** for the debug adapter — derived from the
  `quirk.quirkHome` setting when launching the debuggee, so the child can
  find runtime.so and the stdlib.

### Tooling
- **DAP adapter resilience** — pending `bt`/`locals`/`p` requests now reject
  when the child exits/errors, so VSCode panels stop hanging on a Promise
  that would never resolve.

### Breaking
- The synthetic main shadow frame's `file` field changed from the literal
  string `"main"` to the user's entry-file path. Tooling that parsed the
  old value should update.
- The hover provider's prefix format changed from no prefix to
  `(<kind>) name`. Documented for consistency with Python's Pylance.

## [0.3.0] — 2026-05-23

### New stdlib libraries
- **`random`** — xoshiro256**-backed RNG: `random()`, `randint(lo, hi)`, `uniform(lo, hi)`, `bool()`, `choice(list)`, `shuffle(list)`, `sample(list, k)`, `seed(n)`. Auto-seeded from time + pid at startup.
- **`uuid`** — RFC 4122 v4 UUIDs: `v4()`, `nil()`, `is_valid(s)`. Pure-Quirk, composes on `random`.
- **`itertools`** — eager combinators on Lists: `range_list`, `repeat`, `cycle`, `enumerate`, `zip`, `chain`, `take_while`, `drop_while`, `partition`, `groupby`, `unique`.
- **`datetime`** — now owns the high-level `DateTime` struct (moved from `time`), plus calendar helpers: `is_leap`, `days_in_month`, `is_weekend`, `day_name`, `month_name`, `start_of_day/week/month/year`, `diff_minutes/hours/days`, `humanize(dt, relative_to = now())`. The `time` lib retains only the low-level epoch + component externs.
- **`statistics`** — `mean`, `median` (+ `median_low`/`median_high`), `mode`, `variance`/`pvariance`, `stdev`/`pstdev`, `min_val`, `max_val`, `quantile(items, q)` with linear interpolation.

### Language features
- **Modulo operator** `%` and `%=` for Int/Double, including `__mod__` magic method on structs.
- **Hex / binary integer literals** — `0xFF`, `0b1010`, normalized to decimal at lex time.
- **Triple-quoted strings** `"""..."""` (and `'''...'''`) with multi-line content, interpolation, and automatic common-indent dedent.
- **Match guards** — `case x if cond =>`. A bare lowercase identifier `case x` now binds the scrutinee (instead of being a value-equality check against a free variable).
- **Pattern destructuring in `match`** — tuple form `case (a, b) =>` and list form `case [a, b, c] =>` with positional bindings.
- **Tuple `.N` field access** — `t.0`, `t.1` sugar for `t[0]`, `t[1]`. Works through `List.get` round-trips.
- **Optional chaining `?.`** — `obj?.field` and `obj?.method()` short-circuit on null without crashing.
- **List & map comprehensions** — `[expr for x in iter if cond]`, `{k: v for x in iter if cond}`. Both `where` and `if` accepted as the filter introducer.
- **f-string format specs** — `${x:.2f}`, `${n:04d}`, `${n:x}`. Legacy `%`/`|` separators still work.
- **Module-level mutable state** — top-level `counter := 0` materialises as an LLVM `GlobalVariable`, visible across all functions in the module.
- **Type narrowing** — inside `if x is T { … }` (and `and`-chained variants), `x` is treated as `T` for member/method lookup.
- **Property accessors** — `obj.name` (no parens) auto-calls a zero-arg method when no field by that name exists.
- **`finally` blocks** in `try`/`catch` (was already wired up; documented and covered with tests).
- **String comparison** — `<`, `<=`, `>`, `>=` on `String` now use proper `strcmp`. Was relying on raw pointer comparison and giving wrong answers like `"5" <= "9"` → false.
- **`ref` keyword removed** — was reserved but unused. `ref` is now a usable identifier.
- **Default argument values can be function calls** — `define foo(x = some_fn())` now parses correctly.

### Tooling
- **`quirk repl`** — interactive shell with persistent session state (preamble + bindings persist across turns; `:state`/`:reset`/`:quit` meta commands).
- **`quirk test`** — first-class test runner walking `tests/*_test.quirk`, exit-code-driven pass/fail.
- **`quirk fmt`** — canonical formatter with `--check` and `--stdout`. Handles inline docstrings, negative numbers, compound assigns, empty maps.
- **`quirk sync`** — install missing dependencies from `quirk.toml`.

### File extension
- **`.qk` → `.quirk`** across libs, tests, and the compiler's module-resolution paths. The VSCode extension associates `.quirk` as the language; old `.qk` files are no longer recognized.

### JSON improvements (`encoding.json`)
- **Typed `loads`** — numbers come back as real `Int`/`Double`, booleans as `Bool`, `null` as null (not as the strings `"42"`/`"true"`/`"null"`).
- **Pretty-print** — `dumps_pretty(val, indent=2)` for indented output that still round-trips through `loads`.

### HTTP (`net.http`)
- **Client** — `request`/`get`/`post`/`delete`/`post_json` accept custom `headers`, query `params` (auto-percent-encoded), `follow_redirects=true` (chases up to 5 hops, handles relative `Location`).
- **Server** — new `net.server` lib: `Server.listen(host, port, handler)` + `Request { method, path, query, headers, body }`. Hides the accept-loop / request-parsing / response-formatting boilerplate. Runtime now sets `SO_REUSEADDR` on `bind` so server restarts don't wait on TIME_WAIT.

### Compiler internals
- **Bigger codegen fix-list**: tuple `.N` access through `Tuple___get`; loops inside lambda bodies now capture outer-scope refs (free-var walker descends into `WhileNode`/`ForNode`/`MatchNode`/`TryCatchNode`); module globals materialise before non-main function bodies are emitted so cross-function reads resolve.
- **Runtime hygiene**: `strdup` now routed through a GC-backed copy (was leaking on every Map/Set key); Queue mutators all guard `if (!self)`; `cmd_release` now bails on git-push failure; regex `realloc` checks for NULL; `static` PackageManager helpers switched to `inline` (cleared two `-Wunused-function` warnings).

### VSCode extension
- Triple-quoted-string–aware masking in `maskLine`, with interpolation references preserved as code.
- Match-arm bindings (`case x`, `case (a, b)`) recognised so the body's diagnostics stay clean.
- Top-level `counter := 0` promoted to file-globals; `counter = …` inside a function no longer counted as a new local declaration (was producing "declared but never used" on real globals).
- Default-value params in the param regex (`(x: Int = -1, …)`) — every name after the first now lands in the local set.
- Format-spec characters inside `${…}` no longer leak as undefined identifiers.
- Semantic tokens skip docstrings + line comments so module names mentioned in commentary stay in the comment color.
- Logo refresh: alternate quark-themed icon at `icons/icon_quark.svg`.

### Known limitations carried into 0.3.0
- Enums with associated values (algebraic data types like `Result.Ok(v)`) — deferred; needs a runtime-representation design pass.
- BigInt / Decimal — deferred; each is a multi-day integration with libgmp / libmpdec.
- Storing raw `Double` primitives in a `List` doesn't auto-box; workaround is to keep values as `Any` and unbox at read.
- `Int 0` stored in a nonlocal cell reads back as null (boxed int 0 == NULL pointer); iterator-style code that uses `null` as an end-of-stream sentinel should avoid emitting raw `Int 0` through nonlocal state.

## [0.2.0]

Initial public version. See git history for details.
