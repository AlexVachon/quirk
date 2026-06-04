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
  DefinitionParams,
  DocumentSymbol,
  DocumentSymbolParams,
  DocumentFormattingParams,
  Hover,
  HoverParams,
  Location,
  MarkupKind,
  ProposedFeatures,
  ReferenceParams,
  WorkspaceFolder,
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
let workspaceFolders: WorkspaceFolder[] = [];

connection.onInitialize((params: InitializeParams): InitializeResult => {
  // The client may pass `initializationOptions = { quirk: { executablePath: ... } }`
  // (Neovim / Helix LSP configs commonly do this) — pick it up if present.
  const opts = params.initializationOptions as { quirk?: { executablePath?: string } } | undefined;
  quirkBinary = resolveQuirkBin(opts?.quirk?.executablePath);

  // `workspaceFolders` drives the find-references walk: every .quirk
  // file under each folder gets scanned (with a few sensible
  // exclusions). Editors that don't advertise folders (rare) fall
  // back to `rootUri`; both deprecated and modern keys handled.
  if (params.workspaceFolders && params.workspaceFolders.length) {
    workspaceFolders = params.workspaceFolders;
  } else if (params.rootUri) {
    workspaceFolders = [{ uri: params.rootUri, name: '<root>' }];
  }

  connection.console.info(`quirk-lsp: using compiler at '${quirkBinary}'`);
  connection.console.info(`quirk-lsp: ${workspaceFolders.length} workspace folder(s)`);

  return {
    capabilities: {
      // Full document sync keeps the LSP simple — every change ships the
      // whole buffer. Incremental sync would be a measurable win only
      // for very large files (Quirk projects don't have those yet).
      textDocumentSync: TextDocumentSyncKind.Full,
      documentSymbolProvider: true,
      documentFormattingProvider: true,
      definitionProvider: true,
      referencesProvider: true,
      hoverProvider: true,
    },
    serverInfo: { name: 'quirk-lsp', version: '0.6.0' },
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

// ──────────────────────────────────────────────────────────────────────
// Go-to-definition (`textDocument/definition`)
// ──────────────────────────────────────────────────────────────────────
// Scope: same-file declarations only — define/struct/enum/interface
// at the top level, plus methods inside a struct. Local variables,
// struct fields, parameters, and cross-file imports are deferred to
// later 1.6.x releases (the latter needs a compiler-side resolver
// query so we don't duplicate the C++ logic in TypeScript).
//
// The cursor position is interpreted character-precisely: we grab the
// identifier the cursor is inside (or immediately to the right of)
// and walk the whole file for any declaration that names it. Multiple
// hits — e.g. two `define foo` for an overloaded interface — all
// return; LSP clients render that as a chooser.

const IDENT_RE = /[A-Za-z_][A-Za-z0-9_]*/g;

// Find the identifier word that contains (or starts at) `character` on
// `line`. Returns null when the cursor isn't on a word.
function identifierAt(line: string, character: number): string | null {
  IDENT_RE.lastIndex = 0;
  let match: RegExpExecArray | null;
  while ((match = IDENT_RE.exec(line)) !== null) {
    const start = match.index;
    const end   = start + match[0].length;
    if (character >= start && character <= end) return match[0];
    if (start > character) return null;
  }
  return null;
}

// All same-file declarations the LSP knows how to navigate to. Anchored
// to start-of-line (after optional whitespace) so the regex doesn't
// pick up the *uses* of the identifier elsewhere in the file. The
// `define` form is used both for top-level functions and for struct
// methods — both are valid jump targets, so we accept either.
const DECL_PATTERNS: { re: RegExp; nameGroup: number }[] = [
  { re: /^[ \t]*define\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(/,         nameGroup: 1 },
  { re: /^[ \t]*struct\s+([A-Za-z_][A-Za-z0-9_]*)\b/,            nameGroup: 1 },
  { re: /^[ \t]*enum\s+([A-Za-z_][A-Za-z0-9_]*)\b/,              nameGroup: 1 },
  { re: /^[ \t]*interface\s+([A-Za-z_][A-Za-z0-9_]*)\b/,         nameGroup: 1 },
];

function findDeclarations(doc: TextDocument, name: string): Location[] {
  const lines = doc.getText().split(/\r?\n/);
  const out: Location[] = [];
  for (let i = 0; i < lines.length; i++) {
    for (const { re, nameGroup } of DECL_PATTERNS) {
      const m = lines[i].match(re);
      if (m && m[nameGroup] === name) {
        const startCol = lines[i].indexOf(name, m.index ?? 0);
        out.push({
          uri: doc.uri,
          range: {
            start: { line: i, character: startCol },
            end:   { line: i, character: startCol + name.length },
          },
        });
        break;       // one decl per line; don't double-count
      }
    }
  }
  return out;
}

// Cross-file lookup helpers. The LSP doesn't try to reimplement the
// Quirk resolver in TypeScript — it shells out to `quirk resolve <name>`
// instead, so we stay byte-identical with `use foo` semantics. A miss
// just returns the empty list; the editor will fall back to its own
// fuzzy navigation (workspace symbol search, etc).

// Pull the module name out of `use X` or `from X use { ... }`. Returns
// null when the line isn't an import. Module names are dotted, so we
// match `[A-Za-z_][A-Za-z0-9_.]*`.
const USE_RE  = /^[ \t]*use\s+([A-Za-z_][A-Za-z0-9_.]*)/;
const FROM_RE = /^[ \t]*from\s+([A-Za-z_][A-Za-z0-9_.]*)\s+use\b/;

interface ImportLine {
  module: string;
  imported: Set<string>;   // empty for plain `use X`
}

// Build a name → module map by scanning the file's import block. The
// scanner accepts both single-line (`from foo use { A, B }`) and
// multi-line (`from foo use {\n    A,\n    B,\n}`) forms; the typing
// module heavily uses the multi-line one.
function scanImports(text: string): { byName: Map<string, string>; modules: Set<string> } {
  const byName  = new Map<string, string>();
  const modules = new Set<string>();
  const lines = text.split(/\r?\n/);

  let i = 0;
  while (i < lines.length) {
    const line = lines[i];
    const fromMatch = line.match(FROM_RE);
    if (fromMatch) {
      const mod = fromMatch[1];
      modules.add(mod);
      // Collect everything between the first `{` (anywhere in the
      // current line) and the matching `}` (possibly on a later line).
      let body = '';
      const openIdx = line.indexOf('{');
      if (openIdx >= 0) body += line.slice(openIdx + 1);
      let j = i;
      while (body.indexOf('}') === -1 && j + 1 < lines.length) {
        j++;
        body += '\n' + lines[j];
      }
      const close = body.indexOf('}');
      if (close >= 0) body = body.slice(0, close);
      for (const tok of body.split(/[,\s]+/)) {
        const t = tok.trim();
        if (!t) continue;
        // Strip `as alias` if present — we record the local alias
        // name, since that's what the user types at the call site.
        const asMatch = t.match(/^([A-Za-z_][A-Za-z0-9_]*)(?:\s+as\s+([A-Za-z_][A-Za-z0-9_]*))?$/);
        if (asMatch) byName.set(asMatch[2] ?? asMatch[1], mod);
      }
      i = j + 1;
      continue;
    }
    const useMatch = line.match(USE_RE);
    if (useMatch) {
      modules.add(useMatch[1]);
      // Bare `use X` also exposes the module name itself for member
      // access (`X.foo`). The bare name maps to itself for jump.
      // Multi-dot names like `net.http` create a separate entry per
      // segment chain so `use net.http` is jumpable as either
      // `net` (unlikely) or the full `net.http`.
      const parts = useMatch[1].split('.');
      byName.set(parts[parts.length - 1], useMatch[1]);
    }
    i++;
  }
  return { byName, modules };
}

// Cache resolved module paths so repeat lookups in the same session
// don't repeatedly spawn the compiler. Cleared on shutdown via process
// exit — no LRU eviction needed (compile-time module count is small).
const resolveCache = new Map<string, string | null>();

function resolveModule(moduleName: string): string | null {
  if (resolveCache.has(moduleName)) return resolveCache.get(moduleName) ?? null;
  const r = spawnSync(quirkBinary, ['resolve', moduleName], {
    encoding: 'utf8',
    timeout: 5_000,
  });
  let result: string | null = null;
  if (r.status === 0 && r.stdout) {
    result = r.stdout.trim();
    if (!result) result = null;
  }
  resolveCache.set(moduleName, result);
  return result;
}

// Find declarations matching `name` in the file at `absPath`. Reads the
// file synchronously off disk so we don't have to keep the imported
// docs in `documents`. Returns [] on read failure (file moved/deleted
// since the import was written).
function findDeclarationsInFile(absPath: string, name: string): Location[] {
  let text: string;
  try { text = fs.readFileSync(absPath, 'utf8'); }
  catch { return []; }
  const lines = text.split(/\r?\n/);
  const uri = new URL('file://' + path.resolve(absPath)).toString();
  const out: Location[] = [];
  for (let i = 0; i < lines.length; i++) {
    for (const { re, nameGroup } of DECL_PATTERNS) {
      const m = lines[i].match(re);
      if (m && m[nameGroup] === name) {
        const startCol = lines[i].indexOf(name, m.index ?? 0);
        out.push({
          uri,
          range: {
            start: { line: i, character: startCol },
            end:   { line: i, character: startCol + name.length },
          },
        });
        break;
      }
    }
  }
  return out;
}

connection.onDefinition((params: DefinitionParams): Location[] => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return [];
  const text = doc.getText();
  const lineText = text.split(/\r?\n/)[params.position.line] ?? '';
  const ident = identifierAt(lineText, params.position.character);
  if (!ident) return [];

  // 1. Same-file declarations win. Local edits should be visible
  // immediately, regardless of what the compiler's resolver thinks.
  const same = findDeclarations(doc, ident);
  if (same.length) return same;

  // 2. Cross-file: walk the file's imports. The cursor identifier
  // could be either the module itself or a name imported from one.
  const { byName, modules } = scanImports(text);
  const moduleHit = byName.get(ident) ?? (modules.has(ident) ? ident : null);
  if (!moduleHit) return [];

  const resolved = resolveModule(moduleHit);
  if (!resolved) return [];

  // If we landed because the cursor was the module name itself, jump
  // to the top of the resolved file. Otherwise look for a declaration
  // matching `ident` *inside* the resolved module.
  if (ident === moduleHit.split('.').pop()) {
    return [{
      uri: new URL('file://' + path.resolve(resolved)).toString(),
      range: { start: { line: 0, character: 0 }, end: { line: 0, character: 0 } },
    }];
  }
  return findDeclarationsInFile(resolved, ident);
});

// ──────────────────────────────────────────────────────────────────────
// Find references (`textDocument/references`)
// ──────────────────────────────────────────────────────────────────────
// Word-boundary text search across every `.quirk` file under the
// workspace folders. Coarse on purpose — finds every textual match
// regardless of scope, including same-name parameters in unrelated
// functions. A real semantic search needs Sema's symbol table; until
// the compiler exposes that, this is the best we can do without
// duplicating Sema in TypeScript.
//
// Excludes:
//   - `packages/` (third-party + bundled stdlib copies)
//   - `.venv/`, `.git/`, `node_modules/`, `build/`, `out/`, `obj/`
//   - Files larger than 1 MiB (defends against accidental binary blobs
//     with .quirk extensions — vanishingly rare but the cap is cheap)
// Walks at most `MAX_FILES` matching files; if the workspace is
// larger, the first N win.

const SKIP_DIRS = new Set(['packages', '.venv', '.git', 'node_modules', 'build', 'out', 'obj', 'target', '.cache']);
const MAX_FILES = 5000;
const MAX_FILE_BYTES = 1 << 20;     // 1 MiB

function uriToPath(uri: string): string | null {
  try { return fileURLToPath(uri); } catch { return null; }
}

function pathToUri(p: string): string {
  return new URL('file://' + path.resolve(p)).toString();
}

function* walkQuirkFiles(root: string): Generator<string> {
  const stack: string[] = [root];
  let visited = 0;
  while (stack.length) {
    const dir = stack.pop()!;
    let entries: fs.Dirent[];
    try { entries = fs.readdirSync(dir, { withFileTypes: true }); }
    catch { continue; }
    for (const e of entries) {
      const full = path.join(dir, e.name);
      if (e.isDirectory()) {
        if (SKIP_DIRS.has(e.name)) continue;
        stack.push(full);
      } else if (e.isFile() && e.name.endsWith('.quirk')) {
        yield full;
        if (++visited >= MAX_FILES) return;
      }
    }
  }
}

// Word-boundary scan: an identifier hit needs non-`[A-Za-z0-9_]`
// flanking characters (or buffer boundary). We do the regex
// per-line so the Location ranges are easy to compute.
function findOccurrencesInText(text: string, name: string, uri: string): Location[] {
  const re = new RegExp('(?<![A-Za-z0-9_])' + name.replace(/[.*+?^${}()|[\]\\]/g, '\\$&') + '(?![A-Za-z0-9_])', 'g');
  const lines = text.split(/\r?\n/);
  const out: Location[] = [];
  for (let i = 0; i < lines.length; i++) {
    re.lastIndex = 0;
    let m: RegExpExecArray | null;
    while ((m = re.exec(lines[i])) !== null) {
      const start = m.index;
      out.push({
        uri,
        range: {
          start: { line: i, character: start },
          end:   { line: i, character: start + name.length },
        },
      });
    }
  }
  return out;
}

connection.onReferences((params: ReferenceParams): Location[] => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return [];
  const docLines = doc.getText().split(/\r?\n/);
  const lineText = docLines[params.position.line] ?? '';
  const ident = identifierAt(lineText, params.position.character);
  if (!ident) return [];

  const out: Location[] = [];

  // Scan every open document first so unsaved edits show up. Then walk
  // the workspace folders on disk for files not currently open.
  const seenUris = new Set<string>();
  for (const open of documents.all()) {
    seenUris.add(open.uri);
    out.push(...findOccurrencesInText(open.getText(), ident, open.uri));
  }
  for (const folder of workspaceFolders) {
    const root = uriToPath(folder.uri);
    if (!root) continue;
    for (const file of walkQuirkFiles(root)) {
      const uri = pathToUri(file);
      if (seenUris.has(uri)) continue;
      let buf: Buffer;
      try { buf = fs.readFileSync(file); } catch { continue; }
      if (buf.length > MAX_FILE_BYTES) continue;
      out.push(...findOccurrencesInText(buf.toString('utf8'), ident, uri));
    }
  }
  // `includeDeclaration: false` is a hint that the caller (find-refs
  // panel) doesn't want the declaration site in the list. Most LSP
  // clients flip this on for `Find Usages` and off for hover-style
  // peek; we honor it without trying to distinguish *which* hit is
  // the declaration, by stripping any hit that's adjacent to the
  // canonical decl keywords.
  if (params.context && !params.context.includeDeclaration) {
    return out.filter((loc) => {
      const lines = (loc.uri === doc.uri ? docLines :
        (() => { const p = uriToPath(loc.uri); if (!p) return null;
                 try { return fs.readFileSync(p, 'utf8').split(/\r?\n/); } catch { return null; } })());
      if (!lines) return true;
      const line = lines[loc.range.start.line] ?? '';
      // Strip if the line is a declaration of this identifier.
      for (const { re, nameGroup } of DECL_PATTERNS) {
        const m = line.match(re);
        if (m && m[nameGroup] === ident) return false;
      }
      return true;
    });
  }
  return out;
});

// ──────────────────────────────────────────────────────────────────────
// Hover (`textDocument/hover`)
// ──────────────────────────────────────────────────────────────────────
// Show the declaration's signature line + the preceding `---` docstring
// block (Quirk's doc-comment convention) as a single markdown payload.
// Reuses the same lookup chain as go-to-definition: same-file decls
// first, then cross-file via `quirk resolve`.

// Pull the `---`-fenced block ending at the line ABOVE `declLine`, if
// one exists. Returns an empty string when there's no docstring. The
// scan walks backward over consecutive non-blank lines that *don't*
// belong to another declaration, stopping at the opening `---`.
function extractDocstring(lines: string[], declLine: number): string {
  if (declLine <= 0) return '';
  // Skip blank lines between the docstring close and the declaration.
  let i = declLine - 1;
  while (i >= 0 && lines[i].trim() === '') i--;
  if (i < 0) return '';

  // Two doc-comment styles are recognized:
  //   1. `---` block (one line) or block fence (`---\n…\n---`)
  //   2. consecutive `//` line comments
  if (lines[i].trim() === '---') {
    // Walk up to the matching opening fence.
    const end = i - 1;
    let start = end;
    while (start >= 0 && lines[start].trim() !== '---') start--;
    if (start < 0) return '';
    return lines.slice(start + 1, end + 1).join('\n');
  }
  if (lines[i].trimStart().startsWith('//')) {
    let start = i;
    while (start - 1 >= 0 && lines[start - 1].trimStart().startsWith('//')) start--;
    return lines.slice(start, i + 1)
      .map((l) => l.replace(/^\s*\/\/\s?/, ''))
      .join('\n');
  }
  return '';
}

// Build the hover payload for a declaration at `lines[declLine]`. Wraps
// the signature line in a `quirk` code fence so editors with syntax
// highlighting present it correctly.
function hoverFromDecl(lines: string[], declLine: number): string {
  const sig = (lines[declLine] ?? '').replace(/^\s+/, '').replace(/\s*\{\s*$/, '').trimEnd();
  const doc = extractDocstring(lines, declLine);
  let body = '```quirk\n' + sig + '\n```';
  if (doc) body += '\n\n' + doc;
  return body;
}

connection.onHover((params: HoverParams): Hover | null => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return null;
  const text = doc.getText();
  const docLines = text.split(/\r?\n/);
  const lineText = docLines[params.position.line] ?? '';
  const ident = identifierAt(lineText, params.position.character);
  if (!ident) return null;

  // 1. Same-file decl.
  for (let i = 0; i < docLines.length; i++) {
    for (const { re, nameGroup } of DECL_PATTERNS) {
      const m = docLines[i].match(re);
      if (m && m[nameGroup] === ident) {
        return {
          contents: { kind: MarkupKind.Markdown, value: hoverFromDecl(docLines, i) },
        };
      }
    }
  }

  // 2. Cross-file via import map.
  const { byName, modules } = scanImports(text);
  const moduleHit = byName.get(ident) ?? (modules.has(ident) ? ident : null);
  if (!moduleHit) return null;
  const resolved = resolveModule(moduleHit);
  if (!resolved) return null;
  let imported: string;
  try { imported = fs.readFileSync(resolved, 'utf8'); } catch { return null; }
  const importedLines = imported.split(/\r?\n/);
  for (let i = 0; i < importedLines.length; i++) {
    for (const { re, nameGroup } of DECL_PATTERNS) {
      const m = importedLines[i].match(re);
      if (m && m[nameGroup] === ident) {
        const body = hoverFromDecl(importedLines, i);
        // Suffix the source path so the user knows which file the
        // signature was lifted from (handy when several stdlib
        // modules export same-named types).
        const trailer = `\n\n*from \`${path.basename(resolved)}\`*`;
        return { contents: { kind: MarkupKind.Markdown, value: body + trailer } };
      }
    }
  }
  return null;
});

documents.listen(connection);
connection.listen();
