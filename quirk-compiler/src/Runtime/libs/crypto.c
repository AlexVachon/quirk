#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gc.h>
#include <openssl/md5.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <stdint.h>
#include "../types.h"

// ---------------------------------------------------------------------------
//  Cryptographic helpers exported to libs/crypto/index.quirk.
//  Naming: Crypto_<name>  (matches `linkageName = "Crypto_<name>"` from
//  Parser::computeModulePrefix for libs/crypto/index.quirk).
//
//  Uses OpenSSL's libcrypto (linked via CMake) for correctness and speed.
//  All hash functions take a String, return the digest as a lowercase hex
//  String (so callers can compare or store directly without further decoding).
// ---------------------------------------------------------------------------

// Convert a binary digest of `n` bytes into a 2*n-char lowercase hex string.
// Uses GC_malloc so the returned String is GC-managed.
static String* hex_digest(const unsigned char* digest, int n) {
    char* out = (char*)GC_malloc(n * 2 + 1);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        out[i * 2]     = hex[(digest[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[digest[i]        & 0xF];
    }
    out[n * 2] = '\0';
    return make_String_taking_ownership(out);
}

// Generic helper using EVP — works for any digest OpenSSL supports.
static String* digest_evp(const EVP_MD* md, String* s) {
    if (!s || !s->buffer) return make_String("");
    unsigned char buf[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return make_String("");
    if (EVP_DigestInit_ex(ctx, md, NULL) != 1 ||
        EVP_DigestUpdate(ctx, s->buffer, s->length) != 1 ||
        EVP_DigestFinal_ex(ctx, buf, &len) != 1) {
        EVP_MD_CTX_free(ctx);
        return make_String("");
    }
    EVP_MD_CTX_free(ctx);
    return hex_digest(buf, (int)len);
}

String* Crypto_md5(String* s)    { return digest_evp(EVP_md5(),    s); }
String* Crypto_sha1(String* s)   { return digest_evp(EVP_sha1(),   s); }
String* Crypto_sha256(String* s) { return digest_evp(EVP_sha256(), s); }
String* Crypto_sha512(String* s) { return digest_evp(EVP_sha512(), s); }

// HMAC-SHA256 — used for signed cookies, JWT, webhook verification, etc.
String* Crypto_hmac_sha256(String* key, String* msg) {
    if (!key || !key->buffer || !msg || !msg->buffer) return make_String("");
    unsigned int len = 0;
    unsigned char buf[EVP_MAX_MD_SIZE];
    if (!HMAC(EVP_sha256(),
              key->buffer, key->length,
              (const unsigned char*)msg->buffer, msg->length,
              buf, &len)) {
        return make_String("");
    }
    return hex_digest(buf, (int)len);
}

// ---------------------------------------------------------------------------
//  Random bytes / UUID v4
// ---------------------------------------------------------------------------

extern void quirk_throw_exception(const char* type_name, const char* message);

// `n` bytes from OpenSSL's CSPRNG (seeded by the OS). Raises when there are
// none: no fallback to rand(), whose output is guessable — these bytes are
// session ids, tokens and salts.
static void crypto_secure_bytes(unsigned char* buf, int n) {
    if (RAND_bytes(buf, n) != 1)
        quirk_throw_exception("RuntimeError", "crypto: no secure random bytes available");
}

// Returns `n` cryptographically-strong random bytes encoded as lowercase hex.
String* Crypto_random_hex(int64_t n) {
    if (n <= 0) return make_String("");
    if (n > 4096) n = 4096;
    unsigned char* buf = (unsigned char*)GC_malloc(n);
    crypto_secure_bytes(buf, (int)n);
    return hex_digest(buf, n);
}

// Generates a v4 (random) UUID per RFC 4122 §4.4.
// Format: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx where y ∈ {8,9,a,b}.
String* Crypto_uuid() {
    unsigned char b[16];
    crypto_secure_bytes(b, 16);
    // Set version (top nibble of byte 6) to 4, and variant (top two bits of
    // byte 8) to 10xx — RFC 4122 layout.
    b[6] = (b[6] & 0x0F) | 0x40;
    b[8] = (b[8] & 0x3F) | 0x80;

    char* out = (char*)GC_malloc(37);
    snprintf(out, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3],
             b[4], b[5],
             b[6], b[7],
             b[8], b[9],
             b[10], b[11], b[12], b[13], b[14], b[15]);
    return make_String_taking_ownership(out);
}


// ---------------------------------------------------------------------------
//  Constant-time comparison
// ---------------------------------------------------------------------------

// a == b, taking the same time wherever they differ (a signature or a token
// compared with == tells an attacker how many leading bytes were right).
int32_t Crypto_equal(String* a, String* b) {
    int64_t la = a ? a->length : 0, lb = b ? b->length : 0;
    if (la != lb) return 0;
    if (la == 0) return 1;
    return CRYPTO_memcmp(a->buffer, b->buffer, (size_t)la) == 0;
}

// ---------------------------------------------------------------------------
//  Password hashing: scrypt
// ---------------------------------------------------------------------------
//
// Stored as "$scrypt$ln=15,r=8,p=3$<salt>$<hash>" (base64 without padding):
// the parameters travel with the hash, so they can be raised later
// (needs_rehash) without breaking the passwords already stored.
// N = 2^15, r = 8, p = 3: 32 MiB, one of OWASP's scrypt settings — about a
// tenth of a second, on purpose: guessing passwords from a stolen table has
// to pay that for every guess.

#define QPW_LN   15
#define QPW_R    8
#define QPW_P    3
#define QPW_SALT 16
#define QPW_KEY  32

static void qpw_b64(const unsigned char* in, int n, char* out) {
    int len = EVP_EncodeBlock((unsigned char*)out, in, n);
    while (len > 0 && out[len - 1] == '=') out[--len] = 0;
}

// Decode `len` chars of base64 without padding into `out` (at most `max`
// bytes); the byte count, or -1.
static int qpw_unb64(const char* in, size_t len, unsigned char* out, int max) {
    if (len == 0 || len > 128) return -1;
    char buf[136];
    memcpy(buf, in, len);
    size_t pad = (4 - len % 4) % 4;
    if (pad == 3) return -1;
    for (size_t i = 0; i < pad; i++) buf[len + i] = '=';
    buf[len + pad] = 0;
    unsigned char tmp[104];
    int n = EVP_DecodeBlock(tmp, (const unsigned char*)buf, (int)(len + pad));
    if (n < 0) return -1;
    n -= (int)pad;
    if (n <= 0 || n > max) return -1;
    memcpy(out, tmp, (size_t)n);
    return n;
}

static int qpw_scrypt(String* pw, const unsigned char* salt, int saltlen,
                      int ln, int r, int p, unsigned char* key, int keylen) {
    uint64_t N = 1ULL << ln;
    uint64_t maxmem = 128ULL * (uint64_t)r * (N + (uint64_t)p + 2) + (1ULL << 20);
    quirk_gil_release();                     // other async tasks run meanwhile
    int ok = EVP_PBE_scrypt(pw && pw->buffer ? pw->buffer : "", pw ? (size_t)pw->length : 0,
                            salt, (size_t)saltlen, N, (uint64_t)r, (uint64_t)p, maxmem,
                            key, (size_t)keylen);
    quirk_gil_acquire();
    return ok == 1;
}

// The parts of a stored hash. 0 when it isn't one of ours, or its
// parameters are out of range (a value from a database is checked before
// it decides how much memory to use).
static int qpw_parse(String* stored, int* ln, int* r, int* p,
                     unsigned char* salt, int* saltlen, unsigned char* key, int* keylen) {
    if (!stored || !stored->buffer) return 0;
    const char* s = stored->buffer;
    int used = 0;
    if (sscanf(s, "$scrypt$ln=%d,r=%d,p=%d$%n", ln, r, p, &used) != 3 || used == 0) return 0;
    if (*ln < 10 || *ln > 20 || *r < 1 || *r > 32 || *p < 1 || *p > 16) return 0;
    const char* saltp = s + used;
    const char* dollar = strchr(saltp, '$');
    if (!dollar) return 0;
    *saltlen = qpw_unb64(saltp, (size_t)(dollar - saltp), salt, 64);
    *keylen = qpw_unb64(dollar + 1, strlen(dollar + 1), key, 64);
    return *saltlen >= 8 && *keylen >= 16;
}

String* Crypto_hash_password(String* pw) {
    unsigned char salt[QPW_SALT], key[QPW_KEY];
    crypto_secure_bytes(salt, QPW_SALT);
    if (!qpw_scrypt(pw, salt, QPW_SALT, QPW_LN, QPW_R, QPW_P, key, QPW_KEY))
        quirk_throw_exception("RuntimeError", "crypto: scrypt failed");
    char sb[32], kb[64];
    qpw_b64(salt, QPW_SALT, sb);
    qpw_b64(key, QPW_KEY, kb);
    char* out = (char*)GC_malloc(160);
    snprintf(out, 160, "$scrypt$ln=%d,r=%d,p=%d$%s$%s", QPW_LN, QPW_R, QPW_P, sb, kb);
    return make_String_taking_ownership(out);
}

// Is `pw` the password `stored` (from hash_password) was made from? False
// for anything that isn't such a hash.
int32_t Crypto_verify_password(String* pw, String* stored) {
    int ln, r, p, saltlen, keylen;
    unsigned char salt[64], key[64], again[64];
    if (!qpw_parse(stored, &ln, &r, &p, salt, &saltlen, key, &keylen)) return 0;
    if (!qpw_scrypt(pw, salt, saltlen, ln, r, p, again, keylen)) return 0;
    return CRYPTO_memcmp(key, again, (size_t)keylen) == 0;
}

// Made with weaker parameters than hash_password uses now (or not one of
// ours): hash the password again when its owner next logs in.
int32_t Crypto_needs_rehash(String* stored) {
    int ln, r, p, saltlen, keylen;
    unsigned char salt[64], key[64];
    if (!qpw_parse(stored, &ln, &r, &p, salt, &saltlen, key, &keylen)) return 1;
    return ln < QPW_LN || r < QPW_R || p < QPW_P || keylen < QPW_KEY;
}
