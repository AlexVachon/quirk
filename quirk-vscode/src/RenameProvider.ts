import * as vscode from 'vscode';

export class QuirkRenameProvider implements vscode.RenameProvider {
    
    // 1. Validates if the word under the cursor is allowed to be renamed
    public prepareRename(
        document: vscode.TextDocument,
        position: vscode.Position,
        token: vscode.CancellationToken
    ): vscode.ProviderResult<vscode.Range> {
        const range = document.getWordRangeAtPosition(position, /[a-zA-Z0-9_]+/);
        if (!range) throw new Error('You cannot rename this element.');

        const word = document.getText(range);
        
        // Prevent renaming core language keywords and built-ins
        const protectedWords = new Set([
            'define', 'struct', 'if', 'else', 'elif', 'while', 'for', 'in',
            'return', 'break', 'continue', 'use', 'from', 'with', 'as',
            'extern', 'true', 'false', 'null', 'del', 'init', 'def', 'extend',
            'print', 'exit', 'Char', 'String', 'List', 'Map', 'File', 'Int', 
            'Double', 'Bool', 'Any', 'void', 'self'
        ]);

        if (protectedWords.has(word)) {
            throw new Error(`Cannot rename Quirk keyword or built-in '${word}'.`);
        }

        return range;
    }

    // 2. Executes the workspace-wide find and replace
    public async provideRenameEdits(
        document: vscode.TextDocument,
        position: vscode.Position,
        newName: string,
        token: vscode.CancellationToken
    ): Promise<vscode.WorkspaceEdit> {
        
        const range = document.getWordRangeAtPosition(position, /[a-zA-Z0-9_]+/);
        const oldName = document.getText(range!);
        const edit = new vscode.WorkspaceEdit();

        // Find all Quirk files in the current workspace
        const files = await vscode.workspace.findFiles('**/*.qk', '**/node_modules/**');

        for (const uri of files) {
            const doc = await vscode.workspace.openTextDocument(uri);
            const text = doc.getText();
            const lines = text.split(/\r?\n/);
            
            let inDocBlock = false;

            for (let i = 0; i < lines.length; i++) {
                const line = lines[i];
                
                // Skip markdown docstrings
                if (line.trim() === '---') { inDocBlock = !inDocBlock; continue; }
                if (inDocBlock) continue;

                // Fast skip if the line doesn't contain the word at all
                if (!line.includes(oldName)) continue;

                // Mask strings and comments so we don't accidentally rename text inside them
                const maskedLine = this.maskLine(line);

                // Find exact word boundaries
                const regex = new RegExp(`\\b${oldName}\\b`, 'g');
                let match;
                
                while ((match = regex.exec(maskedLine)) !== null) {
                    const matchRange = new vscode.Range(i, match.index, i, match.index + oldName.length);
                    edit.replace(uri, matchRange, newName);
                }
            }
        }

        return edit;
    }

    // Safely masks strings and comments with spaces to preserve index positions
    private maskLine(line: string): string {
        let masked = "";
        let inString = false;
        let inInterpolation = false;
        let braceDepth = 0;

        for (let j = 0; j < line.length; j++) {
            const char = line[j];
            const nextChar = line[j + 1];

            if (!inString) {
                if (char === '/' && nextChar === '/') {
                    masked += ' '.repeat(line.length - j);
                    break;
                } else if (char === '"') {
                    inString = true;
                    masked += ' ';
                } else {
                    masked += char;
                }
            } else {
                if (char === '\\') {
                    masked += '  '; j++;
                } else if (!inInterpolation && char === '$' && nextChar === '{') {
                    inInterpolation = true; braceDepth = 1;
                    masked += '  '; j++;
                } else if (inInterpolation) {
                    if (char === '{') braceDepth++;
                    if (char === '}') braceDepth--;
                    if (braceDepth === 0) { inInterpolation = false; masked += ' '; } 
                    else { masked += char; }
                } else if (char === '"') {
                    inString = false; masked += ' ';
                } else {
                    masked += ' ';
                }
            }
        }
        return masked;
    }
}