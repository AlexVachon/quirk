# The Quirk (.qk) Formal Specification

**Version:** 1.0

**Design Philosophy:** Performance via explicit memory control; Readability via English-like keywords; Simplicity via limited core primitives.

---

## 1. Lexical Grammar

### 1.1 Identifiers

Identifiers must start with a letter (a-z, A-Z) or underscore `_`, followed by any number of letters, digits, or underscores.

### 1.2 Keywords

The following are reserved words and cannot be used as identifiers:
`use`, `struct`, `extend`, `define`, `const`, `if`, `else`, `while`, `for`, `ref`, `move`, `del`, `inline`, `return`, `true`, `false`.

### 1.3 Operators

* **Initialization:** `:=` (Declares and assigns)
* **Assignment:** `=` (Updates existing)
* **Arithmetic:** `+`, `-`, `*`, `/`, `%`
* **Comparison:** `==`, `!=`, `<`, `>`, `<=`, `>=`
* **Logical:** `and`, `or`, `not`

---

## 2. Type System

Quirk is **statically typed**. Types must be known at compile time.

### 2.1 Primitive Types (Classic)

| Type | Bits | Description |
| --- | --- | --- |
| `int` | 32 | Standard signed integer |
| `float` | 32 | Standard floating point |
| `double` | 64 | High-precision floating point |
| `string` | - | UTF-8 character sequence |
| `bool` | 8 | Boolean value (`true`/`false`) |
| `char` | 8 | Single ASCII character |

### 2.2 Complex Types

* **Struct:** A contiguous block of memory.
* **List(T):** A heap-allocated, dynamic array of type `T`.
* **Array [N]T:** A stack-allocated, fixed-size array of `N` elements of type `T`.

---

## 3. Memory Management Model

Quirk uses **Explicit Ownership**.

1. **Scope-Based:** Variables declared with `:=` are owned by the current `{}` block. They are freed when the block ends unless moved.
2. **References (`ref`):** Creates an alias to existing data. Does not claim ownership. Prevents expensive memory copying.
3. **Moves (`move`):** Transfers the memory address and ownership to a new variable or function. The original identifier becomes invalid.
4. **Manual (`del`):** Immediately releases heap-allocated memory.

---

## 4. Syntax Structures

### 4.1 Function Definition

```rust
[inline] define name(param: type, ...) -> return_type {
    // Body
}

```

### 4.2 Data & Behavior

```rust
struct Name {
    field: type
}

extend Name {
    define method(self, ...) { ... }
}

```

### 4.3 Control Flow

```rust
for ref item in collection { ... }

while condition { ... }

if condition { ... } else { ... }

```

---

## 5. Compiler Directives

* **`use`**: Resolves symbols from other `.apx` files.
* **`inline`**: A hint to the compiler to replace function calls with the function body to eliminate overhead.

---

## 6. Execution Model

1. **Source Code:** Plain text `.apx` file.
2. **Lexical Analysis:** Text is converted into tokens.
3. **Parsing:** Tokens are organized into an Abstract Syntax Tree (AST).
4. **Semantic Analysis:** Types are checked; ownership is verified.
5. **Codegen:** AST is converted to LLVM IR (Intermediate Representation).
6. **Optimization:** Dead code is removed; functions are inlined.
7. **Binary:** A standalone machine-code executable is produced.

---

### Your Language Identity Card

* **Name:** Quirk
* **Extension:** `.qk`
* **Speed Goal:** 95-100% of C performance.
* **Readability:** High (Similar to Python/Swift but with C structure).
