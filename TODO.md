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
window 1.8 / ui 1.10 (7.41.0): Windows UI Automation bridge, experimental
(QUIRK_ACCESSIBILITY=1) — turn it on by default once tried with NVDA /
Narrator. Next: macOS NSAccessibility, Linux AT-SPI. For RTL, explicit
direction marks (embeddings / isolates) and per-paragraph direction in
word-wrapped areas; complex scripts beyond joining forms (Indic) would
need a shaping engine.

## 3. `web` follow-ups

web 1.0 (7.42.0): routes, middleware, Router, Request / Response helpers,
static files, templates, errors, keep-alive server, TestClient, `quirk new
--web`. Next ideas: WebSockets (`app.ws`), multipart uploads, sessions
(signed cookies), gzip, HTTPS in the server (TLS through the runtime's
OpenSSL), a dev mode that restarts on file changes.

## 4. `live` follow-ups

live 1.0 (7.43.0): server components, events, patching, live pages with
q-link navigation, TestSession. Next ideas: nested components, server
push (a component updating itself from a timer or another visitor:
needs streaming responses — SSE — in web), loading states, file uploads,
and moving components to the browser later if a JS / WebAssembly backend
is ever added.

## Housekeeping

- Optional: Open VSX ownership claim for `Quirk` (Option 4) to get the
  "verified" badge.
