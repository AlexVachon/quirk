# `statistics` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `statistics/index.quirk`


### Module-level functions

#### `define mean(items: List) -> Double`

Arithmetic mean. Throws `ValueError` on empty input.

@example
mean([1, 2, 3, 4])   // 2.5

#### `define median(items: List) -> Double`

Median value (50th percentile). For even-length inputs returns the
average of the two middle values.

@example
median([1, 2, 3, 4, 5])   // 3.0
median([1, 2, 3, 4])      // 2.5

#### `define median_low(items: List) -> Double`

Lower median (no averaging — returns one of the input values). For odd
n this matches `median`; for even n it returns the lower of the two
midpoints.

#### `define median_high(items: List) -> Double`

Upper median. Mirror of `median_low`: for even n returns the upper
midpoint.

#### `define mode(items: List) -> String`

Most common element. Ties resolve to the first-seen value. Returns the
stringified form (since elements can be heterogeneous types).

@example
mode([1, 2, 2, 3, 3, 3])   // "3"

#### `define variance(items: List) -> Double`

Sample variance (divides by `n - 1`, the unbiased estimator). Throws if
`n < 2`. Use `pvariance` when treating the input as the entire population.

#### `define pvariance(items: List) -> Double`

Population variance (divides by `n`). Use when the input *is* the
population, not a sample of it.

#### `define stdev(items: List) -> Double`

Sample standard deviation: `sqrt(variance(items))`.

#### `define pstdev(items: List) -> Double`

Population standard deviation: `sqrt(pvariance(items))`.

#### `define min_val(items: List) -> Double`

Smallest value as Double. Throws on empty input.

#### `define max_val(items: List) -> Double`

Largest value as Double. Throws on empty input.

#### `define quantile(items: List, q: Double) -> Double`

The `q`-th quantile of `items` for `q` in `[0.0, 1.0]`. Uses linear
interpolation between the two surrounding sample points. `quantile(xs,
0.5)` is the median; `0.25` is the lower quartile; `0.75` the upper.

@example
quantile([1, 2, 3, 4, 5], 0.25)   // 2.0
quantile([1, 2, 3, 4, 5], 0.75)   // 4.0
