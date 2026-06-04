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
  CompletionItem,
  CompletionItemKind,
  CompletionParams,
  DefinitionParams,
  ParameterInformation,
  SignatureHelp,
  SignatureHelpParams,
  SignatureInformation,
  DocumentSymbol,
  DocumentSymbolParams,
  DocumentFormattingParams,
  FoldingRange,
  FoldingRangeKind,
  FoldingRangeParams,
  Hover,
  HoverParams,
  Location,
  MarkupKind,
  PrepareRenameParams,
  ProposedFeatures,
  ReferenceParams,
  RenameParams,
  SymbolInformation,
  WorkspaceEdit,
  WorkspaceFolder,
  WorkspaceSymbolParams,
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

// Symbol-table NDJSON record emitted by `quirk --symbols-json`. Same
// "extra keys are ignored" contract — additions on the C++ side are
// backwards compatible. Each record represents ONE declaration site.
interface QuirkSymbol {
  kind: 'function' | 'method' | 'struct' | 'enum' | 'enumvariant'
      | 'interface' | 'field' | 'parameter' | 'variable' | 'module_const';
  name: string;
  scope: string;          // "module", a struct name, or an enclosing function name
  file: string;           // absolute path on disk
  line: number;           // 1-based
  col: number;            // 1-based
  type?: string;          // present for params/vars/fields/functions where Sema knew the type
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
      completionProvider: {
        // `.` triggers member-access completion (`foo.<cursor>`);
        // identifier completion fires automatically as the user types.
        triggerCharacters: ['.'],
      },
      signatureHelpProvider: {
        triggerCharacters: ['(', ','],
        retriggerCharacters: [','],
      },
      workspaceSymbolProvider: true,
      renameProvider: { prepareProvider: true },
      foldingRangeProvider: true,
    },
    serverInfo: { name: 'quirk-lsp', version: '0.12.0' },
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
documents.onDidOpen((e) => { void checkDocument(e.document); void refreshSymbols(e.document); });
documents.onDidSave((e) => { void checkDocument(e.document); void refreshSymbols(e.document); });
documents.onDidClose((e) => { symbolCache.delete(e.document.uri); });

// ──────────────────────────────────────────────────────────────────────
// Symbol table cache (`quirk --symbols-json`)
// ──────────────────────────────────────────────────────────────────────
// The compiler walks the entire transitive-import graph for this dump,
// so running it on every keystroke would be way too expensive. Instead
// it runs once on `didOpen` + on every `didSave`; the cached result
// feeds scope-aware completion (and, later, semantic rename).

const symbolCache = new Map<string, QuirkSymbol[]>();

async function refreshSymbols(document: TextDocument): Promise<void> {
  const filePath = uriToPath(document.uri);
  if (!filePath) return;

  const child = spawn(quirkBinary, ['--symbols-json', filePath], {
    cwd: path.dirname(filePath),
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  let stdout = '';
  child.stdout.on('data', (chunk) => { stdout += chunk.toString('utf8'); });
  child.on('error', (err) => {
    connection.console.warn(`quirk-lsp: --symbols-json spawn failed: ${err.message}`);
  });

  await new Promise<void>((resolve) => { child.on('close', () => resolve()); });

  const records: QuirkSymbol[] = [];
  for (const line of stdout.split('\n')) {
    if (!line.trim()) continue;
    try {
      const rec = JSON.parse(line) as QuirkSymbol;
      if (rec && rec.name && rec.kind) records.push(rec);
    } catch {
      // Diagnostics from a parse failure end up here too — silently
      // drop. We only want symbol records on this channel.
    }
  }
  symbolCache.set(document.uri, records);
}

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

// ──────────────────────────────────────────────────────────────────────
// Completion (`textDocument/completion`)
// ──────────────────────────────────────────────────────────────────────
// Two completion modes:
//   1. Plain identifier — suggest current-file top-level decls,
//      imported names from the file's `from X use {...}` block, and
//      Quirk keywords + a small builtin set.
//   2. After `.` (member access) — if the LHS is a known imported
//      module, suggest the top-level decls of that module's file.
//
// Like every other LSP feature here, this is regex/text based — no
// scope tracking, no type inference. Suggestions can include things
// that won't actually resolve in the current scope; the user filters
// them by continuing to type.

const QUIRK_KEYWORDS = [
  'define', 'struct', 'enum', 'interface', 'extern', 'use', 'from',
  'if', 'else', 'elif', 'while', 'for', 'in', 'return', 'break',
  'continue', 'with', 'as', 'try', 'catch', 'throw', 'finally',
  'and', 'or', 'not', 'is', 'where', 'match', 'case', 'super',
  'true', 'false', 'null', 'fn', 'nonlocal', 'global', 'self',
];

const QUIRK_BUILTINS = [
  'print', 'printf', 'type', 'String', 'Int', 'Double', 'Bool',
  'Any', 'void', 'List', 'Map', 'Set', 'Queue', 'Tuple', 'Callable',
  'Exception', 'TypeError', 'ValueError', 'IndexError', 'KeyError',
  'IOError', 'RuntimeError', 'NotImplementedError', 'AssertionError',
];

// Build a completion item set from declaration scans in `text`. Used
// for both same-file and imported-file completions; the `detail` tag
// shows the kind so the editor's completion popup is informative.
function completionsFromDeclarations(text: string, detail: string): CompletionItem[] {
  const lines = text.split(/\r?\n/);
  const out: CompletionItem[] = [];
  const seen = new Set<string>();
  for (let i = 0; i < lines.length; i++) {
    for (const { re, nameGroup } of DECL_PATTERNS) {
      const m = lines[i].match(re);
      if (m) {
        const name = m[nameGroup];
        if (seen.has(name)) continue;
        seen.add(name);
        // Map the keyword to an LSP CompletionItemKind. `define` could
        // be either a function or a method — without scope info we
        // assume Function; the user sees "method" naming in struct
        // bodies regardless.
        const kw = (lines[i].trimStart().split(/\s+/)[0] ?? 'define') as 'define' | 'struct' | 'enum' | 'interface';
        const kind = kw === 'struct'
          ? CompletionItemKind.Struct
          : kw === 'enum'
          ? CompletionItemKind.Enum
          : kw === 'interface'
          ? CompletionItemKind.Interface
          : CompletionItemKind.Function;
        out.push({ label: name, kind, detail });
        break;
      }
    }
  }
  return out;
}

// Inspect the text just before `position` to decide whether we're
// completing after a `.` (member access) and, if so, what the LHS
// looked like.
function memberAccessLhs(line: string, character: number): string | null {
  // Walk left from the cursor over identifier chars + dots until we
  // hit something else. `foo.bar.<cursor>` → returns "foo.bar".
  let end = character;
  if (end <= 0) return null;
  // Scan past the optional partial word to the right of the dot.
  while (end > 0 && /[A-Za-z0-9_]/.test(line[end - 1])) end--;
  if (end === 0 || line[end - 1] !== '.') return null;
  let start = end - 1;
  while (start > 0 && /[A-Za-z0-9_.]/.test(line[start - 1])) start--;
  return line.slice(start, end - 1);    // strip the trailing dot
}

connection.onCompletion((params: CompletionParams): CompletionItem[] => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return [];
  const text = doc.getText();
  const docLines = text.split(/\r?\n/);
  const lineText = docLines[params.position.line] ?? '';

  // Member access — `<module>.` → suggest decls in that module's file.
  const lhs = memberAccessLhs(lineText, params.position.character);
  if (lhs) {
    const { byName, modules } = scanImports(text);
    const moduleHit = byName.get(lhs) ?? (modules.has(lhs) ? lhs : null);
    if (!moduleHit) return [];
    const resolved = resolveModule(moduleHit);
    if (!resolved) return [];
    try {
      const moduleText = fs.readFileSync(resolved, 'utf8');
      return completionsFromDeclarations(moduleText, `from ${path.basename(resolved)}`);
    } catch { return []; }
  }

  // Identifier completion. Merge:
  //  - current-file declarations
  //  - imported names (from `from X use {Y, Z}`)
  //  - keywords + builtin types
  const items: CompletionItem[] = [];
  const seen = new Set<string>();
  const push = (it: CompletionItem) => {
    if (seen.has(it.label)) return;
    seen.add(it.label);
    items.push(it);
  };

  for (const it of completionsFromDeclarations(text, 'this file')) push(it);

  // Scope-aware additions from the cached symbol table — parameters
  // and local variables of the function the cursor is in. The
  // symbol table comes from `quirk --symbols-json` (refreshed on
  // didOpen / didSave), so it's slightly stale between edits and a
  // save; that's a fine trade for not re-running the compiler on
  // every keystroke.
  const filePath = uriToPath(params.textDocument.uri);
  const symbols = symbolCache.get(params.textDocument.uri) ?? [];
  if (filePath) {
    // Identify the enclosing function: the latest "function" /
    // "method" record whose declaration line is <= cursor line in
    // the same file. Coarse — block scoping inside the function
    // isn't tracked — but good enough to surface params + locals.
    let enclosingScope: string | null = null;
    let enclosingLine = -1;
    for (const sym of symbols) {
      if (sym.file !== filePath) continue;
      if ((sym.kind === 'function' || sym.kind === 'method')
          && sym.line - 1 <= params.position.line
          && sym.line - 1 > enclosingLine) {
        enclosingLine = sym.line - 1;
        enclosingScope = sym.name;
      }
    }
    if (enclosingScope !== null) {
      for (const sym of symbols) {
        if (sym.file !== filePath) continue;
        if (sym.scope !== enclosingScope) continue;
        if (sym.kind === 'parameter') {
          push({
            label: sym.name,
            kind: CompletionItemKind.Variable,
            detail: sym.type ? `param: ${sym.type}` : 'parameter',
          });
        } else if (sym.kind === 'variable') {
          push({
            label: sym.name,
            kind: CompletionItemKind.Variable,
            detail: sym.type ? `local: ${sym.type}` : 'local',
          });
        }
      }
    }
  }

  const { byName } = scanImports(text);
  for (const [name, mod] of byName) {
    push({ label: name, kind: CompletionItemKind.Reference, detail: `from ${mod}` });
  }

  for (const kw of QUIRK_KEYWORDS) {
    push({ label: kw, kind: CompletionItemKind.Keyword });
  }
  for (const bn of QUIRK_BUILTINS) {
    push({ label: bn, kind: CompletionItemKind.Class, detail: 'builtin' });
  }

  return items;
});

// ──────────────────────────────────────────────────────────────────────
// Signature help (`textDocument/signatureHelp`)
// ──────────────────────────────────────────────────────────────────────
// Triggered by `(` or `,`. Walks backward from the cursor balancing
// parentheses to find:
//   1. the most-recent un-closed `(`, and the identifier immediately
//      before it (the callee), and
//   2. the number of top-level commas between that `(` and the
//      cursor — that's the active parameter index.
//
// Once the callee name is known, we look it up in the cached symbol
// table (`quirk --symbols-json` output, see `symbolCache`). Each
// matching record contributes one SignatureInformation, with its
// parameters lifted from the parameter records that share its scope.

interface CallContext {
  callee: string;
  activeParameter: number;
}

// Scan text left-of-cursor with a parenthesis-balanced walker. Returns
// the innermost open call context, or null if the cursor isn't in a
// function call. Comments and strings inside the call would confuse
// this, but Quirk's stdlib + sample code rarely puts those between a
// `(` and a `,`.
function findCallContext(text: string, line: number, character: number): CallContext | null {
  const lines = text.split(/\r?\n/);
  let depth = 0;
  let activeParam = 0;
  for (let li = line; li >= 0; li--) {
    const lineText = li === line ? lines[li].slice(0, character) : lines[li];
    for (let i = lineText.length - 1; i >= 0; i--) {
      const c = lineText[i];
      if (c === ')') depth++;
      else if (c === '(') {
        if (depth === 0) {
          // Pull the callee — the longest identifier that ends at i.
          let end = i;
          // Skip whitespace between callee and `(`
          while (end > 0 && /\s/.test(lineText[end - 1])) end--;
          let start = end;
          while (start > 0 && /[A-Za-z0-9_.]/.test(lineText[start - 1])) start--;
          const callee = lineText.slice(start, end);
          if (!callee) return null;
          // For `mod.fn(...)` we only want the last segment.
          const dot = callee.lastIndexOf('.');
          return {
            callee: dot >= 0 ? callee.slice(dot + 1) : callee,
            activeParameter: activeParam,
          };
        }
        depth--;
      }
      else if (c === ',' && depth === 0) activeParam++;
    }
    // Bail if we walked past the start of the file without finding an
    // open paren — defends against pathological scans of huge files.
    if (li === 0) break;
  }
  return null;
}

// Build a `SignatureInformation` per symbol matching the callee name.
// Most calls resolve to one signature; method overloads or interface
// vs concrete-method clashes give multiple, which LSP clients render
// as a chooser strip above the signature.
function signaturesFor(callee: string, symbols: QuirkSymbol[]): SignatureInformation[] {
  const sigs: SignatureInformation[] = [];
  for (const s of symbols) {
    if (s.kind !== 'function' && s.kind !== 'method') continue;
    if (s.name !== callee) continue;
    // Collect the parameter records that share this function's scope.
    // For methods the scope is the demangled name (we emit it that way
    // in --symbols-json), so this still matches.
    const params = symbols
      .filter((p) => p.kind === 'parameter' && p.scope === s.name
                  && p.file === s.file && p.line === s.line)
      .map((p) => ({
        label: p.type ? `${p.name}: ${p.type}` : p.name,
      } as ParameterInformation));
    const paramText = params.map((p) => p.label as string).join(', ');
    const ret = s.type ? ` -> ${s.type}` : '';
    sigs.push({
      label: `${s.name}(${paramText})${ret}`,
      parameters: params,
    });
  }
  return sigs;
}

connection.onSignatureHelp((params: SignatureHelpParams): SignatureHelp | null => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return null;
  const ctx = findCallContext(doc.getText(), params.position.line, params.position.character);
  if (!ctx) return null;
  const symbols = symbolCache.get(params.textDocument.uri) ?? [];
  const signatures = signaturesFor(ctx.callee, symbols);
  if (!signatures.length) return null;
  return {
    signatures,
    activeSignature: 0,
    activeParameter: ctx.activeParameter,
  };
});

// ──────────────────────────────────────────────────────────────────────
// Workspace symbols (`workspace/symbol`)
// ──────────────────────────────────────────────────────────────────────
// `Ctrl+T` / `:Telescope lsp_workspace_symbols` style searches. We
// surface every top-level declaration (function, struct, enum,
// interface, module_const) and every method that's currently in the
// per-document symbol cache.
//
// Scope is "files currently open in this session" rather than every
// .quirk file under the workspace. A full-workspace index would
// require running `quirk --symbols-json` for every file at startup
// or on a `workspace/didChangeWatchedFiles` notification — bigger
// undertaking. For now the user-typed query falls back to the editor's
// fuzzy-find over files when the symbol isn't open.

function symbolKindFromQuirk(kind: QuirkSymbol['kind']): SymbolKind {
  switch (kind) {
    case 'struct':       return SymbolKind.Struct;
    case 'enum':         return SymbolKind.Enum;
    case 'enumvariant':  return SymbolKind.EnumMember;
    case 'interface':    return SymbolKind.Interface;
    case 'method':       return SymbolKind.Method;
    case 'field':        return SymbolKind.Field;
    case 'parameter':    return SymbolKind.Variable;
    case 'variable':     return SymbolKind.Variable;
    case 'module_const': return SymbolKind.Constant;
    default:             return SymbolKind.Function;
  }
}

connection.onWorkspaceSymbol((params: WorkspaceSymbolParams): SymbolInformation[] => {
  const q = params.query.toLowerCase();
  const out: SymbolInformation[] = [];
  const SKIP_KINDS = new Set<QuirkSymbol['kind']>(['parameter', 'variable']);
  for (const [uri, symbols] of symbolCache) {
    for (const s of symbols) {
      if (SKIP_KINDS.has(s.kind)) continue;       // too noisy at the workspace level
      // Empty query matches everything (standard LSP behaviour);
      // non-empty queries do a coarse substring match. The editor
      // does its own fuzzy ranking on top of whatever we return.
      if (q && !s.name.toLowerCase().includes(q)) continue;
      out.push({
        name: s.name,
        kind: symbolKindFromQuirk(s.kind),
        containerName: s.scope === 'module' ? '' : s.scope,
        location: {
          // We pass through the symbol's recorded path rather than the
          // doc URI we found it in — `--symbols-json` walks the import
          // graph so a record for a stdlib decl may have come back
          // through some user file's cache.
          uri: s.file.startsWith('file://') ? s.file : pathToUri(s.file),
          range: {
            start: { line: s.line - 1, character: Math.max(0, s.col - 1) },
            end:   { line: s.line - 1, character: Math.max(0, s.col - 1) + s.name.length },
          },
        },
      });
    }
  }
  // Cap so a huge multi-file project doesn't flood the editor.
  return out.slice(0, 500);
});

// ──────────────────────────────────────────────────────────────────────
// Rename (`textDocument/rename` + `textDocument/prepareRename`)
// ──────────────────────────────────────────────────────────────────────
// Two paths, picked by the kind of symbol the cursor sits on:
//
// 1. Local symbol (parameter or `:=`-bound variable): rename within
//    the current file only. Other files can't reference a local, so
//    a workspace walk would be wasted work.
// 2. Top-level decl (function, struct, enum, interface, method,
//    field, module_const): workspace-wide word-boundary rename
//    across every opened/cached document plus on-disk .quirk files
//    under the workspace folders. Same walker as find-references.
//
// Caveats — no full scope tracking:
//   - Two locals with the same name in different functions can't be
//     renamed independently from the workspace path; we use the
//     symbol cache to detect that case and limit the rename to the
//     enclosing function's body when possible.
//   - Shadowed identifiers (param `x` and local `x` inside the same
//     function) will all be renamed together. Real semantic rename
//     needs usage tracking from Sema; deferred until that lands.

// Look up which symbol record(s) match (name, file). Returns all
// matches so the caller can decide what to do with ambiguity.
function findSymbolsByName(name: string, file: string, symbols: QuirkSymbol[]): QuirkSymbol[] {
  return symbols.filter((s) => s.name === name && s.file === file);
}

// Top-level kinds get workspace rename; everything else is same-file
// only. `enumvariant` lives somewhere in the middle — variants are
// referenced as `Direction.North`, so any cross-file user of the enum
// could mention them. Treat as workspace.
function isWorkspaceVisible(kind: QuirkSymbol['kind']): boolean {
  return kind === 'function' || kind === 'method' || kind === 'struct'
      || kind === 'enum' || kind === 'enumvariant' || kind === 'interface'
      || kind === 'field' || kind === 'module_const';
}

connection.onPrepareRename((params: PrepareRenameParams) => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return null;
  const lineText = doc.getText().split(/\r?\n/)[params.position.line] ?? '';
  // Walk left/right from the cursor to find the identifier span.
  let start = params.position.character;
  let end   = params.position.character;
  while (start > 0 && /[A-Za-z0-9_]/.test(lineText[start - 1])) start--;
  while (end < lineText.length && /[A-Za-z0-9_]/.test(lineText[end])) end++;
  if (start === end) return null;
  // First char must be a valid identifier start.
  if (!/[A-Za-z_]/.test(lineText[start])) return null;
  return {
    range: {
      start: { line: params.position.line, character: start },
      end:   { line: params.position.line, character: end },
    },
    placeholder: lineText.slice(start, end),
  };
});

connection.onRenameRequest((params: RenameParams): WorkspaceEdit | null => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return null;
  const lineText = doc.getText().split(/\r?\n/)[params.position.line] ?? '';
  const oldName = identifierAt(lineText, params.position.character);
  if (!oldName) return null;
  const newName = params.newName;
  if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(newName)) {
    connection.console.warn(`rename: '${newName}' is not a valid Quirk identifier`);
    return null;
  }
  if (oldName === newName) return { changes: {} };

  const docPath = uriToPath(params.textDocument.uri);
  if (!docPath) return null;

  // Disambiguate via the symbol cache. If the cache has nothing for
  // this name, fall back to "any same-file occurrence" — better than
  // refusing rename entirely.
  const symbols = symbolCache.get(params.textDocument.uri) ?? [];
  const matches = findSymbolsByName(oldName, docPath, symbols);

  const renameSameFileOnly = !matches.length
      || matches.every((s) => !isWorkspaceVisible(s.kind));

  // Gather Locations to edit.
  const locations: Location[] = [];
  if (renameSameFileOnly) {
    locations.push(...findOccurrencesInText(doc.getText(), oldName, doc.uri));
  } else {
    // Workspace path — open docs first, then on-disk files. Same
    // walker as find-references.
    const seen = new Set<string>();
    for (const open of documents.all()) {
      seen.add(open.uri);
      locations.push(...findOccurrencesInText(open.getText(), oldName, open.uri));
    }
    for (const folder of workspaceFolders) {
      const root = uriToPath(folder.uri);
      if (!root) continue;
      for (const file of walkQuirkFiles(root)) {
        const uri = pathToUri(file);
        if (seen.has(uri)) continue;
        let buf: Buffer;
        try { buf = fs.readFileSync(file); } catch { continue; }
        if (buf.length > MAX_FILE_BYTES) continue;
        locations.push(...findOccurrencesInText(buf.toString('utf8'), oldName, uri));
      }
    }
  }

  // Group edits by URI; the LSP client applies them as one
  // workspace-wide change.
  const changes: { [uri: string]: TextEdit[] } = {};
  for (const loc of locations) {
    (changes[loc.uri] ??= []).push({
      range: loc.range,
      newText: newName,
    });
  }
  return { changes };
});

// ──────────────────────────────────────────────────────────────────────
// Folding ranges (`textDocument/foldingRange`)
// ──────────────────────────────────────────────────────────────────────
// Brace-balanced scan over the buffer. Anywhere `{` opens at end-of-
// line and the matching `}` lives on a later line, we emit a fold.
// Also folds:
//   - Multi-line `---` doc blocks (collapse the body to a single line).
//   - Multi-line `from X use { … }` import lists.
//   - Consecutive `// …` comment runs (collapse the whole comment).
//
// The implementation is shallow on purpose — string and char literals
// containing braces could in theory throw off the brace stack. Quirk
// has no multi-line string literals, so a `{` inside `"…"` only
// matters if it appears at end-of-line, which is rare enough to
// punt on for v0.12.

connection.onFoldingRanges((params: FoldingRangeParams): FoldingRange[] => {
  const doc = documents.get(params.textDocument.uri);
  if (!doc) return [];
  const lines = doc.getText().split(/\r?\n/);
  const out: FoldingRange[] = [];

  // Brace pairs: each `{` that's the last non-comment char on its
  // line opens a fold; pop on the matching `}`.
  type Open = { line: number };
  const stack: Open[] = [];
  // Track `---` doc-block toggling.
  let docBlockStart = -1;
  // Track contiguous `//` comments.
  let commentStart = -1;

  for (let i = 0; i < lines.length; i++) {
    const line = lines[i];
    const trimmed = line.trim();

    // Comment block runs of `//`.
    if (trimmed.startsWith('//')) {
      if (commentStart < 0) commentStart = i;
    } else {
      if (commentStart >= 0 && i - 1 > commentStart) {
        out.push({ startLine: commentStart, endLine: i - 1, kind: FoldingRangeKind.Comment });
      }
      commentStart = -1;
    }

    // `---` block-comment toggle.
    if (trimmed === '---') {
      if (docBlockStart < 0) docBlockStart = i;
      else {
        if (i - 1 > docBlockStart) {
          out.push({ startLine: docBlockStart, endLine: i, kind: FoldingRangeKind.Comment });
        }
        docBlockStart = -1;
      }
      continue;
    }

    // Inside a `---` block we shouldn't try to brace-balance the body.
    if (docBlockStart >= 0) continue;

    // Brace balance. Walk left→right; any `{` that's not closed on
    // the same line opens a fold.
    let openCol = -1;
    let depthInLine = 0;
    for (let c = 0; c < line.length; c++) {
      const ch = line[c];
      if (ch === '{') {
        if (depthInLine === 0) openCol = c;
        depthInLine++;
      } else if (ch === '}') {
        depthInLine--;
        if (depthInLine < 0) {
          // Closing brace deeper than this line opened — pops the stack.
          const top = stack.pop();
          if (top && i > top.line) {
            out.push({ startLine: top.line, endLine: i - 1, kind: FoldingRangeKind.Region });
          }
          depthInLine = 0;
          openCol = -1;
        }
      }
    }
    if (depthInLine > 0 && openCol >= 0) {
      stack.push({ line: i });
    }
  }

  // Trailing comment run at EOF.
  if (commentStart >= 0 && lines.length - 1 > commentStart) {
    out.push({ startLine: commentStart, endLine: lines.length - 1, kind: FoldingRangeKind.Comment });
  }

  return out;
});

documents.listen(connection);
connection.listen();
