# `math` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `math/index.quirk`


### Module-level functions

#### `extern define atan2(y: Double, x: Double) -> Double`

Two-argument arctangent — angle of the point `(x, y)` from the origin.
Returns radians in `(-pi, pi]`. Handles all four quadrants correctly.
@example math.atan2(1.0, 0.0)  // pi/2

#### `extern define pow(base: Double, exponent: Double) -> Double`

`base ** exponent`. Handles fractional and negative exponents.
For Int powers prefer `Int.pow`, which avoids floating-point rounding.

#### `extern define fmod(a: Double, b: Double) -> Double`

Floating-point remainder of `a / b` with the sign of `a`.
@example math.fmod(10.0, 3.0)  // 1.0

#### `extern define hypot(x: Double, y: Double) -> Double`

Euclidean distance from origin: `sqrt(x*x + y*y)` without intermediate overflow.
@example math.hypot(3.0, 4.0)  // 5.0

#### `define abs_f(x: Double) -> Double { return fabs(x) }`

Absolute value of a Double. (`abs` is the Int version.)

#### `define sign_i(x: Int) -> Int { return sign_int(x) }`

Sign of an Int: -1, 0, or 1.

#### `define min(a: Int, b: Int) -> Int`

Smaller of two Ints.

#### `define max(a: Int, b: Int) -> Int`

Larger of two Ints.

#### `define clamp(value: Int, lo: Int, hi: Int) -> Int`

Restrict `value` to the closed range [lo, hi].
@example math.clamp(150, 0, 100) // 100

#### `extern define seed(s: Int) -> void`

Seed the pseudo-random generator. The same seed always produces the same
sequence of `random()` / `random_int()` results — useful for reproducible
tests and procedural generation.

#### `extern define random() -> Double`

Uniform Double in `[0.0, 1.0)`. 53 bits of precision.

#### `extern define random_int(lo: Int, hi: Int) -> Int`

Uniform Int in `[lo, hi]` (inclusive on both ends).
Returns `lo` if `hi <= lo`.
@example math.random_int(1, 6)  // dice roll

#### `define choice(items: List) -> Any`

Pick one element of `list` uniformly at random.
@throws ValueError if the list is empty.

#### `define shuffle(items: List) -> List`

Shuffle `items` in place using Fisher-Yates. Returns the same list for chaining.


## `math/vectors.quirk`

### `struct Vector2`

A 2D vector with `Double` components and operator overloading for
`+`, `-`, and scalar `*`.

#### `define dot(self, other: Vector2) -> Double`

Dot product: `a · b = a.x*b.x + a.y*b.y`. Useful for projections,
angle calculations (`cos θ = dot(a, b) / (|a| * |b|)`), and detecting
orthogonality (dot == 0).

### `struct Vector3`

A 3D vector with `Double` components. Currently supports `+` only —
extend with `__sub`, `__mul`, `dot`, `cross` as needed.
