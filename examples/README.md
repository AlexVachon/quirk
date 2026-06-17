# Quirk examples

Small, complete, runnable Quirk programs. Each one is its own
directory with a `main.quirk` + a `README.md` walking through which
language feature it exercises. Read top-to-bottom to see idiomatic
Quirk in a real shape, then crib whatever you need.

| Example                               | Shape                       | Lines | Highlights                                                      |
|---------------------------------------|-----------------------------|-------|-----------------------------------------------------------------|
| [`todo_cli/`](todo_cli/README.md)     | Data + dispatch             | ~140  | Tagged unions, Option / Result, match with payload narrow-bind  |
| [`calc/`](calc/README.md)             | Interpreter                 | ~150  | Recursive AST, Pratt-style parser, match-based evaluator        |
| [`md2html/`](md2html/README.md)       | Text in, text out           | ~100  | String slicing, multi-pass replacement, functional decomposition|
| [`recipe_search/`](recipe_search/README.md) | Fuzzy lookup TUI      | ~150  | `String.distance`, slash-command drawer, ANSI styling, Option API|
| [`recipe_web/`](recipe_web/README.md) | Fuzzy lookup web app          | ~150  | `net.server` HTTP loop, inline HTML/CSS, `List.sort`, form decoding|

## Running

```bash
quirk run examples/todo_cli/main.quirk
quirk run examples/calc/main.quirk "(1 + 2) * 3"
quirk run examples/md2html/main.quirk
quirk run examples/recipe_search/main.quirk
quirk run examples/recipe_web/main.quirk  # then open http://127.0.0.1:8080/
```

## Why these shapes

Most non-trivial programs are some mix of three patterns:

- **Data + dispatch** — model the domain as a tagged union or
  struct hierarchy, then dispatch on it. (todo_cli)
- **Interpretation** — parse input into a tree, walk it. (calc)
- **Text transformation** — read text, mutate it through a series
  of passes, write text out. (md2html)

The three examples here are deliberately one of each, so a new
Quirk programmer can map their own use case to whichever feels
closest.
