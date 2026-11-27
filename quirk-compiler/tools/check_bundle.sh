#!/usr/bin/env bash
# `quirk bundle`: a folder that runs anywhere — moved away from where it was
# built, with an empty environment (no QUIRK_HOME, no quirk on PATH). The
# binary finds the runtime next to it; a component's template file is found
# from the bundle; data (.db) and tests stay out.
set -uo pipefail
cd "$(dirname "$0")/.."
QUIRK="${QUIRK:-$PWD/bin/quirk}"
fails=0
ok()   { echo "  ok $1"; }
fail() { echo "FAIL $1"; fails=$((fails + 1)); }
work=$(mktemp -d)
trap 'kill $pid 2>/dev/null; rm -rf "$work"' EXIT
pid=0
app="$work/app"
mkdir -p "$app/src/pages" "$app/public" "$app/tests"
cat > "$app/main.qk" <<'Q'
use web
use live
use sys
from src.pages.about use { AboutPage }
define main() -> void {
    router := live.Router()
    router.add("/", AboutPage)
    app := web.App()
    app.use(web.static("public"))
    app.mount("/", router)
    app.listen(Int(sys.getenv("PORT")))
}
Q
cat > "$app/src/pages/about.qk" <<'Q'
use live
struct AboutPage : live.Page {
    n: Int
    define on_load(self) -> void { self.n = 41 }
}
Q
echo '<p class="from-file">template file {{ n }}</p>' > "$app/src/pages/about_page.html"
echo 'body { color: teal }' > "$app/public/site.css"
echo 'not to be shipped' > "$app/data.db"
echo 'define test_x() -> void {}' > "$app/tests/x_test.qk"

out=$(cd "$app" && "$QUIRK" bundle main.qk -o "$work/dist" 2>&1)
[ -x "$work/dist/main" ] && ok "the binary" || { fail "no binary: $out"; exit 1; }
[ -f "$work/dist/runtime.so" ] && ok "the runtime next to it" || fail "no runtime.so"
[ -f "$work/dist/src/pages/about_page.html" ] && ok "the component's template file" || fail "no template file"
[ -f "$work/dist/public/site.css" ] && ok "public/" || fail "no public/"
[ ! -e "$work/dist/data.db" ] && ok "no .db file" || fail "data.db was copied"
[ ! -e "$work/dist/tests" ] && ok "no tests" || fail "tests/ was copied"

mv "$work/dist" "$work/moved"
port=$((20000 + RANDOM % 20000))
(cd "$work/moved" && exec env -i PATH=/usr/bin:/bin PORT=$port ./main > "$work/log" 2>&1) &
pid=$!
for _ in $(seq 1 60); do curl -s -o /dev/null "http://127.0.0.1:$port/" && break; sleep 0.5; done
page=$(curl -s "http://127.0.0.1:$port/")
grep -q 'template file 41' <<< "$page" && ok "runs moved, with an empty environment (template found)" \
    || { fail "the moved bundle: $(printf '%s' "$page" | head -c 300) $(tail -5 "$work/log")"; }
css=$(curl -s "http://127.0.0.1:$port/site.css")
[ "$css" = "body { color: teal }" ] && ok "its static files" || fail "static: $css"

# A desktop app: --name, --icon, --windowed (on Linux a .desktop launcher and
# the icon next to the program; on macOS Name.app).
desk="$work/desk"
mkdir -p "$desk"
printf 'define main() -> void {\n    print("desk ok")\n}\n' > "$desk/main.qk"
python3 - "$desk/icon.png" <<'PY2'
import struct, sys, zlib
w = h = 32
raw = b"".join(b"\x00" + bytes([120, 60, 200, 255]) * w for _ in range(h))
def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
open(sys.argv[1], "wb").write(png)
PY2
out=$(cd "$desk" && "$QUIRK" bundle main.qk -o "$work/deskdist" --name "Test App" --icon icon.png --windowed 2>&1)
if [ "$(uname)" = "Darwin" ]; then
    m="$work/deskdist/Test App.app/Contents"
    [ -x "$m/MacOS/test-app" ] && ok "Test App.app with its program" || fail "no .app program: $out"
    grep -q '<string>test-app</string>' "$m/Info.plist" 2>/dev/null && grep -q 'Test App' "$m/Info.plist" \
        && ok "its Info.plist" || fail "Info.plist: $(cat "$m/Info.plist" 2>/dev/null | head -5)"
    [ "$(head -c 4 "$m/Resources/test-app.icns" 2>/dev/null)" = "icns" ] && ok "its icon (.icns)" || fail "no .icns"
    [ "$(cd "$m/MacOS" && env -i PATH=/usr/bin:/bin ./test-app 2>&1)" = "desk ok" ] && ok "it runs" || fail "the .app's program doesn't run"
else
    d="$work/deskdist"
    [ -x "$d/test-app" ] && ok "the program, named after --name" || fail "no test-app: $out"
    grep -q '^Name=Test App$' "$d/test-app.desktop" 2>/dev/null && grep -q "^Icon=.*test-app.png$" "$d/test-app.desktop" \
        && grep -q '^Exec=".*test-app" %F$' "$d/test-app.desktop" && ok "its .desktop launcher" \
        || fail ".desktop: $(cat "$d/test-app.desktop" 2>/dev/null)"
    cmp -s "$desk/icon.png" "$d/test-app.png" && ok "its icon next to it" || fail "no test-app.png"
    [ "$(cd "$d" && env -i PATH=/usr/bin:/bin ./test-app 2>&1)" = "desk ok" ] && ok "it runs" || fail "test-app doesn't run"
fi
# `quirk build`: quirk.toml's [build]; embed = true puts the files in the
# program (read from there, run from anywhere); the manifest keeps [build].
proj="$work/proj"
mkdir -p "$proj/assets/sub" "$proj/elsewhere"
printf 'name = "proj"\nversion = "0.1.0"\n\n[deps]\n\n[build]\nname = "Built App"\ninclude = ["assets"]   # data\nembed = true\noutput = "out"\n' > "$proj/quirk.toml"
echo "inside" > "$proj/assets/msg.txt"
echo "deep" > "$proj/assets/sub/x.txt"
cat > "$proj/main.qk" <<'Q'
use fs
use sys
define main() -> void {
    here := fs.dirname(String(sys.argv()[0]))
    print(fs.read_text("assets/msg.txt").trim() + " " + fs.read_text(fs.join(here, "./assets/sub/x.txt")).trim()
          + " " + String(fs.is_dir("assets")) + " " + String(fs.size("assets/msg.txt")) + " " + String(fs.exists("assets/no.txt"))
          + " " + String(fs.list_dir(fs.join(here, "assets")).sort()))
}
Q
built=$(cd "$proj/elsewhere" && "$QUIRK" build 2>&1)
bin="$proj/out/built-app"
[ "$(uname)" = "Darwin" ] && bin="$proj/out/Built App.app/Contents/MacOS/built-app"
[ -x "$bin" ] && ok "quirk build: the app from [build]" || fail "quirk build: $built"
[ ! -e "$proj/out/assets" ] && [ ! -e "$(dirname "$bin")/assets" ] && ok "embed: no assets/ copied" || fail "assets/ was copied"
got=$(cd / && env -i PATH=/usr/bin:/bin "$bin" 2>&1)
[ "$got" = "inside deep true 7 false [msg.txt, sub]" ] && ok "embed: the files read from the program (and listed), run from anywhere" || fail "embedded files: $got"
grep -q '^embed = true$' "$proj/quirk.toml" && ok "quirk.toml keeps [build]" || fail "[build] lost"

notpng=$(cd "$desk" && "$QUIRK" bundle main.qk -o "$work/bad" --icon main.qk 2>&1)
grep -q "not a PNG" <<< "$notpng" && ok "--icon refuses what isn't a PNG" || fail "--icon main.qk: $notpng"

[ $fails -eq 0 ] && echo "quirk bundle: ok" || { echo "quirk bundle: $fails failed"; exit 1; }
