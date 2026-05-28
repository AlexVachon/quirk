# Quirk (.quirk) Language Reference

**Compiler:** 1.0.0  
**Language spec:** rev 2  
**Extension:** `.quirk`  
**Runtime:** LLVM JIT + native binary output

Quirk is a compiled, statically-typed language with Python-like syntax, struct-based OOP, first-class functions, and a rich standard library. It compiles to native code via LLVM and uses the Boehm GC for memory management.

---

## Install

Linux x86_64, one line:

```bash
curl -fsSL https://raw.githubusercontent.com/AlexVachon/quirk/main/install.sh | sh
```

Drops the compiler + runtime + stdlib into `~/.quirk/`. After it runs, add the two `export` lines it prints to your shell profile, reopen the shell, then:

```bash
quirk --version
```

Source builds, other platforms, and dependency lists: see [INSTALL.md](INSTALL.md).

---

## Table of Contents

1. [Quick Start](#1-quick-start)
2. [Variables & Types](#2-variables--types)
3. [Primitive Types](#3-primitive-types)
4. [Collections](#4-collections)
5. [Control Flow](#5-control-flow)
6. [Functions](#6-functions)
7. [Lambdas](#7-lambdas)
8. [Structs](#8-structs)
9. [Enumerations](#9-enumerations)
10. [Pattern Matching](#10-pattern-matching)
11. [Error Handling](#11-error-handling)
12. [Optional Types & Null Safety](#12-optional-types--null-safety)
13. [String Interpolation](#13-string-interpolation)
14. [Type Casting & Where Clauses](#14-type-casting--where-clauses)
15. [Type Aliases & Union Types](#15-type-aliases--union-types)
16. [Imports & Modules](#16-imports--modules)
17. [Context Managers](#17-context-managers)
18. [Comprehensions](#18-comprehensions)
19. [Operators](#19-operators)
20. [Standard Library](#20-standard-library)
21. [Compiler Usage](#21-compiler-usage)

---

## 1. Quick Start

```quirk
// Hello, World!
print("Hello, World!")

// Variables
name := "Quirk"
version := 2

// Function
define greet(name: String) -> String {
    return "Welcome to ${name}!"
}

print(greet("Quirk"))
```

```bash
quirk hello.quirk
```

---

## 2. Variables & Types

### Declaration

```quirk
x := 42              // Type-inferred (Int)
name: String = "Alex" // Explicit type annotation
const PI := 3.14159  // Immutable constant
```

### Reassignment

```quirk
x := 5
x = 10   // Update existing variable
```

### Multiple Assignment

```quirk
x, y, z := 0         // Assign 0 to all three
a, b := (10, 20)     // Tuple destructuring
```

---

## 3. Primitive Types

### Int

32-bit signed integer.

```quirk
n := 42
print(n.abs())        // 42
print(n.is_even())    // true
print(n.is_odd())     // false
print(n.pow(2))       // 1764
print(n.to_float())   // 42.0
print(n.str())        // "42"
total := Int.parse("123")
```

### Double

64-bit floating-point number.

```quirk
pi := 3.14159
print(pi.ceil())      // 4
print(pi.floor())     // 3
print(pi.round())     // 3
print(pi.abs())       // 3.14159
print(pi.sqrt())      // 1.7724...
print(pi.to_int())    // 3
print(pi.str())       // "3.14159"
val := Double.parse("3.14")
```

### Bool

```quirk
flag := true
flag2 := false
print(flag.str())     // "true"
b := Bool.parse("false")
```

### String

```quirk
s := "Hello, World!"
print(s.length())          // 13
print(s.upper())           // "HELLO, WORLD!"
print(s.lower())           // "hello, world!"
print(s.title())           // "Hello, World!"
print(s.trim())            // Remove surrounding whitespace
print(s.contains("World")) // true
print(s.startswith("Hello"))  // true
print(s.endswith("!"))     // true
print(s.find("World"))     // 7
print(s.count("l"))        // 3
print(s.replace("l", "L")) // "HeLLo, WorLd!"
print(s.split(", "))       // ["Hello", "World!"]
print("-".join(["a","b","c"])) // "a-b-c"
print(s.reverse())         // "!dlroW ,olleH"
print(s.repeat(2))         // "Hello, World!Hello, World!"
print(s.substring(0, 5))   // "Hello"
print(s[0:5])              // "Hello" (slice)

// Padding & alignment
print(s.zfill(20))
print(s.ljust(20, " "))
print(s.rjust(20, " "))
print(s.center(20, " "))

// Type conversion
print(s.to_int())    // Parse as Int (throws ValueError on failure)
print(s.to_float())  // Parse as Double
print(s.to_bool())   // Parse as Bool

// Character iteration
for ch in s {
    print(ch)
}

// String formatting
print("Hello {0}, you are {1}!".format("Alice", 30))
print("Pi is approximately {pi%.4f}".format(pi = 3.14159))

// Levenshtein distance
print("kitten".distance("sitting"))  // 3
```

### Char

Single character (single-quoted literal).

```quirk
c := 'A'
print(c.is_upper())   // true
print(c.is_lower())   // false
print(c.is_digit())   // false
print(c.is_alpha())   // true
print(c.is_space())   // false
print(c.to_lower())   // 'a'
print(c.to_upper())   // 'A'
print(c.str())        // "A"
ch := Char.parse("X")
```

---

## 4. Collections

### List

Dynamic ordered array.

```quirk
l := [1, 2, 3, 4, 5]
l.append(6)
val := l.pop()         // Remove and return last element
print(l[0])            // Index access
l[1] = 99             // Index assignment
print(l.length())      // Size
print(l.is_empty())    // false
l.clear()

// Functional methods
doubled := l.map(fn(x: Int) => x * 2)
evens   := l.filter(fn(x: Int) => x % 2 == 0)
sum     := l.reduce(0, fn(acc: Int, x: Int) => acc + x)
found   := l.find(fn(x: Int) => x > 3)
has_big := l.any(fn(x: Int) => x > 4)
all_pos := l.all(fn(x: Int) => x > 0)
l.each(fn(x: Int) => print(x))

// Comprehension
squares := [x * x for x in [1, 2, 3, 4, 5]]
evens   := [x for x in nums if x % 2 == 0]
```

### Map

Key-value store (String keys, Any values).

```quirk
m := {"name": "Alice", "age": 30}
m.put("city", "Paris")
print(m.get("name"))       // "Alice"
print(m["name"])           // "Alice" — throws KeyError if missing
print(m.has("age"))        // true
m.remove("age")
print(m.length())
print(m.is_empty())
m.clear()

// Iteration
m.each(fn(k: String, v: Any) => print("${k}: ${v}"))
m.each_key(fn(k: String) => print(k))
m.each_value(fn(v: Any) => print(v))

keys   := m.keys()
values := m.values()

// Membership test
print("name" in m)         // true
print("missing" in m)      // false

// Comprehension
upper_map := {k.upper(): v for k, v in m}
inverted  := {v: k for k, v in m}
```

### Set

Unordered collection of unique values.

```quirk
s := {1, 2, 3, 4, 5}
s.add(6)
s.remove(2)
print(s.size())            // 5
print(s.has(3))            // true
print(3 in s)              // true
print(s.is_empty())        // false
s.clear()

// Set operations
a := {1, 2, 3}
b := {2, 3, 4}
print(a.union(b))          // {1, 2, 3, 4}
print(a.intersection(b))   // {2, 3}
print(a.difference(b))     // {1}

// Import
from typing.collections use { Set }
```

### Queue

Double-ended queue (deque) with O(1) push/pop at both ends.

```quirk
from typing.collections use { Queue }

q := Queue()
q.push_back(10)
q.push_back(20)
q.push_front(0)
print(q.pop_front())       // 0
print(q.pop_back())        // 20
print(q.peek_front())      // 10
print(q.peek_back())       // 10
print(q.size())            // 1
print(q.is_empty())        // false
q.clear()
```

### Tuple

Fixed-size, heterogeneous, immutable sequence.

```quirk
t := (1, "hello", true)
single := (42,)            // Single-element (trailing comma required)
empty  := ()

print(t[0])                // 1 — index access
print(t.length())          // 3

// Destructuring
x, y := (10, 20)

// In loops
pairs := [(1, "one"), (2, "two"), (3, "three")]
for item in pairs {
    n, word := item
    print("${n} = ${word}")
}
```

---

## 5. Control Flow

### If / Elif / Else

```quirk
if x > 0 {
    print("positive")
} elif x < 0 {
    print("negative")
} else {
    print("zero")
}
```

### While

```quirk
i := 0
while i < 10 {
    print(i)
    i = i + 1
}
```

### For (iteration)

```quirk
// Over a list
for item in [1, 2, 3] {
    print(item)
}

// Over a string (character iteration)
for ch in "hello" {
    print(ch)
}

// Over a map (key iteration)
m := {"a": 1, "b": 2}
for key in m {
    print("${key}: ${m[key]}")
}

// Over a set
s := {10, 20, 30}
for val in s {
    print(val)
}

// Over a tuple
t := (1, "hello", true)
for elem in t {
    print(elem)
}
```

### Break & Continue

```quirk
for i in [1, 2, 3, 4, 5] {
    if i == 3 { break }
    if i == 2 { continue }
    print(i)
}
```

---

## 6. Functions

### Definition

```quirk
define add(a: Int, b: Int) -> Int {
    return a + b
}

// No return type (void)
define greet(name: String) {
    print("Hello, ${name}!")
}
```

### Default Parameters

```quirk
define connect(host: String, port: Int = 8080, ssl: Bool = false) -> void {
    print("Connecting to ${host}:${port} (ssl=${ssl})")
}

connect("example.com")
connect("example.com", port = 443, ssl = true)
```

### Named Arguments

```quirk
define func(url: String, timeout: Int = 30, ssl: Bool = false) -> void { }

func(timeout = 60, url = "https://example.com", ssl = true)
```

### Variadic Functions

```quirk
define log(prefix: String, sep: String = " ", ...parts) -> void {
    result := prefix
    for p in parts {
        result = result + sep + p
    }
    print(result)
}

log("Items:", " | ", "apple", "banana", "cherry")
log(prefix = ">>", "hello", "world")
```

### Where Clause (Preconditions)

```quirk
define divide(a: Int, b: Int) -> Double where b != 0 {
    return a as Double / b as Double
}

define clamp_positive(x: Int) -> Int where x >= 0 {
    return x
}
```

### Recursion

```quirk
define factorial(n: Int) -> Int {
    if n <= 1 { return 1 }
    return n * factorial(n - 1)
}
```

---

## 7. Lambdas

Anonymous functions that can capture variables from their surrounding scope.

```quirk
// Basic lambda
square := fn(x: Int) => x * x
print(square(5))   // 25

// Multi-line lambda body
process := fn(x: Int) {
    if x > 0 {
        print("positive: ${x}")
    }
}

// Closures (capture outer variable)
factor := 3
multiply := fn(x: Int) => x * factor

// Used with higher-order functions
nums := [1, 2, 3, 4, 5]
nums.map(fn(x: Int) => x * 2)
nums.filter(fn(x: Int) => x % 2 == 0)
nums.each(fn(x: Int) => print(x))
nums.reduce(0, fn(acc: Int, x: Int) => acc + x)

// Lambda as Callable type
f: Callable = fn(x: Int, y: Int) => x + y

// Variadic lambda
variadic := fn(...args: List) {
    for a in args { print(a) }
}
variadic(1, 2, 3)
```

---

## 8. Structs

Quirk's object model uses structs — value types with methods and inheritance.

### Basic Struct

```quirk
struct Point {
    x: Int
    y: Int

    define __init(self, x: Int, y: Int) -> void {
        self.x = x
        self.y = y
    }

    define distance_from_origin(self) -> Double {
        return (self.x * self.x + self.y * self.y).to_float().sqrt()
    }

    define __str(self) -> String {
        return "(${self.x}, ${self.y})"
    }
}

p := Point(3, 4)
print(p.distance_from_origin())  // 5.0
print(p)                         // "(3, 4)"
```

### Inheritance

```quirk
struct Animal {
    name: String
    sound: String

    define __init(self, name: String, sound: String) -> void {
        self.name = name
        self.sound = sound
    }

    define speak(self) -> String {
        return "${self.name} says ${self.sound}"
    }
}

struct Dog: Animal {
    breed: String

    define __init(self, name: String, breed: String) -> void {
        super().__init(name, "Woof")
        self.breed = breed
    }

    define describe(self) -> String {
        return super().speak() + " (${self.breed})"
    }
}

d := Dog("Rex", "Labrador")
print(d.describe())   // "Rex says Woof (Labrador)"
```

### Special Methods

| Method | Purpose |
|--------|---------|
| `__init(self, ...)` | Constructor |
| `__del(self)` | Destructor / cleanup |
| `__str(self) -> String` | String representation (`print`) |
| `__repr(self) -> String` | Detailed representation |
| `__eq(self, other) -> Bool` | Equality (`==`) |
| `__add(self, other)` | Addition (`+`) |
| `__iter(self)` | Iteration support (`for x in obj`) |
| `__get(self, index)` | Index read (`obj[i]`) |
| `__set(self, index, value)` | Index write (`obj[i] = v`) |
| `__enter(self) -> Self` | Context manager entry (`with`) |
| `__exit(self) -> void` | Context manager exit (`with`) |

### Extend (Add Methods to Existing Types)

```quirk
extend String {
    define shout(self) -> String {
        return self.upper() + "!!!"
    }
}

print("hello".shout())  // "HELLO!!!"
```

### Docstrings

```quirk
---
Represents a 2D point in space.
@param x: X coordinate
@param y: Y coordinate
---
struct Point {
    x: Int
    y: Int
    ...
}
```

---

## 9. Enumerations

```quirk
enum Color {
    Red
    Green
    Blue
}

enum Direction {
    North
    South
    East
    West
}

c := Color.Green

if c == Color.Red {
    print("It's red!")
}

match c {
    case Color.Red   => print("red")
    case Color.Green => print("green")
    case Color.Blue  => print("blue")
}
```

---

## 10. Pattern Matching

### Value Match

```quirk
match x {
    case 1        => print("one")
    case 2, 3     => print("two or three")
    case 4 {
        doubled := x * 2
        print("four, doubled: ${doubled}")
    }
    case _        => print("other")  // wildcard
}
```

### String Match

```quirk
match lang {
    case "Python" => print("snake")
    case "Quirk"  => print("quirky!")
    case _        => print("unknown")
}
```

### Bool Match

```quirk
match flag {
    case true  => print("yes")
    case false => print("no")
}
```

### Enum Match

```quirk
match color {
    case Color.Red   => print("red")
    case Color.Green => print("green")
    case Color.Blue  => print("blue")
}
```

### Type Match

Match on the runtime type of a value. Useful for `Any`-typed parameters.

```quirk
define describe(x) {
    match x {
        case Int => { print("integer: ${x}") }
        case String => { print("string: ${x}") }
        case List => { print("list with items") }
        case _ => { print("something else") }
    }
}

describe(42)         // "integer: 42"
describe("hello")    // "string: hello"
```

### Type Match with Binding

```quirk
match x {
    case Int as n => { print("Int value: ${n}") }
    case String as s => { print("String: ${s}") }
}
```

### Union Type Match

```quirk
match x {
    case Int | Float => { print("numeric") }
    case String => { print("text") }
    case _ => { }
}
```

---

## 11. Error Handling

### Throw

```quirk
throw ValueError("bad value")
throw TypeError("wrong type")
throw RuntimeError("something failed")
```

### Try / Catch / Finally

```quirk
try {
    throw ValueError("oops")
} catch (e: ValueError) {
    print("Caught: " + e.message)
} catch (e: Exception) {
    print("Generic: " + e.message)
} finally {
    print("Always runs")
}
```

### Anonymous Catch (no binding)

```quirk
try {
    risky()
} catch (ZeroDivisionError) {
    print("Division by zero")
}

// Multiple types in one catch
try {
    risky()
} catch (NullError, RuntimeError) {
    print("Null or runtime error")
}
```

### Bare Re-raise

```quirk
try {
    throw ValueError("root cause")
} catch (e: ValueError) {
    print("Logging error...")
    throw  // Re-raises the same exception
}
```

### Exception Chaining

```quirk
try {
    throw ValueError("root cause")
} catch (e: ValueError) {
    throw RuntimeError("higher-level error") from e
}
```

### Automatic Exceptions

```quirk
// IndexError — list/string out of bounds
items := [1, 2, 3]
_ := items[99]    // Throws IndexError

// KeyError — missing map key
m := {"x": 1}
_ := m["missing"] // Throws KeyError

// ZeroDivisionError
_ := 10 / 0      // Throws ZeroDivisionError
```

### Built-in Exception Hierarchy

```
Exception
├── TypeError
├── ValueError
│   ├── IndexError
│   └── KeyError
├── IOError
│   └── FileNotFoundError
├── RuntimeError
│   ├── NotImplementedError
│   └── NullError
├── SocketError
├── ZeroDivisionError
├── AssertionError
└── WhereConditionError
```

---

## 12. Optional Types & Null Safety

### Optional Declaration

```quirk
name: String? = null    // Nullable
name: String? = "Alex"  // Non-null
```

### Null Coalesce (`??`)

```quirk
a: String? = null
print(a ?? "default")       // "default"

print(a ?? b ?? c ?? "none")  // First non-null, or "none"
```

### Safe Call (`?.`)

```quirk
s: String? = "hello"
print(s?.upper() ?? "(null)")   // "HELLO"

n: String? = null
print(n?.upper() ?? "(null)")   // "(null)"
```

### Null Check (`?`)

```quirk
p: String? = "present"
q: String? = null

if p? { print("has value") }
if not q? { print("is null") }
print(p?)   // true
print(q?)   // false
```

### Optional Parameters

```quirk
define greet(name: String?) -> String {
    return "Hello, " + (name ?? "stranger") + "!"
}
```

---

## 13. String Interpolation

```quirk
name := "Quirk"
version := 2

// Simple interpolation
print("Hello ${name}!")              // "Hello Quirk!"
print("Version: ${version}")         // "Version: 2"

// Expression interpolation
print("Sum: ${1 + 2 + 3}")           // "Sum: 6"

// Format specifier (printf-style)
pi := 3.14159265
print("Pi = ${pi % .4f}")            // "Pi = 3.1416"
print("Hex: ${255 % x}")             // "Hex: ff"

// Block interpolation
user := "Alice"
age := 30
print("${user} is ${age} years old")
```

---

## 14. Type Casting & Where Clauses

### Casting with `as`

```quirk
i: Int = 7
d := i as Double       // 7.0

pi := 3.14
n := pi as Int         // 3 (truncated)

flag := 1 as Bool      // true
flag2 := 0 as Bool     // false

c := 65 as Char        // 'A'
ci := c as Int         // 65
```

### Where Clauses

Preconditions that are checked at call time and throw `WhereConditionError` on violation.

```quirk
define divide(a: Int, b: Int) -> Double where b != 0 {
    return a as Double / b as Double
}

define sqrt_positive(x: Double) -> Double where x >= 0.0 {
    return x.sqrt()
}

try {
    divide(10, 0)
} catch (e: WhereConditionError) {
    print("Precondition failed: " + e.message)
}
```

---

## 15. Type Aliases & Union Types

### Type Aliases

```quirk
type Name = String
type Number = Int
type ID = Int

define greet(n: Name) -> String {
    return "Hello, " + n + "!"
}
```

### Union Types

```quirk
// In function parameters
define process(val: Int | String) -> void {
    print("Got a value")
}

process(42)
process("hello")

// In variables (via Any)
type NumberOrText = Int | String
```

---

## 16. Imports & Modules

### Import Entire Module

```quirk
use io
use sys
use encoding.json
use net.http
```

### Selective Import

```quirk
from typing.collections use { List, Map }
from typing.collections use { Set, Queue }
from typing.exceptions use { ValueError, KeyError }
from io.file use { File }
from encoding.json use { json }
from math.vectors use { Vector2, Vector3 }
```

### Relative Imports

```quirk
from .utils use { helper }
from .models use { User, Post }
```

### Module Alias

```quirk
from .greet_lib as greet

greet.greet("Alice")
greet.shout("hello")
```

---

## 17. Context Managers

Any struct implementing `__enter` and `__exit` can be used in a `with` block. The resource is automatically cleaned up when the block exits.

```quirk
with File("data.txt", "w") as f {
    f.write("Hello!\n")
    f.write("Goodbye!\n")
}   // f.close() called automatically

with File("data.txt", "r") as f {
    content := f.read()
    print(content)
}
```

### Custom Context Manager

```quirk
struct Timer {
    define __enter(self) -> Self {
        print("Starting timer")
        return self
    }
    define __exit(self) -> void {
        print("Timer stopped")
    }
}

with Timer() as t {
    // ... timed work ...
}
```

---

## 18. Comprehensions

### List Comprehension

```quirk
squares := [x * x for x in [1, 2, 3, 4, 5]]
// [1, 4, 9, 16, 25]

evens := [x for x in [1, 2, 3, 4, 5] if x % 2 == 0]
// [2, 4]

upper_names := [name.upper() for name in ["alice", "bob"]]
// ["ALICE", "BOB"]
```

### Map Comprehension

```quirk
m := {"a": 1, "b": 2, "c": 3}

upper_keys := {k.upper(): v for k, v in m}
// {"A": 1, "B": 2, "C": 3}

inverted := {v: k for k, v in m}
// {1: "a", 2: "b", 3: "c"}

// Just the keys (Set comprehension-style)
keys_set := {k for k in m}
```

---

## 19. Operators

| Category | Operators |
|----------|-----------|
| Arithmetic | `+` `-` `*` `/` `%` |
| Comparison | `==` `!=` `<` `<=` `>` `>=` |
| Logical | `and` `or` `not` |
| Assignment | `:=` (declare) `=` (assign) `+=` `-=` `*=` `/=` |
| Null safety | `??` `?.` `?` |
| Membership | `in` `not in` |
| Type check | `is` |
| Type cast | `as` |
| Ellipsis | `...` (variadic spread) |
| String | `+` (concat) `[start:end]` (slice) |

### `is` operator (type check)

```quirk
x := 42
print(x is Int)       // true
print(x is String)    // false

name := "Quirk"
print(name is String) // true
```

---

## 20. Standard Library

### Typing (Core Types)

| Module | Types |
|--------|-------|
| `typing` | `Int`, `Double`, `Bool`, `Char`, `String`, `Any` |
| `typing.collections` | `List`, `Map`, `Set`, `Queue`, `Tuple` |
| `typing.exceptions` | All exception classes |
| `typing.serializable` | `ISerializable` interface |

### I/O

```quirk
use io

with io.File("output.txt", "w") as f {
    f.write("Hello!\n")
}

with io.File("input.txt", "r") as f {
    content := f.read()
    line := f.read_line()
}

// stderr
use sys
stderr := sys.stderr()
stderr.write("Error message\n")
```

### JSON

```quirk
from encoding.json use { json }

// Encode
m := {"name": "Alice", "age": 30}
print(json.dumps(m))   // {"name":"Alice","age":30}

// Decode
data: Map = json.loads("{\"key\":\"value\"}")
print(data.get("key")) // "value"

// File I/O
with File("config.json", "w") as f { json.dump(m, f) }
with File("config.json", "r") as f { loaded: Map = json.load(f) }
```

### HTTP

```quirk
use net.http

resp := http.get("https://example.com")
if resp.ok {
    print(resp.status_code)
    print(resp.text)
    print(resp.headers.get("Content-Type"))
}

resp2 := http.post("https://api.example.com/data", "body=content")
```

### Networking (Sockets)

```quirk
from net use { Socket }

server := Socket()
server.bind("0.0.0.0", 8080)
server.listen(5)

with server.accept() as client {
    data := client.recv(1024)
    client.send("HTTP/1.1 200 OK\r\n\r\nHello!")
}
```

### Math / Vectors

```quirk
from math.vectors use { Vector2, Vector3 }

v := Vector2(3.0, 4.0)
print(v)
```

---

## 21. Compiler Usage

```bash
# Run immediately (JIT)
quirk program.quirk

# Step through interactively under the (qdb) debugger
quirk --debug program.quirk

# Emit LLVM IR
quirk --emit-ir program.quirk

# Compile to native binary
quirk -o output program.quirk

# Verbose compilation output
quirk -v program.quirk

# Dump AST
quirk --emit-ast program.quirk
```

For the full CLI reference (subcommands, package manager, debugger commands, …) see [COMMANDS.md](COMMANDS.md).

### Building the Compiler

```bash
cd quirk-compiler
make -j$(nproc)
```

The compiler outputs to `bin/quirk`. The runtime shared library is built at `bin/runtime.so`.

---

## Comments

```quirk
// Single-line comment

---
Multi-line docstring comment.
Used for function and struct documentation.

@param name: The name to greet
@returns String
---
define greet(name: String) -> String {
    return "Hello, " + name + "!"
}
```

---

## Complete Example

```quirk
from typing.collections use { Set }
from typing.exceptions use { ValueError }
from encoding.json use { json }

// Type alias
type Score = Int

// Struct with inheritance
struct Person {
    name: String
    age: Int

    define __init(self, name: String, age: Int) -> void {
        self.name = name
        self.age = age
    }

    define __str(self) -> String {
        return "${self.name} (${self.age})"
    }

    define is_adult(self) -> Bool {
        return self.age >= 18
    }
}

struct Employee: Person {
    role: String

    define __init(self, name: String, age: Int, role: String) -> void {
        super().__init(name, age)
        self.role = role
    }

    define __str(self) -> String {
        return super().__str() + " - ${self.role}"
    }
}

// Function with default params and where clause
define grade(score: Score, max: Score = 100) -> String where max > 0 {
    pct := score as Double / max as Double * 100.0
    match pct as Int / 10 {
        case 10, 9 => return "A"
        case 8     => return "B"
        case 7     => return "C"
        case 6     => return "D"
        case _     => return "F"
    }
}

// Main logic
employees := [
    Employee("Alice", 30, "Engineer"),
    Employee("Bob", 17, "Intern"),
    Employee("Carol", 25, "Manager"),
]

scores: Map = {"Alice": 92, "Bob": 67, "Carol": 85}

for emp in employees {
    name := emp.name
    score: Score = scores.get(name) as Int
    g := grade(score)

    if emp.is_adult() {
        print("${emp} — Grade: ${g}")
    } else {
        print("${emp} (minor) — Grade: ${g}")
    }
}

// Set operations
passed := {name for name, score in scores if score >= 70}
print("Passed: ${passed.size()} employees")

// Error handling
try {
    result := grade(50, 0)   // Violates where clause
} catch (e: WhereConditionError) {
    print("Error: ${e.message}")
}
```
