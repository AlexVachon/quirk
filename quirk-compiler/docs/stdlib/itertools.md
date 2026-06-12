# `itertools` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `itertools/index.quirk`


### Module-level functions

#### `define range_list(start: Int, count: Int = -1, step: Int = 1) -> List`

List of `n` arithmetic-progression values starting at `start`, stepping
by `step`. `range_list(5)` → [0,1,2,3,4]; `range_list(2, 5)` →
[2,3,4,5,6]; `range_list(0, 10, 2)` → [0,2,4,6,8,10,12,14,16,18].

#### `define repeat(value: Any, n: Int) -> List`

List of `n` copies of `value`.
@example repeat("x", 3)   // ["x", "x", "x"]

#### `define cycle(items: List, total: Int) -> List`

Cycle `items` to produce a List of length `total`. Empty input yields an
empty list regardless of `total`.

@example
cycle([1, 2, 3], 7)   // [1, 2, 3, 1, 2, 3, 1]

#### `define enumerate(items: List, start: Int = 0) -> List`

Pair each value with a running index, returning `(idx, value)` tuples.
`start` defaults to 0 — pass `start=1` for 1-based numbering. Access the
returned pairs with `t.0` / `t.1` or `t[0]` / `t[1]`.

@example
for pair in enumerate(["a", "b", "c"]) {
    print("${pair.0}: ${pair.1}")
}

#### `define zip(a: List, b: List) -> List`

Pair up two Lists element-wise as tuples. Stops at the shorter length —
matches Python's `zip`, not `zip_longest`.

@example
zip([1, 2, 3], ["a", "b"])   // [(1, "a"), (2, "b")]

#### `define partition(pred: Callable, items: List) -> Tuple`

Split into `(matching, non_matching)` based on `pred`. Useful for "yes/no"
splits where you want both halves at once.

@example
p := partition(fn(x) => x > 0, [-2, 3, -1, 7])
print(p.0)   // [3, 7]
print(p.1)   // [-2, -1]

#### `define chain(a: List, b: List) -> List`

Concatenate two Lists in order.
@example chain([1, 2], [3, 4])   // [1, 2, 3, 4]

#### `define take_while(pred: Callable, items: List) -> List`

Take items from the front while `pred` is true; stop at the first false.

@example
take_while(fn(x) => x < 5, [1, 3, 5, 4, 2])   // [1, 3]

#### `define drop_while(pred: Callable, items: List) -> List`

Drop items from the front while `pred` is true, return the remainder.
@example drop_while(fn(x) => x < 5, [1, 3, 5, 4, 2])   // [5, 4, 2]

#### `define groupby(key_fn: Callable, items: List) -> Map`

Group items by the value produced by `key_fn`. Returns a Map from
stringified key to a List of items with that key. Order of insertion in
each group is preserved.

@example
words := ["pear", "plum", "apple", "peach", "apricot"]
groupby(fn(w) => w.substring(0, 1), words)
// {"p": ["pear", "plum", "peach"], "a": ["apple", "apricot"]}

#### `define unique(items: List) -> List`

Distinct elements, preserving the first-seen order. Items are deduped by
their string representation, which matches the rest of Quirk's value-
equality conventions.

@example
unique([3, 1, 4, 1, 5, 9, 2, 6, 5, 3])   // [3, 1, 4, 5, 9, 2, 6]
