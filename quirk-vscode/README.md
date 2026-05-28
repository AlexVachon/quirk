# Quirk for VSCode

Official language support for [Quirk](https://github.com/AlexVachon/quirk) — a compiled, statically-typed language with Python-like syntax that JIT-compiles via LLVM 14.

![Quirk in VSCode](https://raw.githubusercontent.com/AlexVachon/quirk/main/quirk-vscode/icons/icon.png)

## Features

- **Full syntax highlighting** — tuned TextMate grammar covering Quirk's structs, lambdas, pattern matching, interpolated strings, generics, and the spread/range operators.
- **Interactive debugger** — F5 launches the qdb stepper with gutter breakpoints, the Variables panel, the Call Stack, hover-evaluate, and Python-style inline value decorations next to each variable reference on paused lines.
- **Smart completions** — context-aware suggestions for stdlib modules, struct fields, method names, lambda params, generic type params, and `case`-arm bindings.
- **Hover docs** — Python-Pylance-style `(kind) name` prefixes (`(function)`, `(method)`, `(struct)`, `(parameter)`, `(constant)`, `(variable)`), inline docstrings pulled from `--- ... ---` blocks above definitions, and full signature display.
- **Definition + reference jumps** — Ctrl+click navigates to functions, methods, struct fields, lambda params, variadic `...args`, and module aliases. Find All References surfaces every call site.
- **Diagnostics** — undefined-variable warnings, unused-symbol hints, strict-import errors (`Cannot call module 'X' directly`), and dead-code highlighting.
- **Refactoring** — rename symbol, quick-fix actions (add `use`, fix imports, etc.), and the formatter (`quirk fmt`).
- **Document outline + symbol search** — breadcrumbs and `Ctrl+T` navigation work across structs, functions, methods, and enum variants.
- **Run / Debug toolbar** — combo button in the editor title with grouped sections: *Run File*, *Run in Dedicated Terminal*, *Debug File*, *Debug using launch.json*.

## Getting started

1. **Install the Quirk compiler.** One-liner for Linux x86_64:
   ```bash
   curl -fsSL https://raw.githubusercontent.com/AlexVachon/quirk/main/install.sh | sh
   ```
   Add the two `export` lines it prints to your shell profile. Other platforms: see the [INSTALL guide](https://github.com/AlexVachon/quirk/blob/main/INSTALL.md).

2. **Install this extension.** Quirk ships its VSCode extension as a `.vsix` on the GitHub Releases page (not the Marketplace yet):

   - Browse to https://github.com/AlexVachon/quirk/releases
   - Find the latest **`quirk-vscode vX.Y.Z`** release (tagged `vscode-v*`)
   - Download `quirk-X.Y.Z.vsix`
   - Install it:
     ```bash
     code --install-extension /path/to/quirk-X.Y.Z.vsix
     ```
     Or, in VSCode: open the Extensions sidebar (`Ctrl+Shift+X`) → click the `…` menu → **Install from VSIX…**

   One-liner (Linux/macOS, requires the [`gh`](https://cli.github.com/) CLI):
   ```bash
   gh release download --repo AlexVachon/quirk --pattern '*.vsix' --dir /tmp && \
     code --install-extension /tmp/quirk-*.vsix
   ```

3. **Open a `.quirk` file.** The extension auto-activates. If the compiler isn't found, the status bar shows a warning — click it to set `quirk.compilerPath` or `quirk.quirkHome`.

4. **Run** with `Ctrl+F5` or click the `▷` button in the editor title. **Debug** with `F5` — set a gutter breakpoint and the stepper pauses there.

## Settings

| Setting | Default | What it does |
|---------|---------|--------------|
| `quirk.compilerPath` | `""` | Override the `quirk` binary path. Leave empty to auto-detect from `QUIRK_HOME` → `PATH`. |
| `quirk.quirkHome` | `""` | Override `QUIRK_HOME` (directory containing `bin/` and `lib/quirk/`). Picked up by the debugger when launching the debuggee. |

## Debugger configuration

A minimal `.vscode/launch.json`:

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "type": "quirk",
      "request": "launch",
      "name": "Quirk: current file",
      "program": "${file}",
      "stopOnEntry": false
    }
  ]
}
```

Optional fields: `args`, `cwd`, `env`, `compilerPath`, `quirkHome`. F5 with no `launch.json` synthesizes a config for the active file.

## Requirements

- VSCode 1.75+
- A working `quirk` compiler ([install guide](https://github.com/AlexVachon/quirk/blob/main/INSTALL.md))

## Links

- [Language reference](https://github.com/AlexVachon/quirk/blob/main/README.md)
- [Standard library](https://github.com/AlexVachon/quirk/blob/main/STDLIB.md)
- [CLI / debugger commands](https://github.com/AlexVachon/quirk/blob/main/COMMANDS.md)
- [Source / issues](https://github.com/AlexVachon/quirk)

## License

[MIT](./LICENSE) — same as the Quirk compiler.
