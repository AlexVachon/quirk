# Quirk TODO

Bigger pieces of work, roughly in priority order. Small fixes go straight
into a release; this list is for things that need a plan.

## 1. Native Windows port — release (Stage 5)

First Windows release in 7.25.0 (experimental): `install.ps1`, package
manager, AOT, venvs, REPL; a failing Windows job in `test.yml` since
7.28.0. Left: drop `continue-on-error` from the release job after a
couple of clean releases (7.25.0-7.27.0 were clean). See
[quirk-compiler/docs/WINDOWS-PORT.md](quirk-compiler/docs/WINDOWS-PORT.md).

## 2. `ui` follow-ups

Done in ui 1.1 (compiler 7.26.0): selection + clipboard, TextArea,
ScrollView, Dropdown, tooltips. Next ideas: word wrap in TextArea, undo
/ redo, tabs, radio buttons, a progress bar, menus.

## Housekeeping

- Optional: Open VSX ownership claim for `Quirk` (Option 4) to get the
  "verified" badge.
