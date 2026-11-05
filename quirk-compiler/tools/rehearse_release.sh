#!/bin/bash
# Rehearse a release end to end, without publishing anything: package this
# checkout exactly as .github/workflows/release.yml does (compiler tarball +
# per-package stdlib assets + stdlib.lock), serve it from a local `file://`
# dir, and install it with `quirk compiler install` the way users get it.
# Fails if any step a user would hit goes wrong. CI runs it on every push.
#
# Usage (from quirk-compiler/, after `make bootstrap-stdlib` and `make`):
#   tools/rehearse_release.sh
#
# Everything happens in a temp dir with its own HOME, so ~/.quirk and the
# GitHub releases are never touched.
set -euo pipefail

compiler=$(cd "$(dirname "$0")/.." && pwd)
cd "$compiler"
version=$(QUIRK_HOME="$compiler" ./bin/quirk --version | awk '{print $2}')
case "$(uname -s)-$(uname -m)" in
    Linux-x86_64)  platform=linux-x86_64 ;;
    Darwin-arm64)  platform=darwin-arm64 ;;
    *) echo "rehearse_release: no release build for $(uname -s) $(uname -m)" >&2; exit 1 ;;
esac

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
rel="$work/releases/v$version"
fail() { echo "::error::release rehearsal: $*"; exit 1; }

echo "── packaging quirk $version ($platform)"
pkg=$(tools/package_release.sh "$version" "$platform" "$rel")
make --no-print-directory stdlib-assets STDLIB_ASSETS="$rel"
expected=$(grep -vc '^#' "$rel/stdlib.lock")

# An existing install to upgrade from: same stdlib, but with typing one
# version behind and an extra package, so the run also exercises the
# changed / removed reporting.
dest="$work/install"
mkdir -p "$dest" "$work/home"
cp -R packages "$dest/packages"
echo v0.0.1 > "$dest/packages/typing/.version"
mkdir "$dest/packages/oldpkg" && echo v0.9.0 > "$dest/packages/oldpkg/.version"

run_install() {
    HOME="$work/home" QUIRK_HOME="$compiler" QUIRK_NO_UPDATE_CHECK=1 \
    QUIRK_RELEASE_URL="file://$work/releases" INSTALL_DIR="$dest" \
        ./bin/quirk compiler install "v$version" 2>&1 | sed 's/\x1b\[[0-9;]*m//g'
}

echo "── installing v$version from the rehearsal release"
out=$(run_install) || { echo "$out"; fail "compiler install exited non-zero"; }
echo "$out"
grep -q "Verified sha256" <<<"$out"        || fail "compiler tarball wasn't sha256-verified"
grep -qE "⚠|✗" <<<"$out"                   && fail "install reported a warning or error"
# One line for the compiler tarball, one (wrapped) line for the whole
# stdlib batch naming every package — `name (version)` — under one bar.
grep -qE "^ *Downloading quirk-$version-$platform\.tar\.gz \([0-9.]+ [kM]?B\)$" <<<"$out" \
    || fail "no download line for the compiler tarball"
[ "$(grep -c '^ *Downloading' <<<"$out")" -eq 2 ] || fail "expected 2 Downloading lines (compiler + stdlib batch)"
while read -r name tag _; do
    case "$name" in '#'*|'') continue ;; esac
    grep -qE "(^|[ ,])$name \(" <<<"$out" || fail "the stdlib batch line doesn't name $name"
done < "$rel/stdlib.lock"
grep -q "typing (0.0.1 → " <<<"$out" || fail "typing wasn't reported as updated"
grep -q "Removed oldpkg" <<<"$out"         || fail "oldpkg wasn't reported as removed"

echo "── checking the installed tree"
[ "$("$dest/bin/quirk" --version | awk '{print $2}')" = "$version" ] || fail "installed binary isn't $version"
[ "$(ls "$dest/packages" | wc -l | tr -d ' ')" -eq "$expected" ]     || fail "installed stdlib isn't $expected packages"
while read -r name tag _; do
    case "$name" in '#'*|'') continue ;; esac
    [ "$(cat "$dest/packages/$name/.version")" = "$tag" ] || fail "$name isn't $tag"
done < "$rel/stdlib.lock"
printf 'use console\ndefine main() -> void { console.log("rehearsal ok") }\n' > "$work/smoke.quirk"
smoke=$(HOME="$work/home" QUIRK_HOME="$dest" QUIRK_NO_UPDATE_CHECK=1 "$dest/bin/quirk" "$work/smoke.quirk")
[ "$smoke" = "rehearsal ok" ] || fail "installed compiler couldn't run a program: $smoke"

# Packages named on the stdlib batch line(s): `name (version)` entries.
count_pkg_downloads() { grep -oE '(^|[ ,])[a-z]+ \([0-9][^)]*\)' <<<"$1" | grep -vc 'quirk-' || true; }

echo "── reinstalling over it (nothing changed: no package downloads)"
again=$(run_install) || { echo "$again"; fail "reinstall exited non-zero"; }
grep -qE "→|Removed" <<<"$again" && { echo "$again"; fail "reinstall reported changes"; }
[ "$(count_pkg_downloads "$again")" -eq 0 ] || { echo "$again"; fail "reinstall downloaded stdlib packages"; }
grep -q "Up to date $expected stdlib packages" <<<"$again" || { echo "$again"; fail "reinstall didn't reuse all $expected packages"; }

echo "── reinstalling with one package's checksum stale (only it downloads)"
echo 0000 > "$dest/packages/csv/.sha256"
third=$(run_install) || { echo "$third"; fail "reinstall exited non-zero"; }
[ "$(count_pkg_downloads "$third")" -eq 1 ] && grep -q "Downloading csv (" <<<"$third" \
    || { echo "$third"; fail "a stale package wasn't the only one downloaded"; }
[ "$(cat "$dest/packages/csv/.sha256")" = "$(awk '$1=="csv"{print $4}' "$rel/stdlib.lock")" ] \
    || fail "csv's .sha256 wasn't refreshed"

echo "── installing over HTTP from a flaky server (csv: one 504, crypto: always 504)"
# A transient 504 must be retried; a persistent one must fall back to the
# bundled copy for that package only, keeping the other downloads.
fresh="$work/flaky-install"
port=$((20000 + $$ % 20000))
python3 -I -u tools/flaky_server.py "$work/releases" "$port" 2>/dev/null &
server=$!
trap 'kill $server 2>/dev/null; rm -rf "$work"' EXIT
sleep 1
flaky=$(HOME="$work/home" QUIRK_HOME="$compiler" QUIRK_NO_UPDATE_CHECK=1 \
        QUIRK_RELEASE_URL="http://127.0.0.1:$port" INSTALL_DIR="$fresh" \
        ./bin/quirk compiler install "v$version" 2>&1 </dev/null | sed 's/\x1b\[[0-9;]*m//g') \
    || { echo "$flaky"; fail "install from the flaky server exited non-zero"; }
kill $server 2>/dev/null || true
grep -q "crypto: HTTP 504 — installed the copy bundled" <<<"$flaky" || { echo "$flaky"; fail "crypto didn't fall back on its own"; }
grep -q "csv:" <<<"$flaky" && { echo "$flaky"; fail "csv's transient 504 wasn't retried"; }
[ -f "$fresh/packages/csv/.sha256" ] || fail "csv wasn't installed from its (retried) download"
[ ! -f "$fresh/packages/crypto/.sha256" ] && [ -d "$fresh/packages/crypto" ] || fail "crypto should be the bundled copy"
[ "$(ls "$fresh"/packages/*/.sha256 | wc -l | tr -d ' ')" -eq $((expected - 1)) ] \
    || fail "the other packages didn't keep their downloads"

echo "release rehearsal: ok ($pkg + $expected stdlib packages)"
