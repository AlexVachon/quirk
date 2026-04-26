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

function findCompiler(): string | undefined {
    const config = vscode.workspace.getConfiguration('quirk');

    const explicit = config.get<string>('compilerPath');
    if (explicit && explicit.trim() !== '') {
        return explicit.trim();
    }

    const quirkHome = config.get<string>('quirkHome')?.trim()
        || process.env['QUIRK_HOME'];
    if (quirkHome) {
        const candidate = path.join(quirkHome, 'bin', 'quirk');
        if (fs.existsSync(candidate)) {
            return candidate;
        }
    }

    // Fall back to whatever is on PATH
    return 'quirk';
}

export function activate(context: vscode.ExtensionContext) {
    const logChannel = vscode.window.createOutputChannel("Quirk Language Server");
    logChannel.appendLine("=== Quirk Extension Activated ===");

    const selector = { language: 'quirk', scheme: 'file' };

    // --- SETUP DIAGNOSTICS (LINTER) ---
    const quirkDiagnostics = vscode.languages.createDiagnosticCollection("quirk");
    context.subscriptions.push(quirkDiagnostics);
    subscribeToDocumentChanges(context, quirkDiagnostics);

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

            const compiler = findCompiler();
            if (!compiler) {
                vscode.window.showErrorMessage(
                    'Quirk compiler not found. Set quirk.compilerPath or QUIRK_HOME in settings.'
                );
                return;
            }

            const filePath = editor.document.fileName;
            const terminal = vscode.window.createTerminal({
                name: `Quirk: ${path.basename(filePath)}`,
                cwd: path.dirname(filePath),
            });
            terminal.show(true);
            terminal.sendText(`${compiler} "${filePath}"`);
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
            '.', '{', ',', ' '
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