import * as vscode from 'vscode';

export class QuirkDocumentSymbolProvider implements vscode.DocumentSymbolProvider {
    public provideDocumentSymbols(
        document: vscode.TextDocument,
        token: vscode.CancellationToken
    ): vscode.ProviderResult<vscode.DocumentSymbol[]> {

        const symbols: vscode.DocumentSymbol[] = [];
        const lines = document.getText().split(/\r?\n/);

        let currentStruct: vscode.DocumentSymbol | null = null;
        let structStartLine = -1;
        let braceDepth = 0;
        let inDocBlock = false;

        for (let i = 0; i < lines.length; i++) {
            const line = lines[i];
            const trimmed = line.trim();

            // Skip docstrings from brace counting
            if (trimmed === '---') { inDocBlock = !inDocBlock; continue; }
            if (inDocBlock) continue;

            // Count braces (ignoring strings would be ideal but brace-counting is robust enough here)
            const opens = (line.match(/\{/g) || []).length;
            const closes = (line.match(/\}/g) || []).length;
            braceDepth += opens - closes;

            // Close current struct when we return to its brace depth
            if (currentStruct && braceDepth === 0 && closes > 0) {
                // Extend the struct range to include its closing brace
                currentStruct.range = new vscode.Range(currentStruct.range.start, new vscode.Position(i, line.length));
                currentStruct = null;
                structStartLine = -1;
            }

            // ---- Struct definition ----
            const structMatch = /^\s*(extern\s+)?struct\s+([a-zA-Z_]\w*)(?:\s*:\s*([a-zA-Z_]\w*))?/.exec(line);
            if (structMatch) {
                const name = structMatch[2];
                const parent = structMatch[3];
                const detail = parent ? `: ${parent}` : 'Struct';
                const selStart = line.indexOf(name, (structMatch.index || 0));

                currentStruct = new vscode.DocumentSymbol(
                    name,
                    detail,
                    vscode.SymbolKind.Struct,
                    new vscode.Range(i, 0, i, line.length),   // will be extended when closed
                    new vscode.Range(i, selStart, i, selStart + name.length)
                );
                structStartLine = i;
                symbols.push(currentStruct);
                continue;
            }

            // ---- Field (inside struct, before any method) ----
            if (currentStruct && braceDepth === 1) {
                const fieldMatch = /^\s*([a-zA-Z_]\w*)\s*:\s*([a-zA-Z_]\w*)/.exec(line);
                if (fieldMatch && !line.includes('(') && !line.includes('return') && !line.includes('=')) {
                    const fname = fieldMatch[1];
                    const ftype = fieldMatch[2];
                    const selStart = line.indexOf(fname);
                    const fieldSymbol = new vscode.DocumentSymbol(
                        fname,
                        ftype,
                        vscode.SymbolKind.Field,
                        new vscode.Range(i, 0, i, line.length),
                        new vscode.Range(i, selStart, i, selStart + fname.length)
                    );
                    currentStruct.children.push(fieldSymbol);
                    continue;
                }
            }

            // ---- Function / method definition ----
            const funcMatch = /^\s*(extern\s+)?(?:define|def|init)\s+([a-zA-Z_]\w*)\s*(\([^)]*\))?(?:\s*->\s*([a-zA-Z_]\w*))?/.exec(line);
            if (funcMatch) {
                const name = funcMatch[2];
                const params = funcMatch[3] || '()';
                const returnType = funcMatch[4];
                const detail = returnType ? `${params.trim()} → ${returnType}` : params.trim();

                const isSpecial = name.startsWith('__');
                const kind = isSpecial ? vscode.SymbolKind.Constructor : vscode.SymbolKind.Function;

                const selStart = line.indexOf(name, (funcMatch.index || 0));
                const funcSymbol = new vscode.DocumentSymbol(
                    name,
                    detail,
                    kind,
                    new vscode.Range(i, 0, i, line.length),
                    new vscode.Range(i, selStart, i, selStart + name.length)
                );

                if (currentStruct && braceDepth > 0) {
                    funcSymbol.kind = vscode.SymbolKind.Method;
                    currentStruct.children.push(funcSymbol);
                } else {
                    symbols.push(funcSymbol);
                }
            }
        }

        return symbols;
    }
}