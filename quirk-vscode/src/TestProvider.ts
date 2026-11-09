// Quirk tests in VS Code's Testing panel (extension 0.7.0, compiler 7.10.0+).
//
// Every `*_test.qk` / `*_test.quirk` file in the workspace is a test item;
// its `define test_*()` functions are its children, with ▶ buttons in the
// editor gutter. Running uses the same generated runner as `quirk test`:
//
//   quirk run --test-harness [--test-k=<name>] <file>
//
// which prints `##quirk-test:start/pass/fail <name>` around each test. The
// test's output goes to the Test Results panel; a failure is shown at the
// failing line of the test (from its traceback). A file with its own main()
// is a single test that passes when it exits 0.

import * as vscode from 'vscode';
import * as path from 'path';
import * as fs from 'fs';
import { spawn, ChildProcess } from 'child_process';
import { findProjectRootFor } from './ImportProvider';

const TEST_GLOB = '**/*_test.{qk,quirk}';
const EXCLUDE = '**/{node_modules,packages,.venv,.git,site-packages}/**';
const TEST_FN_RE = /^define\s+(test_\w+)\s*\(\s*\)/;
const MAIN_RE = /^define\s+main\s*\(/m;

interface FileInfo { hasMain: boolean; }
const fileInfo = new WeakMap<vscode.TestItem, FileInfo>();

export function registerTestProvider(context: vscode.ExtensionContext, command: string, quirkHome?: string): void {
    const ctrl = vscode.tests.createTestController('quirkTests', 'Quirk');
    context.subscriptions.push(ctrl);
    const env: NodeJS.ProcessEnv = { ...process.env, QUIRK_DEBUG_SKIP: '1', NO_COLOR: '1',
        ...(quirkHome ? { QUIRK_HOME: quirkHome } : {}) };

    // ---- discovery ------------------------------------------------------

    function fileItem(uri: vscode.Uri): vscode.TestItem {
        const id = uri.toString();
        let item = ctrl.items.get(id);
        if (!item) {
            item = ctrl.createTestItem(id, vscode.workspace.asRelativePath(uri), uri);
            item.canResolveChildren = true;
            ctrl.items.add(item);
        }
        return item;
    }

    function parse(item: vscode.TestItem, text: string): void {
        const lines = text.split(/\r?\n/);
        const children: vscode.TestItem[] = [];
        for (let i = 0; i < lines.length; i++) {
            const m = TEST_FN_RE.exec(lines[i]);
            if (!m) continue;
            const child = ctrl.createTestItem(`${item.id}::${m[1]}`, m[1], item.uri);
            child.range = new vscode.Range(i, 0, i, lines[i].length);
            children.push(child);
        }
        const hasMain = MAIN_RE.test(text);
        fileInfo.set(item, { hasMain });
        // A main() file runs as one test (the file item itself).
        item.children.replace(hasMain ? [] : children);
        item.canResolveChildren = false;
    }

    async function parseFile(uri: vscode.Uri, text?: string): Promise<void> {
        if (text === undefined) {
            try { text = Buffer.from(await vscode.workspace.fs.readFile(uri)).toString('utf8'); }
            catch { return; }
        }
        parse(fileItem(uri), text);
    }

    const isTestFile = (uri: vscode.Uri) => uri.scheme === 'file' && /_test\.(qk|quirk)$/.test(uri.fsPath);

    ctrl.resolveHandler = async (item) => {
        if (!item) {
            for (const uri of await vscode.workspace.findFiles(TEST_GLOB, EXCLUDE)) await parseFile(uri);
            return;
        }
        if (item.uri) await parseFile(item.uri);
    };

    const watcher = vscode.workspace.createFileSystemWatcher(TEST_GLOB);
    context.subscriptions.push(
        watcher,
        watcher.onDidCreate(uri => { if (!/[\\/](node_modules|packages|\.venv)[\\/]/.test(uri.fsPath)) void parseFile(uri); }),
        watcher.onDidChange(uri => { if (ctrl.items.get(uri.toString())) void parseFile(uri); }),
        watcher.onDidDelete(uri => ctrl.items.delete(uri.toString())),
        // Unsaved edits update the gutter buttons as you type.
        vscode.workspace.onDidChangeTextDocument(e => {
            if (isTestFile(e.document.uri)) parse(fileItem(e.document.uri), e.document.getText());
        }),
        vscode.workspace.onDidOpenTextDocument(doc => {
            if (isTestFile(doc.uri)) parse(fileItem(doc.uri), doc.getText());
        }),
    );

    // ---- running --------------------------------------------------------

    function runProcess(args: string[], cwd: string, token: vscode.CancellationToken):
            Promise<{ out: string; code: number | null }> {
        return new Promise(resolve => {
            let out = '';
            let child: ChildProcess;
            try { child = spawn(command, args, { cwd, env }); }
            catch (e) { resolve({ out: String(e), code: -1 }); return; }
            const kill = token.onCancellationRequested(() => child.kill());
            child.stdout?.on('data', (d: Buffer) => { out += d.toString(); });
            child.stderr?.on('data', (d: Buffer) => { out += d.toString(); });
            child.on('error', (e: Error) => { out += e.message; });
            child.on('close', (code: number | null) => { kill.dispose(); resolve({ out, code }); });
        });
    }

    // The failing line inside the test, from its traceback; else the
    // test's `define` line.
    function failureMessage(output: string, file: string, test: vscode.TestItem | undefined): vscode.TestMessage {
        const lines = output.trimEnd().split('\n');
        // The exception line (`ValueError: …`), or a crash report's
        // `Quirk runtime error: stack overflow …`.
        const last = lines.find(l => l.startsWith('Quirk runtime error:'))
            ?? [...lines].reverse().find(l => /^\w*(Error|Exception)\b.*:/.test(l))
            ?? lines[lines.length - 1] ?? 'failed';
        const msg = new vscode.TestMessage(last);
        const frameRe = /^\s*File "(.+)", line (\d+), in (.+)$/;
        let loc: vscode.Location | undefined;
        for (const l of lines) {
            const m = frameRe.exec(l);
            if (!m) continue;
            const sameFile = path.resolve(m[1]) === path.resolve(file);
            // The deepest frame in this file wins (the test's own line, or
            // a helper it called in the same file).
            if (sameFile) loc = new vscode.Location(vscode.Uri.file(file), new vscode.Position(Number(m[2]) - 1, 0));
        }
        if (!loc && test?.uri && test.range) loc = new vscode.Location(test.uri, test.range);
        if (loc) msg.location = loc;
        return msg;
    }

    async function runFile(run: vscode.TestRun, fileIt: vscode.TestItem, only: vscode.TestItem[] | undefined,
                           token: vscode.CancellationToken): Promise<void> {
        const file = fileIt.uri!.fsPath;
        const cwd = findProjectRootFor(file) || path.dirname(file);
        const info = fileInfo.get(fileIt) ?? { hasMain: MAIN_RE.test(fs.readFileSync(file, 'utf8')) };

        if (info.hasMain) {
            run.started(fileIt);
            const t0 = Date.now();
            const { out, code } = await runProcess(['run', file], cwd, token);
            run.appendOutput(out.replace(/\n/g, '\r\n'), undefined, fileIt);
            if (code === 0) run.passed(fileIt, Date.now() - t0);
            else run.failed(fileIt, failureMessage(out || `exit ${code}`, file, fileIt), Date.now() - t0);
            return;
        }

        const tests = only ?? collect(fileIt.children);
        if (tests.length === 0) return;
        for (const t of tests) run.enqueued(t);
        // One process for the file; a single test narrows it with -k (a
        // substring, so similar names may run too — only the requested
        // ones are reported).
        const args = ['run', '--test-harness'];
        if (only && only.length === 1) args.push(`--test-k=${only[0].label}`);
        args.push(file);

        const byName = new Map(tests.map(t => [t.label, t]));
        const t0 = Date.now();
        const { out, code } = await runProcess(args, cwd, token);
        let current: vscode.TestItem | undefined;
        let buf = '';
        let started = 0;
        const reported = new Set<vscode.TestItem>();
        for (const line of out.split('\n')) {
            let m = /^##quirk-test:start (\S+)/.exec(line);
            if (m) {
                current = byName.get(m[1]);
                buf = '';
                started = Date.now();
                if (current) run.started(current);
                continue;
            }
            m = /^##quirk-test:(pass|fail) (\S+)/.exec(line);
            if (m) {
                const t = byName.get(m[2]);
                if (t) {
                    if (buf) run.appendOutput(buf.replace(/\n/g, '\r\n'), undefined, t);
                    if (m[1] === 'pass') run.passed(t, Date.now() - started);
                    else run.failed(t, failureMessage(buf, file, t), Date.now() - started);
                    reported.add(t);
                }
                current = undefined;
                continue;
            }
            if (line.startsWith('##quirk-test:begin')) continue;
            if (current) buf += line + '\n';
        }
        // Didn't report: a crash, a compile error, or a cancelled run.
        for (const t of tests) {
            if (reported.has(t)) continue;
            if (token.isCancellationRequested) { run.skipped(t); continue; }
            if (t === current) {
                run.appendOutput(buf.replace(/\n/g, '\r\n'), undefined, t);
                run.errored(t, failureMessage(buf || out, file, t), Date.now() - started);
            } else if (!/##quirk-test:begin/.test(out)) {
                // Never got to run any test: compile error / old compiler.
                run.errored(t, new vscode.TestMessage(out.trim() || `exit ${code}`), Date.now() - t0);
            } else {
                run.skipped(t);   // after a crash
            }
        }
    }

    function collect(items: vscode.TestItemCollection): vscode.TestItem[] {
        const all: vscode.TestItem[] = [];
        items.forEach(i => all.push(i));
        return all;
    }

    async function runHandler(request: vscode.TestRunRequest, token: vscode.CancellationToken): Promise<void> {
        const run = ctrl.createTestRun(request);
        // Group the requested items by file.
        const files = new Map<vscode.TestItem, vscode.TestItem[] | undefined>();
        const roots: vscode.TestItem[] = [];
        if (request.include) roots.push(...request.include);
        else ctrl.items.forEach(i => roots.push(i));
        for (const item of roots) {
            if (item.parent) {
                const list = files.get(item.parent);
                if (list !== undefined || !files.has(item.parent)) files.set(item.parent, [...(list ?? []), item]);
            } else {
                if (item.canResolveChildren && item.uri) await parseFile(item.uri);
                files.set(item, undefined);   // the whole file
            }
        }
        const excluded = new Set(request.exclude ?? []);
        for (const [fileIt, only] of files) {
            if (token.isCancellationRequested) break;
            if (excluded.has(fileIt)) continue;
            const chosen = only?.filter(t => !excluded.has(t));
            const fullFile = chosen === undefined && !request.exclude?.length;
            await runFile(run, fileIt, fullFile ? undefined : (chosen ?? collect(fileIt.children).filter(t => !excluded.has(t))), token);
        }
        run.end();
    }

    ctrl.createRunProfile('Run', vscode.TestRunProfileKind.Run, runHandler, true);
}
