#!/usr/bin/env node
/**
 * Quirk Language Server — v1.6.0 foundation.
 *
 * What this provides today:
 *   • Lifecycle (initialize/initialized/shutdown/exit).
 *   • Diagnostics via `quirk --check --diagnostics-json <file>`, triggered
 *     on open + save (NOT on every keystroke — keeps the compiler from
 *     running thrash-style and matches how `quirk run` already
 *     debounces in the existing VSCode extension).
 *
 * What's deferred to later 1.6.x releases:
 *   • Hover, completion, go-to-definition, references, rename, signature
 *     help, semantic tokens, formatter, outline. The VSCode extension
 *     keeps its in-process providers covering all of these for now.
 *
 * Why shell-out instead of porting the compiler to TypeScript:
 *   the compiler is the source of truth for what Quirk considers an
 *   error. Implementing a second-source semantic analyzer in TS would
 *   either drift or duplicate work. Shell-out keeps the LSP thin and
 *   correct by construction.
 */
import {
  createConnection,
  TextDocuments,
  Diagnostic,
  DiagnosticSeverity,
  ProposedFeatures,
  InitializeParams,
  TextDocumentSyncKind,
  InitializeResult,
  Position,
  StreamMessageReader,
  StreamMessageWriter,
} from 'vscode-languageserver/node';
import { TextDocument } from 'vscode-languageserver-textdocument';
import { spawn } from 'child_process';
import { URL, fileURLToPath } from 'url';
import * as path from 'path';

// One JSON-RPC connection over stdio, plus a document store.
//
// Pin the transport to (stdin, stdout) explicitly. Without this the
// `createConnection(ProposedFeatures.all)` shape errors out unless an
// `--stdio` / `--node-ipc` / `--socket=N` CLI flag is also passed —
// fragile when the editor doesn't add the flag (vim-lsp didn't last
// time I checked). Hard-wiring stdio keeps the spawn contract simple:
// `quirk-lsp` reads from stdin, writes to stdout. Nothing else needed.
const connection = createConnection(
  ProposedFeatures.all,
  new StreamMessageReader(process.stdin),
  new StreamMessageWriter(process.stdout),
);
const documents = new TextDocuments(TextDocument);

// Per-NDJSON-record shape emitted by `quirk --check --diagnostics-json`.
// Matches the C++ side (Sema.cpp / Parser.cpp). New fields can be added
// over time — extra keys are ignored on the LSP side.
interface QuirkDiag {
  level: 'error' | 'warning' | 'info';
  msg: string;
  path: string;
  line: number;   // 1-based, like the compiler prints
  col: number;    // 1-based, ditto
}

// Resolve the compiler binary in this order:
//   1. `quirk.executablePath` workspace setting (when present)
//   2. `QUIRK_BIN` env var (handy for monorepo dev loops)
//   3. `quirk` on PATH
// Caller is expected to fail gracefully if none of those work — the
// LSP just stops emitting diagnostics; everything else still functions.
function resolveQuirkBin(settingsPath: string | undefined): string {
  if (settingsPath && settingsPath.length > 0) return settingsPath;
  if (process.env.QUIRK_BIN && process.env.QUIRK_BIN.length > 0) return process.env.QUIRK_BIN;
  return 'quirk';
}

let quirkBinary = 'quirk';

connection.onInitialize((params: InitializeParams): InitializeResult => {
  // The client may pass `initializationOptions = { quirk: { executablePath: ... } }`
  // (Neovim / Helix LSP configs commonly do this) — pick it up if present.
  const opts = params.initializationOptions as { quirk?: { executablePath?: string } } | undefined;
  quirkBinary = resolveQuirkBin(opts?.quirk?.executablePath);

  connection.console.info(`quirk-lsp: using compiler at '${quirkBinary}'`);

  return {
    capabilities: {
      // Full document sync keeps the LSP simple — every change ships the
      // whole buffer. Incremental sync would be a measurable win only
      // for very large files (Quirk projects don't have those yet).
      textDocumentSync: TextDocumentSyncKind.Full,
    },
    serverInfo: { name: 'quirk-lsp', version: '0.1.0' },
  };
});

// Convert the compiler's 1-based (line, col) → LSP 0-based (line, character).
// Returns a 1-character-wide range starting at the reported column. The
// compiler today doesn't carry width info, so we fake it; if/when it
// gains it, this can produce a more accurate underline.
function rangeFromQuirkDiag(d: QuirkDiag): { start: Position; end: Position } {
  const line = Math.max(0, d.line - 1);
  const ch   = Math.max(0, d.col - 1);
  return {
    start: { line, character: ch },
    end:   { line, character: ch + 1 },
  };
}

function severityFromLevel(level: QuirkDiag['level']): DiagnosticSeverity {
  switch (level) {
    case 'error':   return DiagnosticSeverity.Error;
    case 'warning': return DiagnosticSeverity.Warning;
    default:        return DiagnosticSeverity.Information;
  }
}

// Run `quirk --check --diagnostics-json <file>` and turn the NDJSON
// records into LSP `Diagnostic` objects. Exits cleanly when the compiler
// fails to launch (e.g. not on PATH) — the LSP keeps running and the
// editor stays usable.
async function checkDocument(document: TextDocument): Promise<void> {
  let filePath: string;
  try { filePath = fileURLToPath(document.uri); }
  catch {
    // untitled:// or other non-file URIs aren't checkable; skip silently.
    return;
  }

  const child = spawn(quirkBinary, ['--check', '--diagnostics-json', filePath], {
    cwd: path.dirname(filePath),
    stdio: ['ignore', 'pipe', 'pipe'],
  });

  let stdout = '';
  child.stdout.on('data', (chunk) => { stdout += chunk.toString('utf8'); });

  // Collect stderr only so we can surface launch failures; we don't
  // parse anything from it (`--diagnostics-json` keeps everything
  // structured to stdout).
  let stderr = '';
  child.stderr.on('data', (chunk) => { stderr += chunk.toString('utf8'); });

  child.on('error', (err) => {
    connection.console.warn(`quirk-lsp: failed to spawn '${quirkBinary}': ${err.message}`);
  });

  await new Promise<void>((resolve) => {
    child.on('close', () => resolve());
  });

  // Bucket diagnostics by their reported `path` so each open document
  // gets only the diagnostics that belong to it. Cross-file errors (e.g.
  // `from foo use { Bar }` where `foo`'s parse fails) end up attached to
  // whichever file the compiler blamed — which is the right behaviour.
  const byUri = new Map<string, Diagnostic[]>();
  for (const line of stdout.split('\n')) {
    if (!line.trim()) continue;
    let rec: QuirkDiag;
    try { rec = JSON.parse(line) as QuirkDiag; }
    catch {
      connection.console.warn(`quirk-lsp: dropping unparseable diagnostic line: ${line}`);
      continue;
    }
    const uri = recordUri(rec.path) ?? document.uri;
    const list = byUri.get(uri) ?? [];
    list.push({
      severity: severityFromLevel(rec.level),
      range: rangeFromQuirkDiag(rec),
      message: rec.msg,
      source: 'quirk',
    });
    byUri.set(uri, list);
  }

  // Always publish for the document we ran the check on — even when
  // empty — so the editor clears previous errors. Then publish anything
  // else surfaced for other open files.
  if (!byUri.has(document.uri)) byUri.set(document.uri, []);
  for (const [uri, diagnostics] of byUri) {
    connection.sendDiagnostics({ uri, diagnostics });
  }

  // If the compiler couldn't even start, surface that as one
  // information-level diagnostic at the top of the file so the user
  // notices it instead of staring at an empty problems list.
  if (stderr && !stdout) {
    connection.console.warn(`quirk-lsp: compiler stderr: ${stderr.slice(0, 400)}`);
  }
}

// Turn an absolute path on disk into the `file://` URI the editor uses.
// Returns null for paths we can't parse (e.g. relative or empty).
function recordUri(p: string): string | null {
  if (!p) return null;
  try { return new URL('file://' + path.resolve(p)).toString(); }
  catch { return null; }
}

// Trigger a check on open + save. Skipping on `didChangeContent` means
// the LSP doesn't run the compiler on every keystroke — fast typists
// would queue up many parallel compilations otherwise.
documents.onDidOpen((e) => { void checkDocument(e.document); });
documents.onDidSave((e) => { void checkDocument(e.document); });

documents.listen(connection);
connection.listen();
