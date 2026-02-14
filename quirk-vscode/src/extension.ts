import * as vscode from 'vscode';
import { QuirkDefinitionProvider } from './ImportProvider';
import { QuirkCompletionProvider } from './CompletionProvider';
import { QuirkHoverProvider } from './HoverProvider';
import { QuirkSemanticTokensProvider, legend } from './SemanticTokensProvider';
// --- NEW IMPORT ---
import { subscribeToDocumentChanges } from './DiagnosticsProvider';

export function activate(context: vscode.ExtensionContext) {
    const logChannel = vscode.window.createOutputChannel("Quirk Language Server");
    logChannel.show(true); 
    logChannel.appendLine("=== Quirk Extension Activated ===");

    const selector = { language: 'quirk', scheme: 'file' };

    // --- SETUP DIAGNOSTICS (LINTER) ---
    const quirkDiagnostics = vscode.languages.createDiagnosticCollection("quirk");
    context.subscriptions.push(quirkDiagnostics);
    subscribeToDocumentChanges(context, quirkDiagnostics);

    // Register existing providers
    context.subscriptions.push(
        vscode.languages.registerDefinitionProvider(selector, new QuirkDefinitionProvider(logChannel)),
        vscode.languages.registerCompletionItemProvider(
            selector,
            new QuirkCompletionProvider(logChannel),
            '.', '{', ',', ' '
        ),
        vscode.languages.registerHoverProvider(selector, new QuirkHoverProvider()),
        vscode.languages.registerDocumentSemanticTokensProvider(selector, new QuirkSemanticTokensProvider(), legend)
    );
}

export function deactivate() {}