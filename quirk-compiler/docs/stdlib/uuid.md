# `uuid` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `uuid/index.quirk`


### Module-level functions

#### `define v4() -> String`

Generate a random RFC 4122 v4 UUID. Two calls back-to-back return
different values (with overwhelming probability — 122 random bits of
entropy each).

@example
id := uuid.v4()
print(id.length())           // 36

#### `define nil() -> String`

The all-zero "nil" UUID — useful as a sentinel for "no UUID yet" in
schemas that disallow nulls.

#### `define is_valid(s: String) -> Bool`

Lightweight format check. Returns true iff `s` matches the canonical
36-char shape with dashes at positions 8 / 13 / 18 / 23 and hex
characters everywhere else. The version / variant nibbles aren't
enforced — anything you parse cleanly via `v4()` will round-trip.

@example
uuid.is_valid("550e8400-e29b-41d4-a716-446655440000")   // true
uuid.is_valid("not-a-uuid")                             // false
uuid.is_valid("550E8400-E29B-41D4-A716-446655440000")   // true (case-insensitive)
