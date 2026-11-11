# Quirk TODO

Bigger pieces of work, roughly in priority order. Small fixes go straight
into a release; this list is for things that need a plan.

## 1. Native Windows port — release (Stage 5)

First Windows release in 7.25.0 (experimental): `install.ps1`, package
manager, AOT, venvs, REPL; a failing Windows job in `test.yml` since
7.28.0; the release job fails on a broken Windows build since 7.29.0. See
[quirk-compiler/docs/WINDOWS-PORT.md](quirk-compiler/docs/WINDOWS-PORT.md).

## 2. `ui` follow-ups

Done in ui 1.1 (compiler 7.26.0): selection + clipboard, TextArea,
ScrollView, Dropdown, tooltips; ui 1.2 (7.29.0): undo / redo,
RadioGroup, ProgressBar, Tabs; ui 1.3 (7.30.0): menus, context menus,
TreeView, Table. Next ideas: word wrap in TextArea, editable table
cells, drag and drop, a color picker, a file dialog.

## Housekeeping

- Optional: Open VSX ownership claim for `Quirk` (Option 4) to get the
  "verified" badge.
