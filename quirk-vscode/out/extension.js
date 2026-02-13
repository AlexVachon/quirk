"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.deactivate = exports.activate = void 0;
const vscode = require("vscode");
const ImportProvider_1 = require("./ImportProvider");
const CompletionProvider_1 = require("./CompletionProvider");
const HoverProvider_1 = require("./HoverProvider");
const SemanticTokensProvider_1 = require("./SemanticTokensProvider");
function activate(context) {
    const logChannel = vscode.window.createOutputChannel("Quirk Language Server");
    logChannel.show(true);
    logChannel.appendLine("=== Quirk Extension Activated ===");
    const selector = { language: 'quirk', scheme: 'file' };
    context.subscriptions.push(vscode.languages.registerDefinitionProvider(selector, new ImportProvider_1.QuirkDefinitionProvider(logChannel)), vscode.languages.registerCompletionItemProvider(selector, new CompletionProvider_1.QuirkCompletionProvider(logChannel), '.', '{', ',', ' '), vscode.languages.registerHoverProvider(selector, new HoverProvider_1.QuirkHoverProvider()), 
    // --- Register Semantic Tokens Provider ---
    vscode.languages.registerDocumentSemanticTokensProvider(selector, new SemanticTokensProvider_1.QuirkSemanticTokensProvider(), SemanticTokensProvider_1.legend));
}
exports.activate = activate;
function deactivate() { }
exports.deactivate = deactivate;
//# sourceMappingURL=extension.js.map