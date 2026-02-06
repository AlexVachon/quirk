To make **Quirk (`.qk`)** a first-class citizen in code editors, we need a **Syntax Highlighting Specification**. This defines how an IDE like VS Code or Sublime Text identifies "parts of speech" in your code to apply colors.

Because Quirk is designed for **scannability**, the colors should emphasize memory management (`ref`, `move`, `del`) and structural definitions (`struct`, `extend`, `define`).

---

### Quirk Syntax Highlighting Guide

Here is the "TextMate" scope mapping for your language. This is the logic used to create themes.

| Category | Keywords / Symbols | Recommended Color |
| --- | --- | --- |
| **Structure** | `struct`, `extend`, `define`, `use` | **Bold Purple/Magenta** |
| **Storage** | `const`, `ref`, `move`, `del` | **Orange/Gold** |
| **Control** | `if`, `else`, `while`, `for`, `return` | **Pink/Red** |
| **Types** | `int`, `float`, `double`, `string`, `bool`, `char` | **Cyan/Light Blue** |
| **Operators** | `:=`, `=`, `->` | **Bright White/Yellow** |
| **Comments** | `//` (Single line), `/* */` (Multi-line) | **Grey/Green** |

---

### How it looks in "Dark Mode"

When you open a `.apx` file, the syntax highlighting helps the developer see the "memory flow" at a glance.

```rust
// Quirk Example: Combat Logic
use system.math

struct Warrior {
    name: string
    power: int
}

extend Warrior {
    inline define slash(self, target: ref Enemy) {
        target.hp -= self.power
    }
}

define main() {
    p1 := Warrior(name: "Kael", power: 45)
    
    // The 'ref' and 'move' stand out in Orange
    for ref enemy in world.enemies {
        p1.slash(enemy)
    }

    del world // Manual cleanup is visually distinct
}

```

---

### Creating the VS Code Extension

To actually get these colors in VS Code, you would create a `package.json` and a `syntaxes/quirk.tmLanguage.json`.

**The Regex for your `:=` operator would look like this:**

```json
{
  "name": "keyword.operator.assignment.quirk",
  "match": ":="
}

```

**The Regex for `define` would be:**

```json
{
  "name": "keyword.declaration.function.quirk",
  "match": "\\b(define)\\b"
}

```

---

### The Final Step: The Compiler Architecture

Now that the "look and feel" (Syntax + Highlighting) and the "Rules" (Specification) are done, the next move is the **Compiler**.

Since you want it to be **fast and performant**, I recommend the following tech stack for your compiler:

1. **Frontend:** Written in **C++** or **Rust** (for speed).
2. **Parser:** Use a tool like **tree-sitter** (this is what GitHub and Neovim use to understand code structure).
3. **Backend:** **LLVM**. You feed your Quirk code into LLVM, and it handles the complex machine-code optimization for Intel, AMD, and Apple Silicon.
