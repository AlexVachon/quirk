# Installing Quirk

Quirk is a self-hosted language compiler built on LLVM 14. It produces native binaries via JIT (default) or AOT (`-o <file>`).

---

## One-liner install (prebuilt binary, Linux x86_64)

```bash
curl -fsSL https://raw.githubusercontent.com/AlexVachon/quirk/main/install.sh | sh
```

Drops the compiler + runtime + stdlib into `~/.quirk/`. After it runs, add the two `export` lines it prints to your shell profile.

To install a specific version:

```bash
QUIRK_VERSION=v1.0.0 curl -fsSL https://raw.githubusercontent.com/AlexVachon/quirk/main/install.sh | sh
```

Other platforms (macOS, Windows, ARM): build from source — see below.

---

## What's in the install

The install gives you two artifacts in `bin/`:

| File | Role |
|------|------|
| `quirk` | The compiler / package manager CLI |
| `runtime.so` | C runtime (GC, stdlib externs, OS bridges) |

Both are needed at run time. `quirk` resolves `runtime.so` from its own directory, so as long as they ship together, you can place them anywhere.

---

## Requirements

| Dependency | Why | Debian / Ubuntu package |
|------------|-----|-------------------------|
| LLVM 14 | Code generation + JIT | `llvm-14-dev libllvm14` |
| Boehm GC | Runtime memory management | `libgc-dev` |
| libcurl | `net.http` runtime support | `libcurl4-openssl-dev` |
| OpenSSL | `crypto.*` hashes / HMAC / UUID | `libssl-dev` |
| build tools | C++ compile (C++17) | `build-essential pkg-config cmake git` |

macOS and Windows aren't packaged yet — the runtime uses some POSIX-only paths (`/proc/self/exe`, `termios`). Should be portable with modest work.

---

## Quick install (Ubuntu / Debian / WSL)

```bash
git clone https://github.com/AlexVachon/quirk.git
cd quirk/quirk-compiler
bash setup.sh
```

`setup.sh` does four things:

1. `apt install` the system dependencies.
2. Build the compiler + runtime via `make`.
3. Append `export QUIRK_HOME=<repo>/quirk-compiler` and `PATH` to your shell rc.
4. Smoke-test `bin/quirk --help` and verify `bin/runtime.so` is in place.

When it finishes:

```bash
source ~/.bashrc           # or ~/.zshrc
quirk --version
# quirk 1.0.0
```

---

## Manual install

If you'd rather drive the build yourself:

```bash
# 1. System packages
sudo apt-get install -y build-essential cmake pkg-config git \
    libgc-dev libcurl4-openssl-dev libssl-dev \
    llvm-14 llvm-14-dev libllvm14

# 2. Configure + build
cd quirk-compiler
cmake -B build -S .
cmake --build build -j$(nproc)

# 3. Verify
./bin/quirk --version
echo 'define main() { print("Hello, Quirk!") }' > /tmp/hi.quirk
./bin/quirk /tmp/hi.quirk
# Hello, Quirk!
```

The compiler + runtime end up in `quirk-compiler/bin/`.

### Putting `quirk` on PATH

Two options. Pick whichever fits your workflow.

**Option A — point PATH at the build tree (dev style):**

```bash
echo 'export QUIRK_HOME="'"$PWD"'"' >> ~/.bashrc
echo 'export PATH="$QUIRK_HOME/bin:$PATH"' >> ~/.bashrc
source ~/.bashrc
```

`QUIRK_HOME` is how the runtime finds the standard library (`$QUIRK_HOME/libs/`).

**Option B — install to `/usr/local` (system-wide):**

```bash
sudo install -m 755 bin/quirk     /usr/local/bin/
sudo install -m 755 bin/runtime.so /usr/local/bin/

sudo mkdir -p /usr/local/lib/quirk
sudo cp -r libs/* /usr/local/lib/quirk/
```

No `QUIRK_HOME` needed in this case — Quirk falls back to `/usr/local/lib/quirk/` when the env var is unset.

---

## Verifying

```bash
quirk --version                          # quirk 1.0.0
quirk --help                             # full command reference
echo 'define main() { print("ok") }' | tee /tmp/x.quirk
quirk /tmp/x.quirk                          # ok
```

If you see `Error: could not load runtime.so`, the `quirk` binary couldn't find `runtime.so` next to itself. Make sure they're in the same directory (or that `$QUIRK_HOME/bin/runtime.so` exists).

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| `find_package(LLVM 14)` fails | LLVM 14 not installed or installed in a non-default prefix | `apt install llvm-14-dev`; if it's elsewhere, set `CMAKE_PREFIX_PATH=/path/to/llvm-14/cmake` |
| `Package 'bdw-gc' not found` | libgc-dev missing | `apt install libgc-dev` |
| `Could not open module 'typing'` | Compiler can't find stdlib | Verify `$QUIRK_HOME` is set (`echo $QUIRK_HOME`) or that you run from a directory next to a `libs/` |
| `error: -lcurl: No such file` | libcurl-dev missing | `apt install libcurl4-openssl-dev` |
| `Crypto_*` symbols missing | OpenSSL not linked | `apt install libssl-dev` then rebuild |

---

## Updating

```bash
cd quirk-compiler
git pull
cmake --build build -j$(nproc)
```

The CLI is forward-compatible — venvs and `packages/` directories from earlier versions keep working without reinstall.

---

Next: [PACKAGES.md](PACKAGES.md) for using and creating Quirk packages.
