"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.deactivate = exports.activate = void 0;
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
exports.activate = activate;
function deactivate() { }
exports.deactivate = deactivate;
//# sourceMappingURL=extension.js.map