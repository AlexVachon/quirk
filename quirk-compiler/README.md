# quirk-compiler

The compiler + runtime source tree for [Quirk](../README.md).

- **Language reference & install** → [`../README.md`](../README.md)
- **Build from source** → [`../INSTALL.md`](../INSTALL.md)
- **Full CLI reference (run, --debug, package manager, …)** → [`../COMMANDS.md`](../COMMANDS.md)
- **Standard library reference** → [`../STDLIB.md`](../STDLIB.md)
- **Release notes** → [`./CHANGELOG.md`](./CHANGELOG.md)

## Build

```bash
make            # builds bin/quirk-cpp, bin/runtime.so, bin/quirk (symlink)
make test       # builds + runs the test suite
make clean
```

Requires `llvm-14-dev`, `libgc-dev`, `libssl-dev`, `libcurl4-openssl-dev` (or the equivalent on your distro). See [INSTALL.md](../INSTALL.md) for the full list.

The unity build at [`src/Runtime/runtime.c`](src/Runtime/runtime.c) compiles into `bin/runtime.so` — that's the file the JIT and emitted native binaries link against at run time.

## The compiler

Quirk ships one production compiler:

| Binary | Written in | Role |
| --- | --- | --- |
| `bin/quirk-cpp` | C++ (LLVM) | Production compiler. Full language surface, package manager subcommands, LLVM JIT + native code emit. |
| `bin/quirk` | symlink | Points to `bin/quirk-cpp` for `#!/usr/bin/env quirk` shebangs and habit-typing. |

### Self-hosted compiler

The Quirk-in-Quirk compiler `bin/quirk-selfhost` was extracted to its own repo in **v5.2.0**:

**→ [github.com/AlexVachon/quirk-selfhost](https://github.com/AlexVachon/quirk-selfhost)**

It reached a stable milestone (40/60 corpus + byte-identical bootstrap fixed point) in v5.0.0-alpha.43 (later renamed v5.1.0 when the alpha push was closed). The bootstrap proof — `bin/quirk-selfhost` compiling its own source to byte-identical IR under itself — is preserved as an independently checkable artifact in that repo:

```bash
git clone https://github.com/AlexVachon/quirk-selfhost
cd quirk-selfhost && make fixedpoint
```

New language features land here in `bin/quirk-cpp` first; the self-host adopts them lazily via its own release cycle.
