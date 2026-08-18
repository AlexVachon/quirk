#pragma once
//
// ErrorCodes — assign a stable numeric ID to well-known diagnostic
// messages so users can `quirk explain Q0100` for a proper docs page
// instead of the terse one-liner.
//
// Design constraint: don't touch the ~68 `fatalError()` / `reportError()`
// call sites. Instead, `matchErrorCode(msg)` regex-matches the message
// text at print time and returns a code if the pattern is known. Only
// pattern matches get a `[Q0100 ERROR]` prefix; unmatched errors still
// print as `[ERROR]`.
//
// Code layout (loose):
//   Q0001–Q0099   lexer + parser
//   Q0100–Q0199   name resolution + imports
//   Q0200–Q0299   type checking + operators
//   Q0300–Q0399   struct construction + inheritance
//   Q0400–Q0499   contracts + preconditions
//
// Adding a code: add its regex + entry to KNOWN below. Message text is
// authoritative — matcher regex must be strict enough to not misfire
// on unrelated messages. Full docs live in the `describeCode` table
// (title + summary + hint + example).

#include <regex>
#include <string>
#include <vector>

namespace quirk {

struct ErrorCode {
    int         code;
    std::regex  pattern;     // matched against the raw message text
};

struct ErrorDoc {
    int          code;
    const char*  title;
    const char*  summary;     // one-line explanation
    const char*  hint;        // typical fix, one sentence
    const char*  example;     // 3-6 lines showing bad + good side-by-side
};

// Match table — order matters when patterns could overlap; put the
// more-specific one first.
inline const std::vector<ErrorCode>& knownErrorCodes() {
    static const std::vector<ErrorCode> KNOWN = {
        // Lexer / parser
        {1,   std::regex("^Unexpected token")},
        {2,   std::regex("^Expected '\\{' to open function body")},
        {3,   std::regex("^Expected '\\{' or '=>'")},
        {4,   std::regex("^Expected '\\)'")},
        {5,   std::regex("^Expected '\\}'")},
        {6,   std::regex("^Expected function name")},
        // Name resolution / imports
        {100, std::regex("^undefined variable or function")},
        {101, std::regex("^module '[^']*' does not export symbol")},
        {102, std::regex("^Cannot resolve module")},
        {103, std::regex("^module '[^']*' has no function")},
        {104, std::regex("^Unknown method '")},
        // Type checking
        {200, std::regex("^argument \\d+ of ")},
        {201, std::regex("^'.' operands must be numeric")},
        {202, std::regex("^'\\+' operands must be numeric or include a String")},
        {203, std::regex("^operator '[^']*' incompatible types")},
        {204, std::regex("^cannot mutate const variable")},
        {205, std::regex("^type '[^']*' does not support indexing")},
        {206, std::regex("^array index must be 'Int'")},
        // Struct / inheritance
        {300, std::regex("^can only throw struct objects")},
        {301, std::regex("^catch type '[^']*' does not inherit from 'Exception'")},
        // Contracts
        {400, std::regex("^default value for parameter")},
    };
    return KNOWN;
}

// Return the code that matches `msg`, or 0 if no known pattern matches.
inline int matchErrorCode(const std::string& msg) {
    for (const auto& e : knownErrorCodes()) {
        if (std::regex_search(msg, e.pattern)) return e.code;
    }
    return 0;
}

// Format a code as `Q0042` — always 4-digit zero-padded so `quirk
// explain Q0042` and `quirk explain 42` both parse cleanly.
inline std::string formatCode(int code) {
    char buf[8];
    snprintf(buf, sizeof(buf), "Q%04d", code);
    return buf;
}

// Docs table — one entry per code. `quirk explain <code>` prints
// title + summary + hint + example. Keep entries short (~10 lines
// each) so the command output stays scannable in a terminal.
inline const std::vector<ErrorDoc>& errorDocs() {
    static const std::vector<ErrorDoc> DOCS = {
        {1, "Unexpected token",
         "The parser reached a token it couldn't fit into the current syntactic context.",
         "Check for a missing comma, colon, brace, or misplaced keyword right before the flagged position.",
         "// bad — bare tuple in a spot that requires parens:\n"
         "let coords = 1, 2, 3      // Q0001 near ','\n\n"
         "// good:\n"
         "let coords = (1, 2, 3)\n"
         "let x, y, z = 1, 2, 3      // bare tuples work in return / destructuring positions"},

        {2, "Expected '{' to open function body",
         "A `define`/`init` header wasn't followed by `{` — the body is missing.",
         "Add a `{ ... }` body, or use `=> expr` for a single-expression body.",
         "// bad:\n"
         "define double(x: Int) -> Int\n"
         "    return x * 2          // Q0002 (no braces)\n\n"
         "// good (both forms):\n"
         "define double(x: Int) -> Int { return x * 2 }\n"
         "define double(x: Int) -> Int => x * 2"},

        {3, "Expected '{' or '=>' for control body",
         "An `if`/`elif`/`else`/`while`/`for` header must be followed by either a braced block or a single-statement `=> stmt`.",
         "Wrap the body in braces or prefix a single statement with `=>`.",
         "// bad:\n"
         "if x < 0 return -x          // Q0003\n\n"
         "// good:\n"
         "if x < 0 { return -x }\n"
         "if x < 0 => return -x       // v5.3.0+ single-statement form"},

        {100, "Undefined variable or function",
         "The identifier isn't declared in the current scope and isn't imported by any `use` / `from ... use { ... }` line.",
         "Check the spelling, declare the variable, or import it. In script-mode files, top-level `if`/`while` blocks are declaration-tracked (v5.3.5+ tooling).",
         "// bad:\n"
         "if cmd == \"build\" {\n"
         "    site := \".\"\n"
         "}\n"
         "build(site)                 // Q0100 — site is out of scope\n\n"
         "// good — declare at the enclosing scope:\n"
         "site := \".\"\n"
         "if len(argv) > 2 { site = argv[2] }\n"
         "build(site)"},

        {101, "Module 'X' does not export symbol 'Y'",
         "The `from X use { Y }` import references a name that isn't defined or re-exported by the target module.",
         "Check the spelling of Y in both files; verify the module actually declares (or re-exports) Y at top level.",
         "// bad — spelling mismatch:\n"
         "// helper.quirk defines `split_formatter`\n"
         "from .helper use { split_frontmatter }   // Q0101\n\n"
         "// good — align the names:\n"
         "from .helper use { split_formatter }"},

        {102, "Cannot resolve module",
         "The compiler can't find a file for the referenced module path. Relative paths (`.foo`, `..foo`) are resolved against the importing file; bare names go through the venv/QUIRK_HOME lookup chain.",
         "Verify the file exists at the expected path. For third-party packages, run `quirk install <pkg>` inside your venv.",
         "// bad:\n"
         "from .missing use { foo }   // Q0102\n\n"
         "// good — file must exist:\n"
         "// $ ls ./missing.quirk\n"
         "// missing.quirk\n"
         "from .missing use { foo }"},

        {104, "Unknown method 'Type.method'",
         "The receiver type has no method with that name. Sema knows the receiver's static type (e.g. `Int`, `String`, or a user struct) and checks against its declared method set.",
         "Check the spelling; if the receiver could be `Any`, cast first (`x.to_str()`, `x.to_int()`). For structs, extend the type with `extend Foo { define ... }` to add a method.",
         "// bad:\n"
         "n := 42\n"
         "n.definitely_not_a_method()   // Q0104\n\n"
         "// good — use an actual method:\n"
         "n := 42\n"
         "n.str()"},

        {200, "Argument N of 'foo' expected T but got U",
         "A function/method call passed an argument whose static type doesn't match the declared parameter type.",
         "Cast the argument (`x.to_int()`, `x.str()`) or fix the declaration. If the receiver is `Any`, its return type is also `Any` — you may need to cast the intermediate value.",
         "// bad — .trim() on Any used to type as void:\n"
         "b := blocks[i].trim()\n"
         "render_block(b)               // Q0200 pre-v5.3.5 (fixed since)\n\n"
         "// good — annotate or cast:\n"
         "b: String = blocks[i].trim()\n"
         "render_block(b)"},

        {201, "'*' / '-' / '/' / '%' operands must be numeric",
         "Arithmetic operators (other than `+`) require both operands to be `Int` or `Double`. Type-Any operands slip past Sema; the check fires when Sema knows the static type is a non-numeric.",
         "For String repetition use `s * n` (String * Int → String, v3.17.0+). For other cases, cast first (`.to_int()`).",
         "// bad:\n"
         "\"hello\" - 1              // Q0201 — String - Int isn't defined\n\n"
         "// good:\n"
         "\"hello\" * 3              // \"hellohellohello\""},

        {202, "'+' operands must be numeric or include a String",
         "The `+` operator accepts numeric-numeric OR String-plus-anything (concatenation via `.__str`). Other combinations are rejected.",
         "Convert one side to String or Int/Double explicitly.",
         "// bad:\n"
         "[1, 2] + \"foo\"           // Q0202\n\n"
         "// good:\n"
         "[1, 2].str() + \"foo\""},

        {203, "Operator incompatible types",
         "Comparison or equality across unrelated types. Sema rejects `String == Int`, `Bool < String`, etc. up front instead of letting LLVM assert.",
         "Convert one side, or use `.str()` for stringified comparison.",
         "// bad:\n"
         "5 == \"5\"                 // Q0203\n\n"
         "// good:\n"
         "5.str() == \"5\""},

        {204, "Cannot mutate const variable",
         "A variable declared `const` (or `let` in some contexts) can't be reassigned after its initial binding.",
         "Change the declaration to `var` / mutable form, or introduce a new binding.",
         "// bad:\n"
         "const pi := 3.14\n"
         "pi = 3.14159              // Q0204\n\n"
         "// good — new binding, or use var:\n"
         "pi := 3.14\n"
         "pi = 3.14159"},

        {301, "Catch type does not inherit from 'Exception'",
         "The `catch (e: T)` type must be a struct that inherits (directly or transitively) from `Exception`.",
         "Extend your custom error type from `Exception`.",
         "// bad:\n"
         "struct MyErr { message: String }\n"
         "try { ... } catch (e: MyErr) { ... }   // Q0301\n\n"
         "// good:\n"
         "struct MyErr : Exception {}\n"
         "try { ... } catch (e: MyErr) { ... }"},
    };
    return DOCS;
}

inline const ErrorDoc* lookupErrorDoc(int code) {
    for (const auto& d : errorDocs()) if (d.code == code) return &d;
    return nullptr;
}

} // namespace quirk
