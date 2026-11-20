#!/usr/bin/env bash
# Packages installed into a project (`quirk install`: snapshots in
# site-packages/) that define the same names — a struct `Theme`, a function
# `dark()` — each reached through its own module: `alpha.dark()` is alpha's.
# (Their module names kept the "site-packages." prefix, so `beta.dark()`
# went to alpha's dark().)
set -uo pipefail
cd "$(dirname "$0")/.."
QUIRK="${QUIRK:-$PWD/bin/quirk}"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
for name in alpha beta; do
    mkdir -p "$work/$name/src"
    printf 'name = "%s"\nversion = "0.1.0"\n' "$name" > "$work/$name/quirk.toml"
    cat > "$work/$name/src/index.quirk" <<Q
struct Theme {
    name: String
    define __init(self, name: String) -> void { self.name = name }
}
define dark() -> Theme {
    return Theme("$name dark")
}
Q
done
mkdir -p "$work/app"
printf 'name = "app"\nversion = "0.1.0"\n\n[deps]\nalpha = "../alpha"\nbeta = "../beta"\n' > "$work/app/quirk.toml"
cat > "$work/app/main.qk" <<'Q'
use alpha
use beta
define main() -> void {
    a: alpha.Theme = alpha.dark()
    b: beta.Theme = beta.dark()
    print(a.name + " / " + b.name)
}
Q
(cd "$work/app" && "$QUIRK" install > "$work/install.log" 2>&1) || { echo "FAIL install"; cat "$work/install.log"; exit 1; }
out=$(cd "$work/app" && "$QUIRK" run --no-cache main.qk 2>&1)
if [ "$out" = "alpha dark / beta dark" ]; then
    echo "  ok same names in two installed packages, each through its own module"
    echo "site-packages: ok"
else
    echo "FAIL: $out"
    exit 1
fi
