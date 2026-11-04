#!/bin/bash
# Build the release tarball for this checkout: quirk-<version>-<platform>.tar.gz
# plus its .sha256 sidecar, in <outdir>. Used by .github/workflows/release.yml
# (both platforms) and by tools/rehearse_release.sh, so CI rehearses exactly
# what a release ships.
#
# Usage:  tools/package_release.sh <version> <platform> <outdir>
#   e.g.  tools/package_release.sh 7.4.0 linux-x86_64 "$GITHUB_WORKSPACE"
# Run after `make bootstrap-stdlib` and `make`. Prints the package name.
#
# The layout mirrors the source tree, which is what the compiler's stdlib
# resolution expects:
#   <QUIRK_HOME>/bin/quirk
#   <QUIRK_HOME>/bin/runtime.so        (sibling of the binary)
#   <QUIRK_HOME>/packages/<name>/...   (top-level, with .version files)
set -euo pipefail

version=${1:?usage: package_release.sh <version> <platform> <outdir>}
platform=${2:?usage: package_release.sh <version> <platform> <outdir>}
outdir=${3:?usage: package_release.sh <version> <platform> <outdir>}

compiler=$(cd "$(dirname "$0")/.." && pwd)
root=$(dirname "$compiler")
pkg="quirk-${version}-${platform}"

# A tag that doesn't match QUIRK_VERSION would ship a mislabelled build
# (and `compiler update` would keep offering it).
built=$(QUIRK_HOME="$compiler" "$compiler/bin/quirk" --version | awk '{print $2}')
if [ "$built" != "$version" ]; then
    echo "package_release: binary is quirk $built, not $version — bump QUIRK_VERSION or fix the tag" >&2
    exit 1
fi

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/$pkg/bin" "$outdir"
cp "$compiler/bin/quirk" "$compiler/bin/runtime.so" "$stage/$pkg/bin/"
cp -R "$compiler/packages" "$stage/$pkg/packages"
cp "$root/README.md" "$root/INSTALL.md" "$compiler/CHANGELOG.md" "$stage/$pkg/" 2>/dev/null || true

tar czf "$outdir/$pkg.tar.gz" -C "$stage" "$pkg"
# `<sha256>  <file>`, as sha256sum prints it (macOS has shasum instead).
( cd "$outdir"
  if command -v sha256sum >/dev/null; then sha256sum "$pkg.tar.gz"
  else shasum -a 256 "$pkg.tar.gz" | awk '{print $1"  "$2}'; fi ) > "$outdir/$pkg.tar.gz.sha256"
echo "$pkg"
