import * as vscode from 'vscode';
import * as path from 'path';
import * as fs from 'fs';

export class QuirkDefinitionProvider implements vscode.DefinitionProvider {

    public provideDefinition(
        document: vscode.TextDocument,
        position: vscode.Position,
        token: vscode.CancellationToken
    ): vscode.ProviderResult<vscode.Definition> {

        const workspaceFolder = vscode.workspace.getWorkspaceFolder(document.uri);
        if (!workspaceFolder) return null;
        const rootPath = workspaceFolder.uri.fsPath;
        const currentFilePath = document.uri.fsPath;

        const lineText = document.lineAt(position).text;
        const wordRange = document.getWordRangeAtPosition(position, /[a-zA-Z0-9_.]+/); // Fixed range to include dots
        if (!wordRange) return null;

        const symbol = document.getText(wordRange);

        // 1. Ignore Comments
        const textBeforeCursor = lineText.substring(0, wordRange.start.character);
        if (textBeforeCursor.includes('//')) return null;

        // ---------------------------------------------------------
        // 2. Handle Module Imports (Clicking the path itself)
        //    Matches: "use .sys" OR "from ..utils"
        // ---------------------------------------------------------
        // Regex updated to allow leading dots: \.?\.*
        const moduleMatch = /^\s*(?:use|from)\s+([.a-zA-Z0-9_]+)/.exec(lineText);

        if (moduleMatch) {
            const fullModulePath = moduleMatch[1];
            const matchIndex = lineText.indexOf(fullModulePath);
            const matchEnd = matchIndex + fullModulePath.length;

            if (wordRange.start.character >= matchIndex && wordRange.end.character <= matchEnd) {
                // Determine exactly which part was clicked
                // For relative imports, we usually just want to resolve the whole thing if clicked anywhere
                const file = this.resolvePath(rootPath, currentFilePath, fullModulePath);
                if (file) {
                    return new vscode.Location(vscode.Uri.file(file), new vscode.Position(0, 0));
                }
            }
        }

        // ---------------------------------------------------------
        // 3. Handle Symbol Imports (Clicking the symbol inside {})
        // ---------------------------------------------------------
        const fromImportRegex = /^\s*from\s+([.a-zA-Z0-9_]+)\s+use\s+\{([^}]*)/;
        const fromMatch = fromImportRegex.exec(lineText);

        if (fromMatch) {
            const modulePath = fromMatch[1];
            const importsContent = fromMatch[2];
            const openBraceIndex = lineText.indexOf('{');

            if (position.character > openBraceIndex) {
                if (importsContent.includes(symbol)) {
                    const filePath = this.resolvePath(rootPath, currentFilePath, modulePath);
                    if (filePath) {
                        const targetLoc = this.findDefinitionDeep(rootPath, filePath, symbol);
                        return targetLoc || new vscode.Location(vscode.Uri.file(filePath), new vscode.Position(0, 0));
                    }
                }
            }
        }

        // ---------------------------------------------------------
        // 4. Handle Usage in Code (Deep Search)
        // ---------------------------------------------------------

        // A. Check Local File
        const localDef = this.findSymbolInFile(document.fileName, symbol);
        if (localDef && !localDef.range.contains(position)) return localDef;

        // B. Check Imports
        const text = document.getText();

        // Scan "from ... use { ... }"
        const fromRegex = /from\s+([.a-zA-Z0-9_]+)\s+use\s+\{([^}]+)\}/g;
        let match;
        while ((match = fromRegex.exec(text)) !== null) {
            if (match[2].includes(symbol)) {
                const filePath = this.resolvePath(rootPath, currentFilePath, match[1]);
                if (filePath) {
                    return this.findDefinitionDeep(rootPath, filePath, symbol);
                }
            }
        }

        return null;
    }

    // ---------------------------------------------------------
    // HELPER: Path Resolution (Handles Relative & Absolute)
    // ---------------------------------------------------------
    private resolvePath(rootPath: string, currentFile: string, modulePath: string): string | null {
        // 1. Relative Imports (starts with .)
        if (modulePath.startsWith('.')) {
            // Count dots: "." = 1 (current), ".." = 2 (parent)
            let dotCount = 0;
            while (dotCount < modulePath.length && modulePath[dotCount] === '.') {
                dotCount++;
            }

            const subPath = modulePath.substring(dotCount); // "sys" from ".sys"
            let searchDir = path.dirname(currentFile);

            // Move up directories (start at 1 because dirname is already current dir)
            for (let i = 1; i < dotCount; i++) {
                searchDir = path.dirname(searchDir);
            }

            const relativeParts = subPath.replace(/\./g, '/'); // "utils.math" -> "utils/math"

            const variants = [
                path.join(searchDir, relativeParts + '.qk'),
                path.join(searchDir, relativeParts, '__init.qk')
            ];

            for (const v of variants) {
                if (fs.existsSync(v)) return v;
            }
            return null;
        }

        // 2. Absolute Imports
        const relativePath = modulePath.replace(/\./g, '/');
        const searchRoots = [
            path.join(rootPath, '.venv', 'lib', 'quirk', 'packages'),
            path.join(rootPath, '.venv', 'lib', 'quirk'),
            path.join(rootPath, 'quirk-compiler', '.venv', 'lib', 'quirk', 'packages'),
            path.join(rootPath, 'quirk-compiler', '.venv', 'lib', 'quirk'),
            path.join(rootPath, 'libs'),
            path.join(rootPath, 'src'),
            path.join(rootPath, 'quirk-compiler', 'libs'),
            rootPath
        ];

        const variants = [
            relativePath + '.qk',
            path.join(relativePath, '__init.qk')
        ];

        for (const root of searchRoots) {
            for (const variant of variants) {
                const fullPath = path.join(root, variant);
                if (fs.existsSync(fullPath)) return fullPath;
            }
        }
        return null;
    }

    private findDefinitionDeep(rootPath: string, filePath: string, symbol: string, visited: Set<string> = new Set()): vscode.Location | null {
        if (visited.has(filePath)) return null;
        visited.add(filePath);

        const loc = this.findSymbolInFile(filePath, symbol);
        if (loc) return loc;

        try {
            const content = fs.readFileSync(filePath, 'utf-8');
            const lines = content.split(/\r?\n/);
            for (const line of lines) {
                const clean = line.replace(/\/\/.*$/, '').trim();

                // Recursively follow "from .sub use { ... }" or "use .sub"
                const importMatch = /^\s*(?:use|from)\s+([.a-zA-Z0-9_]+)/.exec(clean);
                if (importMatch) {
                    const resolvedPath = this.resolvePath(rootPath, filePath, importMatch[1]);
                    if (resolvedPath) {
                        const subLoc = this.findDefinitionDeep(rootPath, resolvedPath, symbol, visited);
                        if (subLoc) return subLoc;
                    }
                }
            }
        } catch (e) { }
        return null;
    }

    private findSymbolInFile(filePath: string, symbol: string): vscode.Location | null {
        try {
            const content = fs.readFileSync(filePath, 'utf-8');
            const lines = content.split(/\r?\n/);
            for (let i = 0; i < lines.length; i++) {
                const defRegex = new RegExp(`^\\s*(?:extern\\s+)?(?:struct|define|def|init)\\s+${symbol}\\b`);
                if (defRegex.test(lines[i])) {
                    return new vscode.Location(vscode.Uri.file(filePath), new vscode.Position(i, 0));
                }
            }
        } catch (e) { }
        return null;
    }
}