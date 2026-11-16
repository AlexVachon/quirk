# `argparse` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `argparse/index.quirk`


### Module-level functions

#### `define parser(name: String, description: String) -> Parser`

Construct a new Parser. `name` is shown in the usage line; `description` is
printed under it.

#### `define flag(args: Map, name: String) -> Bool`

Convenience: read a flag from a parsed-args Map as a Bool. Equivalent to
`args.get(name) == "true"`.
@example  if argparse.flag(args, "verbose") { ... }
