# Quirk TODO

Bigger pieces of work, roughly in priority order. Small fixes go straight
into a release; this list is for things that need a plan.

## 1. PNG / JPG images in `window`

Only BMP loads today. Vendor `stb_image` (single public-domain C file)
into the runtime; no new system dependency.

## 2. `print(a, b)` on one line

Each argument currently prints on its own line. Make it space-separated
like Python. Changes existing programs' output — needs a changelog
warning.

## 3. Windows support for `process` and `window`

Both are POSIX-only (`fork`/`exec`, `dlopen`). Only worth doing if
Windows users matter.

## 4. Untyped receivers and String method names

`x.contains(...)` / `.length()` / `.find(...)` etc. on an untyped value
(a plain List's element) always take the String path, because String has
those methods — a struct with a same-named method crashes there. Dispatch
on the runtime type instead (String when it's a String, else the struct
method). `ui` itself calls them on typed `Widget` values, so it's not
affected.

## 5. `ui` follow-ups

Text selection / copy-paste in `TextInput`, scrollable containers,
dropdown, tooltips, multi-line text area.

## Housekeeping

- Optional: Open VSX ownership claim for `Quirk` (Option 4) to get the
  "verified" badge.
