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

#### `extern define hash_password(password: String) -> String`

What to store for a password: a salted scrypt hash, as one String —
`$scrypt$ln=15,r=8,p=3$<salt>$<hash>` (the settings travel with it). It
takes about a tenth of a second and 32 MiB, on purpose: whoever steals
the table pays that for every guess. Two calls with the same password
give different Strings (a new random salt each time); check one with
`verify_password`. Never store the password itself, nor a plain sha256.

@example
db.insert("INSERT INTO users (email, password) VALUES (?, ?)", [email, crypto.hash_password(pw)])

#### `extern define verify_password(password: String, stored: String) -> Bool`

Is `password` the one `stored` (from hash_password) was made from? False
for a wrong password — and for anything that isn't such a hash. Compared
in constant time.

@example
row := db.query_one("SELECT password FROM users WHERE email = ?", [email])
if row == null or not crypto.verify_password(pw, String(row["password"])) {
    self.set_error("password", "Wrong email or password")
}

#### `extern define needs_rehash(stored: String) -> Bool`

True when `stored` was made with weaker settings than hash_password uses
now (or isn't such a hash): when its owner logs in — you have the
password then — store `hash_password(password)` instead.

#### `extern define equal(a: String, b: String) -> Bool`

`a == b` in constant time: the time taken doesn't depend on where they
differ. Use it for secrets — signatures, tokens, API keys — where `==`
lets an attacker find the right value byte by byte from how long the
comparison took. (The lengths aren't secret: different lengths are
just false.)
