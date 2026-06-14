# `time` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `time/index.quirk`


### Module-level functions

#### `extern define unix_now() -> Int`

Current Unix epoch in seconds. -1 if the host clock is unavailable.

#### `extern define year(epoch: Int, utc: Int) -> Int`

Year (e.g. 2026) for `epoch`. `utc=1` for UTC, `0` for local.

#### `extern define month(epoch: Int, utc: Int) -> Int`

Month 1-12 (1=January).

#### `extern define day(epoch: Int, utc: Int) -> Int`

Day of month, 1-31.

#### `extern define hour(epoch: Int, utc: Int) -> Int`

Hour 0-23.

#### `extern define minute(epoch: Int, utc: Int) -> Int`

Minute 0-59.

#### `extern define second(epoch: Int, utc: Int) -> Int`

Second 0-60 (60 for leap seconds, normally 0-59).

#### `extern define weekday(epoch: Int, utc: Int) -> Int`

Day of week, 0-6 (0=Sunday, 6=Saturday).

#### `extern define yearday(epoch: Int, utc: Int) -> Int`

Day of year, 1-366.

#### `extern define to_unix(year: Int, month: Int, day: Int, hour: Int, minute: Int, second: Int, utc: Int) -> Int`

Convert calendar components to a Unix epoch. `utc=1` interprets the inputs
as UTC; `utc=0` as the host's local time zone (DST handled).
Returns -1 if the components are out of range.

#### `extern define format_at(epoch: Int, fmt: String, utc: Int) -> String`

Format `epoch` with a strftime-style spec. Empty spec falls back to
`"%Y-%m-%dT%H:%M:%S"`.

#### `extern define iso_at(epoch: Int, utc: Int) -> String`

ISO-8601 form. UTC: `2026-01-15T12:34:56Z`. Local: `...+05:30` style offset.

#### `extern define parse_iso(s: String) -> Int`

Parse an ISO-8601 string. Trailing `Z` means UTC; absent → local. Returns
-1 on malformed input.
