# `test` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `test/index.quirk`

### `struct TestCase`

A single named test. Construct with `TestCase("name", fn() { ... })`.
The lambda body uses `test.assert_*` helpers; any AssertionError fails
the case. Other exceptions also fail it (their type is shown).

#### `define run_all(cases: List) -> Int`

Run every case in `cases`, printing pass/fail marks and a final summary.
Returns the number of failures (0 == all green).


### Module-level functions

#### `define assert_eq(actual: Any, expected: Any, msg: String = "") -> void`

Asserts `actual == expected` by value. For primitive types and anything
with a `__str` method this matches stringified equality; for arbitrary
struct refs it amounts to pointer identity.

#### `define assert_approx(actual: Double, expected: Double, tolerance: Double = 0.0001, msg: String = "") -> void`

Asserts `actual` and `expected` differ by at most `tolerance`. Use for
Doubles where exact equality is fragile.

#### `define assert_throws(cb: Callable, expected_type: String = "", msg: String = "") -> void`

Asserts `cb` throws an exception. If `expected_type` is non-empty, the
thrown exception's type must match (e.g. `"ValueError"`).

#### `define assert_contains(haystack: Any, needle: Any, msg: String = "") -> void`

Asserts `haystack` contains `needle` (substring for Strings, element for
Lists, or anything else with a `.contains` method).
