import * as vscode from 'vscode';
import * as fs from 'fs';
import * as path from 'path';

export class QuirkCompletionProvider implements vscode.CompletionItemProvider {
    
    public provideCompletionItems(
        document: vscode.TextDocument,
        position: vscode.Position
    ): vscode.ProviderResult<vscode.CompletionItem[]> {

        const linePrefix = document.lineAt(position).text.substring(0, position.character);

        // 1. Module Import: "use core."
        const moduleImportMatch = /^\s*(?:use|from)\s+([a-zA-Z0-9_.]*)$/.exec(linePrefix);
        if (moduleImportMatch) {
            return this.provideModuleCompletions(document, moduleImportMatch[1]);
        }

        // 2. Symbol Import: "from core.list use { "
        const symbolImportMatch = /^\s*from\s+([a-zA-Z0-9_.]+)\s+use\s+\{(.*)$/.exec(linePrefix);
        if (symbolImportMatch) {
            return this.provideSymbolCompletions(document, symbolImportMatch[1]);
        }

        return undefined;
    }

    // --- Module Suggestions (Folders/Files) ---
    private provideModuleCompletions(document: vscode.TextDocument, typedPath: string): vscode.CompletionItem[] {
        let relativeDir = typedPath.replace(/\./g, '/');
        let parentDir = "";

        if (relativeDir.endsWith('/')) {
            parentDir = relativeDir;
        } else {
            const lastSlash = relativeDir.lastIndexOf('/');
            if (lastSlash !== -1) {
                parentDir = relativeDir.substring(0, lastSlash + 1);
            } else {
                parentDir = "";
            }
        }

        const workspaceFolder = vscode.workspace.getWorkspaceFolder(document.uri);
        if (!workspaceFolder) return [];
        const rootPath = workspaceFolder.uri.fsPath;

        const searchRoots = [
            path.join(rootPath, 'libs'),
            path.join(rootPath, 'src'),
            path.join(rootPath, 'quirk-compiler', 'libs'),
            path.join(rootPath, 'quirk-compiler', 'src')
        ];

        const completionItems: vscode.CompletionItem[] = [];

        for (const root of searchRoots) {
            const targetPath = path.join(root, parentDir);

            if (fs.existsSync(targetPath) && fs.lstatSync(targetPath).isDirectory()) {
                try {
                    const files = fs.readdirSync(targetPath);
                    for (const file of files) {
                        if (file.startsWith('.')) continue;

                        const fullPath = path.join(targetPath, file);
                        const isDir = fs.lstatSync(fullPath).isDirectory();
                        let name = file;
                        let kind = vscode.CompletionItemKind.Folder;

                        if (!isDir) {
                            if (!file.endsWith('.qk')) continue;
                            if (file === '__init.qk') continue;
                            name = file.substring(0, file.length - 3);
                            kind = vscode.CompletionItemKind.File;
                        } else {
                            kind = vscode.CompletionItemKind.Module;
                        }
                        
                        // Prevent duplicates
                        if (!completionItems.some(i => i.label === name)) {
                            completionItems.push(new vscode.CompletionItem(name, kind));
                        }
                    }
                } catch (e) {}
            }
        }
        return completionItems;
    }

    // --- Symbol Suggestions (Top-level Structs/Defines only) ---
    private provideSymbolCompletions(document: vscode.TextDocument, modulePath: string): vscode.CompletionItem[] {
        const filePath = this.resolveModulePath(document, modulePath);
        if (!filePath) return [];

        const items: vscode.CompletionItem[] = [];
        let braceDepth = 0;
        
        try {
            const content = fs.readFileSync(filePath, 'utf-8');
            const lines = content.split(/\r?\n/);

            for (const line of lines) {
                // 1. Remove comments to avoid false brace counting
                const cleanLine = line.replace(/\/\/.*$/, '').trim();
                if (cleanLine.length === 0) continue;

                // 2. ONLY check for definitions if we are at Top Level (Depth 0)
                if (braceDepth === 0) {
                    // Match "struct List"
                    const structMatch = /^\s*struct\s+([a-zA-Z0-9_]+)/.exec(cleanLine);
                    if (structMatch) {
                        const item = new vscode.CompletionItem(structMatch[1], vscode.CompletionItemKind.Struct);
                        item.detail = `struct ${structMatch[1]}`;
                        items.push(item);
                    }

                    // Match "define myFunc" (but NOT "extern define" inside implied scope if brace logic fails)
                    // We assume standard formatting: top level defines start at beginning of line or depth 0
                    const funcMatch = /^\s*(?:extern\s+)?define\s+([a-zA-Z0-9_]+)/.exec(cleanLine);
                    if (funcMatch) {
                        const funcName = funcMatch[1];
                        if (!funcName.startsWith('__')) { // Optional: Hide internal dunder methods
                            const item = new vscode.CompletionItem(funcName, vscode.CompletionItemKind.Function);
                            item.detail = `function ${funcName}`;
                            items.push(item);
                        }
                    }
                }

                // 3. Update Brace Depth
                for (const char of cleanLine) {
                    if (char === '{') braceDepth++;
                    if (char === '}') braceDepth--;
                }
            }
        } catch (e) {
            console.error(`Error parsing module ${filePath}:`, e);
        }

        return items;
    }

    private resolveModulePath(document: vscode.TextDocument, modulePath: string): string | null {
        const workspaceFolder = vscode.workspace.getWorkspaceFolder(document.uri);
        if (!workspaceFolder) return null;

        const rootPath = workspaceFolder.uri.fsPath;
        const relativePath = modulePath.replace(/\./g, '/');

        const searchRoots = [
            path.join(rootPath, 'libs'),
            path.join(rootPath, 'src'),
            path.join(rootPath, 'quirk-compiler', 'libs'),
            path.join(rootPath, 'quirk-compiler', 'src')
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
}