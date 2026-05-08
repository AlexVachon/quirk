#include <math.h>
#include <stdlib.h>
#include "../types.h"

// ---------------------------------------------------------------------------
//  Math runtime functions exported to libs/math/index.qk.
//  Naming: every symbol is prefixed Math_ to match `linkageName = "Math_<name>"`
//  produced by Parser::computeModulePrefix for libs/math/index.qk.
// ---------------------------------------------------------------------------

// --- Trigonometry (radians) ---
double Math_sin(double x)   { return sin(x); }
double Math_cos(double x)   { return cos(x); }
double Math_tan(double x)   { return tan(x); }
double Math_asin(double x)  { return asin(x); }
double Math_acos(double x)  { return acos(x); }
double Math_atan(double x)  { return atan(x); }
double Math_atan2(double y, double x) { return atan2(y, x); }

// --- Hyperbolic ---
double Math_sinh(double x) { return sinh(x); }
double Math_cosh(double x) { return cosh(x); }
double Math_tanh(double x) { return tanh(x); }

// --- Exponential / logarithmic ---
double Math_exp(double x)   { return exp(x); }
double Math_log(double x)   { return log(x); }    // natural log
double Math_log2(double x)  { return log2(x); }
double Math_log10(double x) { return log10(x); }

// --- Power / roots ---
double Math_pow(double base, double exponent) { return pow(base, exponent); }
double Math_sqrt(double x) { return sqrt(x); }
double Math_cbrt(double x) { return cbrt(x); }

// --- Rounding (Double->Double; Quirk wrappers can cast to Int) ---
double Math_floor(double x) { return floor(x); }
double Math_ceil(double x)  { return ceil(x);  }
double Math_round(double x) { return round(x); }
double Math_trunc(double x) { return trunc(x); }

// --- Misc ---
double Math_fmod(double a, double b)  { return fmod(a, b); }
double Math_hypot(double x, double y) { return hypot(x, y); }
double Math_fabs(double x) { return fabs(x); }
int    Math_abs(int x)     { return x < 0 ? -x : x; }

// Signum: -1, 0, +1 for negative/zero/positive. Returns 0 for NaN.
int Math_sign_int(int x) {
    if (x > 0) return  1;
    if (x < 0) return -1;
    return 0;
}
double Math_sign(double x) {
    if (x > 0.0) return  1.0;
    if (x < 0.0) return -1.0;
    return 0.0;
}

// Lightweight predicate helpers — Quirk Bool returns are i32 in the C ABI.
int Math_is_nan(double x)    { return isnan(x)    ? 1 : 0; }
int Math_is_finite(double x) { return isfinite(x) ? 1 : 0; }
int Math_is_inf(double x)    { return isinf(x)    ? 1 : 0; }

// Constants exposed as 0-arg functions because Quirk doesn't yet support
// module-level value declarations from extern.
double Math_pi()       { return 3.14159265358979323846; }
double Math_e()        { return 2.71828182845904523536; }
double Math_tau()      { return 6.28318530717958647692; }
double Math_infinity() { return INFINITY; }
double Math_nan()      { return NAN; }

// ---------------------------------------------------------------------------
//  Pseudo-random (linear congruential, deterministic and portable).
//  We use our own state instead of rand()/srand() so tests can pin a seed
//  without disturbing libc state another lib might depend on.
// ---------------------------------------------------------------------------

static unsigned long long math_rng_state = 0x9E3779B97F4A7C15ULL;

void Math_seed(int s) {
    math_rng_state = (unsigned long long)(unsigned int)s * 0x9E3779B97F4A7C15ULL + 1;
    if (math_rng_state == 0) math_rng_state = 1;
}

// xorshift64* — fast, non-crypto, reasonable distribution.
static unsigned long long math_rng_next() {
    unsigned long long x = math_rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    math_rng_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

// Returns a pseudo-random Int in [lo, hi] (inclusive). Falls back to lo
// if the range is degenerate to avoid signed division surprises.
int Math_random_int(int lo, int hi) {
    if (hi <= lo) return lo;
    unsigned long long span = (unsigned long long)((long long)hi - (long long)lo + 1LL);
    return lo + (int)(math_rng_next() % span);
}

// Returns a pseudo-random Double in [0.0, 1.0).
double Math_random() {
    // 53 high bits → uniform double in [0, 1)
    unsigned long long bits = math_rng_next() >> 11;
    return (double)bits / (double)(1ULL << 53);
}
