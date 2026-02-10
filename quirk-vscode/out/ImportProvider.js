"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.QuirkDefinitionProvider = void 0;
const vscode = require("vscode");
const path = require("path");
const fs = require("fs");
class QuirkDefinitionProvider {
    provideDefinition(document, position, token) {
        const lineText = document.lineAt(position).text;
        const wordRange = document.getWordRangeAtPosition(position);
        if (!wordRange)
            return null;
        const symbol = document.getText(wordRange); // e.g., "math" or "Vector3"
        // ---------------------------------------------------------
        // 1. Ignore Comments
        // ---------------------------------------------------------
        const textBeforeCursor = lineText.substring(0, wordRange.start.character);
        if (textBeforeCursor.includes('//'))
            return null;
        // ---------------------------------------------------------
        // 2. Handle Import Paths (Sub-path resolution)
        //    Matches: "use math.vectors" or "from math.vectors ..."
        // ---------------------------------------------------------
        const moduleMatch = /^\s*(?:use|from)\s+([a-zA-Z0-9_.]+)/.exec(lineText);
        if (moduleMatch) {
            const fullModulePath = moduleMatch[1]; // "math.vectors"
            const matchIndex = lineText.indexOf(fullModulePath); // Start index of "math.vectors"
            const matchEnd = matchIndex + fullModulePath.length;
            // Check if cursor is strictly INSIDE the module path string
            if (wordRange.start.character >= matchIndex && wordRange.end.character <= matchEnd) {
                // CRITICAL LOGIC: Take substring up to the end of the clicked word
                // Click "math"    -> substring(start, end of 'math')    -> "math"
                // Click "vectors" -> substring(start, end of 'vectors') -> "math.vectors"
                const clickedPath = lineText.substring(matchIndex, wordRange.end.character);
                const file = this.resolveModulePath(document, clickedPath);
                if (file) {
                    return new vscode.Location(vscode.Uri.file(file), new vscode.Position(0, 0));
                }
            }
        }
        // ---------------------------------------------------------
        // 3. Handle Explicit Symbols in Imports
        //    Matches: "... use { Vector3 }"
        // ---------------------------------------------------------
        const fromImportLineRegex = /^\s*from\s+([a-zA-Z0-9_.]+)\s+use\s+\{(.*)\}/;
        const fromMatch = fromImportLineRegex.exec(lineText);
        if (fromMatch) {
            const modulePath = fromMatch[1]; // "math.vectors"
            const importsContent = fromMatch[2]; // " Vector2, Vector3 "
            // If we are NOT on the module path (handled above), check if we are on a symbol
            if (importsContent.includes(symbol)) {
                const filePath = this.resolveModulePath(document, modulePath);
                if (filePath) {
                    const targetLoc = this.findSymbolInFile(filePath, symbol);
                    return targetLoc || new vscode.Location(vscode.Uri.file(filePath), new vscode.Position(0, 0));
                }
            }
        }
        // ---------------------------------------------------------
        // 4. Handle Usage (Go to Definition for variables/types)
        // ---------------------------------------------------------
        // 4a. Check Local File
        const localDef = this.findSymbolInFile(document.fileName, symbol);
        if (localDef && !localDef.range.contains(position))
            return localDef;
        // 4b. Check Imported Modules
        const text = document.getText();
        // Specific imports: from X use { Y }
        const fromRegex = /from\s+([a-zA-Z0-9_.]+)\s+use\s+\{([^}]+)\}/g;
        let match;
        while ((match = fromRegex.exec(text)) !== null) {
            if (match[2].includes(symbol)) {
                const filePath = this.resolveModulePath(document, match[1]);
                if (filePath)
                    return this.findSymbolInFile(filePath, symbol);
            }
        }
        // Wildcard imports: use X
        const useRegex = /^use\s+([a-zA-Z0-9_.]+)/gm;
        while ((match = useRegex.exec(text)) !== null) {
            const filePath = this.resolveModulePath(document, match[1]);
            if (filePath) {
                const targetLoc = this.findSymbolInFile(filePath, symbol);
                if (targetLoc)
                    return targetLoc;
            }
        }
        return null;
    }
    findSymbolInFile(filePath, symbol) {
        try {
            const content = fs.readFileSync(filePath, 'utf-8');
            const lines = content.split(/\r?\n/);
            for (let i = 0; i < lines.length; i++) {
                // Matches: "struct Vector2", "define main", "extern define print"
                const defRegex = new RegExp(`^\\s*(?:extern\\s+)?(?:struct|define)\\s+${symbol}\\b`);
                if (defRegex.test(lines[i])) {
                    return new vscode.Location(vscode.Uri.file(filePath), new vscode.Position(i, 0));
                }
            }
        }
        catch (e) { }
        return null;
    }
    resolveModulePath(document, modulePath) {
        const workspaceFolder = vscode.workspace.getWorkspaceFolder(document.uri);
        if (!workspaceFolder)
            return null;
        const rootPath = workspaceFolder.uri.fsPath;
        const relativePath = modulePath.replace(/\./g, '/');
        const searchRoots = [
            path.join(rootPath, 'libs'),
            path.join(rootPath, 'src'),
            path.join(rootPath, 'quirk-compiler', 'libs'),
            path.join(rootPath, 'quirk-compiler', 'src'),
            rootPath
        ];
        const variants = [
            relativePath + '.qk',
            path.join(relativePath, '__init.qk')
        ];
        for (const root of searchRoots) {
            for (const variant of variants) {
                const fullPath = path.join(root, variant);
                if (fs.existsSync(fullPath))
                    return fullPath;
            }
        }
        return null;
    }
}
exports.QuirkDefinitionProvider = QuirkDefinitionProvider;
//# sourceMappingURL=ImportProvider.js.map