import * as vscode from 'vscode';

export class QuirkHoverProvider implements vscode.HoverProvider {
    public provideHover(
        document: vscode.TextDocument,
        position: vscode.Position,
        token: vscode.CancellationToken
    ): vscode.ProviderResult<vscode.Hover> {
        const range = document.getWordRangeAtPosition(position);
        if (!range) return null;

        const word = document.getText(range);

        // 1. Hover for Keywords
        if (word === 'use' || word === 'from') {
            return new vscode.Hover('**Keyword**: Import a module or specific symbols.\n\nExample: `use core.sys`');
        }
        if (word === 'define') {
            return new vscode.Hover('**Keyword**: Define a function.\n\nExample: `define main() -> void { ... }`');
        }
        if (word === 'struct') {
            return new vscode.Hover('**Keyword**: Define a data structure.\n\nExample: `struct Vector { x: double }`');
        }

        // 2. Hover for Standard Types
        if (['int', 'double', 'bool', 'cstring'].includes(word)) {
            return new vscode.Hover(`**Primitive Type**: \`${word}\`\n\nBasic data type supported by the runtime.`);
        }
        if (['String', 'List', 'Map', 'File'].includes(word)) {
            return new vscode.Hover(`**Standard Library Class**: \`${word}\`\n\nPart of the Quirk Core library.`);
        }

        return null;
    }
}