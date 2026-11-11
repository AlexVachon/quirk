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

Done in ui 1.1 (compiler 7.26.0): selection + clipboard, TextArea,
ScrollView, Dropdown, tooltips. Next ideas: word wrap in TextArea, undo
/ redo, tabs, radio buttons, a progress bar, menus.

## Housekeeping

- Compiler bug: inside a package's own directory, a test file's helper
  `define open(...)` loses to the package's `window.open` (`use window`
  resolving to the project's `src/`) — "Incorrect number of arguments".
  Same file passes from anywhere else. Seen in 7.24.0+; window tests
  work around it with `open_headless`.
- Compiler bug: methods whose names start with `_` aren't virtual — a
  subclass override of `_hook` is ignored when the base class calls
  `self._hook()` (public names dispatch fine). ui 1.1 avoids it.
- Compiler bug: `-self.f()` doesn't parse as `-(self.f())` ("operator '-'
  incompatible types: 'Int' and '<Struct>'"); write `0 - self.f()`.
- Compiler limitation: `base == sub` (a `Widget` against a `Dropdown`)
  is a type error; compare through a variable of the base type.

- Optional: Open VSX ownership claim for `Quirk` (Option 4) to get the
  "verified" badge.
