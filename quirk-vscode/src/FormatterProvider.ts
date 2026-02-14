import * as vscode from 'vscode';

export class QuirkDocumentFormattingEditProvider implements vscode.DocumentFormattingEditProvider {
    
    public provideDocumentFormattingEdits(
        document: vscode.TextDocument,
        options: vscode.FormattingOptions,
        token: vscode.CancellationToken
    ): vscode.ProviderResult<vscode.TextEdit[]> {
        
        const edits: vscode.TextEdit[] = [];
        let indentLevel = 0;
        
        // Use the user's VS Code settings for Tabs vs Spaces
        const indentString = options.insertSpaces ? ' '.repeat(options.tabSize) : '\t';
        let inDocBlock = false;

        for (let i = 0; i < document.lineCount; i++) {
            const line = document.lineAt(i);
            const text = line.text;
            const trimmed = text.trim();

            // 1. Remove trailing whitespace and handle empty lines
            if (trimmed === '') {
                if (text.length > 0) edits.push(vscode.TextEdit.replace(line.range, ''));
                continue;
            }

            // 2. Handle Docstrings (---)
            if (trimmed === '---') {
                inDocBlock = !inDocBlock;
                const expectedIndent = indentLevel > 0 ? indentString.repeat(indentLevel) : "";
                if (text !== expectedIndent + trimmed) {
                    edits.push(vscode.TextEdit.replace(line.range, expectedIndent + trimmed));
                }
                continue;
            }

            if (inDocBlock) {
                continue; // Keep docstring contents aligned, but don't touch internal spacing
            }

            // 3. Adjust Indent for current line ONLY
            let currentIndent = indentLevel;
            if (trimmed.startsWith('}') || trimmed.startsWith(']') || trimmed.startsWith('else') || trimmed.startsWith('elif')) {
                currentIndent = Math.max(0, currentIndent - 1);
            }

            // 4. Format Spacing (Safe basic formatting)
            const formattedText = this.formatLineSpacing(trimmed);

            // 5. Apply Indentation
            const expectedLine = (currentIndent > 0 ? indentString.repeat(currentIndent) : "") + formattedText;
            if (text !== expectedLine) {
                edits.push(vscode.TextEdit.replace(line.range, expectedLine));
            }

            // 6. Calculate Indent for NEXT line
            indentLevel += this.getIndentChange(formattedText);
            
            // 🐛 CRASH PREVENTION: Never let the indent level drop below 0
            indentLevel = Math.max(0, indentLevel); 
        }

        return edits;
    }

    /**
     * Calculates if the next line should be indented further by counting { and }
     * safely ignoring brackets inside strings and comments.
     */
    private getIndentChange(line: string): number {
        let change = 0;
        let inString = false;

        for (let i = 0; i < line.length; i++) {
            const char = line[i];
            const nextChar = line[i + 1];

            if (!inString && char === '/' && nextChar === '/') break; // Stop at comment

            if (char === '"' && (i === 0 || line[i - 1] !== '\\')) {
                inString = !inString;
                continue;
            }

            if (!inString) {
                if (char === '{' || char === '[') change++;
                else if (char === '}' || char === ']') change--;
            }
        }
        return change;
    }

    /**
     * Safely applies spacing to operators (:=, ->, commas) without destroying strings.
     */
    private formatLineSpacing(line: string): string {
        let result = "";
        let inString = false;
        let buffer = "";

        for (let i = 0; i < line.length; i++) {
            const char = line[i];
            
            // Toggle String State
            if (char === '"' && (i === 0 || line[i - 1] !== '\\')) {
                if (!inString) {
                    result += this.applySpacingRules(buffer);
                    buffer = "";
                } else {
                    result += buffer + '"';
                    buffer = "";
                    inString = false;
                    continue;
                }
                inString = true;
            }
            
            if (inString) {
                buffer += char;
            } else {
                // Stop formatting at comments
                if (char === '/' && line[i+1] === '/') {
                    result += this.applySpacingRules(buffer) + line.substring(i);
                    buffer = "";
                    break;
                }
                buffer += char;
            }
        }

        if (buffer.length > 0) {
            result += inString ? buffer : this.applySpacingRules(buffer);
        }

        return result;
    }

    /**
     * The actual regex rules for spacing Quirk code syntax.
     */
    private applySpacingRules(text: string): string {
        return text
            // Ensure exactly 1 space after commas (foo, bar)
            .replace(/,\s*/g, ', ')
            // Ensure exactly 1 space around operators and arrows
            .replace(/\s*(:=|->|==|!=|<=|>=|\+|-|\*|\/)\s*/g, ' $1 ')
            // Space out colons (url: String) but DON'T break := assignments
            .replace(/:(?!=)\s*/g, ': ')
            // Ensure space before opening curly brace
            .replace(/\s*\{\s*/g, ' { ')
            // Clean up accidental double spaces
            .replace(/ +/g, ' ')
            .trim();
    }
}