# `time` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `time/index.quirk`


### Module-level functions

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
