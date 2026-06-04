# quirk-lsp

Language Server Protocol implementation for [Quirk](https://github.com/AlexVachon/quirk).

## What's in v0.4 (compiler 1.6.3)

- **Diagnostics** on open + save via `quirk --check --diagnostics-json`.
- **Document symbols** (`textDocument/documentSymbol`) — outline,
  breadcrumbs, `@` symbol picker.
- **Formatting** (`textDocument/formatting`) — shells out to `quirk fmt --stdout`.
- **Go-to-definition** (`textDocument/definition`) — same-file decls
  *and* cross-file via the compiler's new `quirk resolve <name>`
  subcommand. Ctrl-click a name imported via `use X` or `from X use { Y }`
  and the LSP jumps to its declaration in the resolved module.
  Caches resolved module paths per session.
- **Lifecycle** — `initialize`, `shutdown`, `exit`, document open/save/close.

Coming later in 1.6.x: hover, completion, references, rename, signature
help, semantic tokens. The VSCode extension keeps its in-process
providers for those.

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
