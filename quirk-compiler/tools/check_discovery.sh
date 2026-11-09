#!/usr/bin/env bash
# `quirk search` / `quirk show` without the network: a throwaway HOME with
# a file:// registry index and the per-repo quirk.toml / README / tag
# caches already filled in, so nothing reaches GitHub.
set -uo pipefail
cd "$(dirname "$0")/.."
QUIRK="$(pwd)/${QUIRK:-bin/quirk}"
fails=0
expect() {   # expect <output> <grep -E pattern> <what>
    if printf '%s\n' "$1" | grep -qE -- "$2"; then echo "  ok $3"; else echo "FAIL $3"; fails=$((fails + 1)); fi
}
reject() {
    if printf '%s\n' "$1" | grep -qE -- "$2"; then echo "FAIL $3"; fails=$((fails + 1)); else echo "  ok $3"; fi
}

H=$(mktemp -d)
trap 'rm -rf "$H"' EXIT
mkdir -p "$H/.quirk/cache/_tags" "$H/work"
cat > "$H/index.toml" <<'EOF'
# test registry
colorful = "github.com/someone/colorful"
dates    = "github.com/someone/quirk-dates"
EOF
echo "registry = \"file://$H/index.toml\"" > "$H/.quirk/config.toml"

meta=$H/.quirk/cache/_meta
mkdir -p "$meta/someone-colorful" "$meta/someone-quirk-dates"
cat > "$meta/someone-colorful/quirk.toml" <<'EOF'
name        = "colorful"
version     = "0.3.0"
description = "ANSI colors and styles for terminal output"
author      = "Someone"
license     = "MIT"
quirk-version = ">=7.0.0"

[deps]
EOF
{ echo "# colorful"; echo; for i in $(seq 1 40); do echo "readme line $i"; done; } \
    > "$meta/someone-colorful/README.md"
printf 'v0.3.0\nv0.2.0\n' > "$H/.quirk/cache/_tags/someone-colorful.tags"
cat > "$meta/someone-quirk-dates/quirk.toml" <<'EOF'
name        = "dates"
version     = "1.2.0"
description = "Calendar helpers: weekdays, month lengths, Easter"
EOF
echo > "$meta/someone-quirk-dates/README.md.missing"
printf 'v1.2.0\n' > "$H/.quirk/cache/_tags/someone-quirk-dates.tags"
# The stdlib packages are listed too; mark their manifests absent so
# search doesn't fetch them.
for p in packages/*/; do
    n=$(basename "$p")
    mkdir -p "$meta/AlexVachon-quirk-$n"
    echo > "$meta/AlexVachon-quirk-$n/quirk.toml.missing"
done

q() { (cd "$H/work" && HOME="$H" NO_COLOR=1 "$QUIRK" "$@" 2>&1); }

out=$(q search); rc=$?
[ $rc -eq 0 ] && echo "  ok search with no terms succeeds" || { echo "FAIL search rc $rc"; fails=$((fails + 1)); }
expect "$out" '^  colorful +0\.3\.0 +ANSI colors'      "registry package with version + description"
expect "$out" '^  argparse .*\(stdlib\)'               "stdlib packages listed and labelled"
[ -f "$H/.quirk/registry-cache.toml" ] && echo "  ok registry index fetched into the cache" \
    || { echo "FAIL registry index not cached"; fails=$((fails + 1)); }

out=$(q search TERMINAL)
expect "$out" '^  colorful '                           "search matches descriptions, any case"
reject "$out" 'dates|argparse'                         "non-matching packages left out"
out=$(q search calendar easter)
expect "$out" '^  dates '                              "every term must match"
out=$(q search calendar nope); rc=$?
[ $rc -ne 0 ] && echo "  ok no match exits non-zero" || { echo "FAIL no-match rc 0"; fails=$((fails + 1)); }
expect "$out" "No packages match 'calendar' 'nope'"    "no-match message"

out=$(q show colorful)
expect "$out" '^name: +colorful'                       "show: name"
expect "$out" '^latest: +0\.3\.0'                      "show: latest tag"
expect "$out" '^description: +ANSI colors'             "show: description"
expect "$out" '^requires: +quirk >=7\.0\.0'            "show: quirk-version"
expect "$out" '^installed: +no +\(quirk install colorful\)' "show: install hint"
expect "$out" 'readme line 28$'                        "show: README start"
reject "$out" 'readme line 29$'                        "show: README capped"
expect "$out" '… 12 more lines \(quirk show colorful --readme\)' "show: pointer to the rest"

out=$(q show colorful --readme)
expect "$out" 'readme line 40$'                        "--readme prints it all"
reject "$out" '^name:'                                 "--readme prints only the README"
out=$(q show colorful --no-readme)
reject "$out" 'readme line'                            "--no-readme skips it"

out=$(q show dates)
expect "$out" '^latest: +1\.2\.0'                      "show: package without a README"
out=$(q show nothere); rc=$?
[ $rc -ne 0 ] && echo "  ok unknown package exits non-zero" || { echo "FAIL unknown rc 0"; fails=$((fails + 1)); }
expect "$out" "isn't installed or in the registry"     "unknown package message"
expect "$out" 'quirk search nothere'                   "unknown package suggests search"

echo "package discovery: $([ $fails -eq 0 ] && echo ok || echo "$fails failed")"
exit $fails
