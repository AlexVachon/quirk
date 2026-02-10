"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.activate = activate;
exports.deactivate = deactivate;
const vscode = require("vscode");
const ImportProvider_1 = require("./ImportProvider");
const CompletionProvider_1 = require("./CompletionProvider");
function activate(context) {
    const selector = { language: 'quirk', scheme: 'file' };
    // Register Definition Provider
    context.subscriptions.push(vscode.languages.registerDefinitionProvider(selector, new ImportProvider_1.QuirkDefinitionProvider()));
    // Register Completion Provider
    context.subscriptions.push(vscode.languages.registerCompletionItemProvider(selector, new CompletionProvider_1.QuirkCompletionProvider(), '.', // Trigger on dot (for use core.)
    '{', // Trigger on brace (for use { )
    ',', // Trigger on comma (for use { A, )
    ' ' // Trigger on space (generic)
    ));
}
function deactivate() { }
//# sourceMappingURL=extension.js.map