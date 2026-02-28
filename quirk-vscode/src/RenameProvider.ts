import * as vscode from 'vscode';

export class QuirkRenameProvider implements vscode.RenameProvider {

    private readonly protectedWords = new Set([
        'define', 'struct', 'if', 'else', 'elif', 'while', 'for', 'in',
        'return', 'break', 'continue', 'use', 'from', 'with', 'as',
        'extern', 'true', 'false', 'null', 'del', 'init', 'def', 'extend',
        'print', 'exit', 'Char', 'String', 'List', 'Map', 'File', 'Int',
        'Double', 'Bool', 'Any', 'void', 'self', 'super',
        'and', 'or', 'not', 'try', 'catch', 'throw', 'trigger',
    ]);

    public prepareRename(
        document: vscode.TextDocument,
        position: vscode.Position
    ): vscode.ProviderResult<vscode.Range> {
        const range = document.getWordRangeAtPosition(position, /[a-zA-Z0-9_]+/);
        if (!range) throw new Error('Nothing to rename here.');

        const word = document.getText(range);
        if (this.protectedWords.has(word)) throw new Error(`'${word}' is a built-in keyword and cannot be renamed.`);

        return range;
    }

    public async provideRenameEdits(
        document: vscode.TextDocument,
        position: vscode.Position,
        newName: string,
        token: vscode.CancellationToken
    ): Promise<vscode.WorkspaceEdit> {

        // Validate the new identifier
        if (!/^[a-zA-Z_][a-zA-Z0-9_]*$/.test(newName)) {
            throw new Error(`'${newName}' is not a valid Quirk identifier.`);
        }
        if (this.protectedWords.has(newName)) {
            throw new Error(`'${newName}' is a reserved keyword.`);
        }

        const range = document.getWordRangeAtPosition(position, /[a-zA-Z0-9_]+/);
        const oldName = document.getText(range!);
        const edit = new vscode.WorkspaceEdit();

        const isLocal = this.isLocalSymbol(document, position, oldName);

        // Locals: rename only within the enclosing function in the current file
        // Globals: rename across all .qk files in the workspace
        let filesToSearch: vscode.Uri[];
        if (isLocal) {
            filesToSearch = [document.uri];
        } else {
            filesToSearch = await vscode.workspace.findFiles('**/*.qk', '**/node_modules/**');
        }

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
                if (!line.includes(oldName)) continue;

                // For locals, skip lines outside the enclosing function scope
                if (isLocal && !this.isLineInSameScope(document, position, i)) continue;

                const maskedLine = this.maskLine(line);
                const regex = new RegExp(`\\b${escapeRegex(oldName)}\\b`, 'g');
                let match: RegExpExecArray | null;

                while ((match = regex.exec(maskedLine)) !== null) {
                    edit.replace(uri, new vscode.Range(i, match.index, i, match.index + oldName.length), newName);
                }
            }
        }

        return edit;
    }

    private isLocalSymbol(document: vscode.TextDocument, position: vscode.Position, symbol: string): boolean {
        const text = document.getText(new vscode.Range(new vscode.Position(0, 0), position));
        const lines = text.split(/\r?\n/).reverse();

        for (const line of lines) {
            if (new RegExp(`^\\s*(?:extern\\s+)?(?:define|def|struct)\\s+${escapeRegex(symbol)}\\b`).test(line)) return false;
            if (new RegExp(`\\b${escapeRegex(symbol)}\\s*(?::=|:\\s*[A-Za-z])`).test(line)) return true;
            const funcMatch = /^\s*(?:extern\s+)?(?:define|def|init)\s+[a-zA-Z0-9_]+\s*\(([^)]*)\)/.exec(line);
            if (funcMatch && funcMatch[1].split(',').some(p => p.trim().split(/[\s:]/)[0] === symbol)) return true;
        }
        return false;
    }

    private isLineInSameScope(document: vscode.TextDocument, cursorPos: vscode.Position, targetLine: number): boolean {
        const funcStart = this.findEnclosingFunctionStart(document, cursorPos.line);
        if (funcStart === -1) return true;
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
        let depth = 0, started = false;
        for (let i = startLine; i < document.lineCount; i++) {
            for (const ch of document.lineAt(i).text) {
                if (ch === '{') { depth++; started = true; }
                if (ch === '}') { depth--; if (started && depth === 0) return i; }
            }
        }
        return -1;
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