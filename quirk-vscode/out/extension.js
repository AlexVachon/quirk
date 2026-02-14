"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.deactivate = exports.activate = void 0;
const vscode = require("vscode");
const ImportProvider_1 = require("./ImportProvider");
const CompletionProvider_1 = require("./CompletionProvider");
const HoverProvider_1 = require("./HoverProvider");
const SemanticTokensProvider_1 = require("./SemanticTokensProvider");
const DiagnosticsProvider_1 = require("./DiagnosticsProvider");
const OutlineProvider_1 = require("./OutlineProvider");
const SignatureHelpProvider_1 = require("./SignatureHelpProvider");
function activate(context) {
    const logChannel = vscode.window.createOutputChannel("Quirk Language Server");
    logChannel.show(true);
    logChannel.appendLine("=== Quirk Extension Activated ===");
    const selector = { language: 'quirk', scheme: 'file' };
    // --- SETUP DIAGNOSTICS (LINTER) ---
    const quirkDiagnostics = vscode.languages.createDiagnosticCollection("quirk");
    context.subscriptions.push(quirkDiagnostics);
    (0, DiagnosticsProvider_1.subscribeToDocumentChanges)(context, quirkDiagnostics);
    // Register existing providers
    context.subscriptions.push(vscode.languages.registerDefinitionProvider(selector, new ImportProvider_1.QuirkDefinitionProvider(logChannel)), vscode.languages.registerCompletionItemProvider(selector, new CompletionProvider_1.QuirkCompletionProvider(logChannel), '.', '{', ',', ' '), vscode.languages.registerHoverProvider(selector, new HoverProvider_1.QuirkHoverProvider()), vscode.languages.registerDocumentSemanticTokensProvider(selector, new SemanticTokensProvider_1.QuirkSemanticTokensProvider(), SemanticTokensProvider_1.legend), vscode.languages.registerDocumentSymbolProvider(selector, new OutlineProvider_1.QuirkDocumentSymbolProvider()), vscode.languages.registerSignatureHelpProvider(selector, new SignatureHelpProvider_1.QuirkSignatureHelpProvider(), '(', ','));
}
exports.activate = activate;
function deactivate() { }
exports.deactivate = deactivate;
//# sourceMappingURL=extension.js.map