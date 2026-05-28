import * as vscode from 'vscode';
import * as path from 'path';
import * as fs from 'fs';

// One candidate interpreter shown in the QuickPick.
interface Interpreter {
    label: string;        // human-readable, shown in status bar / picker
    description: string;  // path or hint shown beside the label
    detail?: string;      // longer explanation, shown in the picker
    quirkHome: string;    // absolute path stored in the setting (empty = unset)
    kind: 'venv' | 'system' | 'dev-tree' | 'env-var' | 'configured' | 'none' | 'browse';
}

// A Quirk venv is any directory that looks like `quirk venv` built it:
// has `bin/activate` AND (`bin/quirk` OR `lib/quirk/`). The name doesn't matter.
function isQuirkVenv(candidate: string): boolean {
    try {
        if (!fs.existsSync(path.join(candidate, 'bin', 'activate'))) return false;
        return fs.existsSync(path.join(candidate, 'bin', 'quirk'))
            || fs.existsSync(path.join(candidate, 'lib', 'quirk'));
    } catch { return false; }
}

// Short, friendly label: `.venv` when at a workspace root, else `<parent>/<name>`.
function venvLabel(candidate: string): string {
    const venvName = path.basename(candidate);
    const parent = path.dirname(candidate);
    const wsFolders = vscode.workspace.workspaceFolders ?? [];
    if (wsFolders.some(f => f.uri.fsPath === parent)) return venvName;
    return `${path.basename(parent)}/${venvName}`;
}

// Dirs we never recurse into when scanning — saves a lot of stat() calls.
const SKIP_DIRS = new Set([
    '.git', '.svn', '.hg', 'node_modules', 'build', 'dist', 'out', 'obj',
    'target', 'vcpkg', 'vcpkg_installed', '__pycache__', '.gradle', '.cargo',
    'bin', 'include', 'lib', 'libs',   // never look INSIDE these for a venv
]);

function scanForVenvs(root: string, depth: number, seen: Set<string>, out: Interpreter[]): void {
    if (depth < 0) return;
    let entries: string[] = [];
    try { entries = fs.readdirSync(root); } catch { return; }
    for (const name of entries) {
        const full = path.join(root, name);
        let isDir = false;
        try { isDir = fs.statSync(full).isDirectory(); } catch { continue; }
        if (!isDir) continue;
        // Don't recurse into well-known noise, but DO test them — `.venv`
        // starts with `.` and would be skipped if we filtered dotfiles up-front.
        if (isQuirkVenv(full) && !seen.has(full)) {
            seen.add(full);
            out.push({
                label: venvLabel(full),
                description: full,
                detail: 'Quirk virtual environment',
                quirkHome: full,
                kind: 'venv',
            });
            continue;   // don't recurse into the venv itself
        }
        if (SKIP_DIRS.has(name) || name.startsWith('.')) continue;
        scanForVenvs(full, depth - 1, seen, out);
    }
}

// Find every Quirk venv reachable from the workspace folders + any open doc.
function findWorkspaceVenvs(): Interpreter[] {
    const results: Interpreter[] = [];
    const seen = new Set<string>();

    // Scan each workspace folder up to 2 levels deep.
    for (const folder of vscode.workspace.workspaceFolders ?? []) {
        scanForVenvs(folder.uri.fsPath, 2, seen, results);
    }

    // Walk up from every open Quirk doc; at each level scan that dir 1 deep.
    for (const doc of vscode.workspace.textDocuments) {
        if (doc.languageId !== 'quirk') continue;
        let dir = path.dirname(doc.uri.fsPath);
        for (let i = 0; i < 6 && dir.length > 3; i++) {
            scanForVenvs(dir, 1, seen, results);
            const parent = path.dirname(dir);
            if (parent === dir) break;
            dir = parent;
        }
    }

    return results;
}

// A dev tree is identified by a `libs/typing/` directory next to a `bin/quirk`.
// Useful when working inside the compiler repo itself.
function findDevTree(): Interpreter | null {
    const candidates: string[] = [];
    for (const folder of vscode.workspace.workspaceFolders ?? []) {
        candidates.push(folder.uri.fsPath);
        // Common case: workspace is the parent (`quirk/`) of the compiler dir.
        candidates.push(path.join(folder.uri.fsPath, 'quirk-compiler'));
    }
    for (const c of candidates) {
        if (fs.existsSync(path.join(c, 'libs', 'typing')) &&
            fs.existsSync(path.join(c, 'bin', 'quirk'))) {
            return {
                label: 'dev tree',
                description: c,
                detail: 'Quirk compiler source tree',
                quirkHome: c,
                kind: 'dev-tree',
            };
        }
    }
    return null;
}

function detectInterpreters(): Interpreter[] {
    const items: Interpreter[] = [];

    // 1. Workspace venvs (highest priority for IDE)
    items.push(...findWorkspaceVenvs());

    // 2. Dev tree
    const dev = findDevTree();
    if (dev) items.push(dev);

    // 3. Env var
    const envHome = process.env['QUIRK_HOME'];
    if (envHome && !items.some(i => i.quirkHome === envHome)) {
        items.push({
            label: 'environment: $QUIRK_HOME',
            description: envHome,
            detail: 'Set in your shell environment',
            quirkHome: envHome,
            kind: 'env-var',
        });
    }

    // 4. System install
    for (const sys of ['/usr/local/lib/quirk', '/usr/lib/quirk']) {
        if (fs.existsSync(path.join(sys, 'typing'))) {
            items.push({
                label: 'system',
                description: sys.replace(/\/lib\/quirk$/, ''),
                detail: 'System-wide installation',
                quirkHome: sys.replace(/\/lib\/quirk$/, ''),
                kind: 'system',
            });
            break;
        }
    }

    // 5. User-global ~/.quirk — a first-class "global" interpreter using
    //    system stdlib + ~/.quirk/packages. Selecting this clears the
    //    workspace QUIRK_HOME so resolution falls through to global paths.
    const homeEnv = process.env['HOME'];
    if (homeEnv) {
        const userPkgs = path.join(homeEnv, '.quirk', 'packages');
        const description = fs.existsSync(userPkgs)
            ? `~/.quirk/packages + system stdlib`
            : `~/.quirk/packages (empty) + system stdlib`;
        items.push({
            label: 'Quirk Global',
            description,
            detail: 'No venv. Packages live in ~/.quirk/packages/ (shared across projects).',
            quirkHome: '',                 // empty = clear the setting on select
            kind: 'none',
        });
    }

    // 6. Browse for a directory
    items.push({
        label: 'browse…',
        description: '',
        detail: 'Pick a directory containing bin/quirk and lib/quirk/',
        quirkHome: '',
        kind: 'browse',
    });

    return items;
}

// Human-readable title used in the picker's label column.
// Examples:
//   "Quirk venv (.venv)"
//   "Quirk dev tree"
//   "Quirk system install"
function prettyTitle(i: Interpreter): string {
    switch (i.kind) {
        case 'venv':       return `Quirk venv (${path.basename(i.quirkHome)})`;
        case 'dev-tree':   return 'Quirk dev tree';
        case 'env-var':    return 'Quirk (from $QUIRK_HOME)';
        case 'system':     return 'Quirk system install';
        case 'configured': return 'Quirk (configured path)';
        default:           return i.label;
    }
}

function iconFor(kind: Interpreter['kind']): string {
    switch (kind) {
        case 'venv':     return 'symbol-folder';
        case 'dev-tree': return 'tools';
        case 'env-var':  return 'terminal';
        case 'system':   return 'desktop-download';
        default:         return 'symbol-namespace';
    }
}

// Prompt for a venv name + parent dir, then run `quirk venv <name>` via the
// configured compiler. Sets that venv as the active interpreter on success.
async function createVenvFlow(): Promise<void> {
    const folders = vscode.workspace.workspaceFolders;
    let parent: string;
    if (folders && folders.length === 1) {
        parent = folders[0].uri.fsPath;
    } else if (folders && folders.length > 1) {
        const pick = await vscode.window.showQuickPick(
            folders.map(f => ({ label: f.name, description: f.uri.fsPath, fsPath: f.uri.fsPath })),
            { title: 'Where should the venv live?' },
        );
        if (!pick) return;
        parent = pick.fsPath;
    } else {
        const picked = await vscode.window.showOpenDialog({
            canSelectFiles: false, canSelectFolders: true, canSelectMany: false,
            openLabel: 'Create venv inside this folder',
        });
        if (!picked || picked.length === 0) return;
        parent = picked[0].fsPath;
    }

    const name = await vscode.window.showInputBox({
        title: 'Create Quirk Virtual Environment',
        prompt: 'Name for the venv (will be created inside the workspace folder)',
        value: '.venv',
        validateInput: v => {
            if (!v) return 'Name required';
            if (v.includes('/')) return 'No slashes in the name';
            if (fs.existsSync(path.join(parent, v))) return `Directory ${v} already exists`;
            return null;
        },
    });
    if (!name) return;

    const venvPath = path.join(parent, name);
    const quirkBin = findQuirkBinary();
    if (!quirkBin) {
        vscode.window.showErrorMessage("Can't find the `quirk` compiler. Set `quirk.compilerPath` or QUIRK_HOME first.");
        return;
    }

    const term = vscode.window.createTerminal({ name: `quirk venv ${name}`, cwd: parent });
    term.show(true);
    term.sendText(`"${quirkBin}" venv "${name}"`);

    // Wait briefly, then if the venv exists, set it as active and source the
    // activate script in any open terminals.
    const tryActivate = async () => {
        for (let i = 0; i < 30; i++) {
            await new Promise(r => setTimeout(r, 200));
            if (fs.existsSync(path.join(venvPath, 'bin', 'activate'))) {
                await vscode.workspace.getConfiguration('quirk').update(
                    'quirkHome', venvPath,
                    vscode.workspace.workspaceFolders
                        ? vscode.ConfigurationTarget.Workspace
                        : vscode.ConfigurationTarget.Global,
                );
                activateInAllTerminals(venvPath);
                vscode.window.showInformationMessage(`Quirk interpreter set to: ${venvPath}`);
                return;
            }
        }
    };
    tryActivate();
}

function findQuirkBinary(): string | undefined {
    const cfgPath = vscode.workspace.getConfiguration('quirk').get<string>('compilerPath')?.trim();
    if (cfgPath && fs.existsSync(cfgPath)) return cfgPath;
    const home = vscode.workspace.getConfiguration('quirk').get<string>('quirkHome')?.trim() || process.env['QUIRK_HOME'];
    if (home) {
        const c = path.join(home, 'bin', 'quirk');
        if (fs.existsSync(c)) return c;
    }
    for (const dir of (process.env['PATH'] ?? '').split(path.delimiter)) {
        if (!dir) continue;
        const c = path.join(dir, 'quirk');
        if (fs.existsSync(c)) return c;
    }
    return undefined;
}

function activeInterpreterLabel(): string {
    const inspected = vscode.workspace
        .getConfiguration('quirk').inspect<string>('quirkHome');
    const raw = inspected?.workspaceValue ?? inspected?.globalValue;
    if (raw === undefined) return 'auto';                // never explicitly set
    const trimmed = (raw ?? '').trim();
    if (!trimmed) return 'global';                       // explicit "Quirk Global"
    for (const c of detectInterpreters()) {
        if (c.quirkHome === trimmed && c.kind !== 'none') {
            return c.kind === 'venv' ? path.basename(trimmed) : c.label;
        }
    }
    return path.basename(trimmed) || trimmed;
}

// Returns the shell command that activates a Quirk venv. Sends a leading
// `deactivate 2>/dev/null || true` so re-activation from another venv works
// (the activate script refuses to re-activate over an existing one).
function activateCommandFor(venvPath: string): string | null {
    const script = path.join(venvPath, 'bin', 'activate');
    if (!fs.existsSync(script)) return null;
    // Escape any double quotes in the path (rare but possible).
    const safe = script.replace(/"/g, '\\"');
    return `deactivate 2>/dev/null; source "${safe}"`;
}

function sendActivation(terminal: vscode.Terminal, venvPath: string) {
    const cmd = activateCommandFor(venvPath);
    if (!cmd) return;
    terminal.sendText(cmd, true);
}

// Activate the venv in every currently-open terminal. Used after the user
// picks a new interpreter from the QuickPick.
// Terminals the extension created for run/debug — same rule as
// onDidOpenTerminal: don't pollute them with venv activate/deactivate.
function isExtensionOwnedTerminal(t: vscode.Terminal): boolean {
    return t.name.startsWith('Quirk: ') || t.name.startsWith('Quirk Debugger: ');
}

function activateInAllTerminals(venvPath: string) {
    for (const t of vscode.window.terminals) {
        if (isExtensionOwnedTerminal(t)) continue;
        sendActivation(t, venvPath);
    }
}

// Deactivate any active Quirk venv in every currently-open terminal.
// Silent when nothing is active (the activate-template defines `deactivate`
// as a function, so it's undefined outside a venv shell — `2>/dev/null`
// swallows the "command not found" message).
function deactivateInAllTerminals() {
    for (const t of vscode.window.terminals) {
        if (isExtensionOwnedTerminal(t)) continue;
        t.sendText('deactivate 2>/dev/null; true', true);
    }
}

// Compute the active venv path (or null when global / dev tree / no setting).
function activeVenvPath(): string | null {
    const cfg = vscode.workspace.getConfiguration('quirk').get<string>('quirkHome')?.trim();
    if (!cfg) return null;
    if (!fs.existsSync(path.join(cfg, 'bin', 'activate'))) return null;
    return cfg;
}

export function registerInterpreterPicker(context: vscode.ExtensionContext): void {
    // Auto-activate any newly-opened terminal in the selected venv — EXCEPT
    // the ones the extension itself created to run/debug user files. Those
    // already invoke quirk by its full venv-internal path, so adding a
    // deactivate+source on top just echoes noise after the program output:
    //     $ /path/to/.venv/bin/quirk foo.quirk
    //     hello world
    //     $ deactivate 2>/dev/null; source ".../activate"   ← unwanted
    // Naming convention enforced in extension.ts: every extension-spawned
    // terminal starts with "Quirk: " (run) or "Quirk Debugger: " (debug).
    context.subscriptions.push(
        vscode.window.onDidOpenTerminal(t => {
            if (isExtensionOwnedTerminal(t)) return;
            const venv = activeVenvPath();
            if (venv) sendActivation(t, venv);
        }),
    );
    // Status bar item — clickable, always visible while a Quirk file is open.
    const statusBar = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Right, 100);
    statusBar.command = 'quirk.selectInterpreter';
    statusBar.tooltip = 'Click to choose the Quirk interpreter (QUIRK_HOME) used by IntelliSense.';
    const refreshStatusBar = () => {
        statusBar.text = `$(symbol-namespace) Quirk: ${activeInterpreterLabel()}`;
        const editor = vscode.window.activeTextEditor;
        if (editor && editor.document.languageId === 'quirk') statusBar.show();
        else statusBar.hide();
    };
    refreshStatusBar();
    context.subscriptions.push(
        statusBar,
        vscode.window.onDidChangeActiveTextEditor(refreshStatusBar),
        vscode.workspace.onDidChangeConfiguration(e => {
            if (e.affectsConfiguration('quirk.quirkHome')) refreshStatusBar();
        }),
    );

    // Command: open the picker (Python-extension style — flat list with top
    // actions, placeholder shows the active selection).
    context.subscriptions.push(
        vscode.commands.registerCommand('quirk.selectInterpreter', async () => {
            const interpreters = detectInterpreters().filter(i => i.kind !== 'browse');
            const cfg = vscode.workspace.getConfiguration('quirk');
            const inspected = cfg.inspect<string>('quirkHome');
            const explicit = inspected?.workspaceValue ?? inspected?.globalValue;
            // Active = currently selected interpreter (matched by path).
            // When explicit is "" the user picked Global; if undefined nothing
            // is explicitly set, so don't mark any entry as Selected.
            const active = explicit !== undefined ? (explicit ?? '').trim() : null;

            type Action = 'create' | 'browse';
            type ItemWithRef = vscode.QuickPickItem & { __ref?: Interpreter; __action?: Action };

            const picks: ItemWithRef[] = [
                {
                    label: '$(add) Create Virtual Environment…',
                    description: 'quirk venv',
                    __action: 'create',
                },
                {
                    label: '$(folder-opened) Enter interpreter path…',
                    description: 'Pick a directory containing bin/quirk',
                    __action: 'browse',
                },
            ];

            const isActive = (i: Interpreter): boolean => {
                if (active === null) return false;                 // setting unset
                if (active === '')  return i.kind === 'none';      // Quirk Global
                return i.quirkHome === active && i.kind !== 'browse';
            };

            // Sort: active first, then other venvs, then dev tree, env, system, global, etc.
            const order: Record<Interpreter['kind'], number> = {
                'venv': 0, 'dev-tree': 1, 'env-var': 2, 'system': 3,
                'configured': 4, 'none': 5, 'browse': 6,
            };
            const sorted = interpreters.slice().sort((a, b) => {
                if (isActive(a) !== isActive(b)) return isActive(a) ? -1 : 1;
                return order[a.kind] - order[b.kind];
            });

            for (const i of sorted) {
                const scope = i.quirkHome === active ? 'Selected' : i.kind;
                picks.push({
                    label: prettyTitle(i),
                    description: i.description,
                    detail: undefined,           // keep it one-line, like Python's picker
                    __ref: i,
                    iconPath: new vscode.ThemeIcon(iconFor(i.kind)),
                    // Right-aligned hint
                    // (QuickPick doesn't render `buttons` text; cram it into description.)
                });
                // Annotate "Selected" at the end of description.
                if (isActive(i)) {
                    const last = picks[picks.length - 1];
                    last.description = (last.description || '') + '  • Selected';
                }
            }

            const placeholderTarget = active === null ? '(auto-detect)'
                                    : active === ''  ? 'Quirk Global'
                                    : active;
            const pick = await vscode.window.showQuickPick(picks, {
                title: 'Select Quirk Interpreter',
                placeHolder: `Selected Interpreter: ${placeholderTarget}`,
                matchOnDescription: true,
            });
            if (!pick) return;

            let target: string | undefined;
            if (pick.__action === 'browse') {
                const folder = await vscode.window.showOpenDialog({
                    canSelectFiles: false,
                    canSelectFolders: true,
                    canSelectMany: false,
                    openLabel: 'Use as QUIRK_HOME',
                });
                if (!folder || folder.length === 0) return;
                target = folder[0].fsPath;
            } else if (pick.__action === 'create') {
                await createVenvFlow();
                return;   // createVenvFlow handles its own config write
            } else if (pick.__ref) {
                target = pick.__ref.quirkHome;
            } else {
                return;
            }

            // Preserve empty string for "Quirk Global" — it's a meaningful
            // value (explicit "no venv"), distinct from the setting being unset.
            await cfg.update(
                'quirkHome',
                target === undefined ? undefined : target,
                vscode.workspace.workspaceFolders ? vscode.ConfigurationTarget.Workspace
                                                 : vscode.ConfigurationTarget.Global,
            );
            refreshStatusBar();

            // Sync open terminals to the new selection:
            //   venv  → source its activate script
            //   else  → deactivate any active venv (Global / dev tree / system)
            // Future terminals are handled by the onDidOpenTerminal listener.
            if (target && fs.existsSync(path.join(target, 'bin', 'activate'))) {
                activateInAllTerminals(target);
            } else {
                deactivateInAllTerminals();
            }

            vscode.window.showInformationMessage(
                target ? `Quirk interpreter: ${target}` : 'Quirk interpreter: global'
            );
        }),
    );
}
