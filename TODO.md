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

## 2. Performance — follow-ups

Done in 7.14.0: line tracking only costs on error paths (LineStorePass),
string text is pointer-free for the GC, `"${…}"` results are right-sized,
and `quirk run` builds a native binary in the background. Left:

- `s = s + x` in a loop copies the whole string each time (O(n²): 200k
  appends take ~5 s). Appending in place is only safe when nothing else
  holds the old string, which needs ownership information the compiler
  doesn't have yet. Until then: collect pieces in a List and
  `"".join(parts)`.

## 3. Package discovery

The registry (`quirk-pkg/registry`) exists, but there's no way to browse it.

- `quirk search <term>`: match names and descriptions in the registry.
- `quirk show <pkg>`: also print the package's README and latest version.
