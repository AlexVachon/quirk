#!/bin/bash
# End-to-end check of project dependency installs (`quirk install` /
# `quirk sync`), with local packages only — no network. Locks:
#   - `quirk install <spec>` records the dependency in quirk.toml under the
#     package's own name (not its directory / repo name);
#   - transitive dependencies are installed (and reported `via` the parent);
#   - a repeat install reports everything "already installed";
#   - with site-packages gone, packages come back "from cache";
#   - a fresh clone (quirk.toml + quirk.lock only) installs the same set.
#
# Usage (from quirk-compiler/, after `make`):  tools/check_install.sh
# Runs in a temp dir with its own HOME; ~/.quirk is never touched.
set -euo pipefail

compiler=$(cd "$(dirname "$0")/.." && pwd)
quirk="$compiler/bin/quirk"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export HOME="$work/home" QUIRK_HOME="$compiler" QUIRK_NO_UPDATE_CHECK=1
mkdir -p "$HOME"
fail() { echo "::error::install check: $*"; exit 1; }
q() { "$quirk" "$@" </dev/null 2>&1 | sed 's/\x1b\[[0-9;]*m//g'; }

# Two local packages in `src/` layout; the directory names differ from the
# package names, as a GitHub repo name (`quirk-csv`) differs from `csv`.
mkpkg() {   # <dir> <name> <version> [<dep-name> <dep-path>]
    mkdir -p "$work/$1/src"
    printf 'name = "%s"\nversion = "%s"\n' "$2" "$3" > "$work/$1/quirk.toml"
    [ $# -gt 3 ] && printf '\n[deps]\n%s = "%s"\n' "$4" "$5" >> "$work/$1/quirk.toml"
    echo "define ${2}_ok() -> Bool { return true }" > "$work/$1/src/index.quirk"
}
mkpkg quirk-helper helper 0.1.0
mkpkg quirk-app app 0.2.0 helper "$work/quirk-helper"

mkdir "$work/proj" && cd "$work/proj"
q init >/dev/null

echo "── quirk install <path>"
out=$(q install "$work/quirk-app")
echo "$out"
grep -q "helper 0.1.0 .* via app" <<<"$out" || fail "transitive dependency wasn't installed via app"
[ -d site-packages/app ] && [ -d site-packages/helper ] || fail "site-packages is missing app or helper"
grep -q '^app = ' quirk.toml || fail "quirk.toml doesn't record app under its own name"
grep -q 'quirk-app = \|^helper = ' quirk.toml && fail "quirk.toml records the wrong names (or the transitive dep)"
grep -q 'name    = "helper"' quirk.lock || fail "quirk.lock is missing the transitive dependency"

echo "── quirk install (everything present)"
out=$(q install)
[ "$(grep -c "(already installed)" <<<"$out")" -eq 2 ] || { echo "$out"; fail "repeat install didn't report 2 already installed"; }

echo "── quirk install with site-packages removed"
mv site-packages "$work/old-site-packages"
out=$(q install)
[ "$(grep -c "(from cache)" <<<"$out")" -eq 2 ] || { echo "$out"; fail "reinstall didn't use the cache for both"; }

echo "── fresh clone: quirk.toml + quirk.lock only"
mkdir "$work/clone" && cp quirk.toml quirk.lock "$work/clone/" && cd "$work/clone"
out=$(q sync --no-venv)
[ -d site-packages/app ] && [ -d site-packages/helper ] || { echo "$out"; fail "sync in a fresh clone didn't install app and helper"; }

echo "install check: ok"
