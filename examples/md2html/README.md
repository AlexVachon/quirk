# md2html — tiny markdown → HTML (~100 LOC)

A small inline-markdown converter showing string manipulation in
idiomatic Quirk.

## Run

```bash
quirk run examples/md2html/main.quirk
```

Renders the canned `EXAMPLE` constant. Output:

```html
<h1>Quirk Markdown → HTML</h1>
<p>A <em>tiny</em> converter — try editing the <strong>source</strong>.</p>
<h2>Features</h2>
<p>Inline <code>code</code> and <a href="https://example.com">links</a> work.</p>
```

## Supported subset

| Markdown        | HTML                              |
|-----------------|-----------------------------------|
| `# Heading`     | `<h1>Heading</h1>`                |
| `## Heading`    | `<h2>Heading</h2>`                |
| `### Heading`   | `<h3>Heading</h3>`                |
| `**bold**`      | `<strong>bold</strong>`           |
| `*italic*`      | `<em>italic</em>`                 |
| `` `code` ``    | `<code>code</code>`               |
| `[text](url)`   | `<a href="url">text</a>`          |
| blank line      | paragraph break                   |

Anything else becomes a `<p>…</p>`. No nesting, no lists, no code
blocks, no escaping.

## What it shows

| Feature                 | Where in the code                                        |
|-------------------------|----------------------------------------------------------|
| Functional decomposition| `render_line → render_inline → replace_pairs / replace_links` |
| Token-pair state machine| `replace_pairs` flips `open` each time the marker is hit |
| String slicing          | `substring(start, end)`, `find(sub)`, `length()`         |
| Order-dependent passes  | `**` must replace before `*` (else half-eaten)           |
| `for raw in ... { x: T := raw }` | annotated unbox of List elements                |

## Why this exists

The other two examples (`todo_cli/`, `calc/`) cover data-modeling +
interpretation. This one covers the third common shape:
**text-in, text-out**. Most real Quirk programs are some mix of
all three.
