# `crypto` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `crypto/index.quirk`


### Module-level functions

#### `extern define md5(s: String) -> String`

MD5. Fast but cryptographically broken — use only for non-security checksums.

#### `extern define sha1(s: String) -> String`

SHA-1. Deprecated for new security work; ok for legacy compatibility.

#### `extern define sha256(s: String) -> String`

SHA-256. Current default for integrity, file checksums, content hashes.

#### `extern define sha512(s: String) -> String`

SHA-512. Larger output (128 hex chars); use when 256 bits aren't enough.

#### `extern define hmac_sha256(key: String, msg: String) -> String`

HMAC-SHA256 — message authentication code. Use for signed cookies, JWT
HS256, webhook signature verification, etc. Returns 64 hex chars.

#### `extern define random_hex(n: Int) -> String`

Cryptographically-strong random bytes, returned as a 2n-character lowercase
hex String. Suitable for tokens, salts, session IDs.
@example  token := crypto.random_hex(32)   // 64 hex chars = 256 random bits

#### `extern define uuid() -> String`

RFC 4122 v4 UUID. 36 chars: `xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx` where
the version is fixed at 4 and `y` is one of {8, 9, a, b}.
