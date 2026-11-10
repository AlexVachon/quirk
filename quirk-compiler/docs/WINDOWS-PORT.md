# Native Windows port

Status: **in progress** — Stages 1–3 done (2026-11-10): the compiler
and runtime build with MSVC / clang on `windows-latest`, and Quirk
programs run through the JIT (printing, strings, structs, exceptions,
files, GC, async tasks). Stage 4 (the test suite, `process`, `regex`,
`window`, `net`) next. Until it's done, use Quirk on Windows
through WSL — see [INSTALL.md → Windows (WSL)](../../INSTALL.md#windows-wsl).

Work happens on the `windows-probe` branch. Every change is behind
`#ifdef _WIN32` (or Windows-only files), so Linux and macOS builds are
unaffected. CI: `.github/workflows/windows-probe.yml` runs on every push
to that branch and compiles each source file on `windows-latest`,
reporting errors per file (`tools/windows_probe.sh`; the artifact
`windows-probe` has the full logs).

## Toolchain

| Piece | Source | Notes |
|---|---|---|
| LLVM 14.0.6 (headers + libs) | conda-forge `llvmdev` (MSVC build) | MSYS2 only has LLVM 19+ |
| C++ compiler | MSVC `cl.exe`, or clang ≥ 20 | clang 14 can't read the current Visual Studio STL headers (`STL1000: expected Clang 20 or newer`) |
| C compiler (runtime) | clang 14 (conda-forge) | C needs no STL |
| Boehm GC | conda-forge `bdw-gc` 8.2.x | built for Win32 threads |
| pthreads | conda-forge `pthreads-win32` | task scheduler (`core/task.c`) |
| OpenSSL, SDL2 | conda-forge | `net` TLS, `window` |

## Findings (first probe, 2026-11-10)

Compiler (C++): blocked first by `unistd.h` in `Compiler.cpp` and
`PackageManager.hpp` — the compiler driver uses `fork` / `execv` /
`waitpid` / `popen` / `termios` for downloads, venvs, the AOT pipeline
and the REPL (~10 files).

Runtime (C): 35 errors —
- `windows.h` pulled in before `winsock2.h` (socket type redefinitions)
- `clock_gettime(CLOCK_REALTIME)` (task deadlines)
- `SIGBUS`, `PATH_MAX`
- `regex.h` — no POSIX regex on Windows

## Stages

1. ✅ **Runtime compiles** (syntax) with clang on Windows — include order,
   time, signals, limits; `regex` stubbed (raises "not supported on Windows
   yet") until a real backend.
2. ✅ **Compiler compiles** with MSVC — port the driver's process / terminal
   code (`CreateProcess`, pipes, console API; REPL without linenoise).
3. ✅ **Links and runs hello-world** — runtime as a DLL with every
   `Core_*` / `Fs_*` / ... symbol exported (the JIT resolves them by name),
   ORC JIT on COFF, Boehm GC init; audit `long` (32-bit on Windows) in
   generated code and C externs.
4. **Libraries** — `process` (`CreateProcess` + pipes), `regex` (PCRE2),
   `window` (`LoadLibrary("SDL2.dll")`), `net` (Winsock init), AOT
   linking with `link.exe` / `lld-link`; then run the test suite in a
   Windows CI job.
5. **Release** — Windows build in `release.yml`, `install.ps1`.

## What changed for Windows (so far)

- `src/Runtime/win_compat.h` — Windows headers in Winsock-first order,
  `PATH_MAX`, `clock_gettime`, `nanosleep`; `quirk_crt_*` wrappers the
  JIT binds `printf` / `fopen` / `malloc` / ... to (one CRT, one stdout);
  `QUIRK_SETJMP` (no SEH unwind).
- `src/platform.hpp` + `src/platform_win32.cpp` — the driver's OS layer:
  process spawning (`CreateProcess` + pipes), detached background builds,
  the big-stack JIT thread, terminal width, temp files, `run_shell`
  quoting for `cmd.exe`. Only `platform_win32.cpp` includes `<windows.h>`
  (its `ERROR` / `CONST` / `TRUE` macros and its `TokenType` enum
  constant clash with the compiler's names).
- Codegen: `_setjmp(buf, NULL)` on Windows (no SEH unwinding through JIT
  frames).
- Runtime: tasks register their threads with the GC; `fs` temp files,
  `walk`, `remove_tree`, wildcard matching; sockets close with
  `closesocket`; `regex` raises "not supported on Windows yet".
- `tools/windows_build.sh` builds `runtime.dll` (every external symbol
  exported via a generated `.def`) and `quirk.exe`, then smoke-tests.

## Running the probe

Push to `windows-probe`; read the job log, or download the artifact:

```bash
gh run list --branch windows-probe --limit 1
gh run download <id> -n windows-probe -D /tmp/probe && cat /tmp/probe/summary.txt
```
