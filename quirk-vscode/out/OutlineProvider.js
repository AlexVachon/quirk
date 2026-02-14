"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.QuirkDocumentSymbolProvider = void 0;
const vscode = require("vscode");
class QuirkDocumentSymbolProvider {
    provideDocumentSymbols(document, token) {
        const symbols = [];
        const lines = document.getText().split(/\r?\n/);
        let currentStruct = null;
        let braceDepth = 0;
        for (let i = 0; i < lines.length; i++) {
            const line = lines[i];
            // Keep track of scope depth
            if (line.includes('{'))
                braceDepth++;
            if (line.includes('}')) {
                braceDepth--;
                if (braceDepth === 0 && currentStruct) {
                    currentStruct = null; // Exited struct/extend block
                }
            }
            // 1. Match Structs
            const structMatch = /^\s*struct\s+([a-zA-Z_]\w*)/.exec(line);
            if (structMatch) {
                const name = structMatch[1];
                const range = new vscode.Range(i, 0, i, line.length);
                const selectionRange = new vscode.Range(i, structMatch.index, i, structMatch.index + name.length);
                currentStruct = new vscode.DocumentSymbol(name, 'Struct', vscode.SymbolKind.Struct, range, selectionRange);
                symbols.push(currentStruct);
                continue;
            }
            // 2. Match Functions / Methods
            const funcMatch = /^\s*(?:extern\s+)?(?:define|def|init)\s+([a-zA-Z_]\w*)/.exec(line);
            if (funcMatch) {
                const name = funcMatch[1];
                const range = new vscode.Range(i, 0, i, line.length);
                const selectionRange = new vscode.Range(i, funcMatch.index, i, funcMatch.index + name.length);
                const funcSymbol = new vscode.DocumentSymbol(name, 'Function', name.startsWith('__') ? vscode.SymbolKind.Constructor : vscode.SymbolKind.Function, range, selectionRange);
                // If we are inside a struct block, attach it as a child!
                if (currentStruct && braceDepth > 0) {
                    funcSymbol.kind = vscode.SymbolKind.Method;
                    currentStruct.children.push(funcSymbol);
                }
                else {
                    // Otherwise, it's a global function
                    symbols.push(funcSymbol);
                }
            }
        }
        return symbols;
    }
}
exports.QuirkDocumentSymbolProvider = QuirkDocumentSymbolProvider;
//# sourceMappingURL=OutlineProvider.js.map