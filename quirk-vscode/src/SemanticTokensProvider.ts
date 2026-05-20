import * as vscode from 'vscode';

// Define the types of tokens we will return. 
// Index 0 = 'namespace', which is the standard scope for modules.
const tokenTypes = ['namespace', 'class', 'function', 'variable'];
const tokenModifiers = ['declaration', 'static'];

export const legend = new vscode.SemanticTokensLegend(tokenTypes, tokenModifiers);

export class QuirkSemanticTokensProvider implements vscode.DocumentSemanticTokensProvider {
    
    provideDocumentSemanticTokens(
        document: vscode.TextDocument, 
        token: vscode.CancellationToken
    ): vscode.ProviderResult<vscode.SemanticTokens> {
        
        const builder = new vscode.SemanticTokensBuilder(legend);
        const text = document.getText();
        const lines = text.split(/\r?\n/);

        // 1. Identify all Module Aliases in the file
        //    Scans for: "use path.to.alias" -> alias = "alias"
        const moduleAliases = new Set<string>();

        for (const line of lines) {
            // Regex for "use path" or "use path.to.module"
            const useMatch = /^\s*use\s+([.a-zA-Z0-9_/]+)/.exec(line);
            if (useMatch) {
                const fullPath = useMatch[1];
                // Extract last part: "typing.collections.list" -> "list"
                const alias = fullPath.split(/[\.\/]/).pop();
                if (alias) moduleAliases.add(alias);
            }
        }

        // 2. Scan for usages of these aliases
        //    Looking for: "alias.Member"
        // Track docstring state so we don't paint module references inside
        // `---` blocks. Semantic tokens override TextMate coloring, so
        // without this `print(itertools.foo)` in a docstring example would
        // still get the namespace highlight.
        let inDocBlock = false;
        for (let i = 0; i < lines.length; i++) {
            const line = lines[i];
            const trimmed = line.trim();

            // Single-line docstring (`--- text ---`): entire line is doc,
            // skip without touching `inDocBlock`.
            if (trimmed.startsWith('---') && trimmed.endsWith('---') && trimmed.length > 3) {
                continue;
            }
            // Lone `---` toggles a multi-line docstring block.
            if (trimmed === '---') {
                inDocBlock = !inDocBlock;
                continue;
            }
            if (inDocBlock) continue;

            // Skip `//` line comments — same reason: namespace coloring
            // shouldn't bleed into commentary that happens to mention a
            // module name with a dot.
            const commentIdx = line.indexOf('//');

            // Look for words followed immediately by a dot: "word."
            const regex = /([a-zA-Z_]\w*)\./g;
            let match;

            while ((match = regex.exec(line)) !== null) {
                if (commentIdx !== -1 && match.index >= commentIdx) break;
                const word = match[1];

                if (moduleAliases.has(word)) {
                    // It is a known module alias!
                    // Color it as a 'namespace' (Index 0 in tokenTypes)
                    const startPos = match.index;

                    builder.push(
                        i,                  // Line number
                        startPos,           // Character position
                        word.length,        // Length of the word
                        0,                  // TokenType Index (0 = namespace)
                        0                   // TokenModifiers Bitmask (0 = none)
                    );
                }
            }
        }

        return builder.build();
    }
}