# `datetime` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `datetime/index.quirk`

### `struct DateTime`

Snapshot of a moment in time. Construct via `datetime.now()`,
`datetime.from_unix(s)`, or `datetime.from_iso(s)` rather than calling
`__init` directly.

#### `define format(self, fmt: String) -> String`

Format using a strftime-style spec. Common codes:
  `%Y` 4-digit year   `%m` month 01-12   `%d` day 01-31
  `%H` hour 00-23     `%M` minute 00-59  `%S` second 00-59
  `%A` weekday name   `%a` short day      `%B` month name
  `%j` day-of-year    `%w` weekday 0-6

#### `define from_unix(epoch: Int) -> DateTime`

Build a DateTime from an explicit Unix epoch (seconds), interpreted in the
local time zone.

#### `define from_iso(s: String) -> DateTime`

Parse an ISO-8601 string (`"YYYY-MM-DD[Thh:mm:ss[Z]]"`).
Trailing `Z` → UTC; otherwise local. Throws `ValueError` on bad input.

#### `define make(year: Int, month: Int, day: Int, hour: Int = 0, minute: Int = 0, second: Int = 0) -> DateTime`

Build a DateTime from explicit calendar components (local time).
@throws ValueError if the components are out of range.

#### `define utc_make(year: Int, month: Int, day: Int, hour: Int = 0, minute: Int = 0, second: Int = 0) -> DateTime`

Build a DateTime from explicit calendar components (UTC).
@throws ValueError on out-of-range components.

#### `define is_leap(year: Int) -> Bool`

Standard Gregorian leap-year rule: divisible by 4, except centuries that
aren't divisible by 400. 2000 was leap; 1900 wasn't.

@example
is_leap(2024)   // true
is_leap(2023)   // false
is_leap(2000)   // true
is_leap(1900)   // false

#### `define days_in_month(year: Int, month: Int) -> Int`

Number of days in `month` (1-12) of `year`. Handles February's leap-year
adjustment via `is_leap`.

#### `define is_weekend(dt: DateTime) -> Bool`

True for Saturday and Sunday (where `weekday` is 0=Sunday … 6=Saturday).

#### `define start_of_day(dt: DateTime) -> DateTime`

Truncate `dt` to midnight. Returns a fresh DateTime with the same date
but `hour = minute = second = 0`.

#### `define start_of_week(dt: DateTime) -> DateTime`

Monday of the week containing `dt`, at midnight. Uses ISO weeks
(Monday = first day). If `dt` already falls on Monday, returns midnight
of the same day.

#### `define diff_days(a: DateTime, b: DateTime) -> Int`

Whole days between `a` and `b` (signed). Calendar-day boundaries
aren't honored — this is simply `seconds / 86400`.

#### `define humanize(dt: DateTime, relative_to: DateTime = now()) -> String`

Approximate relative time vs `relative_to`, e.g. "2 hours ago" or
"in 3 days". `relative_to` defaults to "now" via `now()`.
Granularity grows with the gap: seconds → minutes → hours → days →
weeks → months → years.

@example
humanize(yesterday())          // "1 day ago"
humanize(tomorrow())           // "in 1 day"
