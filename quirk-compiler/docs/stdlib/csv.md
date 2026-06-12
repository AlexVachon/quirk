# `csv` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `csv/index.quirk`


### Module-level functions

#### `define parse(text: String, delim: String = ",") -> List`

Parse `text` into a `List<List<String>>` — each outer item is a record,
each inner item is a field. Empty input returns an empty list.

Field rules (RFC 4180):
  - Fields are delimited by `delim` (default `,`).
  - Records end at LF or CRLF.
  - A field can be wrapped in `"..."`. Inside such a field:
      - the delimiter and newlines are taken literally
      - a literal quote is written as `""`
  - Trailing newline at the very end is ignored (no empty extra record).

#### `define parse_dicts(text: String, delim: String = ",") -> List`

Parse `text` taking the first row as a header, returning a
`List<Map<String,String>>`. Each record's keys come from the header row;
missing trailing fields map to the empty String.

#### `define _quote_if_needed(field: String, delim: String) -> String`

Wraps `field` in quotes and doubles internal quotes IF it contains
the delimiter, a quote, or a newline. Otherwise returns it unchanged.

#### `define write_dicts(records: List, delim: String = ",") -> String`

Serialize a `List<Map<String,String>>` with a header row drawn from the
keys of the first record. Records with missing keys emit empty fields.
Returns the empty String for an empty input.

#### `define read_file(path: String, delim: String = ",") -> List`

Read `path` as CSV and parse. Returns `List<List<String>>`.

#### `define write_file(path: String, records: List, delim: String = ",") -> void`

Write `records` to `path` as CSV.
