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
#   Windows: bin/quirk.exe, bin/runtime.dll + runtime.lib, the DLLs they load
#   <QUIRK_HOME>/packages/<name>/...   (top-level, with .version files)
set -euo pipefail

version=${1:?usage: package_release.sh <version> <platform> <outdir>}
platform=${2:?usage: package_release.sh <version> <platform> <outdir>}
outdir=${3:?usage: package_release.sh <version> <platform> <outdir>}

compiler=$(cd "$(dirname "$0")/.." && pwd)
root=$(dirname "$compiler")
pkg="quirk-${version}-${platform}"

exe=quirk
case "$platform" in windows-*) exe=quirk.exe ;; esac

# A tag that doesn't match QUIRK_VERSION would ship a mislabelled build
# (and `compiler update` would keep offering it).
built=$(QUIRK_HOME="$compiler" "$compiler/bin/$exe" --version | tr -d '\r' | awk '{print $2}')
if [ "$built" != "$version" ]; then
    echo "package_release: binary is quirk $built, not $version — bump QUIRK_VERSION or fix the tag" >&2
    exit 1
fi

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/$pkg/bin" "$outdir"
case "$platform" in
windows-*)
    # quirk.exe, runtime.dll (+ runtime.lib, for `-o`), and every DLL they
    # import that isn't part of Windows — found in the conda env the build
    # used ($CONDA_PREFIX: GC, pthreads, OpenSSL, zlib, the MSVC runtime),
    # followed recursively. SDL2.dll is loaded at run time (window), so
    # it's added by name. Everything sits in bin/: Windows looks for a
    # DLL next to the .exe first.
    cp "$compiler/bin/quirk.exe" "$compiler/bin/runtime.dll" "$compiler/bin/runtime.lib" "$stage/$pkg/bin/"
    : "${CONDA_PREFIX:?package_release: windows packaging needs the conda env (CONDA_PREFIX)}"
    objdump_bin=$(command -v llvm-objdump || echo "$CONDA_PREFIX/Library/bin/llvm-objdump")
    find_dll() {   # find_dll <name> -> path in the conda env, or nothing
        local d
        for d in "$CONDA_PREFIX/Library/bin" "$CONDA_PREFIX" "$CONDA_PREFIX/bin"; do
            [ -f "$d/$1" ] && { echo "$d/$1"; return; }
            # case-insensitive match (imports say KERNEL32.dll, files say kernel32.dll)
            local hit; hit=$(ls "$d" 2>/dev/null | grep -ix "$1" | head -1)
            [ -n "$hit" ] && { echo "$d/$hit"; return 0; }
        done
        return 0   # not found: prints nothing (set -e would end the script on a 1)
    }
    queue=("$stage/$pkg/bin/quirk.exe" "$stage/$pkg/bin/runtime.dll")
    for extra in SDL2.dll vcruntime140.dll vcruntime140_1.dll msvcp140.dll; do
        f=$(find_dll "$extra"); [ -n "$f" ] && { cp "$f" "$stage/$pkg/bin/"; queue+=("$stage/$pkg/bin/$(basename "$f")"); }
    done
    while [ ${#queue[@]} -gt 0 ]; do
        cur=${queue[0]}; queue=("${queue[@]:1}")
        for dll in $("$objdump_bin" -p "$cur" 2>/dev/null | tr -d '\r' | awk '/DLL Name:/ {print $3}'); do
            ls "$stage/$pkg/bin" | grep -qix "$dll" && continue
            f=$(find_dll "$dll")
            [ -z "$f" ] && continue          # a Windows DLL (kernel32, ucrtbase, api-ms-*)
            cp "$f" "$stage/$pkg/bin/"
            queue+=("$stage/$pkg/bin/$(basename "$f")")
        done
    done
    echo "package_release: bin/ = $(ls "$stage/$pkg/bin" | tr '\n' ' ')" >&2
    ;;
*)
    cp "$compiler/bin/quirk" "$compiler/bin/runtime.so" "$stage/$pkg/bin/"
    ;;
esac
cp -R "$compiler/packages" "$stage/$pkg/packages"
cp "$root/README.md" "$root/INSTALL.md" "$compiler/CHANGELOG.md" "$stage/$pkg/" 2>/dev/null || true

tar czf "$outdir/$pkg.tar.gz" -C "$stage" "$pkg"
# `<sha256>  <file>`, as sha256sum prints it (macOS has shasum instead).
( cd "$outdir"
  if command -v sha256sum >/dev/null; then sha256sum "$pkg.tar.gz"
  else shasum -a 256 "$pkg.tar.gz" | awk '{print $1"  "$2}'; fi ) > "$outdir/$pkg.tar.gz.sha256"
echo "$pkg"
