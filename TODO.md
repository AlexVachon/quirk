# Quirk TODO

Bigger pieces of work, roughly in priority order. Small fixes go straight
into a release; this list is for things that need a plan.

## 1. Native Windows port — release (Stage 5)

First Windows release in 7.25.0 (experimental): `install.ps1`, package
manager, AOT, venvs, REPL. Left: a Windows job in `test.yml` that fails
the build; drop `continue-on-error` from the release job after a couple
of clean releases. See
[quirk-compiler/docs/WINDOWS-PORT.md](quirk-compiler/docs/WINDOWS-PORT.md).

## 2. `ui` follow-ups

Text selection / copy-paste in `TextInput`, scrollable containers,
dropdown, tooltips, multi-line text area.

## Housekeeping

- Optional: Open VSX ownership claim for `Quirk` (Option 4) to get the
  "verified" badge.
