"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.QuirkSemanticTokensProvider = exports.legend = void 0;
const vscode = require("vscode");
// Define the types of tokens we will return. 
// Index 0 = 'namespace', which is the standard scope for modules.
const tokenTypes = ['namespace', 'class', 'function', 'variable'];
const tokenModifiers = ['declaration', 'static'];
exports.legend = new vscode.SemanticTokensLegend(tokenTypes, tokenModifiers);
class QuirkSemanticTokensProvider {
    provideDocumentSemanticTokens(document, token) {
        const builder = new vscode.SemanticTokensBuilder(exports.legend);
        const text = document.getText();
        const lines = text.split(/\r?\n/);
        // 1. Identify all Module Aliases in the file
        //    Scans for: "use path.to.alias" -> alias = "alias"
        const moduleAliases = new Set();
        for (const line of lines) {
            // Regex for "use path" or "use path.to.module"
            const useMatch = /^\s*use\s+([.a-zA-Z0-9_/]+)/.exec(line);
            if (useMatch) {
                const fullPath = useMatch[1];
                // Extract last part: "core.math" -> "math"
                const alias = fullPath.split(/[\.\/]/).pop();
                if (alias)
                    moduleAliases.add(alias);
            }
        }
        // 2. Scan for usages of these aliases
        //    Looking for: "alias.Member"
        for (let i = 0; i < lines.length; i++) {
            const line = lines[i];
            // Look for words followed immediately by a dot: "word."
            const regex = /([a-zA-Z_]\w*)\./g;
            let match;
            while ((match = regex.exec(line)) !== null) {
                const word = match[1];
                if (moduleAliases.has(word)) {
                    // It is a known module alias! 
                    // Color it as a 'namespace' (Index 0 in tokenTypes)
                    const startPos = match.index;
                    builder.push(i, // Line number
                    startPos, // Character position
                    word.length, // Length of the word
                    0, // TokenType Index (0 = namespace)
                    0 // TokenModifiers Bitmask (0 = none)
                    );
                }
            }
        }
        return builder.build();
    }
}
exports.QuirkSemanticTokensProvider = QuirkSemanticTokensProvider;
//# sourceMappingURL=SemanticTokensProvider.js.map