# `crypto` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `crypto/index.quirk`


### Module-level functions

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
