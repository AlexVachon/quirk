# `recipe_search/` — interactive fuzzy cookbook lookup

A small terminal app that searches a library of cookbooks by
recipe title. Launches into an interactive prompt — type a query
like `lasagne de ricardo` and you get a ranked list of matches
with the book and page where each one lives. Anything starting
with `/` is a slash command; everything else is a fuzzy search.

## Slash commands

| Command          | What it does                                       |
|------------------|----------------------------------------------------|
| `/help`          | List available commands                            |
| `/books`         | List every cookbook in the library                 |
| `/book <name>`   | Show every recipe in a book (name is fuzzy-matched)|
| `/random`        | Pick a recipe uniformly at random                  |
| `/clear`         | Clear the screen + redraw the header               |
| `/quit`          | Leave (Ctrl-D also works)                          |

Anything else is treated as a fuzzy search query.

## The drawer

Type a bare `/` and you get a numbered menu of every command. Type
`/<prefix>` (e.g. `/b`) and the menu narrows to commands matching
that prefix — `/b` shows `/books` and `/book`. If exactly one
command matches the prefix it runs immediately (no menu); if the
matched command takes an argument like `/book <name>`, the app
prompts for it on a second line.

Quirk's stdlib doesn't expose raw terminal access, so the drawer
can't render character-by-character as you type the way `fzf` or
shell completion does. The one-`/`-then-numbered-pick is the
realistic substitute and is plenty discoverable for a 6-command
surface.

## Run

```bash
quirk run examples/recipe_search/main.quirk
```

You'll see (with real ANSI colors in the terminal):

```
  Recipe Search
  fuzzy lookup across 4 cookbooks

  type a recipe name to search, or /help for commands.

?: lasagne de ricardo

  top 5 matches for 'lasagne de ricardo':

  ▸  Lasagne classique
       Ricardo Vol. 2 · p.47  (distance 10)
  ·  Lasagne au saumon fume
       Ricardo Vol. 2 · p.53  (distance 13)
  ...

  best guess: Lasagne classique — Ricardo Vol. 2 p.47

?: q
  Bon appetit.
```

The orange triangle marks the top hit; subsequent matches get a
faint dot. Page numbers are highlighted so they're easy to spot
when you're flipping through a stack of books.

## What it shows

- **`from prompt use { input_optional }`** — interactive read
  that returns `null` on empty input / Ctrl-D, the canonical
  quit signal. `input` (the non-optional variant) re-prompts on
  empty.
- **ANSI color escapes inline** — pure strings, no extra
  dependencies. Works on every modern terminal; piping to a
  file just prints the escapes harmlessly.
- **Canonical `Option[T]`** — `best_match` returns `Some(Match)`
  or `None`; the caller dispatches with `.is_some()` and
  `.unwrap()`. Equally usable with `.map`, `.unwrap_or`, and
  the rest of the v3.2.0 combinator set.
- **Plain structs as records** — `Recipe`, `Book`, `Match`
  instead of nested maps; the `b: Book := book` typed-walrus
  pattern unboxes a `for`-loop variable so field access
  compiles cleanly.
- **`String.distance`** — Levenshtein edit distance, in stdlib.

## Adding cookbooks

`library()` is the seam. Each `Book(name, recipes)` entry holds
a list of `Recipe(title, page)` literals. Drop your own in
there and re-run — the matcher is library-agnostic.

When you outgrow inline data, replace `library()` with one that
reads `data/*.json` from disk (`from encoding.json use { parse }`)
— the call site doesn't change.

## Known limitation: pure Levenshtein

Edit distance over-penalizes length asymmetry: a short query
like `chicken` scores poorly against a long title like
`Roast Chicken with Lemon` because all the extra characters
count as edits. For single-word queries you'll often see
unrelated results above the obvious answer.

Two clean ways to upgrade:

1. **Substring fast-path** — if the lowered title contains the
   lowered query, set distance to `title.length() - query.length()`
   instead of running Levenshtein.
2. **Per-token min-distance** — split both sides on whitespace
   and aggregate per-token. Handles `ricardo lasagne` finding
   `Lasagne classique (Ricardo)`.

Both stay inside the existing `score()` function — easy to try.
