#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SHELL_RC="${HOME}/.bashrc"
if [ -n "${ZSH_VERSION:-}" ]; then SHELL_RC="${HOME}/.zshrc"; fi

# ── 1. Dependencies ────────────────────────────────────────────────────────────
echo "Installing dependencies..."
sudo apt-get update -qq
sudo apt-get install -y \
    build-essential \
    gcc g++ \
    make \
    pkg-config \
    git \
    libgc-dev \
    llvm-14 \
    libllvm14 \
    llvm-14-dev

# ── 2. Build ───────────────────────────────────────────────────────────────────
echo "Building compiler and runtime..."
cd "$SCRIPT_DIR"
make clean
make -j"$(nproc)"

# ── 3. QUIRK_HOME ──────────────────────────────────────────────────────────────
# The compiler searches QUIRK_HOME/libs/ for the standard library at runtime.
QUIRK_HOME_VALUE="$SCRIPT_DIR"

if grep -q "QUIRK_HOME" "$SHELL_RC" 2>/dev/null; then
    # Update existing entry
    sed -i "s|export QUIRK_HOME=.*|export QUIRK_HOME=\"$QUIRK_HOME_VALUE\"|" "$SHELL_RC"
    echo "Updated QUIRK_HOME in $SHELL_RC"
else
    {
        echo ""
        echo "# Quirk compiler"
        echo "export QUIRK_HOME=\"$QUIRK_HOME_VALUE\""
        echo "export PATH=\"\$QUIRK_HOME/bin:\$PATH\""
    } >> "$SHELL_RC"
    echo "Added QUIRK_HOME to $SHELL_RC"
fi

# Tab-completion. `quirk completion <shell>` emits the script; sourcing
# it via `< <(...)` keeps the verb list in sync with the installed
# binary without anyone having to hand-edit a static .bash_completion.d
# file. The marker comment lets us detect a prior install and avoid
# duplicate `source` lines on re-runs.
COMPLETION_MARKER="# Quirk shell completion"
if ! grep -qF "$COMPLETION_MARKER" "$SHELL_RC" 2>/dev/null; then
    SHELL_NAME="bash"
    if [ -n "${ZSH_VERSION:-}" ]; then SHELL_NAME="zsh"; fi
    {
        echo ""
        echo "$COMPLETION_MARKER"
        echo "source <(quirk completion $SHELL_NAME)"
    } >> "$SHELL_RC"
    echo "Added quirk completion to $SHELL_RC"
fi

export QUIRK_HOME="$QUIRK_HOME_VALUE"
export PATH="$QUIRK_HOME/bin:$PATH"

# ── 4. Smoke test ──────────────────────────────────────────────────────────────
echo "Running smoke test..."
smoke_ok=true
if [ ! -x "$SCRIPT_DIR/bin/quirk" ]; then
    echo "  FAIL  bin/quirk not found or not executable"
    smoke_ok=false
elif ! { "$SCRIPT_DIR/bin/quirk" --help 2>&1 || true; } | grep -q "^Quirk"; then
    echo "  FAIL  bin/quirk --help did not print the help banner"
    smoke_ok=false
else
    echo "  OK    bin/quirk"
fi
if [ ! -f "$SCRIPT_DIR/bin/runtime.so" ]; then
    echo "  FAIL  bin/runtime.so not found"
    smoke_ok=false
else
    echo "  OK    bin/runtime.so"
fi
if [ "$smoke_ok" = false ]; then
    echo "One or more smoke tests failed. Check the build output above."
    exit 1
fi

echo ""
echo "Done. Quirk is ready."
echo "  Compiler : $SCRIPT_DIR/bin/quirk"
echo "  Runtime  : $SCRIPT_DIR/bin/runtime.so"
echo "  QUIRK_HOME: $QUIRK_HOME_VALUE"
echo ""
echo "Reload your shell or run: source $SHELL_RC"
