# `debug` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `debug/index.quirk`


### Module-level functions

#### `extern define breakpoint(label: String = "") -> void`

Pause execution at this line and prompt for a debugger command. `label`
shows up in the banner so multiple breakpoints in the same file stay
distinguishable. Defaults to "" — fine when you only have one.

@example
debug.breakpoint()                      // unlabeled, simplest form
debug.breakpoint("before db write")     // labeled
