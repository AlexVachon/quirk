# `encoding` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `encoding/base64.quirk`


### Module-level functions

#### `extern define encode(data: String) -> String`

Encode `data` to a Base64 String. Output length is roughly 4/3 of input,
rounded up to a multiple of 4 with `=` padding.

#### `extern define decode(data: String) -> String`

Decode a Base64 String back to the original. Tolerant of whitespace and
missing padding; returns "" on malformed input.


## `encoding/hex.quirk`


### Module-level functions

#### `extern define encode(data: String) -> String`

Encode each byte of `data` as two lowercase hex digits.
Output length is exactly `2 * data.length()`.

#### `extern define decode(data: String) -> String`

Decode a hex String back to its original bytes. Accepts mixed case.
Returns "" if the input length is odd or contains non-hex characters.


## `encoding/json.quirk`

### `struct JsonMap : ISerializable`

Wraps a Map as an ISerializable for net.http's `json:` parameter.

@example
http.post(url, json: JsonMap(m))

### `struct JsonList : ISerializable`

Wraps a List as an ISerializable for net.http's `json:` parameter.

@example
http.post(url, json: JsonList(l))


### Module-level functions

#### `define dumps_pretty(val: Any, indent: Int = 2) -> String`

Pretty-print `val` with `indent` spaces per nesting level. Indented JSON
is much easier to read for config files and debug output; pass `indent=4`
for a more spacious layout.

@example
m := Map()
m.put("name", "Alice")
m.put("scores", [98, 87, 92])
print(json.dumps_pretty(m))
// {
//   "name": "Alice",
//   "scores": [
//     98,
//     87,
//     92
//   ]
// }

#### `define loads(s: String) -> Any`

Parse a JSON string and return the corresponding Quirk value with
real types — no manual coercion required.

  JSON object  → `Map`     (String keys, mixed-type values)
  JSON array   → `List`    (mixed-type elements)
  JSON string  → `String`
  JSON number  → `Int` (no decimal/exponent) or `Double`
  JSON boolean → `Bool`
  JSON null    → null

@param s  A valid JSON string.
@returns  A Map, List, String, or boxed primitive.

@example
data: Map = json.loads('{"name":"Alice","age":30,"active":true}')
print(data.get("name"))     // Alice  (String)
print(data.get("age"))      // 30     (Int — usable in arithmetic)
print(data.get("active"))   // true   (Bool)

#### `define dump(val: Any, file: File) -> void`

Serialize `val` and write the JSON string to an open File.

@example
with File("config.json", "w") as f {
    json.dump(config_map, f)
}

#### `define load(file: File) -> Any`

Read the entire contents of `file` and parse as JSON.

@example
with File("config.json", "r") as f {
    config: Map = json.load(f)
    print(config.get("host"))
}
