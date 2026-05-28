# quirk-compiler

The compiler + runtime source tree for [Quirk](../README.md).

- **Language reference & install** → [`../README.md`](../README.md)
- **Build from source** → [`../INSTALL.md`](../INSTALL.md)
- **Full CLI reference (run, --debug, package manager, …)** → [`../COMMANDS.md`](../COMMANDS.md)
- **Standard library reference** → [`../STDLIB.md`](../STDLIB.md)
- **Release notes** → [`./CHANGELOG.md`](./CHANGELOG.md)

## Build

```bash
make            # builds bin/quirk and bin/runtime.so
make test       # builds + runs the test suite
make clean
```

Requires `llvm-14-dev`, `libgc-dev`, `libssl-dev`, `libcurl4-openssl-dev` (or the equivalent on your distro). See [INSTALL.md](../INSTALL.md) for the full list.

The unity build at [`src/Runtime/runtime.c`](src/Runtime/runtime.c) compiles into `bin/runtime.so` — that's the file the JIT and emitted native binaries link against at run time.
