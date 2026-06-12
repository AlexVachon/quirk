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


## `typing/primitives/bool.quirk`

### `struct Bool : Primitive, Comparable, Parseable`

Boolean type. Only two values: `true` and `false`.

#### `extern define parse(s: String) -> Bool`

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

#### `extern define sqrt(self) -> Double`

Returns the square root.
@returns: Double
@throws ValueError — if the value is negative

#### `extern define parse(s: String) -> Double`

Parses a string as a floating-point number.
@param s: String
@returns: Double
@throws ValueError — if `s` is not a valid number


## `typing/primitives/int.quirk`

### `struct Int : Primitive, Comparable, Parseable`

32-bit signed integer.
Supports arithmetic operators (`+`, `-`, `*`, `/`, `%`, `**`) and comparisons.

@example:
n := 42
print(n.str())       // "42"
print(n.is_even())   // true
print(n.pow(2))      // 1764

#### `extern define str(self) -> String`

Returns the string representation of this integer.
@returns: String

#### `extern define abs(self) -> Int`

Returns the absolute value.
@returns: Int

#### `extern define pow(self, exp: Int) -> Int`

Returns `self` raised to the power `exp`.
@param exp: Int — exponent (must be ≥ 0)
@returns: Int

#### `extern define parse(s: String) -> Int`

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
