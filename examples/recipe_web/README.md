# `recipe_web/` — browser-served fuzzy cookbook search

The browser-facing companion to [`recipe_search/`](../recipe_search/).
Boots a tiny HTTP server on `127.0.0.1:8080` with an inline-styled
search form; submitting a query like `lasagne de ricardo` returns
ranked matches across the cookbook library, each linked to the
book and page where it lives.

Same matcher and data model as the terminal version — the only
difference is the renderer (HTML cards instead of ANSI lines)
and the I/O loop (HTTP handler instead of `prompt.input_optional`).

## Run

```bash
quirk run examples/recipe_web/main.quirk
# then open http://127.0.0.1:8080/ in a browser
```

Sample browser output:

```
   Recipe Search
   Fuzzy lookup across your cookbook shelf.

   ┌──────────────────────────────────────────────────┐
   │ lasagne de ricardo                       [Search]│
   └──────────────────────────────────────────────────┘

   Top 5 matches for lasagne de ricardo:

   ▌ Lasagne classique
     Ricardo Vol. 2 · p.47

     Lasagne au saumon fume
     Ricardo Vol. 2 · p.53

     ...
```

The accent bar on the first card flags the best match.

## What it shows

- **`from net.server use { Server, Request }`** — full HTTP/1.1
  loop in ~30 LOC. The handler receives a parsed `Request` (path
  + query map + headers + body) and returns a `Response`.
- **`from url use { unquote_form }`** — form-decode the query
  param (browsers encode spaces as `+`; the stdlib's `parse_query`
  only handles `%XX`, so we apply `unquote_form` on top).
- **Inline HTML + CSS** — zero static assets. The server returns
  one complete HTML response with a `<style>` block, so dropping
  this on a fresh machine needs only the Quirk binary.
- **`List.sort(cb)`, Option API, plain-struct records** — same
  matcher core as `recipe_search/`. Both examples demonstrate
  that the data + matching layer is renderer-agnostic.

## Adding cookbooks

Same shape as `recipe_search/`: append to `library()`. When you
outgrow inline data, swap the function for one that loads
`data/*.json` (`from encoding.json use { parse }`) — the call
sites don't change.

## Known limitation

Pure Levenshtein over-penalizes length asymmetry — `chicken`
against `Roast Chicken with Lemon` ranks low because all the
extra characters count as edits. The fix paths (substring fast-
path, per-token min-distance) live inside `score()` in either
example and apply identically to both.
