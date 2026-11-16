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
TreeView, Table; ui 1.4 (7.31.0): word wrap, editable cells,
ColorPicker, FileDialog, drag and drop; ui 1.5 (7.32.0): working menu
shortcuts, SplitView, Toolbar, StatusBar, DatePicker; ui 1.6 (7.33.0):
TrueType fonts, ListItem icons, Canvas, animations; window 1.4
(7.36.0): high DPI; ui 1.7 (7.37.0): IME / composed text input; window
1.6 (7.38.0): font fallback; window 1.7 / ui 1.8 (7.39.0): right-to-left
text; ui 1.9 (7.40.0): accessibility tree (app.accessibility(), on_access).
Next: the screen-reader bridges — Windows UI Automation first (a COM
provider per node in the window runtime, tested with NVDA / Narrator),
then macOS NSAccessibility and Linux AT-SPI. For RTL, explicit
direction marks (embeddings / isolates) and per-paragraph direction in
word-wrapped areas; complex scripts beyond joining forms (Indic) would
need a shaping engine.

## Housekeeping

- Optional: Open VSX ownership claim for `Quirk` (Option 4) to get the
  "verified" badge.
