# `datetime` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `datetime/index.quirk`

### `struct DateTime`

Snapshot of a moment in time. Construct via `datetime.now()`,
`datetime.from_unix(s)`, or `datetime.from_iso(s)` rather than calling
`__init` directly.

#### `define iso(self) -> String`

ISO-8601 representation.

#### `define format(self, fmt: String) -> String`

Format using a strftime-style spec. Common codes:
  `%Y` 4-digit year   `%m` month 01-12   `%d` day 01-31
  `%H` hour 00-23     `%M` minute 00-59  `%S` second 00-59
  `%A` weekday name   `%a` short day      `%B` month name
  `%j` day-of-year    `%w` weekday 0-6

#### `define add_seconds(self, n: Int) -> DateTime`

Returns a new DateTime offset by `n` seconds (can be negative).

#### `define diff_seconds(self, other: DateTime) -> Int`

`self - other` in seconds. Negative if `self` is earlier.

#### `define __str(self) -> String`

Default str: ISO-8601.

#### `define now() -> DateTime`

Current local DateTime.

#### `define utc_now() -> DateTime`

Current UTC DateTime.

#### `define from_unix(epoch: Int) -> DateTime`

Build a DateTime from an explicit Unix epoch (seconds), interpreted in the
local time zone.

#### `define utc_from_unix(epoch: Int) -> DateTime`

Build a DateTime from an explicit Unix epoch, interpreted as UTC.

#### `define from_iso(s: String) -> DateTime`

Parse an ISO-8601 string (`"YYYY-MM-DD[Thh:mm:ss[Z]]"`).
Trailing `Z` → UTC; otherwise local. Throws `ValueError` on bad input.

#### `define make(year: Int, month: Int, day: Int, hour: Int = 0, minute: Int = 0, second: Int = 0) -> DateTime`

Build a DateTime from explicit calendar components (local time).
@throws ValueError if the components are out of range.

#### `define utc_make(year: Int, month: Int, day: Int, hour: Int = 0, minute: Int = 0, second: Int = 0) -> DateTime`

Build a DateTime from explicit calendar components (UTC).
@throws ValueError on out-of-range components.

#### `define today() -> DateTime`

The current local date+time. Synonym for `now()`.

#### `define tomorrow() -> DateTime`

One day in the future, same time-of-day.

#### `define yesterday() -> DateTime`

One day in the past, same time-of-day.

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

#### `define day_name(dt: DateTime) -> String`

Full English weekday name for `dt`. `weekday` is 0=Sunday … 6=Saturday.

#### `define month_name(month: Int) -> String`

Full English month name for `month` (1-12).

#### `define start_of_day(dt: DateTime) -> DateTime`

Truncate `dt` to midnight. Returns a fresh DateTime with the same date
but `hour = minute = second = 0`.

#### `define start_of_week(dt: DateTime) -> DateTime`

Monday of the week containing `dt`, at midnight. Uses ISO weeks
(Monday = first day). If `dt` already falls on Monday, returns midnight
of the same day.

#### `define start_of_month(dt: DateTime) -> DateTime`

First day of `dt`'s month, at midnight.

#### `define start_of_year(dt: DateTime) -> DateTime`

January 1 of `dt`'s year, at midnight.

#### `define diff_minutes(a: DateTime, b: DateTime) -> Int`

Whole minutes between `a` and `b` (signed; `b - a`).

#### `define diff_hours(a: DateTime, b: DateTime) -> Int`

Whole hours between `a` and `b` (signed).

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
