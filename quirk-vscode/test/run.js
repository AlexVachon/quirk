#!/usr/bin/env node
// Tests for the extension's own analyzers, run under plain Node with a stub
// `vscode` (npm test, after `tsc`):
//   1. lint fixtures — test/fixtures/lint/*.qk: a line ending in
//      `// expect: <text>` must get a warning containing <text> (several:
//      `// expect: a | b`); every other line must get none.
//   2. the stdlib — every file under ../quirk-compiler/packages (or
//      $QUIRK_STDLIB) lints clean: no false warnings in the libraries.
//   3. Ctrl+click — in throwaway projects with a venv, go-to-definition
//      lands in the venv's copy of a package.
//   4. dead code — which lines the editor fades as unreachable.
const Module = require('module');
const fs = require('fs');
const os = require('os');
const path = require('path');

const realResolve = Module._resolveFilename;
Module._resolveFilename = function (request, ...rest) {
    if (request === 'vscode') return require.resolve('./vscode_stub.js');
    return realResolve.call(this, request, ...rest);
};
const vscode = require('./vscode_stub.js');
const OUT = path.join(__dirname, '..', 'out');
const { refreshDiagnostics } = require(path.join(OUT, 'DiagnosticsProvider.js'));
const importProvider = require(path.join(OUT, 'ImportProvider.js'));

const STDLIB_ROOT = process.env.QUIRK_STDLIB || path.join(__dirname, '..', '..', 'quirk-compiler');
const STDLIB = path.join(STDLIB_ROOT, 'packages');
const haveStdlib = fs.existsSync(path.join(STDLIB, 'typing'));
// Module resolution (`use process`) looks in $QUIRK_HOME/packages.
if (haveStdlib) process.env.QUIRK_HOME = STDLIB_ROOT;

let failures = 0, passed = 0;
const fail = (what, detail) => { failures++; console.log(`  ✗ ${what}${detail ? '\n      ' + detail : ''}`); };
const ok = (what) => { passed++; console.log(`  ✓ ${what}`); };

function makeDoc(file, text) {
    const lines = text.split('\n');
    return {
        uri: { fsPath: file, toString: () => file }, fileName: file, languageId: 'quirk',
        lineCount: lines.length,
        getText: (r) => {
            if (!r) return text;
            const off = (p) => lines.slice(0, Math.min(p.line, lines.length)).reduce((n, l) => n + l.length + 1, 0) + p.character;
            return text.slice(off(r.start), off(r.end));
        },
        lineAt: (p) => ({ text: lines[typeof p === 'number' ? p : p.line] }),
        getWordRangeAtPosition: (pos) => {
            const l = lines[pos.line];
            let a = pos.character, b = pos.character;
            while (a > 0 && /\w/.test(l[a - 1])) a--;
            while (b < l.length && /\w/.test(l[b])) b++;
            return new vscode.Range(pos.line, a, pos.line, b);
        },
    };
}

function lint(file, text) {
    let got = [];
    refreshDiagnostics(makeDoc(file, text), { set: (_u, d) => { got = d; } });
    return got;
}

// ---- 1. lint fixtures ----
console.log('lint fixtures');
const fixtureDir = path.join(__dirname, 'fixtures', 'lint');
for (const name of fs.readdirSync(fixtureDir).filter(n => n.endsWith('.qk')).sort()) {
    const file = path.join(fixtureDir, name);
    const text = fs.readFileSync(file, 'utf8');
    if (/^\/\/ needs: stdlib/m.test(text) && !haveStdlib) { console.log(`  - ${name} (skipped: no stdlib)`); continue; }
    const expected = new Map();     // line → [substrings]
    text.split('\n').forEach((l, i) => {
        const m = /\/\/ expect: (.+)$/.exec(l);
        if (m) expected.set(i, m[1].split(' | ').map(s => s.trim()));
    });
    const got = lint(file, text);
    const problems = [];
    const byLine = new Map();
    for (const d of got) {
        const line = d.range.start.line;
        if (!byLine.has(line)) byLine.set(line, []);
        byLine.get(line).push(d.message);
    }
    for (const [line, msgs] of byLine) {
        const want = expected.get(line) || [];
        for (const msg of msgs)
            if (!want.some(w => msg.includes(w))) problems.push(`line ${line + 1}: unexpected "${msg}"`);
    }
    for (const [line, wants] of expected) {
        const msgs = byLine.get(line) || [];
        for (const w of wants)
            if (!msgs.some(m => m.includes(w))) problems.push(`line ${line + 1}: missing "${w}"`);
    }
    if (problems.length) fail(name, problems.join('\n      ')); else ok(name);
}

// ---- 2. the stdlib lints clean ----
console.log('stdlib');
if (!haveStdlib) {
    console.log(`  - skipped: no stdlib at ${STDLIB} (make bootstrap-stdlib, or set QUIRK_STDLIB)`);
} else {
    const files = [];
    const walk = (d) => {
        for (const e of fs.readdirSync(d, { withFileTypes: true })) {
            const p = path.join(d, e.name);
            if (e.isDirectory()) walk(p);
            else if (/\.(quirk|qk)$/.test(e.name)) files.push(p);
        }
    };
    walk(STDLIB);
    let dirty = 0;
    for (const file of files.sort()) {
        const got = lint(file, fs.readFileSync(file, 'utf8'))
            .filter(d => !d.message.startsWith('Cannot resolve module'));   // depends on what's installed
        if (got.length) {
            dirty++;
            fail(path.relative(STDLIB, file),
                 got.map(d => `${d.range.start.line + 1}:${d.range.start.character + 1} ${d.message}`).join('\n      '));
        }
    }
    if (!dirty) ok(`${files.length} library files, no warnings`);
}

// ---- 3. Ctrl+click in a venv ----
console.log('go to definition');
function project(layout) {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), 'quirk-ext-test-'));
    for (const [rel, content] of Object.entries(layout)) {
        const p = path.join(root, rel);
        fs.mkdirSync(path.dirname(p), { recursive: true });
        fs.writeFileSync(p, content);
    }
    return root;
}
const Provider = Object.values(importProvider).find(v => typeof v === 'function' && v.prototype && v.prototype.provideDefinition);
function definitionAt(file, line, col) {
    const text = fs.readFileSync(file, 'utf8');
    let loc;
    try { loc = new Provider().provideDefinition(makeDoc(file, text), new vscode.Position(line, col), {}); }
    catch (e) { return { error: String(e && e.stack || e).split('\n').slice(0, 3).join(' ') }; }
    const r = Array.isArray(loc) ? loc[0] : loc;
    return r ? { file: r.uri.fsPath, line: r.range.start.line } : null;
}
const savedHome = process.env.QUIRK_HOME;
process.env.QUIRK_HOME = path.join(os.tmpdir(), 'quirk-ext-test-global');   // a "global" install that must NOT win
{
    // A folder with .venv/ and scripts/main.qk (no quirk.toml): the venv is found
    // from a subfolder, so the root comes from the venv marker.
    const root = project({
        '.venv/bin/activate': '# venv\n',
        '.venv/lib/quirk/site-packages/slug/index.qk': '--- Make a slug. ---\ndefine slug(s: String) -> String {\n    return s\n}\n',
        '.venv/lib/quirk/stdlib/widgets/index.quirk': 'struct Button {\n    define __init(self) -> void { }\n}\n',
        'scripts/main.qk': 'use slug\nuse widgets\nprint(slug.slug("A b"))\nb := widgets.Button()\n',
    });
    const main = path.join(root, 'scripts', 'main.qk');
    const d1 = definitionAt(main, 2, 12);
    if (d1 && d1.file === path.join(root, '.venv/lib/quirk/site-packages/slug/index.qk') && d1.line === 1) ok('venv site-packages: slug.slug');
    else fail('venv site-packages: slug.slug', JSON.stringify(d1));
    const d2 = definitionAt(main, 3, 15);
    if (d2 && d2.file === path.join(root, '.venv/lib/quirk/stdlib/widgets/index.quirk')) ok('venv stdlib: widgets.Button');
    else fail('venv stdlib: widgets.Button', JSON.stringify(d2));
    // The module path in `use slug` opens the module.
    const d3 = definitionAt(main, 0, 5);
    if (d3 && d3.file === path.join(root, '.venv/lib/quirk/site-packages/slug/index.qk')) ok('use slug → its file');
    else fail('use slug → its file', JSON.stringify(d3));
    // resolveModulePath agrees (diagnostics / completion use it).
    const r = importProvider.resolveModulePath(root, main, 'slug');
    if (r === path.join(root, '.venv/lib/quirk/site-packages/slug/index.qk')) ok('resolveModulePath: venv site-packages');
    else fail('resolveModulePath: venv site-packages', String(r));
    fs.rmSync(root, { recursive: true, force: true });
}
{
    // Project-local site-packages win over everything.
    const root = project({
        'quirk.toml': 'name = "app"\n',
        'site-packages/thing/index.qk': 'define go() -> Int {\n    return 1\n}\n',
        'src/index.qk': 'use thing\nprint(thing.go())\n',
    });
    const r = importProvider.resolveModulePath(root, path.join(root, 'src/index.qk'), 'thing');
    if (r === path.join(root, 'site-packages/thing/index.qk')) ok('project site-packages');
    else fail('project site-packages', String(r));
    fs.rmSync(root, { recursive: true, force: true });
}
// Imports without a leading dot (compiler 7.44+): the app's own files.
{
    const root = project({
        'quirk.toml': 'name = "app"\n',
        'web/router.qk': 'router := 1\n',
        'web/main.qk': 'from router use { router }\n',
        'web/src/pages/counter.qk': 'from src.components.note use { Note }\n',
        'web/src/components/note/index.qk': 'struct Note {\n    text: String\n}\n',
    });
    const a = importProvider.resolveModulePath(root, path.join(root, 'web/main.qk'), 'router');
    if (a === path.join(root, 'web/router.qk')) ok('dotless import next to the file');
    else fail('dotless import next to the file', String(a));
    const b = importProvider.resolveModulePath(root, path.join(root, 'web/src/pages/counter.qk'), 'src.components.note');
    if (b === path.join(root, 'web/src/components/note/index.qk')) ok('dotless import from a folder above (index.qk)');
    else fail('dotless import from a folder above (index.qk)', String(b));
    const c = importProvider.resolveModulePath(root, path.join(root, 'web/main.qk'), 'nowhere.at_all');
    if (c === null) ok('an unknown local import is still unresolved');
    else fail('an unknown local import is still unresolved', String(c));
    fs.rmSync(root, { recursive: true, force: true });
}
process.env.QUIRK_HOME = savedHome;

// ---- 4. dead code (faded lines, 1-based) ----
{
    const { computeDeadLines } = require(path.join(OUT, 'DeadCodeProvider.js'));
    const cases = [
        ['a multi-line template after return isn\'t dead',
         'define t(self) -> String {\n    return r"""\n    <div>\n\n    </div>\n    """\n}\n', []],
        ['code after the template\'s closing """ is',
         'define t() -> String {\n    return r"""\n    <p>{ x }</p>\n    """\n    x := 1\n}\n', [5]],
        ['code after a plain return is',
         'define f() -> Int {\n    return 1\n    y := 2\n}\n', [3]],
        ['a string opened on a dead line is dead throughout',
         'define f() -> Int {\n    return 1\n    s := """\n    text\n    """\n}\n', [3, 4, 5]],
        ['"return" inside a string isn\'t a return',
         'define f() -> Int {\n    s := """\nreturn x\n"""\n    y := 2\n    return 1\n}\n', []],
    ];
    for (const [what, src, want] of cases) {
        const got = [...computeDeadLines(src)].map(x => x + 1);
        if (JSON.stringify(got) === JSON.stringify(want)) ok(what);
        else fail(what, `dead lines ${JSON.stringify(got)}, expected ${JSON.stringify(want)}`);
    }
}

console.log(`\n${passed} passed, ${failures} failed`);
process.exit(failures ? 1 : 0);
