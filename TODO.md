# Quirk TODO

Bigger pieces of work, roughly in priority order. Small fixes go straight
into a release; this list is for things that need a plan.

## 1. `print(a, b)` on one line

Each argument currently prints on its own line. Make it space-separated
like Python. Changes existing programs' output — needs a changelog
warning.

## 2. Windows support for `process` and `window`

Both are POSIX-only (`fork`/`exec`, `dlopen`). Only worth doing if
Windows users matter.

## 3. `ui` follow-ups

Text selection / copy-paste in `TextInput`, scrollable containers,
dropdown, tooltips, multi-line text area.

## Housekeeping

- Optional: Open VSX ownership claim for `Quirk` (Option 4) to get the
  "verified" badge.
