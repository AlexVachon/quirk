# `typing` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `typing/callable.quirk`

### `struct Callable`

A first-class function value produced by a lambda expression (`fn(...) => ...`).
Callables can be stored in variables, passed as arguments, and invoked
by higher-order methods such as `List.map`, `List.filter`, and `List.each`.

@example:
double := fn(x: Int) => x * 2
print(double(21))                      // 42
evens := [1, 2, 3, 4].filter(fn(x: Int) => x % 2 == 0)

#### `extern define __str(self) -> String`

String representation of this callable.
@returns:
    String — always `"<Callable>"`


## `typing/collections/list.quirk`

### `struct List : Printable, Representable, Sizeable, Iterable`

A dynamic, ordered sequence of values of any type.
Elements are zero-indexed. Lists grow automatically as items are appended.

@example:
nums := [1, 2, 3]
nums.append(4)
print(nums.length())    // 4
print(nums[0])          // 1
doubled := nums.map(fn(x: Int) => x * 2)

#### `define append(self, item: Any) -> void`

Appends a value to the end of the list.
@param item: Any — the value to add

#### `define append_all(self, other: List) -> void`

Append every element of `other` to `self` in place. Mutates
`self`; returns nothing. Use `+` if you want a fresh List
instead of mutating. (Named `append_all` rather than the
Python-conventional `extend` because `extend` is a reserved
keyword in Quirk — `extend Foo { ... }` blocks add methods to
an existing struct.)

@example xs.append_all([4, 5, 6])  // xs is now [1, 2, 3, 4, 5, 6]

#### `define __add(self, other: List) -> List`

Concatenation. Returns a fresh List containing every element
of `self` followed by every element of `other`. Neither input
is mutated. Called by `xs + ys` at the call site.

@example zs := [1, 2] + [3, 4]   // [1, 2, 3, 4]

#### `define __mul(self, n: Int) -> List`

Repetition. Returns a fresh List with self's elements
repeated `n` times. `n <= 0` returns the empty list (Python
semantics). Element values are SHARED — same shape as
Python's `[0] * 5`; reassigning one element via index in
one row doesn't affect the others, but mutating a shared
inner object would.

@example zs := [0] * 5           // [0, 0, 0, 0, 0]
@example pairs := [1, 2] * 3     // [1, 2, 1, 2, 1, 2]

#### `define insert(self, index: Int, item: Any) -> void`

Put `item` before index `index` (0 to the length: at the end; a
negative index counts from the end). The elements after it move up.
@example xs.insert(0, "first")

#### `define remove_at(self, index: Int) -> Any`

Remove the element at `index` (negative: from the end) and return it.
@throws IndexError — if there is no such element
@example second := xs.remove_at(1)

#### `define slice(self, start: Int, end: Int = 9223372036854775807) -> List`

A new list of the elements from `start` up to (not including) `end`;
negative indexes count from the end, and out-of-range ones are
clamped (`[1, 2, 3, 4].slice(1, 3)` is `[2, 3]`; `xs.slice(-2, 99)`
the last two). The default `end` is the end.
@example rest := xs.slice(1)

#### `define pop(self) -> Any`

Removes and returns the last element.
@returns: Any — the removed element
@throws IndexError — if the list is empty

#### `define length(self) -> Int`

Returns the number of elements.
@returns: Int — element count

#### `define clear(self) -> void`

Removes all elements from the list.

#### `define is_empty(self) -> Bool`

Returns `true` if the list has no elements.
@returns: Bool

#### `define __get(self, index: Int) -> Any`

Returns the element at the given index.
@param index: Int — zero-based position (negative indices not supported)
@returns: Any — the element at that index
@throws IndexError — if index is out of bounds

#### `define __set(self, index: Int, item: Any) -> void`

Sets the element at the given index.
@param index: Int — zero-based position
@param item: Any — the new value
@throws IndexError if index is out of bounds

#### `define map(self, cb: Callable) -> List`

Transforms each element and returns a new list.
@param cb: Callable — `fn(element: T) => U`
@returns: List — new list of transformed values

@example:
doubled := [1, 2, 3].map(fn(x: Int) => x * 2)  // [2, 4, 6]

#### `define filter(self, cb: Callable) -> List`

Returns a new list containing only elements for which `cb` returns `true`.
@param cb: Callable — `fn(element: T) => Bool`
@returns: List — filtered list

@example:
evens := [1, 2, 3, 4].filter(fn(x: Int) => x % 2 == 0)  // [2, 4]

#### `define sort(self, cb: Callable = null) -> List`

Sorts the list in place (stable) and returns it, for chaining.
With no comparator, values sort in natural order: numbers
numerically, Strings alphabetically. With one, `cb(a, b)` returns
a negative Int when `a` goes first, positive when after, zero when
equal.

@param cb: Callable — optional `fn(a, b) => Int`
@returns: List — the (now-sorted) self

@example:
[3, 1, 2].sort()                        // [1, 2, 3]
["b", "a"].sort()                       // ["a", "b"]
[3, 1, 2].sort(fn(a, b) => b - a)       // [3, 2, 1]

#### `define each(self, cb: Callable) -> void`

Calls `cb` for each element. Returns nothing.
@param cb: Callable — `fn(element: T) => void`

@example:
[1, 2, 3].each(fn(x: Int) => print(x))

#### `define reduce(self, initial: Any, cb: Callable) -> Any`

Folds all elements into a single accumulated value.
@param initial: Any — the starting accumulator value
@param cb: Callable — `fn(acc: T, element: T) => T`
@returns: Any — final accumulated value

@example:
sum := [1, 2, 3].reduce(0, fn(acc: Int, x: Int) => acc + x)  // 6

#### `define any(self, cb: Callable) -> Bool`

Returns `true` if at least one element satisfies the predicate.
@param cb: Callable — `fn(element: T) => Bool`
@returns: Bool

#### `define all(self, cb: Callable) -> Bool`

Returns `true` if every element satisfies the predicate.
@param cb: Callable — `fn(element: T) => Bool`
@returns: Bool

#### `define find(self, cb: Callable) -> Any`

Returns the first element that satisfies the predicate, or `null` if none found.
@param cb: Callable — `fn(element: T) => Bool`
@returns: Any — the matching element, or `null`

#### `define first(self) -> Any`

Returns the first element, or `null` when the list is empty.
Same shape as `xs.__get(0)` but null-safe (no index-out-of-range
on empty input).

@example [10, 20, 30].first()   // 10
@example List().first()         // null

#### `define last(self) -> Any`

Returns the last element, or `null` when the list is empty.
Same shape as `xs.__get(-1)` but null-safe and slightly clearer
at the call site.

@example [10, 20, 30].last()    // 30
@example List().last()          // null

#### `define reverse(self) -> List`

Returns a fresh List with elements in reverse order. Does not
mutate `self`.

@example [1, 2, 3].reverse()    // [3, 2, 1]

#### `define sum(self) -> Any`

Sum of all elements. Reduces via `+` starting from 0. Works
for any numeric type — Sema's `+` handles Int/Double widening
so a mixed [Int, Double, ...] list returns a Double total.

@example [1, 2, 3].sum()        // 6
@example List().sum()           // 0

#### `define min(self) -> Any`

Smallest element (by `<`). Returns `null` on an empty list.
Uses the first element as the seed so heterogeneous lists
don't have to start from a sentinel like `Int.MAX_VALUE`.

@example [3, 1, 4, 1, 5].min()  // 1
@example List().min()           // null

#### `define max(self) -> Any`

Largest element (by `>`). Returns `null` on an empty list.
Same shape as `min()`.

@example [3, 1, 4, 1, 5].max()  // 5

#### `define unique(self) -> List`

Returns a fresh List with duplicate elements removed. Order
is preserved: the first occurrence wins. Equality uses `==`
so user structs with `__eq` overloads dedup correctly.

@example [1, 2, 1, 3, 2].unique()    // [1, 2, 3]
@example ["a", "b", "a"].unique()    // ["a", "b"]

#### `define to_set(self) -> Set`

Materialise the list's elements into a fresh `Set`, dropping
duplicates. Useful for quick dedup with set semantics or for
O(1) membership tests after building from a list.

Note: this introduces a `from .set` import inside the List
package — keep that in mind if you're rebuilding the package
graph from scratch.

@example pending := [1, 2, 1, 3].to_set()   // {1, 2, 3}

#### `define count(self, value: Any) -> Int`

Counts how many elements equal `value`. Linear scan; uses `==`
so user structs with `__eq` overloads count correctly.

@example [1, 2, 1, 1, 3].count(1)    // 3


## `typing/collections/map.quirk`

### `struct Map : Printable, Sizeable, Iterable`

A hash map (dictionary) that associates `String` keys with values of any type.
Insertion order is not guaranteed. All keys must be `String`.

@example:
m := {}
m.put("name", "Alice")
m.put("age", 30)
print(m.get("name"))    // "Alice"
print(m.has("age"))     // true
m.remove("age")
print(m.length())       // 1

#### `define put(self, key: String, value: Any) -> void`

Inserts or updates a key-value pair.
@param key: String — the key
@param value: Any — the value to associate

#### `define put_all(self, other: Map) -> void`

Copy every entry from `other` into `self`, overwriting any
same-key values with other's. Mutates `self`; returns nothing.
Use `+` if you want a fresh merged Map instead.

@example m.put_all({ "auth": "token" })   // updates m in place

#### `define __add(self, other: Map) -> Map`

Merge two Maps into a fresh result. Right side wins on key
collision — `{a: 1} + {a: 2}` is `{a: 2}`. Mirrors Python's
`{**m1, **m2}` precedence. Neither input is mutated. Called
via `m1 + m2`.

#### `define get(self, key: String) -> Any`

Returns the value for the given key, or `null` if not found.
@param key: String — the key to look up
@returns: Any — the associated value, or `null`

#### `define get_or(self, key: String, default: Any) -> Any`

Returns the value for `key`, or `default` when the key isn't
present. Matches Python's `dict.get(key, default)`. Avoids the
`if m.has(k) { m.get(k) } else { d }` pattern.

@example m.get_or("retries", 3)
@example headers.get_or("Content-Type", "application/json")

#### `define has(self, key: String) -> Bool`

Returns `true` if the key exists in the map.
@param key: String
@returns: Bool

#### `define remove(self, key: String) -> void`

Removes the entry with the given key. Does nothing if the key is absent.
@param key: String

#### `define length(self) -> Int`

Returns the number of key-value pairs.
@returns: Int

#### `define keys(self) -> List`

Returns all keys as a `List` of `String`.
@returns: List

#### `define items(self) -> List`

Returns the map's entries as a List of `[key, value]` lists.
Order matches insertion order (matches `keys()` / `values()`).
Useful for pairwise iteration:

  for pair in m.items() {
      k := pair.__get(0)
      v := pair.__get(1)
      ...
  }

NOTE: printing a List of nested Lists currently displays as
`[]` due to a separate display-path bug (raw struct pointers
stored in collections aren't recognised by
`quirk_opaque_to_string`). The values are stored correctly
and iteration works; only the `print()` rendering is wrong.

@example for pair in m.items() { ... }

#### `define values(self) -> List`

Returns all values as a `List`.
@returns: List

#### `define clear(self) -> void`

Removes all entries from the map.

#### `define is_empty(self) -> Bool { return self.length() == 0 }`

Returns `true` if the map has no entries.
@returns: Bool

#### `define each(self, cb: Callable) -> void`

Calls `cb` for each key-value pair.
@param cb: Callable — `fn(key: String, value: Any)`

@example:
m.each(fn(k: String, v: Any) => print(k + ": " + v))

#### `define each_value(self, cb: Callable) -> void`

Calls `cb` for each value only.
@param cb: Callable — `fn(value: Any)`

#### `define each_key(self, cb: Callable) -> void`

Calls `cb` for each key only.
@param cb: Callable — `fn(key: String)`


## `typing/collections/queue.quirk`

### `struct Queue : Printable, Sizeable, Iterable`

A double-ended queue (deque) supporting O(1) push/pop at both ends.
Useful for BFS, sliding windows, and task scheduling.

@example:
q := Queue()
q.push_back(1)
q.push_back(2)
q.push_front(0)
print(q.pop_front())  // 0
print(q.peek_front()) // 1
print(q.size())       // 2

#### `define push_back(self, val: Any) -> void`

Appends a value to the back of the queue.
@param val: Any

#### `define push_front(self, val: Any) -> void`

Prepends a value to the front of the queue.
@param val: Any

#### `define pop_front(self) -> Any`

Removes and returns the front element. Returns `null` if empty.
@returns: Any

#### `define pop_back(self) -> Any`

Removes and returns the back element. Returns `null` if empty.
@returns: Any

#### `define peek_front(self) -> Any`

Returns the front element without removing it.
@returns: Any

#### `define peek_back(self) -> Any`

Returns the back element without removing it.
@returns: Any

#### `define size(self) -> Int`

Returns the number of elements.
@returns: Int

#### `define is_empty(self) -> Bool`

Returns `true` if the queue is empty.
@returns: Bool

#### `define clear(self) -> void`

Removes all elements.


## `typing/collections/set.quirk`

### `struct Set : Printable, Sizeable, Iterable`

An unordered collection of unique values. Insertion order is preserved for
iteration. Values are compared by their string representation.

@example:
s := {1, 2, 3}
s.add(4)
print(s.has(2))      // true
s.remove(2)
print(s.size())      // 3
for v in s { print(v) }

#### `define add(self, val: Any) -> void`

Adds a value to the set. Does nothing if already present.
@param val: Any

#### `define remove(self, val: Any) -> void`

Removes a value from the set. Does nothing if absent.
@param val: Any

#### `define has(self, val: Any) -> Bool`

Returns `true` if the value is in the set.
@param val: Any
@returns: Bool

#### `define size(self) -> Int`

Returns the number of elements.
@returns: Int

#### `define is_empty(self) -> Bool`

Returns `true` if the set has no elements.
@returns: Bool

#### `define clear(self) -> void`

Removes all elements.

#### `define union(self, other: Set) -> Set`

Returns a new set containing all elements from both sets.
@param other: Set
@returns: Set

#### `define intersection(self, other: Set) -> Set`

Returns a new set containing only elements present in both sets.
@param other: Set
@returns: Set

#### `define difference(self, other: Set) -> Set`

Returns a new set with elements in this set that are not in `other`.
@param other: Set
@returns: Set

#### `define __add(self, other: Set) -> Set { return self.union(other) }`

Operator-overload aliases. `s1 + s2` is union; `s1 - s2` is
difference. Sema's binary-op dispatch picks these up
automatically via the magic-method path. Both return fresh
sets and leave the operands untouched.

@example a + b   // union
@example a - b   // difference

#### `extern define to_list(self) -> List`

Materialise the set's elements into a fresh List in insertion
order. Useful when downstream code wants index/sort/slice
operations — Set itself is hash-keyed and doesn't support
`__get(i)`. Order matches the iterator (insertion order).

@example users.to_list().sort(fn(a, b) => a.name.cmp(b.name))

#### `define first(self) -> Any`

Returns the first element in insertion order, or `null` when
the set is empty. Parallel to List.first(). Useful for
sentinel-then-iterate patterns where the receiver may or may
not have anything yet.

@example pending.first()    // the oldest queued item

#### `define last(self) -> Any`

Returns the last element in insertion order, or `null` when
the set is empty. Parallel to List.last().


## `typing/collections/tuple.quirk`

### `struct Tuple : Printable, Sizeable, Iterable`

An immutable, fixed-size ordered sequence of heterogeneous values.
Tuples are created with parenthesis syntax and support index access,
`length()`, and destructuring assignment.

A trailing comma is required for single-element tuples: `(x,)`.
An empty tuple is written as `()`.

@example:
coords := (10, 20)
print(coords[0])        // 10
print(coords.length())  // 2
x, y := coords          // destructuring
nested := ((1, 2), "outer")
print(nested[1])        // "outer"

#### `define __get(self, index: Int) -> Any`

Returns the element at the given index.
@param index: Int — zero-based position
@returns: Any — the element at that index
@throws IndexError — if index is out of bounds

#### `define __str(self) -> String`

Returns a human-readable string representation of this tuple.
@returns: String — e.g. `"(1, hello, true)"`

#### `define length(self) -> Int`

Returns the number of elements in this tuple.
@returns: Int — element count

#### `define __iter(self) -> TupleIterator`

Returns an iterator over the elements of this tuple.
@returns: TupleIterator


## `typing/exceptions/base.quirk`

### `struct Exception`

Base class for all exceptions in Quirk.
Catch this type to handle any exception regardless of its specific kind.

Fields: message, type, file, line, traceback, cause_trace

@note Prefer catching a specific subclass (ValueError, IOError, etc.) over
catching Exception directly, so unrelated errors are not silently swallowed.

@warning Catching Exception will also catch RuntimeError, NullError, and all
other built-in exceptions — use with care in broad catch blocks.

@example:
try {
    throw ValueError("bad input")
} catch (e: Exception) {
    print(e.type + ": " + e.message)
}


## `typing/exceptions/types.quirk`

### `struct TypeError : Exception`

Raised when an operation is applied to an object of the wrong type.
Use this when a function receives an argument whose type is incompatible
with the expected type.

@note Prefer ValueError when the type is correct but the value is invalid.

@example:
define require_int(n: Any) -> void {
    if n.type != "Int" {
        throw TypeError("Expected Int, got " + n.type)
    }
}

### `struct ValueError : Exception`

Raised when an argument has the right type but an unacceptable value.
Parent of IndexError and KeyError — catch ValueError to handle both.

@example:
define set_age(age: Int) -> void {
    if age < 0 {
        throw ValueError("Age cannot be negative: " + age.str())
    }
}

### `struct IndexError : ValueError`

Raised when a list or string index is out of range.
Inherits from ValueError.

@note
- The runtime throws this automatically on invalid list access.
- Negative indices are supported: -1 refers to the last element.

@example:
items := [1, 2, 3]
try {
    _ := items[10]
} catch (e: IndexError) {
    print("Out of bounds: " + e.message)
}

### `struct KeyError : ValueError`

Raised when a map key does not exist.
Inherits from ValueError.

@note
- The runtime throws this automatically on missing key access.
- Use map.has(key) to check before accessing if you want to avoid the exception.

@example:
m := {"a": 1}
try {
    _ := m["missing"]
} catch (e: KeyError) {
    print("No such key: " + e.message)
}

### `struct IOError : Exception`

Raised for file and I/O operation failures such as permission errors,
read/write failures, or other OS-level I/O problems.
Parent of FileNotFoundError.

@example:
try {
    f := File("output.log", "w")
} catch (e: IOError) {
    print("I/O failed: " + e.message)
}

### `struct FileNotFoundError : IOError`

Raised when a file or directory cannot be found at the given path.
Inherits from IOError.

@note Catch IOError instead if you want to handle all I/O failures together.

@example:
try {
    f := File("config.json", "r")
} catch (e: FileNotFoundError) {
    print("Missing file: " + e.message)
}

### `struct RuntimeError : Exception`

Raised for generic runtime errors that don't fit a more specific category.
Parent of NotImplementedError and NullError.

@warning Avoid throwing RuntimeError directly when a more specific subclass applies.

@example:
define run(mode: String) -> void {
    if mode != "fast" and mode != "slow" {
        throw RuntimeError("Unknown mode: " + mode)
    }
}

### `struct NotImplementedError : RuntimeError`

Raised when a method that must be overridden is called without a concrete implementation.
Inherits from RuntimeError.

@note Use this as a sentinel in base struct methods to enforce that substructs override them.

@example:
struct Animal {
    define speak(self) -> void {
        throw NotImplementedError("speak() must be overridden")
    }
}

### `struct SocketError : Exception`

Raised for socket and network operation failures such as connection
refused, timeout, or DNS resolution errors.

@example:
try {
    conn := Socket.connect("localhost", 8080)
} catch (e: SocketError) {
    print("Connection failed: " + e.message)
}

### `struct ZeroDivisionError : Exception`

Raised when division or modulo by zero is attempted.

@example:
define divide(a: Int, b: Int) -> Int {
    if b == 0 {
        throw ZeroDivisionError("Cannot divide " + a.str() + " by zero")
    }
    return a / b
}

### `struct AssertionError : Exception`

Raised when an assertion fails.
Use this to enforce invariants or preconditions in your code.

@note Prefer specific exception types (ValueError, NullError, etc.) in library code so callers can catch them precisely.

@example:
define assert_positive(n: Int) -> void {
    if n <= 0 {
        throw AssertionError("Expected positive, got " + n.str())
    }
}

### `struct CancelledError : Exception`

Raised inside a task when `task.cancel()` was called — at the task's next
`sys.sleep`, `await` or `wait` (a sleep in progress is cut short). Catch it
to clean up; awaiting a cancelled task raises it too.

@example:
async define worker() {
    try {
        while true { sys.sleep(100) }
    } catch (e: CancelledError) {
        print("stopping")
    }
}

### `struct NullError : RuntimeError`

Raised when a null value is accessed where a non-null value is required. Inherits from RuntimeError.

@note Check for null explicitly before accessing fields or calling methods on optional values.

@example:
define get_name(user: Any) -> String {
    if user == null {
        throw NullError("user must not be null")
    }
    return user.name
}

### `struct WhereConditionError : RuntimeError`

Raised when a function's `where` precondition is violated at runtime.
Inherits from RuntimeError.

@example:
define sqrt(x: Double) -> Double where x >= 0.0 {
    return x
}

try {
    sqrt(-1.0)
} catch (e: WhereConditionError) {
    print("Precondition failed: " + e.message)
}


## `typing/interfaces/comparable.quirk`

### `interface Comparable : Equatable`

Implemented by types that support ordering and equality comparisons.
Required for use in sorted collections, binary search, and generic
functions that need to compare values.

@example
struct Temperature : Comparable {
    degrees: Double

    define __lt(self, other: Temperature) -> Bool {
        return self.degrees < other.degrees
    }

    define __eq(self, other: Temperature) -> Bool {
        return self.degrees == other.degrees
    }
}


### Module-level functions

#### `define __lt(self, other: Self) -> Bool`

Returns true if this value is strictly less than `other`.


## `typing/interfaces/equatable.quirk`

### `interface Equatable`

Implemented by types that support equality comparison.
All types that implement `Comparable` or `Hashable` extend `Equatable`.

@example
struct Color : Equatable {
    r: Int
    g: Int
    b: Int

    define __eq(self, other: Color) -> Bool {
        return self.r == other.r and self.g == other.g and self.b == other.b
    }
}


### Module-level functions

#### `define __eq(self, other: Self) -> Bool`

Returns true if this value is equal to `other`.


## `typing/interfaces/hashable.quirk`

### `interface Hashable : Equatable`

Implemented by types that can produce a stable integer hash of their value.
Required for use as keys in `Map` or elements in `Set`.
Any `Hashable` type must also be `Equatable`: two equal values must hash identically.

@example
struct Point : Hashable {
    x: Int
    y: Int

    define __eq(self, other: Point) -> Bool {
        return self.x == other.x and self.y == other.y
    }

    define __hash(self) -> Int {
        return self.x * 31 + self.y
    }
}


### Module-level functions

#### `define __hash(self) -> Int`

Returns a stable integer hash of this value.


## `typing/interfaces/iterable.quirk`

### `interface Iterable`

Implemented by types that can be traversed with a `for` loop.
`String`, `List`, `Map`, `Set`, `Queue`, and `Tuple` all implement `Iterable`.

@example
define print_all[T](col: T) where T: Iterable {
    for item in col {
        print(item)
    }
}


### Module-level functions

#### `define __iter(self) -> Iterator`

Returns an iterator over the elements of this collection.


## `typing/interfaces/iterator.quirk`

### `interface Iterator`

Protocol for types that produce values one at a time.
All built-in collection iterators implement `Iterator`.
Enables the `for` loop and any code that calls `__has_next` / `__next` directly.

@example
define collect[T](it: Iterator) -> List {
    result := []
    while it.__has_next() {
        result.append(it.__next())
    }
    return result
}


### Module-level functions

#### `define __has_next(self) -> Bool`

Returns true if there are more elements to yield.

#### `define __next(self) -> Any`

Returns the next element and advances the iterator.


## `typing/interfaces/parseable.quirk`

### `interface Parseable`

Implemented by types that can be constructed from a string representation.
Pairs naturally with `Printable` — a round-trip of `parse(val.str())` should
return the original value.

@example
n := Int.parse("42")     // 42
d := Double.parse("3.14") // 3.14
b := Bool.parse("true")   // true


### Module-level functions

#### `define parse(s: String) -> Self`

Parses `s` and returns the corresponding value.
@param s: String — the string to parse
@throws ValueError — if `s` is not a valid representation


## `typing/interfaces/primitive.quirk`

### `interface Primitive : Printable, Comparable`

Base interface for primitive value types.

All built-in scalar types (`Int`, `Double`, `Bool`, `String`) implement
`Primitive`. Extends both `Printable` and `Comparable`, so any `where T: Primitive`
constraint guarantees string conversion (`__str`) and ordering (`__lt`, `__eq`).

For types that can be constructed from a string, see `Parseable`.
For types usable as collection keys, see `Hashable`.

@example
struct Stack[T] where T: Primitive {
    items: List[T]
    ...
}

define echo[T](val: T) -> String where T: Primitive {
    return val.__str()
}


## `typing/interfaces/printable.quirk`

### `interface Printable`

Implemented by types that have a human-readable string representation.
Any struct implementing `Printable` can be passed directly to `print()`.

@example
struct Point : Printable {
    x: Int
    y: Int

    define __str(self) -> String {
        return "(" + self.x + ", " + self.y + ")"
    }
}

p := Point(3, 4)
print(p)   // (3, 4)


### Module-level functions

#### `define __str(self) -> String`

Returns a human-readable string representation of this value.


## `typing/interfaces/representable.quirk`

### `interface Representable`

Implemented by types that have a developer-facing debug representation,
distinct from the human-readable `__str`. Used by the REPL and logging tools.

@example
struct Token : Representable {
    kind: String
    value: String

    define __repr(self) -> String {
        return "Token(" + self.kind + ", " + self.value + ")"
    }
}


### Module-level functions

#### `define __repr(self) -> String`

Returns a developer-readable debug representation of this value.


## `typing/interfaces/serializable.quirk`

### `struct ISerializable`

Serialization interface for Quirk structs.

Inherit `ISerializable` and override `to_json()` to make any struct
serializable — including as the `json:` argument to `net.http` requests.

@example
struct User : ISerializable {
    name: String
    age: Int

    define __init(self, name: String, age: Int) -> void {
        self.name = name
        self.age = age
    }

    define to_json(self) -> String {
        return "{\"name\":\"" + self.name + "\",\"age\":" + self.age.str() + "}"
    }
}

user := User("Alice", 30)
res := http.post("http://api.example.com/users", json: user)

#### `define to_json(self) -> String`

Serialize this object to a JSON string.
Override this in every subclass.
@returns A valid JSON string representing this object.


## `typing/interfaces/sizeable.quirk`

### `interface Sizeable`

Implemented by types that have a finite, countable number of elements.
`String`, `List`, `Map`, `Set`, and `Queue` all implement `Sizeable`.

@example
define is_empty[T](col: T) -> Bool where T: Sizeable {
    return col.length() == 0
}


### Module-level functions

#### `define length(self) -> Int`

Returns the number of elements in this collection.


## `typing/option.quirk`

### `type Option[T] = Some(value: T) | None()`

#### `define is_some(self) -> Bool`

Returns true when the value is present.

#### `define is_none(self) -> Bool { return not self.is_some() }`

Returns true when the value is absent.

#### `define unwrap_or(self, default_value: T) -> T`

Returns the wrapped value if present, else `default_value`.

#### `define unwrap(self) -> T`

Returns the wrapped value if present; throws `NullError` if `None`.

#### `define unwrap_or_else(self, f: Callable) -> T`

Returns the wrapped value if present, else calls `f()` for a lazy default.

#### `define map(self, f: Callable) -> Option`

Maps `Some(v)` to `Some(f(v))`; `None` stays `None`.

#### `define and_then(self, f: Callable) -> Option`

Monadic bind: `f` must return an `Option`. Chains lookups: `cache.get(k).and_then(parse)`.

#### `define or_else(self, f: Callable) -> Option`

Returns `self` if `Some`, else calls `f()` for an alternative `Option`.

#### `define filter(self, pred: Callable) -> Option`

Keeps `Some(v)` only if `pred(v)` is true; otherwise `None`.

#### `define ok_or(self, err: Any) -> Result`

Converts to `Result`: `Some(v)` → `Ok(v)`; `None` → `Err(err)`.

### `variant Some of Option`

### `variant None of Option`


## `typing/primitives/bool.quirk`

### `struct Bool : Primitive, Comparable, Parseable`

Boolean type. Only two values: `true` and `false`.

#### `define parse(s: String) -> Bool`

Parses `"true"` or `"false"`.
@param s: String
@returns: Bool
@throws ValueError — if `s` is not `"true"` or `"false"`


## `typing/primitives/double.quirk`

### `struct Double : Primitive, Comparable, Parseable`

64-bit IEEE 754 floating-point number.

@example:
x := 3.14
print(x.ceil())    // 4.0
print(x.str())     // "3.14"

#### `define sqrt(self) -> Double`

Returns the square root.
@returns: Double
@throws ValueError — if the value is negative

#### `define parse(s: String) -> Double`

Parses a string as a floating-point number.
@param s: String
@returns: Double
@throws ValueError — if `s` is not a valid number


## `typing/primitives/int.quirk`

### `struct Int : Primitive, Comparable, Parseable`

64-bit signed integer.
Supports arithmetic operators (`+`, `-`, `*`, `/`, `%`; `n.pow(e)` for powers) and comparisons.

@example:
n := 42
print(n.str())       // "42"
print(n.is_even())   // true
print(n.pow(2))      // 1764

#### `define str(self) -> String`

Returns the string representation of this integer.
@returns: String

#### `define abs(self) -> Int`

Returns the absolute value.
@returns: Int

#### `define pow(self, exp: Int) -> Int`

Returns `self` raised to the power `exp`.
@param exp: Int — exponent (must be ≥ 0)
@returns: Int

#### `define parse(s: String) -> Int`

Parses a string as an integer.
@param s: String
@returns: Int
@throws ValueError — if `s` is not a valid integer


## `typing/primitives/string.quirk`

### `struct String : Primitive, Comparable, Sizeable, Iterable, Representable`

An immutable UTF-8 string. String literals use double or single quotes.
Supports interpolation with `"Hello ${name}"`, iteration over characters,
and a rich set of transformation and query methods.

@example:
s := "Hello, world!"
print(s.upper())            // "HELLO, WORLD!"
print(s.contains("world")) // true
parts := s.split(", ")
print(parts.length())       // 2

#### `define __mul(self, n: Int) -> String`

`s * n` repeats `self` `n` times into a fresh String. `n <= 0`
returns the empty string (Python semantics).

@example "-" * 40   // 40-char separator
@example "ab" * 3   // "ababab"

#### `define format(self, ...args: List) -> String`

Returns a new string with each `{0}`, `{1}`, … placeholder replaced by the
corresponding argument.
@param args: List — positional values to interpolate
@returns: String
@example:
"Hello {0}, you are {1}!".format(["Alice", 30])

#### `define split(self, delimiter: String) -> List`

Splits the string on `delimiter` and returns a list of substrings.
@param delimiter: String
@returns: List — list of String parts

#### `define join(self, items: List) -> String`

Joins a list of strings, inserting `self` between each element.
@param items: List — list of String values
@returns: String
@example:
", ".join(["a", "b", "c"])  // "a, b, c"

#### `define substring(self, start: Int, end: Int) -> String`

Returns the substring from `start` (inclusive) to `end` (exclusive).
@param start: Int — start index
@param end: Int — end index
@returns: String
@throws IndexError — if indices are out of range

#### `define count(self, sub: String) -> Int`

Returns the number of non-overlapping occurrences of `sub`.
@param sub: String
@returns: Int

#### `define find(self, sub: String, start: Int = 0) -> Int`

Returns the index of the first occurrence of `sub`, or `-1` if not found.
With `start`: the first one at or after that index
(`"a-b-c".find("-", 2)` is 3).
@param sub: String
@returns: Int

#### `define index(self, sub: String) -> Int`

Like `find`, but throws `ValueError` if `sub` is not present.
@param sub: String
@returns: Int
@throws ValueError — if `sub` is not found

#### `define to_int(self) -> Int`

Parses the string as an integer.
@returns: Int
@throws ValueError — if the string is not a valid integer

#### `define to_float(self) -> Double`

Parses the string as a floating-point number.
@returns: Double
@throws ValueError — if the string is not a valid number

#### `define lines(self) -> List`

Splits on newlines and returns each line as a list element.
@returns: List

#### `define distance(self, other: String) -> Int`

Computes the Levenshtein edit distance between this string and `other`.
@param other: String
@returns: Int — number of single-character edits needed

#### `define code(self, index: Int = 0) -> Int`

The Unicode code point of character `index` (`"é".code()` is 233,
`"\n".code()` is 10), or -1 past the end.
@returns: Int

#### `define codes(self) -> List`

Every character's code point, in order — one pass over the string
(a parser reading characters by number).
@returns: List

#### `define length(self) -> Int`

Returns `true` if the string has zero characters.
@returns: Bool


## `typing/result.quirk`

### `type Result[T, E] = Ok(value: T) | Err(error: E)`

#### `define is_ok(self) -> Bool`

Returns true when the result is `Ok`.

#### `define is_err(self) -> Bool { return not self.is_ok() }`

Returns true when the result is `Err`.

#### `define unwrap_or(self, default_value: T) -> T`

Returns the success value if `Ok`, else `default_value`.

#### `define unwrap(self) -> T`

Returns the success value if `Ok`; throws `ValueError` if `Err`.

#### `define unwrap_err(self) -> E`

Returns the error payload if `Err`; throws `ValueError` if `Ok`.

#### `define unwrap_or_else(self, f: Callable) -> T`

Returns the success value if `Ok`, else calls `f(error)` for a lazy default.

#### `define map(self, f: Callable) -> Result`

Maps `Ok(v)` to `Ok(f(v))`; `Err(e)` stays `Err(e)`.

#### `define map_err(self, f: Callable) -> Result`

Maps `Err(e)` to `Err(f(e))`; `Ok(v)` stays `Ok(v)`.

#### `define and_then(self, f: Callable) -> Result`

Monadic bind: `f` must return a `Result`. Chains fallible steps.

#### `define or_else(self, f: Callable) -> Result`

Returns `self` if `Ok`, else calls `f(error)` for recovery.

#### `define ok(self) -> Option`

Converts to `Option`: `Ok(v)` → `Some(v)`; `Err(_)` → `None`.

#### `define err(self) -> Option`

Converts to `Option` of the error: `Err(e)` → `Some(e)`; `Ok(_)` → `None`.

### `variant Ok of Result`

### `variant Err of Result`
