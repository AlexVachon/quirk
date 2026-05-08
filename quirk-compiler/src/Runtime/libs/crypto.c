#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gc.h>
#include <openssl/md5.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include "../types.h"

// ---------------------------------------------------------------------------
//  Cryptographic helpers exported to libs/crypto/index.qk.
//  Naming: Crypto_<name>  (matches `linkageName = "Crypto_<name>"` from
//  Parser::computeModulePrefix for libs/crypto/index.qk).
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

// Returns `n` cryptographically-strong random bytes encoded as lowercase hex.
// Falls back to a low-quality LCG if /dev/urandom is unavailable (rare).
String* Crypto_random_hex(int n) {
    if (n <= 0) return make_String("");
    if (n > 4096) n = 4096;
    unsigned char* buf = (unsigned char*)GC_malloc(n);
#ifdef _WIN32
    if (RAND_bytes(buf, n) != 1) {
        for (int i = 0; i < n; i++) buf[i] = (unsigned char)rand();
    }
#else
    FILE* f = fopen("/dev/urandom", "rb");
    if (f) {
        if (fread(buf, 1, n, f) != (size_t)n) {
            for (int i = 0; i < n; i++) buf[i] = (unsigned char)rand();
        }
        fclose(f);
    } else {
        for (int i = 0; i < n; i++) buf[i] = (unsigned char)rand();
    }
#endif
    return hex_digest(buf, n);
}

// Generates a v4 (random) UUID per RFC 4122 §4.4.
// Format: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx where y ∈ {8,9,a,b}.
String* Crypto_uuid() {
    unsigned char b[16];
#ifdef _WIN32
    if (RAND_bytes(b, 16) != 1) {
        for (int i = 0; i < 16; i++) b[i] = (unsigned char)rand();
    }
#else
    FILE* f = fopen("/dev/urandom", "rb");
    if (f) {
        if (fread(b, 1, 16, f) != 16) {
            for (int i = 0; i < 16; i++) b[i] = (unsigned char)rand();
        }
        fclose(f);
    } else {
        for (int i = 0; i < 16; i++) b[i] = (unsigned char)rand();
    }
#endif
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
