"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.QuirkDefinitionProvider = void 0;
const vscode = require("vscode");
const path = require("path");
const fs = require("fs");
class QuirkDefinitionProvider {
    provideDefinition(document, position, token) {
        const workspaceFolder = vscode.workspace.getWorkspaceFolder(document.uri);
        if (!workspaceFolder)
            return null;
        const rootPath = workspaceFolder.uri.fsPath;
        const lineText = document.lineAt(position).text;
        const wordRange = document.getWordRangeAtPosition(position);
        if (!wordRange)
            return null;
        const symbol = document.getText(wordRange);
        // 1. Ignore Comments
        const textBeforeCursor = lineText.substring(0, wordRange.start.character);
        if (textBeforeCursor.includes('//'))
            return null;
        // ---------------------------------------------------------
        // 2. Handle Module Imports (Clicking the path itself)
        //    Matches: "use math.vectors" OR "from math.vectors"
        // ---------------------------------------------------------
        const moduleMatch = /^\s*(?:use|from)\s+([a-zA-Z0-9_.]+)/.exec(lineText);
        if (moduleMatch) {
            const fullModulePath = moduleMatch[1];
            const matchIndex = lineText.indexOf(fullModulePath);
            const matchEnd = matchIndex + fullModulePath.length;
            // Check if cursor is specifically on the module path string
            if (wordRange.start.character >= matchIndex && wordRange.end.character <= matchEnd) {
                // Resolve the clicked segment (e.g. clicking 'math' in 'math.vectors')
                const segment = document.getText(wordRange);
                const segmentIndex = fullModulePath.indexOf(segment);
                let clickedPath = fullModulePath;
                if (segmentIndex !== -1) {
                    clickedPath = fullModulePath.substring(0, segmentIndex + segment.length);
                }
                const file = this.resolvePathFromRoot(rootPath, clickedPath);
                if (file) {
                    return new vscode.Location(vscode.Uri.file(file), new vscode.Position(0, 0));
                }
            }
        }
        // ---------------------------------------------------------
        // 3. Handle Symbol Imports (Clicking the symbol inside {})
        //    Matches: "from math use { Vector2 }"
        // ---------------------------------------------------------
        const fromImportRegex = /^\s*from\s+([a-zA-Z0-9_.]+)\s+use\s+\{([^}]*)/;
        const fromMatch = fromImportRegex.exec(lineText);
        if (fromMatch) {
            const modulePath = fromMatch[1];
            const importsContent = fromMatch[2];
            const openBraceIndex = lineText.indexOf('{');
            // Ensure cursor is inside the definition block (after '{')
            if (position.character > openBraceIndex) {
                if (importsContent.includes(symbol)) {
                    const filePath = this.resolvePathFromRoot(rootPath, modulePath);
                    if (filePath) {
                        // --- NEW: Deep Search ---
                        // recursively check imports inside the target file
                        const targetLoc = this.findDefinitionDeep(rootPath, filePath, symbol);
                        return targetLoc || new vscode.Location(vscode.Uri.file(filePath), new vscode.Position(0, 0));
                    }
                }
            }
        }
        // ---------------------------------------------------------
        // 4. Handle Usage in Code (General Go to Definition)
        // ---------------------------------------------------------
        // A. Check Local File First
        const localDef = this.findSymbolInFile(document.fileName, symbol);
        if (localDef && !localDef.range.contains(position))
            return localDef;
        // B. Check Imported Modules
        const text = document.getText();
        // Scan "from X use { Y }"
        const fromRegex = /from\s+([a-zA-Z0-9_.]+)\s+use\s+\{([^}]+)\}/g;
        let match;
        while ((match = fromRegex.exec(text)) !== null) {
            if (match[2].includes(symbol)) {
                const filePath = this.resolvePathFromRoot(rootPath, match[1]);
                if (filePath) {
                    return this.findDefinitionDeep(rootPath, filePath, symbol);
                }
            }
        }
        // Scan "use X"
        const useRegex = /^use\s+([a-zA-Z0-9_.]+)/gm;
        while ((match = useRegex.exec(text)) !== null) {
            const filePath = this.resolvePathFromRoot(rootPath, match[1]);
            if (filePath) {
                // For 'use math', we generally don't deep search automatically unless strictly necessary
                // to avoid performance hits, but for "Go to Definition" it's usually acceptable.
                const targetLoc = this.findDefinitionDeep(rootPath, filePath, symbol);
                if (targetLoc)
                    return targetLoc;
            }
        }
        return null;
    }
    // ---------------------------------------------------------
    // HELPER: Deep Search (Recursive)
    // ---------------------------------------------------------
    findDefinitionDeep(rootPath, filePath, symbol, visited = new Set()) {
        // Prevent infinite loops (circular imports)
        if (visited.has(filePath))
            return null;
        visited.add(filePath);
        // 1. Check current file
        const loc = this.findSymbolInFile(filePath, symbol);
        if (loc)
            return loc;
        // 2. Scan for re-exports / sub-imports
        // We look for 'use <mod>' lines in this file
        try {
            const content = fs.readFileSync(filePath, 'utf-8');
            const lines = content.split(/\r?\n/);
            for (const line of lines) {
                const clean = line.replace(/\/\/.*$/, '').trim();
                if (!clean.startsWith('use') && !clean.startsWith('from'))
                    continue;
                // Match "use math.vectors"
                const useMatch = /^\s*use\s+([a-zA-Z0-9_.]+)/.exec(clean);
                if (useMatch) {
                    const subModPath = useMatch[1];
                    const resolvedSubPath = this.resolvePathFromRoot(rootPath, subModPath);
                    if (resolvedSubPath) {
                        const subLoc = this.findDefinitionDeep(rootPath, resolvedSubPath, symbol, visited);
                        if (subLoc)
                            return subLoc;
                    }
                }
                // Match "from math.vectors use ..." (Re-exporting symbols)
                // If the file does 'from ... use { Vector2 }', we should follow that too.
                const fromMatch = /^\s*from\s+([a-zA-Z0-9_.]+)\s+use/.exec(clean);
                if (fromMatch) {
                    const subModPath = fromMatch[1];
                    const resolvedSubPath = this.resolvePathFromRoot(rootPath, subModPath);
                    if (resolvedSubPath) {
                        const subLoc = this.findDefinitionDeep(rootPath, resolvedSubPath, symbol, visited);
                        if (subLoc)
                            return subLoc;
                    }
                }
            }
        }
        catch (e) { }
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
    resolvePathFromRoot(rootPath, modulePath) {
        const relativePath = modulePath.replace(/\./g, '/');
        const searchRoots = [
            // Priority 1: VENV
            path.join(rootPath, '.venv', 'lib', 'quirk', 'packages'),
            path.join(rootPath, '.venv', 'lib', 'quirk'),
            // Priority 2: Compiler VENV
            path.join(rootPath, 'quirk-compiler', '.venv', 'lib', 'quirk', 'packages'),
            path.join(rootPath, 'quirk-compiler', '.venv', 'lib', 'quirk'),
            // Priority 3: Local Libs
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
                if (fs.existsSync(fullPath))
                    return fullPath;
            }
        }
        return null;
    }
}
exports.QuirkDefinitionProvider = QuirkDefinitionProvider;
//# sourceMappingURL=ImportProvider.js.map