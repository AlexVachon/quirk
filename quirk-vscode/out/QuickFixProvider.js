"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.QuirkQuickFixProvider = void 0;
const vscode = require("vscode");
class QuirkQuickFixProvider {
    provideCodeActions(document, range, context, token) {
        // Find the "unused" diagnostic at the current cursor position
        return context.diagnostics
            .filter(diagnostic => diagnostic.message.includes('never used'))
            .map(diagnostic => this.createRemoveUnusedFix(document, diagnostic));
    }
    createRemoveUnusedFix(document, diagnostic) {
        const fix = new vscode.CodeAction(`Remove unused declaration`, vscode.CodeActionKind.QuickFix);
        fix.edit = new vscode.WorkspaceEdit();
        // Logic to remove the whole line if it's just the declaration
        const line = document.lineAt(diagnostic.range.start.line);
        if (line.text.trim().startsWith('define') || line.text.trim().startsWith('struct')) {
            // For functions/structs, we might want to just flag it; 
            // deleting a whole function block automatically is risky, 
            // so we'll just delete the specific line range for now.
            fix.edit.delete(document.uri, line.rangeIncludingLineBreak);
        }
        else {
            // For local variables (v2 := ...), remove the line
            fix.edit.delete(document.uri, line.rangeIncludingLineBreak);
        }
        fix.diagnostics = [diagnostic];
        fix.isPreferred = true; // Makes it the default choice for Ctrl+.
        return fix;
    }
}
exports.QuirkQuickFixProvider = QuirkQuickFixProvider;
QuirkQuickFixProvider.providedCodeActionKinds = [
    vscode.CodeActionKind.QuickFix
];
//# sourceMappingURL=QuickFixProvider.js.map