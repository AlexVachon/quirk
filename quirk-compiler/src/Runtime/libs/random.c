// ===================================================
//  Random number generator — xoshiro256**
//
//  Naming: Random_<name>  (matches `linkageName = "Random_<name>"`
//  produced by Parser::computeModulePrefix for libs/random/index.quirk).
//
//  Algorithm: xoshiro256** by Blackman & Vigna (CC0). 256-bit state,
//  ~2x faster than MT19937, passes BigCrush, period 2^256-1. The
//  splitmix64 helper is used to expand a single 64-bit seed into the
//  four state words and to avoid the "all-zero state" failure mode.
//  Reference: https://prng.di.unimi.it/
// ===================================================
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "../types.h"

extern void* GC_malloc(size_t);

// Module-global state. Quirk has no per-thread context yet, so a single
// state vector behind the free functions is fine and matches the API
// design (see libs/random/index.quirk).
static uint64_t rng_state[4];

static inline uint64_t rotl(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

// splitmix64 — used only to expand the seed. Standard reference impl.
static uint64_t splitmix64(uint64_t* x) {
    uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static void rng_seed_u64(uint64_t s) {
    uint64_t x = s ? s : 0x12345678DEADBEEFULL;  // never seed with all-zero
    for (int i = 0; i < 4; i++) rng_state[i] = splitmix64(&x);
}

static uint64_t rng_next_u64(void) {
    const uint64_t result = rotl(rng_state[1] * 5, 7) * 9;
    const uint64_t t = rng_state[1] << 17;
    rng_state[2] ^= rng_state[0];
    rng_state[3] ^= rng_state[1];
    rng_state[1] ^= rng_state[2];
    rng_state[0] ^= rng_state[3];
    rng_state[2] ^= t;
    rng_state[3] = rotl(rng_state[3], 45);
    return result;
}

// One-time init — runtime.c calls Random_init() on startup so a fresh
// process gets a fresh stream without the user having to call seed().
// Mixes time + pid so two processes started in the same second still
// diverge. Users can override with seed() at any time.
static int rng_initialized = 0;
void Random_init(void) {
    if (rng_initialized) return;
    uint64_t mix = (uint64_t)time(NULL) * 1000003ULL
                 ^ (uint64_t)getpid() * 2654435761ULL;
    rng_seed_u64(mix);
    rng_initialized = 1;
}

// Public — Quirk's `random.seed(n)`.
void Random_seed(int32_t n) {
    rng_seed_u64((uint64_t)(int64_t)n);
    rng_initialized = 1;
}

// Uniform [0.0, 1.0). Standard xoshiro idiom: take the top 53 bits and
// scale to a double in the right range. Name has the `_next_` prefix so
// the Quirk-side wrapper can own the public `random()` name.
double Random__next_double(void) {
    if (!rng_initialized) Random_init();
    uint64_t v = rng_next_u64() >> 11;          // keep 53 bits
    return (double)v * (1.0 / 9007199254740992.0); // 1 / 2^53
}

// Inclusive [lo, hi]. Backs Quirk's `random.randint` and is also reused
// internally for `choice` and `sample`.
int32_t Random__next_int(int32_t lo, int32_t hi) {
    if (!rng_initialized) Random_init();
    if (hi < lo) { int32_t t = lo; lo = hi; hi = t; }
    uint64_t span = (uint64_t)((int64_t)hi - (int64_t)lo + 1);
    // Lemire's debiased rejection method — uniform across `span`.
    uint64_t x = rng_next_u64();
    __uint128_t m = (__uint128_t)x * (__uint128_t)span;
    uint64_t l = (uint64_t)m;
    if (l < span) {
        uint64_t t = (uint64_t)(-span) % span;
        while (l < t) {
            x = rng_next_u64();
            m = (__uint128_t)x * (__uint128_t)span;
            l = (uint64_t)m;
        }
    }
    return (int32_t)((int64_t)lo + (int64_t)(m >> 64));
}

// Random pick from a List. Returns the boxed void* slot so callers get
// whatever type was stored; the Quirk side does the unwrap.
void* Random_choice(List* list) {
    if (!list) return NULL;
    int n = list->size;
    if (n <= 0) return NULL;
    int32_t idx = Random__next_int(0, n - 1);
    return list->data[idx];
}

// In-place Fisher-Yates shuffle. Returns the same list pointer for
// chaining ergonomics on the Quirk side.
List* Random_shuffle(List* list) {
    if (!list) return list;
    int n = list->size;
    for (int i = n - 1; i > 0; i--) {
        int32_t j = Random__next_int(0, i);
        void* tmp = list->data[i];
        list->data[i] = list->data[j];
        list->data[j] = tmp;
    }
    return list;
}
