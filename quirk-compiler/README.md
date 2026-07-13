# quirk-compiler

The compiler + runtime source tree for [Quirk](../README.md).

- **Language reference & install** → [`../README.md`](../README.md)
- **Build from source** → [`../INSTALL.md`](../INSTALL.md)
- **Full CLI reference (run, --debug, package manager, …)** → [`../COMMANDS.md`](../COMMANDS.md)
- **Standard library reference** → [`../STDLIB.md`](../STDLIB.md)
- **Release notes** → [`./CHANGELOG.md`](./CHANGELOG.md)

## Build

```bash
make            # builds bin/quirk-cpp, bin/quirk-selfhost, bin/runtime.so, bin/quirk driver
make test       # builds + runs the test suite
make clean
```

Requires `llvm-14-dev`, `libgc-dev`, `libssl-dev`, `libcurl4-openssl-dev` (or the equivalent on your distro). See [INSTALL.md](../INSTALL.md) for the full list.

The unity build at [`src/Runtime/runtime.c`](src/Runtime/runtime.c) compiles into `bin/runtime.so` — that's the file the JIT and emitted native binaries link against at run time.

## Two compilers, one language

Quirk ships two compiler binaries plus a driver script:

| Binary | Written in | Corpus coverage | Role |
| --- | --- | --- | --- |
| `bin/quirk-cpp` | C++ (LLVM) | 60/60 tests | Production compiler. Full language surface, package manager subcommands, LLVM JIT + native code emit. |
| `bin/quirk-selfhost` | Quirk (compiled by `bin/quirk-cpp`) | 40/60 tests | Self-hosted compiler. Compiles a working subset of Quirk. Byte-identical fixed point under itself. |
| `bin/quirk` | shell wrapper | — | User-facing driver. Uses selfhost when it can, falls back to `bin/quirk-cpp` for package-manager subcommands and features selfhost doesn't yet handle. |

### Self-host status

`bin/quirk-selfhost` was built up incrementally through the v5.0.0-alpha series (see [CHANGELOG.md](./CHANGELOG.md)). It reached a stable milestone at **v5.0.0-alpha.43**:

- **40/60 corpus tests** clean-exit under the selfhost pipeline (compile → link with `bin/runtime.so` → run).
- **Bootstrap fixed point** — `bin/quirk-selfhost` compiles its own source (`selfhost/*.quirk`) to byte-identical IR under itself. Verify with `make selfhost-fixedpoint`.
- **E2E codegen suite** — 190/190 unit tests pass against the selfhost pipeline (`selfhost/codegen_e2e.sh`).

The remaining 20 corpus tests exercise language features the selfhost doesn't yet implement:

- Closure capture (lambda block bodies referencing outer scope)
- Per-allocation shape tagging (needed to disambiguate `String*` vs raw c-string at runtime)
- Struct inheritance dispatch beyond `super().__init(...)` on `String`
- Distinguishable-null `Int?` / `Bool?` representation
- Named-argument reordering at call sites
- Variadic-arg auto-packing at call sites
- Package-aware name resolution (external structs like `File`, `Match`)

Each is a multi-day project — not a "single alpha" size. **The self-host push is on hold at 40/60**; `bin/quirk-cpp` remains the production compiler for the foreseeable future. New language features should land in the C++ compiler first; the self-host adopts them lazily if/when a specific use case needs them.
