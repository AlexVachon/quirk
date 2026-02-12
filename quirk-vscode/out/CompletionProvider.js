"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.QuirkCompletionProvider = void 0;
const vscode = require("vscode");
const fs = require("fs");
const path = require("path");
class QuirkCompletionProvider {
    provideCompletionItems(document, position) {
        const linePrefix = document.lineAt(position).text.substring(0, position.character);
        // 1. Module Import: "use .core" or "use core."
        // Updated regex to allow leading dots
        const moduleImportMatch = /^\s*(?:use|from)\s+([.a-zA-Z0-9_]*)$/.exec(linePrefix);
        if (moduleImportMatch) {
            return this.provideModuleCompletions(document, moduleImportMatch[1]);
        }
        // 2. Symbol Import: "from .core use { "
        const symbolImportMatch = /^\s*from\s+([.a-zA-Z0-9_]+)\s+use\s+\{(.*)$/.exec(linePrefix);
        if (symbolImportMatch) {
            return this.provideSymbolCompletions(document, symbolImportMatch[1]);
        }
        return undefined;
    }
    provideModuleCompletions(document, typedPath) {
        let searchRoots = [];
        let lookFor = typedPath;
        const workspaceFolder = vscode.workspace.getWorkspaceFolder(document.uri);
        if (!workspaceFolder)
            return [];
        const rootPath = workspaceFolder.uri.fsPath;
        // --- RELATIVE IMPORT LOGIC ---
        if (typedPath.startsWith('.')) {
            let dotCount = 0;
            while (dotCount < typedPath.length && typedPath[dotCount] === '.') {
                dotCount++;
            }
            // Calculate base directory based on dots
            let searchDir = path.dirname(document.uri.fsPath);
            for (let i = 1; i < dotCount; i++) {
                searchDir = path.dirname(searchDir);
            }
            searchRoots = [searchDir];
            // Strip the dots for the file search part
            // e.g. ".sys" -> look for "sys" in current dir
            lookFor = typedPath.substring(dotCount);
        }
        // --- ABSOLUTE IMPORT LOGIC ---
        else {
            searchRoots = [
                path.join(rootPath, '.venv', 'lib', 'quirk', 'packages'),
                path.join(rootPath, '.venv', 'lib', 'quirk'),
                path.join(rootPath, 'quirk-compiler', '.venv', 'lib', 'quirk', 'packages'),
                path.join(rootPath, 'quirk-compiler', '.venv', 'lib', 'quirk'),
                path.join(rootPath, 'libs'),
                path.join(rootPath, 'src'),
                path.join(rootPath, 'quirk-compiler', 'libs')
            ];
        }
        // Handle subdirectories in the typed path (e.g. "collections.li")
        let parentDir = "";
        let filePrefix = lookFor.replace(/\./g, '/');
        if (filePrefix.includes('/')) {
            const lastSlash = filePrefix.lastIndexOf('/');
            parentDir = filePrefix.substring(0, lastSlash);
        }
        const completionItems = [];
        for (const root of searchRoots) {
            const targetPath = path.join(root, parentDir);
            if (fs.existsSync(targetPath) && fs.lstatSync(targetPath).isDirectory()) {
                try {
                    const files = fs.readdirSync(targetPath);
                    for (const file of files) {
                        if (file.startsWith('.') && file !== '..')
                            continue; // Skip hidden files but allow strict parent logic if needed
                        const fullPath = path.join(targetPath, file);
                        const isDir = fs.lstatSync(fullPath).isDirectory();
                        let name = file;
                        let kind = vscode.CompletionItemKind.Folder;
                        if (!isDir) {
                            if (!file.endsWith('.qk'))
                                continue;
                            if (file === '__init.qk')
                                continue;
                            name = file.substring(0, file.length - 3);
                            kind = vscode.CompletionItemKind.File;
                        }
                        else {
                            kind = vscode.CompletionItemKind.Module;
                        }
                        if (!completionItems.some(i => i.label === name)) {
                            completionItems.push(new vscode.CompletionItem(name, kind));
                        }
                    }
                }
                catch (e) { }
            }
        }
        return completionItems;
    }
    provideSymbolCompletions(document, modulePath) {
        // Reuse the logic from Definition Provider (but simplified here for brevity)
        // We need to resolve the path using the relative logic
        let filePath = this.resolvePath(document, modulePath);
        if (!filePath)
            return [];
        const items = [];
        // ... (Parsing logic matches previous version) ...
        try {
            const content = fs.readFileSync(filePath, 'utf-8');
            const lines = content.split(/\r?\n/);
            for (const line of lines) {
                const clean = line.replace(/\/\/.*$/, '').trim();
                const structMatch = /^\s*struct\s+([a-zA-Z0-9_]+)/.exec(clean);
                if (structMatch)
                    items.push(new vscode.CompletionItem(structMatch[1], vscode.CompletionItemKind.Struct));
                const funcMatch = /^\s*(?:extern\s+)?(?:define|def)\s+([a-zA-Z0-9_]+)/.exec(clean);
                if (funcMatch && !funcMatch[1].startsWith('__')) {
                    items.push(new vscode.CompletionItem(funcMatch[1], vscode.CompletionItemKind.Function));
                }
            }
        }
        catch (e) { }
        return items;
    }
    // Duplicated helper for now to avoid circular dependencies in this snippet
    resolvePath(document, modulePath) {
        const workspaceFolder = vscode.workspace.getWorkspaceFolder(document.uri);
        if (!workspaceFolder)
            return null;
        const rootPath = workspaceFolder.uri.fsPath;
        const currentFilePath = document.uri.fsPath;
        if (modulePath.startsWith('.')) {
            let dotCount = 0;
            while (dotCount < modulePath.length && modulePath[dotCount] === '.')
                dotCount++;
            const subPath = modulePath.substring(dotCount);
            let searchDir = path.dirname(currentFilePath);
            for (let i = 1; i < dotCount; i++)
                searchDir = path.dirname(searchDir);
            const relParts = subPath.replace(/\./g, '/');
            const v1 = path.join(searchDir, relParts + '.qk');
            const v2 = path.join(searchDir, relParts, '__init.qk');
            if (fs.existsSync(v1))
                return v1;
            if (fs.existsSync(v2))
                return v2;
            return null;
        }
        const relativePath = modulePath.replace(/\./g, '/');
        const searchRoots = [
            path.join(rootPath, '.venv', 'lib', 'quirk', 'packages'),
            path.join(rootPath, '.venv', 'lib', 'quirk'),
            path.join(rootPath, 'libs'),
            path.join(rootPath, 'src')
        ];
        for (const root of searchRoots) {
            const v1 = path.join(root, relativePath + '.qk');
            const v2 = path.join(root, relativePath, '__init.qk');
            if (fs.existsSync(v1))
                return v1;
            if (fs.existsSync(v2))
                return v2;
        }
        return null;
    }
}
exports.QuirkCompletionProvider = QuirkCompletionProvider;
//# sourceMappingURL=CompletionProvider.js.map