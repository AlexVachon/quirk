#!/usr/bin/env sh
# Quirk installer — fetches the latest GitHub Release and unpacks it.
#
# One-liner:
#     curl -fsSL https://raw.githubusercontent.com/AlexVachon/quirk/main/install.sh | sh
#
# Environment knobs (rarely needed):
#     QUIRK_REPO     — owner/repo  (default: AlexVachon/quirk)
#     QUIRK_VERSION  — tag to install, e.g. v1.0.0  (default: latest release)
#     INSTALL_DIR    — install root  (default: ~/.quirk)
#
# Layout after install:
#     $INSTALL_DIR/bin/quirk
#     $INSTALL_DIR/lib/quirk/runtime.so
#     $INSTALL_DIR/lib/quirk/libs/...   (stdlib)
#
# The script does NOT touch your shell rc files — it prints the two lines
# to add so you can put them wherever you keep your env.

set -e

QUIRK_REPO="${QUIRK_REPO:-AlexVachon/quirk}"
INSTALL_DIR="${INSTALL_DIR:-$HOME/.quirk}"

# --- Platform detection -----------------------------------------------------
os=$(uname -s | tr '[:upper:]' '[:lower:]')
arch=$(uname -m)
case "$os" in
    linux) ;;
    darwin)
        echo "Quirk doesn't ship prebuilt macOS binaries yet."
        echo "Build from source: https://github.com/${QUIRK_REPO}#building"
        exit 1
        ;;
    *)
        echo "Unsupported OS: $os"
        echo "Build from source: https://github.com/${QUIRK_REPO}#building"
        exit 1
        ;;
esac
case "$arch" in
    x86_64|amd64) arch=x86_64 ;;
    *)
        echo "Unsupported architecture: $arch"
        echo "Quirk currently ships only Linux x86_64 binaries."
        exit 1
        ;;
esac

# --- Resolve version --------------------------------------------------------
if [ -z "${QUIRK_VERSION:-}" ]; then
    api="https://api.github.com/repos/${QUIRK_REPO}/releases/latest"
    # Tolerate either "tag_name": "vX.Y.Z" formatting. jq would be nicer
    # but installers shouldn't depend on it.
    tag=$(curl -fsSL "$api" \
        | grep -E '"tag_name"\s*:\s*"' \
        | head -1 \
        | sed -E 's/.*"tag_name"\s*:\s*"([^"]+)".*/\1/')
    if [ -z "$tag" ]; then
        echo "Failed to resolve latest release from $api" >&2
        echo "(maybe no releases yet, or the network is unreachable)" >&2
        exit 1
    fi
else
    tag="$QUIRK_VERSION"
fi
version="${tag#v}"
pkg="quirk-${version}-${os}-${arch}"
url="https://github.com/${QUIRK_REPO}/releases/download/${tag}/${pkg}.tar.gz"

echo "Installing Quirk ${version} → ${INSTALL_DIR}"

# --- Download + verify ------------------------------------------------------
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT
curl -fSL --progress-bar "$url" -o "$tmpdir/quirk.tar.gz"

# Best-effort SHA256 check — the release also publishes a .sha256 sidecar
# so a tampered tarball without a matching sidecar gets caught here.
if curl -fsSL "$url.sha256" -o "$tmpdir/quirk.tar.gz.sha256" 2>/dev/null; then
    expected=$(awk '{print $1}' "$tmpdir/quirk.tar.gz.sha256")
    actual=$(sha256sum "$tmpdir/quirk.tar.gz" | awk '{print $1}')
    if [ "$expected" != "$actual" ]; then
        echo "Checksum mismatch! expected $expected got $actual" >&2
        exit 1
    fi
fi

# --- Extract ----------------------------------------------------------------
tar xzf "$tmpdir/quirk.tar.gz" -C "$tmpdir"
mkdir -p "$INSTALL_DIR"
# Use cp -R + delete-overwrite to handle reinstalls cleanly.
rm -rf "$INSTALL_DIR/bin" "$INSTALL_DIR/lib"
cp -R "$tmpdir/$pkg/." "$INSTALL_DIR/"
chmod +x "$INSTALL_DIR/bin/quirk"

echo
echo "Installed Quirk ${version}."
echo
echo "To use it, add these to your shell profile (~/.bashrc, ~/.zshrc, …):"
echo
echo "    export QUIRK_HOME=\"$INSTALL_DIR\""
echo "    export PATH=\"\$QUIRK_HOME/bin:\$PATH\""
echo
echo "Then reopen your shell and run:  quirk --version"
