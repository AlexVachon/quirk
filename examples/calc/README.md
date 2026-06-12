# calc — expression evaluator (~150 LOC)

A self-contained arithmetic expression interpreter showing the
canonical small-interpreter shape in Quirk: a tagged-union AST,
a recursive-descent parser, and a match-based evaluator.

## Run

```bash
quirk run examples/calc/main.quirk "(1 + 2) * 3 - 4 / 2"     # 7
quirk run examples/calc/main.quirk "2 ^ 10"                  # 1024
quirk run examples/calc/main.quirk "-5 + 10"                 # 5
```

Supports integer literals, `+ - * / ^`, unary minus, and
parentheses. Standard precedence (`^` binds tightest,
right-associative); `*` `/` over `+` `-`, both left-associative.

## What it shows

| Feature                       | Where in the code                                  |
|-------------------------------|----------------------------------------------------|
| Tagged union AST              | `type Expr = Num(...) \| Bin(...) \| Neg(...)`     |
| Recursive struct field type   | `Bin(op: String, lhs: Expr, rhs: Expr)`            |
| Per-struct method via `extend`| `extend Tokenizer { define peek_char(self) ... }`  |
| Match with payload narrowing  | `case Bin as b => { l := eval(b.lhs); ... }`       |
| Stateful struct mutation      | `self.pos = self.pos + 1` inside Tokenizer methods |
| Pratt-style precedence climb  | `parse_atom → parse_pow → parse_mul → parse_add`   |

## Why this exists

The TODO demo (`examples/todo_cli/`) covers v3 idioms in a
data-processing shape. A tree-walking interpreter exercises a
different axis: recursive data, deep match dispatch, and clean
separation of parse vs evaluate. Both are useful baselines for
"what real Quirk code looks like."

## Known caveat

The Sema flow analysis doesn't yet recognise that
`return -recursive_call()` always returns Int, so the Neg arm of
`eval` uses the explicit `0 - eval(...)` form instead. Same final
behaviour; line 137 in `main.quirk`. Tracked as a future cleanup.
