# `math` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `math/index.quirk`


### Module-level functions

#### `extern define sin(x: Double) -> Double`

Sine of `x` (in radians).

#### `extern define cos(x: Double) -> Double`

Cosine of `x` (in radians).

#### `extern define tan(x: Double) -> Double`

Tangent of `x` (in radians). Undefined at `pi/2 + k*pi`.

#### `extern define asin(x: Double) -> Double`

Arc sine — inverse of `sin`. Returns radians in `[-pi/2, pi/2]`. NaN if `|x| > 1`.

#### `extern define acos(x: Double) -> Double`

Arc cosine — inverse of `cos`. Returns radians in `[0, pi]`. NaN if `|x| > 1`.

#### `extern define atan(x: Double) -> Double`

Arc tangent — inverse of `tan`. Returns radians in `(-pi/2, pi/2)`.

#### `extern define atan2(y: Double, x: Double) -> Double`

Two-argument arctangent — angle of the point `(x, y)` from the origin.
Returns radians in `(-pi, pi]`. Handles all four quadrants correctly.
@example math.atan2(1.0, 0.0)  // pi/2

#### `extern define sinh(x: Double) -> Double`

Hyperbolic sine: `(e^x - e^-x) / 2`.

#### `extern define cosh(x: Double) -> Double`

Hyperbolic cosine: `(e^x + e^-x) / 2`.

#### `extern define tanh(x: Double) -> Double`

Hyperbolic tangent: `sinh(x) / cosh(x)`. Range `(-1, 1)`.

#### `extern define exp(x: Double) -> Double`

`e ** x`.

#### `extern define log(x: Double) -> Double`

Natural logarithm (base e). NaN if `x < 0`, `-inf` at 0.

#### `extern define log2(x: Double) -> Double`

Base-2 logarithm.

#### `extern define log10(x: Double) -> Double`

Base-10 logarithm.

#### `extern define pow(base: Double, exponent: Double) -> Double`

`base ** exponent`. Handles fractional and negative exponents.
For Int powers prefer `Int.pow`, which avoids floating-point rounding.

#### `extern define sqrt(x: Double) -> Double`

Square root. NaN if `x < 0`.

#### `extern define cbrt(x: Double) -> Double`

Cube root. Defined for all reals (negative input returns negative root).

#### `extern define floor(x: Double) -> Double`

Largest integer ≤ x, returned as Double. `floor(2.7) == 2.0`.

#### `extern define ceil(x: Double) -> Double`

Smallest integer ≥ x, returned as Double. `ceil(2.1) == 3.0`.

#### `extern define round(x: Double) -> Double`

Round to nearest integer (half-away-from-zero). `round(2.5) == 3.0`.

#### `extern define trunc(x: Double) -> Double`

Truncate toward zero. `trunc(-2.7) == -2.0`.

#### `extern define fmod(a: Double, b: Double) -> Double`

Floating-point remainder of `a / b` with the sign of `a`.
@example math.fmod(10.0, 3.0)  // 1.0

#### `extern define hypot(x: Double, y: Double) -> Double`

Euclidean distance from origin: `sqrt(x*x + y*y)` without intermediate overflow.
@example math.hypot(3.0, 4.0)  // 5.0

#### `extern define abs(x: Int) -> Int`

Absolute value of an Int. Use `abs_f` for Doubles.

#### `extern define fabs(x: Double) -> Double`

Absolute value of a Double. (Wrapped by `abs_f` for naming symmetry.)

#### `extern define sign_int(x: Int) -> Int`

Signum of an Int: -1 (negative), 0, or +1 (positive). Wrapped by `sign_i`.

#### `extern define sign(x: Double) -> Double`

Signum of a Double: -1.0, 0.0, or +1.0. Returns 0 for NaN.

#### `define abs_f(x: Double) -> Double { return fabs(x) }`

Absolute value of a Double. (`abs` is the Int version.)

#### `define sign_i(x: Int) -> Int { return sign_int(x) }`

Sign of an Int: -1, 0, or 1.

#### `extern define is_nan(x: Double) -> Bool`

True if `x` is NaN (Not-a-Number).

#### `extern define is_finite(x: Double) -> Bool`

True if `x` is finite (not `inf`, `-inf`, or NaN).

#### `extern define is_inf(x: Double) -> Bool`

True if `x` is `+inf` or `-inf`.

#### `extern define pi() -> Double`

π — circle's circumference / diameter. ≈ 3.14159265358979

#### `extern define e() -> Double`

e — Euler's number. ≈ 2.71828182845905

#### `extern define tau() -> Double`

τ — full turn in radians. `tau == 2 * pi`. ≈ 6.28318530717959

#### `extern define infinity() -> Double`

Positive infinity (`1.0 / 0.0`).

#### `extern define nan() -> Double`

Not-a-Number (`0.0 / 0.0`). Distinguishable via `is_nan`; `nan != nan`.

#### `define min(a: Int, b: Int) -> Int`

Smaller of two Ints.

#### `define max(a: Int, b: Int) -> Int`

Larger of two Ints.

#### `define clamp(value: Int, lo: Int, hi: Int) -> Int`

Restrict `value` to the closed range [lo, hi].
@example math.clamp(150, 0, 100) // 100

#### `define min_f(a: Double, b: Double) -> Double`

Smaller of two Doubles.

#### `define max_f(a: Double, b: Double) -> Double`

Larger of two Doubles.

#### `define clamp_f(value: Double, lo: Double, hi: Double) -> Double`

Restrict a Double to the closed range `[lo, hi]`.

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

#### `define __init(self, x: Double, y: Double) -> void`

Construct a Vector2 from two Doubles.

#### `define __str(self) -> String`

Compact human-readable form: `v(x, y)`.

#### `define __repr(self) -> String`

Developer-readable form: `Vector2 { x: ..., y: ... }`.

#### `define __add(self, other: Vector2) -> Vector2`

Componentwise addition: `a + b`.

#### `define __sub(self, other: Vector2) -> Vector2`

Componentwise subtraction: `a - b`.

#### `define __mul(self, scalar: Double) -> Vector2`

Scalar multiplication: `v * s` scales each component.

#### `define dot(self, other: Vector2) -> Double`

Dot product: `a · b = a.x*b.x + a.y*b.y`. Useful for projections,
angle calculations (`cos θ = dot(a, b) / (|a| * |b|)`), and detecting
orthogonality (dot == 0).

#### `define __init(self, x: Double, y: Double, z: Double) -> void`

Construct a Vector3 from three Doubles.

#### `define __str(self) -> String`

Compact human-readable form: `Vec3(x, y, z)`.

#### `define __add(self, other: Vector3) -> Vector3`

Componentwise addition: `a + b`.

### `struct Vector3`

A 3D vector with `Double` components. Currently supports `+` only —
extend with `__sub`, `__mul`, `dot`, `cross` as needed.
