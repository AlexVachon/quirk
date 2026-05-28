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

# --- Argument parsing -------------------------------------------------------
# Accept a real flag in addition to the env var. Lets users write
#     curl … | sh -s -- --install-extension
# instead of the easy-to-mess-up
#     curl … | INSTALL_EXTENSION=1 sh
# (the latter still works, but the env var has to land on `sh`, not on
# `curl` — common foot-gun for first-time users).
while [ $# -gt 0 ]; do
    case "$1" in
        --install-extension|--with-extension) INSTALL_EXTENSION=1 ;;
        --code-cmd=*)  CODE_CMD="${1#*=}" ;;
        --version=*)   QUIRK_VERSION="${1#*=}" ;;
        --dir=*)       INSTALL_DIR="${1#*=}" ;;
        -h|--help)
            cat <<'EOF'
Usage: install.sh [flags]
  --install-extension     Also install the Quirk VSCode extension
  --code-cmd=<cmd>        Editor CLI to install into (default: code)
  --version=vX.Y.Z        Pin to a specific release (default: latest)
  --dir=<path>            Install root (default: ~/.quirk)
Env var equivalents: INSTALL_EXTENSION, CODE_CMD, QUIRK_VERSION, INSTALL_DIR
EOF
            exit 0
            ;;
        *)
            echo "install.sh: unknown flag '$1' — see --help" >&2
            exit 2
            ;;
    esac
    shift
done

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
# The compiler ships under tags like `vX.Y.Z`; the VSCode extension ships
# under `vscode-vX.Y.Z`. GitHub's /releases/latest returns whichever was
# published most recently — so once an extension release lands, blindly
# trusting /latest sends us off looking for a compiler tarball at
# `vscode-v0.2.1`. List releases instead and pick the first whose tag
# matches `v<digit>...` — that's a compiler release.
if [ -z "${QUIRK_VERSION:-}" ]; then
    api="https://api.github.com/repos/${QUIRK_REPO}/releases?per_page=30"
    tag=$(curl -fsSL "$api" \
        | grep -E '"tag_name"\s*:\s*"v[0-9]' \
        | head -1 \
        | sed -E 's/.*"tag_name"\s*:\s*"([^"]+)".*/\1/')
    if [ -z "$tag" ]; then
        echo "Failed to resolve a compiler release from $api" >&2
        echo "(maybe no v<digit>... tags yet, or the network is unreachable)" >&2
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

# --- VSCode extension (opt-in) ----------------------------------------------
# Off by default — a `curl | sh` installer that silently mutates the user's
# IDE would be a surprise. Opt-in by running with INSTALL_EXTENSION=1:
#
#     INSTALL_EXTENSION=1 curl -fsSL .../install.sh | sh
#
# Only fires when the `code` CLI is on PATH. To install into a non-default
# editor (VSCodium, Cursor, etc.), set CODE_CMD accordingly:
#
#     INSTALL_EXTENSION=1 CODE_CMD=codium curl -fsSL .../install.sh | sh
CODE_CMD="${CODE_CMD:-code}"

install_quirk_extension() {
    if ! command -v "$CODE_CMD" >/dev/null 2>&1; then
        echo "INSTALL_EXTENSION=1 was set, but '$CODE_CMD' is not on PATH — skipping."
        echo "  (Set CODE_CMD=codium / CODE_CMD=cursor / etc. to target another editor.)"
        return 0
    fi
    # The releases endpoint lists newest-first; only `vscode-v*` releases
    # carry .vsix assets, so the first .vsix we find IS the latest extension.
    vsix_url=$(curl -fsSL "https://api.github.com/repos/${QUIRK_REPO}/releases" 2>/dev/null \
               | grep -oE 'https://[^"]+\.vsix' | head -n 1)
    if [ -z "$vsix_url" ]; then
        echo "Couldn't locate a published Quirk VSCode extension. Skipping."
        return 0
    fi
    echo
    echo "Installing Quirk VSCode extension into '$CODE_CMD'..."
    vsix_path="$tmpdir/quirk.vsix"
    if ! curl -fsSL "$vsix_url" -o "$vsix_path"; then
        echo "Extension download failed; skipping."
        return 0
    fi
    "$CODE_CMD" --install-extension "$vsix_path" || {
        echo "Extension install failed. You can install it manually later:"
        echo "    $CODE_CMD --install-extension $vsix_url"
    }
}

if [ "${INSTALL_EXTENSION:-0}" = "1" ]; then
    install_quirk_extension
else
    echo "VSCode extension (optional):"
    echo "    INSTALL_EXTENSION=1 curl -fsSL https://raw.githubusercontent.com/${QUIRK_REPO}/main/install.sh | sh"
    echo "  or download the .vsix manually:"
    echo "    https://github.com/${QUIRK_REPO}/releases"
    echo
fi
echo "Then reopen your shell and run:  quirk --version"
