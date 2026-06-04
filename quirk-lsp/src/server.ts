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
  DocumentSymbol,
  DocumentSymbolParams,
  DocumentFormattingParams,
  ProposedFeatures,
  InitializeParams,
  TextDocumentSyncKind,
  InitializeResult,
  Position,
  Range,
  StreamMessageReader,
  StreamMessageWriter,
  SymbolKind,
  TextEdit,
} from 'vscode-languageserver/node';
import { TextDocument } from 'vscode-languageserver-textdocument';
import { spawn, spawnSync } from 'child_process';
import { URL, fileURLToPath } from 'url';
import * as fs from 'fs';
import * as os from 'os';
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
      documentSymbolProvider: true,
      documentFormattingProvider: true,
    },
    serverInfo: { name: 'quirk-lsp', version: '0.2.0' },
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

// ──────────────────────────────────────────────────────────────────────
// Document symbols (`textDocument/documentSymbol`)
// ──────────────────────────────────────────────────────────────────────
// Powers the editor's outline / breadcrumbs / `@` symbol picker. We
// don't run the compiler for this — a focused regex over the buffer
// gives us top-level + method-level definitions cheaply. The compiler
// already exposes structured AST via `--emit-ast` but that's heavier
// than what an outline panel actually needs.

// Regexes leverage Quirk's column-0 convention for top-level
// definitions: `define`, `struct`, `enum`, `interface` sit at the
// margin; nested method `define`s are indented at least one column.
// Crucially TOP_LEVEL must NOT accept leading whitespace, otherwise
// every method also matches as a top-level function and methods get
// hoisted out of their enclosing struct in the outline.
const TOP_LEVEL_RE = /^(define|struct|enum|interface)\s+([A-Za-z_][A-Za-z0-9_]*)/;
const METHOD_RE    = /^[ \t]+define\s+([A-Za-z_][A-Za-z0-9_]*)/;

function symbolKindFor(keyword: string): SymbolKind {
  switch (keyword) {
    case 'struct':    return SymbolKind.Struct;
    case 'enum':      return SymbolKind.Enum;
    case 'interface': return SymbolKind.Interface;
    default:          return SymbolKind.Function;
  }
}

// Build a flat list of top-level DocumentSymbols, nesting method
// definitions under their enclosing struct. Doesn't try to be clever
// about closing braces — uses indentation as the nesting signal, which
// works for the canonical formatter output and matches what users
// actually write.
function extractSymbols(doc: TextDocument): DocumentSymbol[] {
  const lines = doc.getText().split(/\r?\n/);
  const out: DocumentSymbol[] = [];
  let currentStruct: DocumentSymbol | null = null;

  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];

    const top = line.match(TOP_LEVEL_RE);
    if (top) {
      const [, kw, name] = top;
      const startCol = line.indexOf(name);
      const range: Range = {
        start: { line: i, character: 0 },
        end:   { line: i, character: line.length },
      };
      const selectionRange: Range = {
        start: { line: i, character: startCol },
        end:   { line: i, character: startCol + name.length },
      };
      const sym: DocumentSymbol = {
        name, kind: symbolKindFor(kw), range, selectionRange, children: [],
      };
      out.push(sym);
      // Only nest methods inside structs/interfaces — enum variants
      // aren't method-shaped, and top-level `define` doesn't contain
      // other defines worth showing in the outline.
      currentStruct = (kw === 'struct' || kw === 'interface') ? sym : null;
      continue;
    }

    if (currentStruct) {
      const meth = line.match(METHOD_RE);
      if (meth) {
        const [, name] = meth;
        const startCol = line.indexOf(name);
        const range: Range = {
          start: { line: i, character: 0 },
          end:   { line: i, character: line.length },
        };
        const selectionRange: Range = {
          start: { line: i, character: startCol },
          end:   { line: i, character: startCol + name.length },
        };
        currentStruct.children!.push({
          name, kind: SymbolKind.Method, range, selectionRange, children: [],
        });
      }
      // A top-level closing brace at column 0 — the struct is over.
      if (/^[ \t]*\}\s*$/.test(line) && !/^[ \t]+/.test(line)) {
        currentStruct = null;
      }
    }
  }
  return out;
}

connection.onDocumentSymbol((params: DocumentSymbolParams): DocumentSymbol[] => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return [];
  return extractSymbols(doc);
});

// ──────────────────────────────────────────────────────────────────────
// Formatting (`textDocument/formatting`)
// ──────────────────────────────────────────────────────────────────────
// Shell out to `quirk fmt --stdout` so the formatter and the LSP can
// never drift — same canonical style as `quirk fmt path/to/file` from
// the CLI. Write the current buffer to a temp file (the formatter
// reads file paths, not stdin); replace the whole document on success.
connection.onDocumentFormatting((params: DocumentFormattingParams): TextEdit[] => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return [];

  const tmp = path.join(os.tmpdir(), `quirk-lsp-fmt-${process.pid}-${Date.now()}.quirk`);
  fs.writeFileSync(tmp, doc.getText(), 'utf8');

  try {
    const result = spawnSync(quirkBinary, ['fmt', '--stdout', tmp], {
      encoding: 'utf8',
      timeout: 15_000,
    });
    if (result.status !== 0) {
      connection.console.warn(
        `quirk-lsp: fmt failed (exit ${result.status}): ${result.stderr?.slice(0, 200) ?? ''}`,
      );
      return [];
    }
    // A no-op edit causes a noisy "buffer modified" hint in some
    // editors; skip publishing when the text didn't actually change.
    if (result.stdout === doc.getText()) return [];

    // Replace the entire document range with the formatter output.
    // LSP's `Range` end is exclusive; pointing at "one past the last
    // line, character 0" covers every byte regardless of trailing
    // newline policy.
    const lastLine = doc.lineCount;
    return [{
      range: {
        start: { line: 0, character: 0 },
        end:   { line: lastLine, character: 0 },
      },
      newText: result.stdout,
    }];
  } finally {
    try { fs.unlinkSync(tmp); } catch { /* fine if already gone */ }
  }
});

documents.listen(connection);
connection.listen();
