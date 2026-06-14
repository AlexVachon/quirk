# `random` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `random/index.quirk`


### Module-level functions

#### `extern define seed(n: Int) -> void`

Re-seed the global RNG. Same `n` => same stream of values.

#### `extern define _next_double() -> Double`

Internal: returns a Double in [0.0, 1.0). Prefer `random()` below.

#### `extern define _next_int(lo: Int, hi: Int) -> Int`

Internal: inclusive `[lo, hi]`. Used by `randint` and `choice`.

#### `extern define choice(items: List) -> Any`

Pick a random element from a non-empty list. Returns null if empty.

#### `extern define shuffle(items: List) -> List`

In-place Fisher-Yates shuffle. Returns the same list.

#### `define random() -> Double`

Uniform double in `[0.0, 1.0)`. The fundamental float-valued primitive
— all higher-level Double helpers build on this.

@example
if random.random() < 0.5 { print("heads") }

#### `define randint(lo: Int, hi: Int) -> Int`

Inclusive integer in `[lo, hi]`. Matches Python's `random.randint`.
Order of arguments doesn't matter — the runtime swaps if `hi < lo`.

@example
die := random.randint(1, 6)

#### `define uniform(lo: Double, hi: Double) -> Double`

Uniform Double in `[lo, hi)`. Useful for sampling continuous ranges
(positions, weights, etc.).

@example
angle := random.uniform(0.0, 6.28318)

#### `define bool() -> Bool`

True or False with 50/50 odds. One-liner around `_next_int(0, 1)`; the
sugar pays off because it reads better at call sites (`if random.bool()`).

#### `define sample(items: List, k: Int) -> List`

Return `k` distinct elements drawn from `items`. Throws if `k > items.length()`.
Implemented as a partial Fisher-Yates over a copy so the caller's list
isn't disturbed. O(k) work in the average case.

@example
deck := [1,2,3,4,5,6,7,8,9,10]
hand := random.sample(deck, 5)
