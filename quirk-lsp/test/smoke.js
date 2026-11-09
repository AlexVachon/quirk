#!/usr/bin/env node
// quirk-lsp smoke test: start the built server (out/server.js) over stdio,
// speak raw JSON-RPC to it and check what an editor would see.
//
//   QUIRK=/path/to/bin/quirk node test/smoke.js
//
// Checks: diagnostics with their codes on open; live diagnostics after an
// unsaved change (compiler 7.9.4+ `--stdin-source`); inlay hints carrying
// the compiler's types (`List[P]`, a `for` loop variable's `P`).
'use strict';
const { spawn } = require('child_process');
const path = require('path');
const fs = require('fs');
const { pathToFileURL } = require('url');

const quirk = process.env.QUIRK || 'quirk';
const fixtures = path.resolve(__dirname, '..', '..', 'quirk-compiler', 'tools', 'fixtures');
const errorsFile = path.join(fixtures, 'editor_errors.qk');
const typesFile = path.join(fixtures, 'editor_types.qk');
const fixesFile = path.join(fixtures, 'editor_fixes.qk');

const server = spawn(process.execPath, [path.join(__dirname, '..', 'out', 'server.js')], {
    stdio: ['pipe', 'pipe', 'inherit'],
});

let buf = Buffer.alloc(0);
let nextId = 1;
const pending = new Map();
const waiters = [];   // { method, pred, resolve }

server.stdout.on('data', (chunk) => {
    buf = Buffer.concat([buf, chunk]);
    for (;;) {
        const head = buf.indexOf('\r\n\r\n');
        if (head < 0) return;
        const m = /Content-Length: (\d+)/i.exec(buf.slice(0, head).toString());
        const len = m ? Number(m[1]) : 0;
        if (buf.length < head + 4 + len) return;
        const msg = JSON.parse(buf.slice(head + 4, head + 4 + len).toString());
        buf = buf.slice(head + 4 + len);
        if (msg.id !== undefined && pending.has(msg.id)) {
            pending.get(msg.id)(msg.result);
            pending.delete(msg.id);
        } else if (msg.method) {
            for (let i = 0; i < waiters.length; i++) {
                const w = waiters[i];
                if (w.method === msg.method && w.pred(msg.params)) {
                    waiters.splice(i, 1);
                    w.resolve(msg.params);
                    break;
                }
            }
        }
    }
});

function send(obj) {
    const body = JSON.stringify({ jsonrpc: '2.0', ...obj });
    server.stdin.write(`Content-Length: ${Buffer.byteLength(body)}\r\n\r\n${body}`);
}
function request(method, params) {
    const id = nextId++;
    return new Promise((resolve) => { pending.set(id, resolve); send({ id, method, params }); });
}
function notify(method, params) { send({ method, params }); }
function waitFor(method, pred, what) {
    return new Promise((resolve, reject) => {
        const t = setTimeout(() => reject(new Error(`timed out waiting for ${what}`)), 20000);
        waiters.push({ method, pred, resolve: (p) => { clearTimeout(t); resolve(p); } });
    });
}

const fails = [];
function check(cond, what) { if (!cond) fails.push(what); else console.log('  ok', what); }

(async () => {
    await request('initialize', {
        processId: process.pid, rootUri: pathToFileURL(fixtures).href, capabilities: {},
        initializationOptions: { quirk: { executablePath: quirk } },
    });
    notify('initialized', {});

    // 1. Diagnostics on open, with codes.
    const errUri = pathToFileURL(errorsFile).href;
    const onOpen = waitFor('textDocument/publishDiagnostics', (p) => p.uri === errUri, 'diagnostics on open');
    notify('textDocument/didOpen', { textDocument: {
        uri: errUri, languageId: 'quirk', version: 1, text: fs.readFileSync(errorsFile, 'utf8') } });
    const d1 = await onOpen;
    const got = d1.diagnostics.map((d) => `${d.range.start.line + 1}:${d.code}`).sort();
    check(JSON.stringify(got) === JSON.stringify(['11:Q0209', '3:Q0209', '4:Q0100', '8:Q0210']),
          `every error with its code on open (got ${got.join(', ')})`);

    // 2. Live: an unsaved edit is checked without a save.
    const live = waitFor('textDocument/publishDiagnostics',
        (p) => p.uri === errUri && p.diagnostics.some((d) => /unsaved_name/.test(d.message)), 'live diagnostics');
    notify('textDocument/didChange', { textDocument: { uri: errUri, version: 2 },
        contentChanges: [{ text: 'define main() -> void {\n    print(unsaved_name)\n}\n' }] });
    const d2 = await live;
    check(d2.diagnostics.length === 1 && d2.diagnostics[0].range.start.line === 1,
          'an unsaved edit is re-checked (one error, on line 2)');

    // 3. Inlay hints use the compiler's types.
    const typesUri = pathToFileURL(typesFile).href;
    notify('textDocument/didOpen', { textDocument: {
        uri: typesUri, languageId: 'quirk', version: 1, text: fs.readFileSync(typesFile, 'utf8') } });
    let hints = [];
    for (let i = 0; i < 40 && hints.length === 0; i++) {   // symbols load asynchronously
        await new Promise((r) => setTimeout(r, 250));
        hints = await request('textDocument/inlayHint', { textDocument: { uri: typesUri },
            range: { start: { line: 0, character: 0 }, end: { line: 100, character: 0 } } }) || [];
    }
    const at = (line) => hints.filter((h) => h.position.line === line - 1).map((h) => `${h.position.character}${h.label}`);
    check(at(13).includes('6: List[P]'), `people: List[P] hint (line 13: ${at(13)})`);
    check(at(8).includes('9: P'), `for-loop variable hint placed after 'p', not inside 'for' (line 8: ${at(8)})`);

    // 4. Quick fixes from the compiler's "did you mean" hints.
    const fixUri = pathToFileURL(fixesFile).href;
    const onFix = waitFor('textDocument/publishDiagnostics', (p) => p.uri === fixUri, 'diagnostics for fixes');
    notify('textDocument/didOpen', { textDocument: {
        uri: fixUri, languageId: 'quirk', version: 1, text: fs.readFileSync(fixesFile, 'utf8') } });
    const d4 = await onFix;
    const titles = [];
    for (const diag of d4.diagnostics) {
        const acts = await request('textDocument/codeAction', { textDocument: { uri: fixUri },
            range: diag.range, context: { diagnostics: [diag] } }) || [];
        for (const a of acts) {
            const edits = a.edit?.changes?.[fixUri] ?? [];
            titles.push(`${diag.code}:${edits.map((e) => e.newText).join(',')}`);
        }
    }
    check(titles.includes('Q0105:name') && titles.includes('Q0104:greet'),
          `member / method typos offer the right fix (got ${titles.join(' ')})`);

    await request('shutdown', null);
    notify('exit', null);
    for (const f of fails) console.log('FAIL', f);
    console.log(`quirk-lsp smoke: ${fails.length ? 'FAILED' : 'ok'}`);
    process.exit(fails.length ? 1 : 0);
})().catch((e) => { console.log('FAIL', e.message); server.kill(); process.exit(1); });
