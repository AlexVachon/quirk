# Quirk TODO

Bigger pieces of work, roughly in priority order. Small fixes go straight
into a release; this list is for things that need a plan.

## 1. Concurrency

Quirk has no threads or async: the runtime never creates a thread and the
`net` HTTP server handles one request at a time, so a slow request blocks
every other client. This is the biggest gap for anything web-facing.

Options to weigh:

- **Tasks + channels** (`spawn(fn() { … })`, `Channel()` with `send` /
  `receive`), on OS threads. Needs the runtime made thread-safe: the
  shadow call stack (tracebacks) and the try/catch stack become
  per-thread, and the GC must know about the threads (Boehm GC's
  `GC_pthread_create`).
- **An event loop for `net`** (non-blocking sockets, callbacks or
  `async`/`await`). Fixes the server without general threading, but is a
  bigger language change if it needs `async`.

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
