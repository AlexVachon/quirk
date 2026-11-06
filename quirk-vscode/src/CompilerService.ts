// Compiler-backed checks (extension 0.4.0+, compiler 7.9.3+; live checks 0.5.0 / 7.9.4).
//
// The rest of the extension reads Quirk with regexes, which can't see type
// errors (Q0209 "cannot assign 'String' to 'n' of type 'Int'"), and can only
// guess a variable's type. This module asks the compiler instead: on open, on
// save and — with compiler 7.9.4+, which reads the unsaved buffer from stdin
// (`--stdin-source`) — as you type, half a second after the last keystroke:
//
//   quirk --check --diagnostics-json <file>  → one JSON diagnostic per line
//   quirk --symbols-json <file>              → declarations with their types
//
// Diagnostics go into their own collection ("quirk-compiler") with their
// stable code; `compilerReportedLines` lets the regex linter skip lines the
// compiler already flagged, so the same mistake isn't reported twice.
// `compilerVarType` gives hover / completion the compiler's type for a
// variable (`List[P]`, `P` for a loop over it, …).

import * as vscode from 'vscode';
import * as path from 'path';
import * as fs from 'fs';
import { spawn, ChildProcess } from 'child_process';

interface CompilerDiag {
    level: 'error' | 'warning' | 'info';
    msg: string;
    path: string;
    line: number;   // 1-based
    col: number;    // 1-based
    code?: string;  // `Q0209`
}

interface CompilerSymbol {
    kind: string;
    name: string;
    scope: string;
    file: string;
    line: number;
    col: number;
    type?: string;
}

const TIMEOUT_MS = 15000;
const TYPING_DELAY_MS = 500;
const VAR_KINDS = new Set(['variable', 'parameter', 'module_const']);

const reportedLines = new Map<string, Set<number>>();       // uri → 0-based lines
const varSymbols = new Map<string, CompilerSymbol[]>();     // uri → this file's variables
const running = new Map<string, ChildProcess[]>();          // uri → in-flight children

let compilerCommand = 'quirk';
let compilerEnv: NodeJS.ProcessEnv = process.env;
let collection: vscode.DiagnosticCollection | undefined;
let onResults: ((doc: vscode.TextDocument) => void) | undefined;
let supportsStdin = false;   // compiler ≥ 7.9.4: `--stdin-source`

function sameFile(a: string, b: string): boolean {
    if (a === b) return true;
    try { return fs.realpathSync(a) === fs.realpathSync(b); } catch { return path.resolve(a) === path.resolve(b); }
}

function runCompiler(args: string[], cwd: string, key: string, input?: string): Promise<string | null> {
    return new Promise(resolve => {
        let out = '';
        let child: ChildProcess;
        try {
            child = spawn(compilerCommand, args, { cwd, env: compilerEnv });
        } catch {
            resolve(null);
            return;
        }
        const list = running.get(key) ?? [];
        list.push(child);
        running.set(key, list);
        const timer = setTimeout(() => child.kill(), TIMEOUT_MS);
        if (input !== undefined) {
            child.stdin?.on('error', () => { /* child exited early */ });
            child.stdin?.end(input);
        }
        child.stdout?.on('data', (d: Buffer) => { out += d.toString(); });
        child.stderr?.on('data', () => { /* human-readable copy; JSON is on stdout */ });
        const done = (ok: boolean) => {
            clearTimeout(timer);
            const l = running.get(key);
            if (l) running.set(key, l.filter(c => c !== child));
            resolve(ok ? out : null);
        };
        child.on('error', () => done(false));
        child.on('close', (_code, signal) => done(signal === null));
    });
}

function parseLines<T>(text: string): T[] {
    const out: T[] = [];
    for (const line of text.split('\n')) {
        const t = line.trim();
        if (!t.startsWith('{')) continue;
        try { out.push(JSON.parse(t) as T); } catch { /* partial / foreign line */ }
    }
    return out;
}

function toDiagnostic(doc: vscode.TextDocument, d: CompilerDiag): vscode.Diagnostic {
    const lineIdx = Math.min(Math.max(d.line - 1, 0), Math.max(doc.lineCount - 1, 0));
    const text = doc.lineAt(lineIdx).text;
    const startCol = Math.min(Math.max(d.col - 1, 0), text.length);
    const word = doc.getWordRangeAtPosition(new vscode.Position(lineIdx, startCol), /[A-Za-z0-9_]+/);
    const range = word && word.start.character === startCol
        ? word
        : new vscode.Range(lineIdx, startCol, lineIdx, Math.max(text.length, startCol + 1));
    const severity = d.level === 'warning' ? vscode.DiagnosticSeverity.Warning
                   : d.level === 'info'    ? vscode.DiagnosticSeverity.Information
                   :                         vscode.DiagnosticSeverity.Error;
    // The compiler appends hints as extra lines ("\n  hint: …"); keep them.
    const diag = new vscode.Diagnostic(range, d.msg.replace(/\n\s+/g, '\n'), severity);
    diag.source = 'quirk';
    if (d.code) diag.code = d.code;
    return diag;
}

async function check(doc: vscode.TextDocument): Promise<void> {
    if (doc.languageId !== 'quirk' || doc.uri.scheme !== 'file' || doc.isUntitled) return;
    const file = doc.uri.fsPath;
    const key = doc.uri.toString();
    for (const c of running.get(key) ?? []) c.kill();   // superseded by this run
    const cwd = path.dirname(file);
    const version = doc.version;
    // With stdin support the compiler checks the buffer as shown, saved
    // or not; otherwise it reads the file from disk.
    const input = supportsStdin ? doc.getText() : undefined;
    const extra = supportsStdin ? ['--stdin-source'] : [];

    const [diagOut, symOut] = await Promise.all([
        runCompiler(['--check', '--diagnostics-json', ...extra, file], cwd, key, input),
        runCompiler(['--symbols-json', ...extra, file], cwd, key, input),
    ]);
    // Edited while the compiler ran: a newer check is queued; positions
    // from this one would be off.
    if (doc.version !== version) return;

    if (diagOut !== null && collection) {
        const diags = parseLines<CompilerDiag>(diagOut).filter(d => d.path && sameFile(d.path, file));
        const list = diags.map(d => toDiagnostic(doc, d));
        collection.set(doc.uri, list);
        reportedLines.set(key, new Set(list.map(d => d.range.start.line)));
    }
    if (symOut !== null) {
        const syms = parseLines<CompilerSymbol>(symOut)
            .filter(s => VAR_KINDS.has(s.kind) && s.file && sameFile(s.file, file));
        // Sema aborts at the first error, so a run with errors can come
        // back empty; keep the last good table rather than losing hovers.
        if (syms.length > 0 || !varSymbols.has(key)) varSymbols.set(key, syms);
    }
    onResults?.(doc);
}

/** Lines (0-based) where the compiler reported a problem in this document. */
export function compilerReportedLines(uri: vscode.Uri): Set<number> | undefined {
    return reportedLines.get(uri.toString());
}

/**
 * The compiler's type for variable `name` as seen at `position`: the
 * closest declaration at or above that line. Undefined when the compiler
 * hasn't run yet, doesn't know the type, or only knows it as `Any`.
 */
export function compilerVarType(doc: vscode.TextDocument, position: vscode.Position, name: string): string | undefined {
    const syms = varSymbols.get(doc.uri.toString());
    if (!syms) return undefined;
    const line = position.line + 1;
    let best: CompilerSymbol | undefined;
    for (const s of syms) {
        if (s.name !== name || !s.type) continue;
        if (s.line <= line && (!best || s.line >= best.line)) best = s;
    }
    if (!best) best = syms.find(s => s.name === name && s.type);   // declared below (module-level use)
    if (!best || !best.type || best.type === 'Any' || best.type === 'unknown') return undefined;
    return best.type;
}

export function startCompilerService(
    context: vscode.ExtensionContext,
    command: string,
    quirkHome: string | undefined,
    afterResults: (doc: vscode.TextDocument) => void,
): void {
    compilerCommand = command;
    compilerEnv = quirkHome ? { ...process.env, QUIRK_HOME: quirkHome } : process.env;
    collection = vscode.languages.createDiagnosticCollection('quirk-compiler');
    onResults = afterResults;
    context.subscriptions.push(collection);

    // `quirk --version` → "quirk 7.9.4": live checking needs ≥ 7.9.4.
    const probe = spawn(command, ['--version'], { env: compilerEnv });
    let vOut = '';
    probe.stdout?.on('data', (d: Buffer) => { vOut += d.toString(); });
    probe.on('error', () => { /* no compiler: open/save checks just fail quietly */ });
    probe.on('close', () => {
        const m = /(\d+)\.(\d+)\.(\d+)/.exec(vOut);
        if (!m) return;
        const [maj, min, pat] = [Number(m[1]), Number(m[2]), Number(m[3])];
        supportsStdin = maj > 7 || (maj === 7 && (min > 9 || (min === 9 && pat >= 4)));
    });

    const typingTimers = new Map<string, ReturnType<typeof setTimeout>>();
    const checkedOnce = new Set<string>();
    const maybeCheck = (doc: vscode.TextDocument) => {
        checkedOnce.add(doc.uri.toString());
        void check(doc);
    };

    for (const ed of vscode.window.visibleTextEditors) maybeCheck(ed.document);
    context.subscriptions.push(
        vscode.workspace.onDidOpenTextDocument(maybeCheck),
        vscode.workspace.onDidSaveTextDocument(maybeCheck),
        vscode.workspace.onDidChangeTextDocument(e => {
            if (!supportsStdin || e.document.languageId !== 'quirk' || e.contentChanges.length === 0) return;
            const key = e.document.uri.toString();
            const t = typingTimers.get(key);
            if (t) clearTimeout(t);
            typingTimers.set(key, setTimeout(() => {
                typingTimers.delete(key);
                maybeCheck(e.document);
            }, TYPING_DELAY_MS));
        }),
        vscode.window.onDidChangeActiveTextEditor(ed => {
            if (ed && !checkedOnce.has(ed.document.uri.toString())) maybeCheck(ed.document);
        }),
        vscode.workspace.onDidCloseTextDocument(doc => {
            const key = doc.uri.toString();
            for (const c of running.get(key) ?? []) c.kill();
            const t = typingTimers.get(key);
            if (t) { clearTimeout(t); typingTimers.delete(key); }
            collection?.delete(doc.uri);
            reportedLines.delete(key);
            varSymbols.delete(key);
            checkedOnce.delete(key);
        }),
    );
}
