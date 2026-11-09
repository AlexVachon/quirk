# Quirk TODO

Bigger pieces of work, roughly in priority order. Small fixes go straight
into a release; this list is for things that need a plan.

## 1. Concurrency — follow-ups

`async define` / `await` (7.12.0) and `cancel` / `wait` / `done`, async
methods, non-blocking DNS / file / console reads (7.13.0) are done. Left:

- Generic and variadic async functions (`async define f[T](…)`,
  `...args`).
- Cancelling a task blocked in socket I/O: it takes effect at its next
  `sleep` / `await` / `wait`, not while `recv` waits.
- Calling a struct method on an untyped (`Any`) value, e.g.
  `items()[0].go()` when `items()` returns a plain `List`, fails at build
  time ("Unknown method 'String.go'"); it needs the element type
  (`-> List[Item]`) or an annotation. Runtime dispatch by type id would fix
  it.

## 2. Performance

- Per-statement line tracking (7.9.2, for tracebacks) costs up to ~30% in
  loops of trivial statements. Skip the store for statements that can't
  fail or call anything (plain arithmetic on locals, assignments of
  literals), or store only before calls and operations that can throw.
- Profile string building (repeated `+`, `.format`) and `Map` lookups,
  the usual hot paths in scripts.

## 3. Package discovery

The registry (`quirk-pkg/registry`) exists, but there's no way to browse it.

- `quirk search <term>`: match names and descriptions in the registry.
- `quirk show <pkg>`: also print the package's README and latest version.
