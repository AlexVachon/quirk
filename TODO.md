# Quirk TODO

Bigger pieces of work, roughly in priority order. Small fixes go straight
into a release; this list is for things that need a plan.

## 1. Native Windows port — release (Stage 5)

Builds from source and passes the test suite on Windows (7.24.0). Left:
a Windows release build + `install.ps1`, AOT linking, the package
manager's `tar` / `git` steps, REPL history, a Windows CI job. See
[quirk-compiler/docs/WINDOWS-PORT.md](quirk-compiler/docs/WINDOWS-PORT.md)
for findings, the toolchain and the stage list. Windows users use WSL
meanwhile (INSTALL.md).

## 2. `ui` follow-ups

Text selection / copy-paste in `TextInput`, scrollable containers,
dropdown, tooltips, multi-line text area.

## Housekeeping

- Optional: Open VSX ownership claim for `Quirk` (Option 4) to get the
  "verified" badge.
