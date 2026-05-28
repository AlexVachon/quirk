import * as vscode from 'vscode';
import * as path from 'path';
import * as fs from 'fs';
import { QuirkDefinitionProvider } from './ImportProvider';
import { QuirkCompletionProvider } from './CompletionProvider';
import { QuirkHoverProvider } from './HoverProvider';
import { QuirkSemanticTokensProvider, legend } from './SemanticTokensProvider';
import { subscribeToDocumentChanges } from './DiagnosticsProvider';
import { QuirkDocumentSymbolProvider } from './OutlineProvider';
import { QuirkSignatureHelpProvider } from './SignatureHelpProvider';
import { QuirkQuickFixProvider } from './QuickFixProvider';
import { QuirkRenameProvider } from './RenameProvider';
import { QuirkReferenceProvider } from './ReferenceProvider';
import { QuirkDocumentFormattingEditProvider } from './FormatterProvider';
import { updateDeadCode } from './DeadCodeProvider';
import { registerInterpreterPicker } from './InterpreterProvider';
import { QuirkDebugSession } from './QuirkDebugAdapter';
import { QuirkInlineValuesProvider } from './QuirkInlineValuesProvider';

interface CompilerLookup {
    /** The command/path to run. Always usable; may be the literal 'quirk' as a last-resort fallback. */
    command: string;
    /** Whether the binary was confirmed to exist on disk. */
    verified: boolean;
    /** How the location was determined, for diagnostics. */
    source: 'compilerPath' | 'quirkHome' | 'PATH' | 'fallback';
}

function searchOnPath(binary: string): string | undefined {
    const exts = process.platform === 'win32'
        ? (process.env['PATHEXT']?.split(';') ?? ['.EXE', '.CMD', '.BAT'])
        : [''];
    const dirs = (process.env['PATH'] ?? '').split(path.delimiter).filter(Boolean);
    for (const dir of dirs) {
        for (const ext of exts) {
            const candidate = path.join(dir, binary + ext);
            if (fs.existsSync(candidate)) return candidate;
        }
    }
    return undefined;
}

function findCompiler(): CompilerLookup {
    const config = vscode.workspace.getConfiguration('quirk');

    const explicit = config.get<string>('compilerPath')?.trim();
    if (explicit) {
        return { command: explicit, verified: fs.existsSync(explicit), source: 'compilerPath' };
    }

    const quirkHome = config.get<string>('quirkHome')?.trim() || process.env['QUIRK_HOME'];
    if (quirkHome) {
        const candidate = path.join(quirkHome, 'bin', 'quirk');
        if (fs.existsSync(candidate)) {
            return { command: candidate, verified: true, source: 'quirkHome' };
        }
    }

    const onPath = searchOnPath('quirk');
    if (onPath) return { command: onPath, verified: true, source: 'PATH' };

    return { command: 'quirk', verified: false, source: 'fallback' };
}

async function warnIfCompilerMissing(lookup: CompilerLookup, logChannel: vscode.OutputChannel): Promise<void> {
    if (lookup.verified) {
        logChannel.appendLine(`Compiler: ${lookup.command} (${lookup.source})`);
        return;
    }

    const detail = lookup.source === 'compilerPath'
        ? `quirk.compilerPath is set to '${lookup.command}' but no file exists there.`
        : `Could not locate the 'quirk' compiler in QUIRK_HOME or on PATH.`;
    logChannel.appendLine(`WARNING: ${detail}`);

    const action = await vscode.window.showWarningMessage(
        `Quirk compiler not found. ${detail}`,
        'Open Settings'
    );
    if (action === 'Open Settings') {
        vscode.commands.executeCommand('workbench.action.openSettings', 'quirk');
    }
}

export function activate(context: vscode.ExtensionContext) {
    const logChannel = vscode.window.createOutputChannel("Quirk Language Server");
    logChannel.appendLine("=== Quirk Extension Activated ===");

    warnIfCompilerMissing(findCompiler(), logChannel);

    // Interpreter picker — Python-extension-style status bar + QuickPick to
    // switch between venvs, dev tree, env, or system install.
    registerInterpreterPicker(context);

    const selector = { language: 'quirk', scheme: 'file' };

    // --- SETUP DIAGNOSTICS (LINTER) ---
    const quirkDiagnostics = vscode.languages.createDiagnosticCollection("quirk");
    context.subscriptions.push(quirkDiagnostics);
    subscribeToDocumentChanges(context, quirkDiagnostics);

    // --- DEAD CODE HIGHLIGHTING ---
    if (vscode.window.activeTextEditor) {
        updateDeadCode(vscode.window.activeTextEditor);
    }
    let deadCodeTimer: ReturnType<typeof setTimeout> | undefined;
    context.subscriptions.push(
        vscode.window.onDidChangeActiveTextEditor(editor => {
            if (editor) updateDeadCode(editor);
        }),
        vscode.workspace.onDidChangeTextDocument(event => {
            const editor = vscode.window.activeTextEditor;
            if (!editor || event.document !== editor.document) return;
            if (deadCodeTimer) clearTimeout(deadCodeTimer);
            deadCodeTimer = setTimeout(() => updateDeadCode(editor), 300);
        })
    );

    // --- DEBUG ADAPTER ---
    // Inline factory: the adapter runs in-process inside the extension host,
    // talking DAP over a Node duplex. That's simpler than shipping a separate
    // adapter binary, at the cost of pinning the adapter's lifetime to the
    // extension host (fine — sessions are short-lived). If a Quirk launch
    // config doesn't supply `compilerPath`, fall back to whatever findCompiler
    // resolved at activation so the user doesn't have to wire it twice.
    context.subscriptions.push(
        vscode.debug.registerDebugAdapterDescriptorFactory('quirk', {
            createDebugAdapterDescriptor() {
                return new vscode.DebugAdapterInlineImplementation(new QuirkDebugSession());
            },
        }),
        // Inline values: shows `x = 10` next to source lines while paused.
        // VSCode invokes the provider on every pause/step and resolves
        // each emitted name against the active scope's variables.
        vscode.languages.registerInlineValuesProvider(
            { language: 'quirk' },
            new QuirkInlineValuesProvider(),
        ),
        vscode.debug.registerDebugConfigurationProvider('quirk', {
            resolveDebugConfiguration(_folder, config) {
                // Allow "F5 with no launch.json" by synthesising a config
                // pointing at the active editor.
                if (!config.type && !config.request && !config.name) {
                    const editor = vscode.window.activeTextEditor;
                    if (editor && editor.document.languageId === 'quirk') {
                        config.type = 'quirk';
                        config.request = 'launch';
                        config.name = 'Quirk: current file';
                        config.program = '${file}';
                        config.stopOnEntry = false;
                    }
                }
                if (!config.compilerPath) {
                    const lookup = findCompiler();
                    if (lookup.verified) config.compilerPath = lookup.command;
                }
                if (!config.quirkHome) {
                    // Mirror the same resolution order as the language
                    // providers: explicit setting → env → derived from the
                    // compiler's bin/ parent. Without this, the debuggee
                    // can't find runtime.so or the stdlib.
                    const cfgHome = vscode.workspace.getConfiguration('quirk')
                        .get<string>('quirkHome')?.trim();
                    if (cfgHome) config.quirkHome = cfgHome;
                    else if (process.env['QUIRK_HOME']) config.quirkHome = process.env['QUIRK_HOME'];
                }
                return config;
            },
        }),
    );

    // --- RUN FILE COMMAND ---
    context.subscriptions.push(
        vscode.commands.registerCommand('quirk.runFile', async () => {
            const editor = vscode.window.activeTextEditor;
            if (!editor) {
                vscode.window.showErrorMessage('No active editor.');
                return;
            }
            if (editor.document.languageId !== 'quirk') {
                vscode.window.showErrorMessage('Active file is not a Quirk file.');
                return;
            }

            if (editor.document.isDirty) {
                await editor.document.save();
            }

            const lookup = findCompiler();
            if (!lookup.verified) {
                await warnIfCompilerMissing(lookup, logChannel);
                if (lookup.source === 'compilerPath') return;
            }

            const filePath = editor.document.fileName;
            const terminal = vscode.window.createTerminal({
                name: `Quirk: ${path.basename(filePath)}`,
                cwd: path.dirname(filePath),
            });
            terminal.show(true);
            terminal.sendText(`${lookup.command} "${filePath}"`);
        }),
        // Dedicated-terminal variant: reuse a single terminal named after the
        // file so successive runs of the SAME file land in the same pane,
        // and switching files spawns a new pane (so two scripts can be
        // running at once without stomping output).
        vscode.commands.registerCommand('quirk.runFileDedicated', async () => {
            const editor = vscode.window.activeTextEditor;
            if (!editor) { vscode.window.showErrorMessage('No active editor.'); return; }
            if (editor.document.languageId !== 'quirk') {
                vscode.window.showErrorMessage('Active file is not a Quirk file.');
                return;
            }
            if (editor.document.isDirty) await editor.document.save();
            const lookup = findCompiler();
            if (!lookup.verified) {
                await warnIfCompilerMissing(lookup, logChannel);
                if (lookup.source === 'compilerPath') return;
            }
            const filePath = editor.document.fileName;
            const termName = `Quirk: ${path.basename(filePath)}`;
            const existing = vscode.window.terminals.find((t) => t.name === termName);
            const terminal = existing ?? vscode.window.createTerminal({
                name: termName,
                cwd: path.dirname(filePath),
            });
            terminal.show(true);
            terminal.sendText(`${lookup.command} "${filePath}"`);
        }),
        vscode.commands.registerCommand('quirk.debugFile', async () => {
            // Mirror the same guards as runFile so the failure messages line
            // up — saves the user from getting two different errors for the
            // same problem (no editor / wrong language / dirty buffer).
            const editor = vscode.window.activeTextEditor;
            if (!editor) {
                vscode.window.showErrorMessage('No active editor.');
                return;
            }
            if (editor.document.languageId !== 'quirk') {
                vscode.window.showErrorMessage('Active file is not a Quirk file.');
                return;
            }
            if (editor.document.isDirty) {
                await editor.document.save();
            }
            // Launch via vscode.debug so the user sees the full debugger UI
            // (gutter, variables, call stack). startDebugging picks up the
            // matching DebugConfigurationProvider — including our
            // compilerPath/quirkHome resolution.
            await vscode.debug.startDebugging(
                vscode.workspace.getWorkspaceFolder(editor.document.uri),
                {
                    type: 'quirk',
                    request: 'launch',
                    name: 'Quirk: current file',
                    program: editor.document.fileName,
                    stopOnEntry: false,
                },
            );
        }),
        // "Debug using launch.json" — opens the standard Run/Debug picker so
        // the user can choose between configs in .vscode/launch.json instead
        // of always running the active file. Matches the Python extension's
        // entry in its own dropdown.
        vscode.commands.registerCommand('quirk.debugLaunchJson', async () => {
            await vscode.commands.executeCommand('workbench.action.debug.selectandstart');
        }),
    );

    // --- LANGUAGE PROVIDERS ---
    context.subscriptions.push(
        vscode.languages.registerDefinitionProvider(
            { scheme: 'file', language: 'quirk' },
            new QuirkDefinitionProvider()
        ),
        vscode.languages.registerCompletionItemProvider(
            selector,
            new QuirkCompletionProvider(),
            '.', '{', ','
        ),
        vscode.languages.registerHoverProvider(selector, new QuirkHoverProvider()),
        vscode.languages.registerDocumentSemanticTokensProvider(selector, new QuirkSemanticTokensProvider(), legend),
        vscode.languages.registerDocumentSymbolProvider(selector, new QuirkDocumentSymbolProvider()),
        vscode.languages.registerSignatureHelpProvider(
            selector,
            new QuirkSignatureHelpProvider(),
            '(', ','
        ),
        vscode.languages.registerCodeActionsProvider(selector, new QuirkQuickFixProvider(), {
            providedCodeActionKinds: QuirkQuickFixProvider.providedCodeActionKinds
        }),
        vscode.languages.registerRenameProvider(selector, new QuirkRenameProvider()),
        vscode.languages.registerReferenceProvider(selector, new QuirkReferenceProvider()),
        vscode.languages.registerDocumentFormattingEditProvider(selector, new QuirkDocumentFormattingEditProvider()),
    );
}

export function deactivate() {}