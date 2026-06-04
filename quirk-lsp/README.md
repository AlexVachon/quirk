# quirk-lsp

Language Server Protocol implementation for [Quirk](https://github.com/AlexVachon/quirk).

## What's in v0.18 (compiler 1.7.3)

Same as 0.17 plus:

- **Document links** (`textDocument/documentLink`) — `use X` and
  `from X use { … }` lines surface the module name as a clickable
  hyperlink. Ctrl-click jumps to the resolved file. Resolution
  goes through the same per-session cache as go-to-definition.

## What's in v0.17 (compiler 1.7.2)

Same as 0.16 plus:

- **Code actions** (`textDocument/codeAction`) — quick fixes for
  Sema's "did you mean … ?" diagnostics. The compiler now attaches
  candidate names to undefined-identifier errors; the LSP turns each
  candidate into a `Replace with 'X'` code action. The closest match
  is marked `isPreferred` so the editor's default-fix keybinding
  applies it in one keystroke.

## What's in v0.16 (compiler 1.7.1)

Same as 0.15 plus:

- **Call hierarchy** (`callHierarchy/prepareCallHierarchy` +
  `callHierarchy/incomingCalls` + `callHierarchy/outgoingCalls`) —
  given a function or method, list everywhere it's called *from*
  (incoming) and everywhere it calls *to* (outgoing). Uses Sema's
  usage records directly: incoming = usages of the function's name;
  outgoing = usages whose `scope` is this function. Both are scope-
  precise (no false matches from text walks).

## What's in v0.15 (compiler 1.7.0)

Same as 0.14 plus:

- **Semantic find-references and rename.** Sema now records every
  identifier resolution in a usage table, surfaced as `usage` records
  in `--symbols-json`. The LSP prefers these over text search:
  - `textDocument/references` returns Sema's exact resolution chain,
    not regex matches. A parameter `x` in function A doesn't appear
    when listing refs of an unrelated `x` in function B.
  - `textDocument/rename` uses the same data to produce precise,
    scope-respecting edits. Falls back to the v1.6.x text-based
    walk when usage records aren't available (e.g. for cross-file
    references in files Sema didn't visit).
  - Both pass each record through a small line-scan that re-finds
    the exact identifier column, since Sema's records use the
    enclosing-expression's start.

## What's in v0.14 (compiler 1.6.13)

Same as 0.13 plus:

- **Document highlights** (`textDocument/documentHighlight`) — when
  the cursor is on an identifier, every other occurrence in the
  current file gets highlighted. Decl sites render as `Write`,
  other uses as `Read`; most editors render this as a subtle
  background tint that makes "where else does this live?" answerable
  at a glance.

## What's in v0.13 (compiler 1.6.12)

Same as 0.12 plus:

- **Inlay hints** (`textDocument/inlayHint`) — Sema's inferred type
  renders next to `x := value` bindings when the user didn't
  annotate. `x := 42` looks like `x: Int := 42` in the editor; the
  hint is virtual (the source file isn't modified). Powered by a new
  `inferredType` field on `VarDeclNode` that Sema fills from the
  RHS check, surfaced through `--symbols-json`.

## What's in v0.12 (compiler 1.6.11)

Same as 0.11 plus:

- **Folding ranges** (`textDocument/foldingRange`) — collapsible
  regions for: function bodies, struct/enum/interface bodies, `if`
  / `else` / `while` / `for` blocks, multi-line `from X use { … }`
  imports, `---` doc-block fences, and runs of `// …` line comments.
  Brace-balanced scan; no compiler call needed.

## What's in v0.11 (compiler 1.6.10)

Same as 0.10 plus:

- **Rename** (`textDocument/rename` + `textDocument/prepareRename`)
  — scope-aware. Renaming a parameter or local variable touches the
  current file only. Renaming a top-level decl (function, struct,
  enum, interface, method, field, module_const) walks the workspace
  the same way find-references does. `prepareRename` returns the
  identifier span so the editor's rename popup pre-fills correctly.

  Caveat: still text-based for the actual replacement. Two locals
  with the same name in different functions can't yet be renamed
  independently from a workspace rename — that needs per-usage
  tracking from Sema, which is the v1.6.11+ direction.

## What's in v0.10 (compiler 1.6.9)

Same as 0.9 plus:

- **Workspace symbols** (`workspace/symbol`) — `Ctrl+T` / picker
  search across every symbol the LSP has seen this session. Lists
  every top-level decl + method + field (skips parameters and local
  variables, which would be too noisy). Substring matches the query
  case-insensitively; results capped at 500.

## What's in v0.9 (compiler 1.6.8)

Same as 0.8 plus:

- **Signature help** (`textDocument/signatureHelp`) — typing `(` or
  `,` inside a call pops up the callee's parameter list with the
  current argument highlighted. Driven by the cached
  `quirk --symbols-json` records so signatures stay in sync with
  what Sema sees. Multiple matching declarations (e.g. interface +
  concrete method) all show up; the editor renders a chooser.

## What's in v0.8 (compiler 1.6.7)

- **Diagnostics** on open + save via `quirk --check --diagnostics-json`.
- **Document symbols**, **Formatting**, **Go-to-definition** (same
  file + cross-file), **Find references**, **Hover**.
- **Completion** (`textDocument/completion`) — three modes:
  - **Scope-aware identifier completion** — current-file declarations,
    plus the *parameters and local variables* of the function the
    cursor is in (pulled from `quirk --symbols-json` and cached
    per-document).
  - **Imported names** from `from X use { Y, Z }` blocks.
  - **Member access** (triggered by `.`) — after a known imported
    module, the LSP reads its file and offers its top-level decls.
- **Lifecycle** — `initialize`, `shutdown`, `exit`, document open/save/close.

The symbol cache refreshes on `didOpen` + `didSave`. Between saves,
local-variable suggestions are computed against the on-disk state —
fine for typical edit loops; just save when you've added new locals
you want to complete against.

Coming later in 1.6.x: signature help, semantic-aware rename (needs
usage tracking in the compiler too). The VSCode extension keeps its
in-process providers for those.

## Install

```bash
npm install -g quirk-lsp        # global bin: `quirk-lsp`
# or run from this repo
cd quirk-lsp && npm install && npm run build
node out/server.js              # speaks LSP over stdio
```

You also need the Quirk compiler on `PATH` (or pass `QUIRK_BIN`, or set the LSP init option below).

## Editor configuration

### Neovim (built-in LSP)

```lua
-- in init.lua, somewhere after vim.lsp is loaded
local lsp_config = {
  cmd = { 'quirk-lsp' },                -- or: 'node', '/path/to/quirk-lsp/out/server.js'
  filetypes = { 'quirk' },
  root_dir = function(fname)
    return vim.fs.dirname(vim.fs.find('quirk.toml', { path = fname, upward = true })[1])
       or vim.fs.dirname(fname)
  end,
  -- Pin the compiler if it isn't on PATH; equivalent to QUIRK_BIN.
  init_options = { quirk = { executablePath = '/usr/local/bin/quirk' } },
}

vim.api.nvim_create_autocmd('FileType', {
  pattern = 'quirk',
  callback = function() vim.lsp.start(lsp_config) end,
})
vim.filetype.add({ extension = { quirk = 'quirk' } })
```

### Helix (`languages.toml`)

```toml
[[language]]
name             = "quirk"
scope            = "source.quirk"
file-types       = ["quirk"]
roots            = ["quirk.toml"]
language-servers = ["quirk-lsp"]

[language-server.quirk-lsp]
command = "quirk-lsp"
# Optional: pin the compiler binary
config = { quirk = { executablePath = "/usr/local/bin/quirk" } }
```

### Zed

```json
// ~/.config/zed/settings.json — under "lsp"
{
  "lsp": {
    "quirk-lsp": {
      "binary": { "path": "quirk-lsp" },
      "initialization_options": {
        "quirk": { "executablePath": "/usr/local/bin/quirk" }
      }
    }
  }
}
```

(Plus a Zed extension that maps `.quirk` files to the `quirk` language; not yet shipped.)

### VSCode

The official VSCode extension at `quirk-vscode/` doesn't yet route through this LSP — it uses its own in-process providers. Switch is on the roadmap for a later 1.6.x release.

## Compiler discovery

`quirk-lsp` finds the `quirk` binary in this order:

1. `initializationOptions.quirk.executablePath` from the editor (see Neovim/Helix examples)
2. `QUIRK_BIN` environment variable
3. `quirk` on `PATH`

If none work, the LSP keeps running but won't produce diagnostics. Check the server's log channel in your editor for the warning.

## Diagnostic timing

The server runs the compiler on:

- `textDocument/didOpen` — when a `.quirk` file is opened
- `textDocument/didSave` — after every save

It does **not** run on every keystroke. Running the full compiler at every change would queue up many concurrent processes for a fast typist. If you want push-button checking on a buffer that hasn't been saved, save the buffer.

## The wire format

`quirk --check --diagnostics-json <file>` emits one [NDJSON](http://ndjson.org/) record per diagnostic to stdout:

```jsonc
{"level":"error","msg":"undefined variable 'foo'","path":"/abs/path/file.quirk","line":3,"col":10}
```

- `level` — `error` | `warning` | `info`
- `line` / `col` — 1-based (the compiler's native form). The LSP converts to 0-based for the protocol.

Other tools can consume this too (CI, custom editor integrations, pre-commit hooks).

## License

MIT (see `LICENSE`).
