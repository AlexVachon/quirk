# Quirk TODO demo — v3.0.0 idioms in one file

A small task tracker that drives every v3.0.0 type-system feature
end-to-end. Self-contained (no file I/O) so it stays portable; the
same patterns scale to a real persisted CLI once you wire in
`io.File` / `encoding.json`.

## Run

```bash
quirk examples/todo_cli/main.quirk
```

Expected output ends with two tasks listed and one task removed.

## What it shows

| Feature                       | Where in the code                                |
|-------------------------------|--------------------------------------------------|
| Tagged union                  | `type Command = Add(title) \| ListAll() \| Done(id) \| Remove(id) \| Help()` |
| Canonical `Result[T, E]`      | `define parse_command(...) -> Result { … return Err(...) }` |
| Canonical `Option[T]`         | `define find_task(...) -> Option { … return None() }` |
| Match w/ payload narrowing    | `case Add as a => save_tasks(cmd_add(tasks, a.title))` |
| Per-variant method            | `extend Task { define label(self) -> String { … } }` |
| Generic struct                | `struct Box[T] { value: T  define get(self) -> T { … } }` |

## Why this exists

The README lists v3 features one section at a time. This example
shows what they look like together in real code. New users should
be able to read `main.quirk` top to bottom and recognise every
pattern from chapters 22–24 of the language reference.

## Known rough edges

A couple of v3 ergonomic gaps show up at the line `// List.get
returns Any; annotate to unbox` — `parts.get(1)` returns `Any`
and storing it into a variant's `title: String` field needs an
explicit `s: String := …` annotation first. The deferred
"per-instantiation Codegen monomorphization" (future v3.x) will
remove this — once `List[String]` has a real String-typed get, the
annotation goes away.
