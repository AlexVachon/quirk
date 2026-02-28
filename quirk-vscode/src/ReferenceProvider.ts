import * as vscode from 'vscode';

export class QuirkReferenceProvider implements vscode.ReferenceProvider {

    public async provideReferences(
        document: vscode.TextDocument,
        position: vscode.Position,
        context: vscode.ReferenceContext,
        token: vscode.CancellationToken
    ): Promise<vscode.Location[]> {

        const range = document.getWordRangeAtPosition(position, /[a-zA-Z0-9_]+/);
        if (!range) return [];

        const targetWord = document.getText(range);

        const protectedWords = new Set([
            'define', 'struct', 'if', 'else', 'elif', 'while', 'for', 'in',
            'return', 'break', 'continue', 'use', 'from', 'with', 'as',
            'extern', 'true', 'false', 'null', 'del', 'init', 'def', 'extend',
            'print', 'exit', 'String', 'List', 'Map', 'File', 'Int',
            'Double', 'Bool', 'Any', 'void', 'self', 'trigger', 'super',
            'and', 'or', 'not', 'try', 'catch', 'throw',
        ]);
        if (protectedWords.has(targetWord)) return [];

        // Determine if the symbol is local (parameter / local variable).
        // If it is, we only search the current file — not the whole workspace.
        const isLocal = this.isLocalSymbol(document, position, targetWord);

        let filesToSearch: vscode.Uri[];
        if (isLocal) {
            filesToSearch = [document.uri];
        } else {
            filesToSearch = await vscode.workspace.findFiles('**/*.qk', '**/node_modules/**');
        }

        const locations: vscode.Location[] = [];

        for (const uri of filesToSearch) {
            if (token.isCancellationRequested) break;

            const doc = await vscode.workspace.openTextDocument(uri);
            const text = doc.getText();
            const lines = text.split(/\r?\n/);

            let inDocBlock = false;

            for (let i = 0; i < lines.length; i++) {
                const line = lines[i];

                if (line.trim() === '---') { inDocBlock = !inDocBlock; continue; }
                if (inDocBlock) continue;
                if (!line.includes(targetWord)) continue;

                // For local symbols: only look inside the function that contains the definition
                if (isLocal && !this.isLineInSameScope(document, position, i)) continue;

                const maskedLine = this.maskLine(line);
                const regex = new RegExp(`\\b${escapeRegex(targetWord)}\\b`, 'g');
                let match: RegExpExecArray | null;

                while ((match = regex.exec(maskedLine)) !== null) {
                    // Skip the declaration itself if includeDeclaration is false
                    if (!context.includeDeclaration && this.isDeclarationAt(line, match.index, targetWord)) continue;

                    locations.push(new vscode.Location(uri, new vscode.Range(i, match.index, i, match.index + targetWord.length)));
                }
            }
        }

        return locations;
    }

    /**
     * Returns true if the symbol is a local variable or parameter
     * (not a top-level function or struct name).
     */
    private isLocalSymbol(document: vscode.TextDocument, position: vscode.Position, symbol: string): boolean {
        const text = document.getText(new vscode.Range(new vscode.Position(0, 0), position));
        const lines = text.split(/\r?\n/).reverse();

        for (const line of lines) {
            // If it's a top-level define/struct, it's global
            if (new RegExp(`^\\s*(?:extern\\s+)?(?:define|def|struct)\\s+${escapeRegex(symbol)}\\b`).test(line)) return false;

            // If it's assigned as a local, it's local
            if (new RegExp(`\\b${escapeRegex(symbol)}\\s*(?::=|:\\s*[A-Za-z])`).test(line)) return true;

            // If it appears as a function parameter
            const funcMatch = /^\s*(?:extern\s+)?(?:define|def|init)\s+[a-zA-Z0-9_]+\s*\(([^)]*)\)/.exec(line);
            if (funcMatch && funcMatch[1].split(',').some(p => p.trim().split(/[\s:]/)[0] === symbol)) return true;
        }
        return false;
    }

    /**
     * Rough scope check: a local reference on line `targetLine` is only valid
     * if it's in the same function body as the cursor position.
     */
    private isLineInSameScope(document: vscode.TextDocument, cursorPos: vscode.Position, targetLine: number): boolean {
        // Find the function boundary containing the cursor
        const funcStart = this.findEnclosingFunctionStart(document, cursorPos.line);
        if (funcStart === -1) return true; // Can't determine — allow

        const funcEnd = this.findFunctionEnd(document, funcStart);
        return targetLine >= funcStart && (funcEnd === -1 || targetLine <= funcEnd);
    }

    private findEnclosingFunctionStart(document: vscode.TextDocument, fromLine: number): number {
        for (let i = fromLine; i >= 0; i--) {
            if (/^\s*(?:extern\s+)?(?:define|def|init)\s+/.test(document.lineAt(i).text)) return i;
        }
        return -1;
    }

    private findFunctionEnd(document: vscode.TextDocument, startLine: number): number {
        let depth = 0;
        let started = false;
        for (let i = startLine; i < document.lineCount; i++) {
            const line = document.lineAt(i).text;
            for (const ch of line) {
                if (ch === '{') { depth++; started = true; }
                if (ch === '}') { depth--; if (started && depth === 0) return i; }
            }
        }
        return -1;
    }

    /**
     * Returns true if the match position is at a `define foo` or `x :=` declaration.
     */
    private isDeclarationAt(line: string, matchIndex: number, word: string): boolean {
        const before = line.substring(0, matchIndex).trimEnd();
        return /(?:define|def|init|struct)\s+$/.test(before) || new RegExp(`\\b${escapeRegex(word)}\\s*(?::=|:\\s)$`).test(before + word);
    }

    private maskLine(line: string): string {
        let masked = "";
        let inString = false;
        let quoteChar = '';
        let inInterpolation = false;
        let braceDepth = 0;

        for (let j = 0; j < line.length; j++) {
            const char = line[j];
            const next = line[j + 1];

            if (!inString) {
                if (char === '/' && next === '/') { masked += ' '.repeat(line.length - j); break; }
                if (char === '"' || char === "'") { inString = true; quoteChar = char; masked += ' '; }
                else { masked += char; }
            } else {
                if (!inInterpolation) {
                    if (char === '\\') { masked += '  '; j++; }
                    else if (char === '$' && next === '{') { inInterpolation = true; braceDepth = 1; masked += '  '; j++; }
                    else if (char === quoteChar) { inString = false; quoteChar = ''; masked += ' '; }
                    else { masked += ' '; }
                } else {
                    if (char === '{') { braceDepth++; masked += char; }
                    else if (char === '}') {
                        braceDepth--;
                        if (braceDepth === 0) { inInterpolation = false; masked += ' '; }
                        else { masked += char; }
                    } else { masked += char; }
                }
            }
        }
        return masked;
    }
}

function escapeRegex(s: string): string {
    return s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}