#!/usr/bin/env bash
# `quirk run --watch` end to end: it runs the file, runs it again when a
# project file changes, doesn't run a version that doesn't compile, picks
# up a new file, and stops (with its child) on SIGINT.
set -uo pipefail
cd "$(dirname "$0")/.."
QUIRK="${QUIRK:-$PWD/bin/quirk}"
fails=0
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
mkdir -p "$dir/src"
cat > "$dir/main.qk" <<'Q'
from src.part use { word }
print("ran: " + word())
Q
printf 'define word() -> String {\n    return "one"\n}\n' > "$dir/src/part.qk"
printf 'name = "w"\nversion = "0.1.0"\n' > "$dir/quirk.toml"

wait_for() {   # wait_for <pattern> <what>: up to 60 s for the log to show it
    for _ in $(seq 1 120); do
        if grep -qE -- "$1" "$dir/log"; then echo "  ok $2"; return 0; fi
        sleep 0.5
    done
    echo "FAIL $2"; tail -20 "$dir/log"; fails=$((fails + 1)); return 1
}

(cd "$dir" && NO_COLOR=1 exec "$QUIRK" run --watch main.qk > "$dir/log" 2>&1) &
pid=$!
wait_for '^ran: one$'                                   "runs the file"
wait_for 'stopped \(exit code 0\) — waiting for a change' "says when it finished"
sleep 1
printf 'define word() -> String {\n    return "two"\n}\n' > "$dir/src/part.qk"
wait_for 'src/part.qk changed — restarting'             "notices the imported file changed"
wait_for '^ran: two$'                                   "runs the new version"
sleep 1
printf 'define word() -> String {\n    return 2 +\n}\n' > "$dir/src/part.qk"
wait_for "changed, but it doesn't compile"              "doesn't run a version that doesn't compile"
if grep -q '^ran: 2' "$dir/log"; then echo "FAIL ran the broken version"; fails=$((fails + 1)); fi
sleep 1
printf 'define word() -> String {\n    return "three"\n}\n' > "$dir/src/part.qk"
wait_for '^ran: three$'                                 "runs again once it compiles"
sleep 1
printf '<p>new</p>\n' > "$dir/src/page.html"
wait_for 'src/page.html \(new\) changed'                "a new file counts"
kill -INT $pid
for _ in $(seq 1 20); do kill -0 $pid 2>/dev/null || break; sleep 0.25; done
if kill -0 $pid 2>/dev/null; then echo "FAIL still running after SIGINT"; kill -9 $pid; fails=$((fails + 1)); else echo "  ok stops on SIGINT"; fi
wait_for '\[watch\] stopped'                            "says it stopped"

[ $fails -eq 0 ] && echo "quirk run --watch: ok" || { echo "quirk run --watch: $fails failed"; exit 1; }
