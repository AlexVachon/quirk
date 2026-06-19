# `prompt` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `prompt/index.quirk`


### Module-level functions

#### `define input(message: String, default: String = "") -> String`

Free-text input. Appends ` [default]: ` when a default is provided so
the user knows what an empty reply gets them. An empty reply with
a default returns the default; an empty reply with no default
re-prompts (mirrors `read -p` behaviour in shells).

#### `define input_optional(message: String) -> String?`

Like `input` but returns `null` on an empty reply instead of re-prompting.
Use when the caller wants to handle the "user didn't say anything" branch
itself — e.g. asking a custom follow-up before re-prompting, or skipping
the field entirely.

    name := prompt.input_optional("Your name")
    match name {
        case null => print("(skipped)")
        case _    => print("hi, " + name)
    }

#### `define password(message: String) -> String`

Hidden input. Shells out to console.password, which already disables
terminal echo via termios in the C runtime. Single-call wrapper so
client code doesn't need to know which stdlib module owns the helper.

#### `define confirm(message: String, default: Bool = true) -> Bool`

Yes/no question. `default=true` means an empty reply (pressing Enter)
accepts; `default=false` means it rejects. The prompt suffix is
`[Y/n]` / `[y/N]` matching most CLI conventions.

#### `define select(message: String, options: List, default_idx: Int = 0) -> String`

Pick one option from a numbered list. Renders:

    Mode?
      1) fast        [default]
      2) thorough
      3) debug
    Choice (1-3, default 1): _

Returns the selected option's text. `default_idx` is zero-based; out-
of-range values fall back to the first option.
