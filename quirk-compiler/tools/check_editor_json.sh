#!/usr/bin/env bash
# The JSON the editors read: `--symbols-json` (hover / completion / inlay
# types) and `--check --diagnostics-json` (squiggles with Q-codes). Both the
# VS Code extension and quirk-lsp depend on these shapes.
set -euo pipefail
cd "$(dirname "$0")/.."
QUIRK="${QUIRK:-bin/quirk-cpp}"
F=tools/fixtures

"$QUIRK" --symbols-json "$F/editor_types.qk" > /tmp/qk_sym.$$ || true
"$QUIRK" --check --diagnostics-json "$F/editor_errors.qk" > /tmp/qk_diag.$$ || true

rc=0
python3 - /tmp/qk_sym.$$ /tmp/qk_diag.$$ <<'PY' || rc=$?
import json, sys
def load(p):
    return [json.loads(l) for l in open(p) if l.strip().startswith('{')]
syms = [s for s in load(sys.argv[1]) if s.get('file', '').endswith('editor_types.qk')]
diags = load(sys.argv[2])
fails = []
def want_type(kind, name, scope, typ):
    hit = [s for s in syms if s['kind'] == kind and s['name'] == name and s['scope'] == scope]
    if not hit:
        fails.append(f"missing {kind} {scope}.{name}")
    elif hit[0].get('type') != typ:
        fails.append(f"{kind} {scope}.{name}: type {hit[0].get('type')!r}, want {typ!r}")
want_type('parameter',    'ps',     'oldest', 'List[P]')
want_type('variable',     'best',   'oldest', 'P')
want_type('variable',     'p',      'oldest', 'P')
want_type('module_const', 'people', 'module', 'List[P]')
want_type('variable',     'i',      'module', 'Int')
want_type('variable',     'q',      'module', 'P')
if [s for s in syms if s['kind'] == 'variable' and s['name'] == 'best' and s['line'] == 10]:
    fails.append("re-assignment `best = p` reported as a declaration")
codes = {(d['line'], d.get('code')) for d in diags}
for want in [(3, 'Q0209'), (4, 'Q0100')]:
    if want not in codes:
        fails.append(f"diagnostic line {want[0]} code {want[1]} missing (got {sorted(codes)})")
for f in fails:
    print("FAIL", f)
print(f"editor json: {6 + 1 + 2 - len(fails)}/9 checks passed")
sys.exit(1 if fails else 0)
PY
rm -f /tmp/qk_sym.$$ /tmp/qk_diag.$$
exit $rc
