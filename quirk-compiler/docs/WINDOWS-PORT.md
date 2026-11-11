# Native Windows port

Status: **builds from source; experimental.** Stages 1–4 done
(2026-11-10): the compiler and runtime build with MSVC / clang on
`windows-latest`, and the test suite passes there (281 / 282 — the last
one waits on `quirk-process` v1.0.2). No Windows release yet (Stage 5). Until it's done, use Quirk on Windows
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
4. ✅ **Libraries** — `process` (`CreateProcess` + pipes), `regex` (vendored musl / TRE),
   `window` (`LoadLibrary("SDL2.dll")`), `net` (Winsock init), AOT
   linking with `link.exe` / `lld-link`; then run the test suite in a
   Windows CI job.
5. **Release** — in progress:
   - ✅ package manager on Windows (`new`, `install`, `test`, `deps`,
     `show`, `search` smoke-tested in CI): shell commands via
     `qplat::popen_cmd` / `run_shell` (`/dev/null` → `NUL`, `cmd.exe`
     quoting), `have_command` (`where`), `git -C`, System32 bsdtar,
     in-process SHA-256, home dir from `USERPROFILE`, `install` refreshes
     the registry index on a miss.
   - ✅ AOT: `-o`, `--build` and the background build cache. IR → COFF
     object in-process (no `llc`), CRT names renamed to the runtime's
     `quirk_crt_*` wrappers (as the JIT binds them), linked with
     `lld-link` / `link.exe` against `runtime.lib`. Needs a linker and the
     Windows SDK libraries (a Visual Studio Build Tools prompt); without
     one, `-o` says so and background builds record `.failed` and keep
     using the JIT. A cached `.exe` runs as a child with quirk's `bin` dir
     on PATH (no `exec` on Windows); `-o` output finds `runtime.dll`
     through PATH.
   - venv `activate` for `cmd` / PowerShell; REPL history.
   - Windows build in `release.yml`, `install.ps1`; a Windows job in
     `test.yml`.

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
- `process`: `libs/process_win32.c` (`CreateProcess`, reader / writer
  threads for `communicate`, `TerminateProcess` with exit code -N);
  `process.shell` uses `cmd.exe /c`.
- `regex`: musl's POSIX regex (TRE) vendored in
  `src/third_party/musl_regex`, linked into `runtime.dll`.
- `window`: `SDL2.dll` via `LoadLibrary`.
- `tools/windows_build.sh` builds `runtime.dll` (every external symbol
  exported via a generated `.def`) and `quirk.exe`, then smoke-tests.

## Running the probe

Push to `windows-probe`; read the job log, or download the artifact:

```bash
gh run list --branch windows-probe --limit 1
gh run download <id> -n windows-probe -D /tmp/probe && cat /tmp/probe/summary.txt
```
