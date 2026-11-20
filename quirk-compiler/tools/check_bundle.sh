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
printf '%s' "$page" | grep -q 'template file 41' && ok "runs moved, with an empty environment (template found)" \
    || { fail "the moved bundle: $(printf '%s' "$page" | head -c 300) $(tail -5 "$work/log")"; }
css=$(curl -s "http://127.0.0.1:$port/site.css")
[ "$css" = "body { color: teal }" ] && ok "its static files" || fail "static: $css"

[ $fails -eq 0 ] && echo "quirk bundle: ok" || { echo "quirk bundle: $fails failed"; exit 1; }
