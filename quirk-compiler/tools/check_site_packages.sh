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
else
    echo "FAIL: $out"
    exit 1
fi

# Run from another folder: the app's own site-packages (next to its
# quirk.toml) is found, not just ./site-packages/ of the current folder.
out=$(cd "$work" && "$QUIRK" run --no-cache app/main.qk 2>&1)
if [ "$out" = "alpha dark / beta dark" ]; then
    echo "  ok quirk run app/main.qk from outside the project finds its packages"
else
    echo "FAIL from outside the project: $out"
    exit 1
fi

# `from .util use { _clamp }` inside a package split into files means its
# own util's _clamp, not another loaded module's (window._clamp(v) once
# answered ui's three-argument calls: the source was the text ".util").
mkdir -p "$work/gamma/src" "$work/delta/src" "$work/app2"
printf 'name = "gamma"\nversion = "0.1.0"\n' > "$work/gamma/quirk.toml"
printf 'name = "delta"\nversion = "0.1.0"\n' > "$work/delta/quirk.toml"
printf 'define _clamp(v: Int) -> Int { return v + 1000 }\ndefine bump(v: Int) -> Int { return _clamp(v) }\n' > "$work/delta/src/index.quirk"
printf 'define _clamp(v: Int, lo: Int, hi: Int) -> Int {\n    if v < lo { return lo }\n    if v > hi { return hi }\n    return v\n}\n' > "$work/gamma/src/util.quirk"
printf 'from .util use { _clamp }\ndefine limit(v: Int) -> Int { return _clamp(v, 0, 10) }\n' > "$work/gamma/src/limits.quirk"
printf 'from .limits use { limit }\n' > "$work/gamma/src/index.quirk"
printf 'name = "app2"\nversion = "0.1.0"\n\n[deps]\ngamma = "../gamma"\ndelta = "../delta"\n' > "$work/app2/quirk.toml"
printf 'use delta\nuse gamma\ndefine main() -> void {\n    print(String(delta.bump(1)) + " " + String(gamma.limit(50)))\n}\n' > "$work/app2/main.qk"
(cd "$work/app2" && "$QUIRK" install > "$work/install2.log" 2>&1) || { echo "FAIL install"; cat "$work/install2.log"; exit 1; }
out=$(cd "$work/app2" && "$QUIRK" run --no-cache main.qk 2>&1)
if [ "$out" = "1001 10" ]; then
    echo "  ok a relative from-import means the sibling file, not a same-named function elsewhere"
    echo "site-packages: ok"
else
    echo "FAIL: $out"
    exit 1
fi
