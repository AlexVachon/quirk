# `sqlite` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `sqlite/index.quirk`

### `struct Error : RuntimeError`

What went wrong in the database: a constraint (a UNIQUE / NOT NULL / CHECK
/ FOREIGN KEY), bad SQL, a locked or read-only file, a closed database.
  `code` — SQLite's result code: `code % 256` is the primary one
           (19 a constraint, 1 an SQL error, 5 busy, 8 read-only …);
           the whole is the extended one (2067 UNIQUE, 1299 NOT NULL …)
  `sql`  — the statement, when there was one

@example:
try {
    db.execute("INSERT INTO users (email) VALUES (?)", [email])
} catch (e: sqlite.Error) {
    if e.is_constraint() { print("already taken") } else { throw }
}

#### `define is_constraint(self) -> Bool`

A constraint failed: UNIQUE, NOT NULL, CHECK, FOREIGN KEY, PRIMARY KEY.

#### `define is_busy(self) -> Bool`

Another connection holds the lock (after waiting `busy_ms`).

### `struct Database`

A connection to a database (`sqlite.open`). Each method runs one statement
and is done with it: nothing to finalize or reset.

Parameters: `?` in the SQL with a List (in order), or `:name` with a Map:
    db.query("SELECT * FROM t WHERE a = ? AND b = ?", [1, "x"])
    db.query("SELECT * FROM t WHERE a = :a AND b = :b", {"a": 1, "b": "x"})
Values go in as they are — never pasted into the SQL, so they can't change
it. Int, Double, String, Bool (stored as 1 / 0) and null.

Rows come back as Maps, column name → value: Int, Double, String or null
(SQLite has no Bool: a stored `true` reads back as 1).

#### `define close(self) -> void`

Close it. Closing twice is fine; using it after raises sqlite.Error.

#### `define execute(self, sql: String, params: Any = null) -> Int`

Run a statement that returns no rows (INSERT, UPDATE, DELETE, CREATE …).
Returns how many rows it changed.

#### `define insert(self, sql: String, params: Any = null) -> Int`

Run an INSERT; returns the new row's id (its INTEGER PRIMARY KEY).

#### `define execute_many(self, sql: String, rows: List) -> Int`

Run the same statement once per item of `rows` (each a List or a Map of
parameters), in one transaction. Returns how many rows changed in all.

#### `define query(self, sql: String, params: Any = null) -> List`

Every row of a SELECT, as Maps (column name → value).

#### `define query_one(self, sql: String, params: Any = null) -> Map`

The first row of a SELECT, or null when there's none.

#### `define value(self, sql: String, params: Any = null, default: Any = null) -> Any`

The first column of the first row (`SELECT count(*) …`), or `default`
when there's no row.

#### `define columns(self, sql: String) -> List`

The column names a SELECT gives (without running it).

#### `define script(self, sql: String) -> void`

Run several statements, separated by `;`, with no parameters: a schema,
a migration.

#### `define transaction(self) -> Transaction`

`with db.transaction() { … }`: the statements inside are done all
together or not at all. At the end of the block they're committed; if
it raises, they're rolled back (and the exception goes on). Inside
another transaction it's a savepoint: only its own part is undone.

#### `define in_transaction(self) -> Bool`

In a transaction now (`transaction()`, or a BEGIN of your own).

#### `define changes(self) -> Int`

How many rows the last INSERT / UPDATE / DELETE changed.

#### `define last_id(self) -> Int`

The id of the last row inserted on this connection.

#### `define tables(self) -> List`

The names of its tables.

### `struct Transaction`

A transaction (`db.transaction()`), for `with`: committed at the end of
the block, rolled back if it raises. Inside another one: a savepoint.


### Module-level functions

#### `extern define version() -> String`

The version of SQLite built into Quirk ("3.54.1").

#### `define open(path: String, read_only: Bool = false, must_exist: Bool = false, busy_ms: Int = 5000) -> Database`

Open the database in the file at `path` — made if it isn't there, unless
`must_exist`. `":memory:"`: a new database in memory, gone when it's closed.
  `read_only` — no writes (a write raises sqlite.Error)
  `busy_ms`   — when another connection is writing: how long to wait for
                it before raising (default 5 s)
Close it with `db.close()`, or use `with sqlite.open(path) as db { … }`.
