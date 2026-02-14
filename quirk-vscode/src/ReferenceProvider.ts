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
        const locations: vscode.Location[] = [];

        // Ignore keywords and built-ins to prevent massive, useless result sets
        const protectedWords = new Set([
            'define', 'struct', 'if', 'else', 'elif', 'while', 'for', 'in',
            'return', 'break', 'continue', 'use', 'from', 'with', 'as',
            'extern', 'true', 'false', 'null', 'del', 'init', 'def', 'extend',
            'print', 'exit', 'String', 'List', 'Map', 'File', 'Int', 'int', 
            'Double', 'double', 'Bool', 'bool', 'any', 'void', 'ptr', 'self'
        ]);

        if (protectedWords.has(targetWord)) {
            return [];
        }

        // Find all Quirk files in the current workspace
        const files = await vscode.workspace.findFiles('**/*.qk', '**/node_modules/**');

        for (const uri of files) {
            if (token.isCancellationRequested) break;

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
                if (!line.includes(targetWord)) continue;

                // Mask strings and comments so we don't accidentally match text inside them
                const maskedLine = this.maskLine(line);

                // Find exact word boundaries
                const regex = new RegExp(`\\b${targetWord}\\b`, 'g');
                let match;
                
                while ((match = regex.exec(maskedLine)) !== null) {
                    const matchRange = new vscode.Range(i, match.index, i, match.index + targetWord.length);
                    locations.push(new vscode.Location(uri, matchRange));
                }
            }
        }

        return locations;
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