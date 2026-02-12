"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.activate = activate;
exports.deactivate = deactivate;
const vscode = require("vscode");
const ImportProvider_1 = require("./ImportProvider");
const CompletionProvider_1 = require("./CompletionProvider");
function activate(context) {
    const logChannel = vscode.window.createOutputChannel("Quirk Language Server");
    logChannel.show(true);
    logChannel.appendLine("=== Quirk Extension Activated ===");
    const selector = { language: 'quirk', scheme: 'file' };
    context.subscriptions.push(vscode.languages.registerDefinitionProvider(selector, new ImportProvider_1.QuirkDefinitionProvider(logChannel)), vscode.languages.registerCompletionItemProvider(selector, new CompletionProvider_1.QuirkCompletionProvider(logChannel), '.', '{', ',', ' '));
}
function deactivate() { }
//# sourceMappingURL=extension.js.map