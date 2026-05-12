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
        })
    );

    // --- LANGUAGE PROVIDERS ---
    context.subscriptions.push(
        vscode.languages.registerDefinitionProvider(
            { scheme: 'file', language: 'quirk' },
            new QuirkDefinitionProvider(logChannel)
        ),
        vscode.languages.registerCompletionItemProvider(
            selector,
            new QuirkCompletionProvider(logChannel),
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