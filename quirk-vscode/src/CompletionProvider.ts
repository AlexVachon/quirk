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
            // 1. Root Venv
            path.join(rootPath, '.venv', 'lib', 'quirk', 'packages'),
            path.join(rootPath, '.venv', 'lib', 'quirk'),
            // 2. Subfolder Venv (Fix for parent folder workspace)
            path.join(rootPath, 'quirk-compiler', '.venv', 'lib', 'quirk', 'packages'),
            path.join(rootPath, 'quirk-compiler', '.venv', 'lib', 'quirk'),
            // 3. Local
            path.join(rootPath, 'libs'),
            path.join(rootPath, 'src'),
            path.join(rootPath, 'quirk-compiler', 'libs')
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

                        if (!completionItems.some(i => i.label === name)) {
                            completionItems.push(new vscode.CompletionItem(name, kind));
                        }
                    }
                } catch (e) {}
            }
        }
        return completionItems;
    }

    private provideSymbolCompletions(document: vscode.TextDocument, modulePath: string): vscode.CompletionItem[] {
        const filePath = this.resolveModulePath(document, modulePath);
        if (!filePath) return [];

        const items: vscode.CompletionItem[] = [];
        let braceDepth = 0;
        
        try {
            const content = fs.readFileSync(filePath, 'utf-8');
            const lines = content.split(/\r?\n/);

            for (const line of lines) {
                const cleanLine = line.replace(/\/\/.*$/, '').trim();
                if (cleanLine.length === 0) continue;

                if (braceDepth === 0) {
                    const structMatch = /^\s*struct\s+([a-zA-Z0-9_]+)/.exec(cleanLine);
                    if (structMatch) {
                        const item = new vscode.CompletionItem(structMatch[1], vscode.CompletionItemKind.Struct);
                        item.detail = `struct ${structMatch[1]}`;
                        items.push(item);
                    }

                    const funcMatch = /^\s*(?:extern\s+)?define\s+([a-zA-Z0-9_]+)/.exec(cleanLine);
                    if (funcMatch) {
                        const funcName = funcMatch[1];
                        if (!funcName.startsWith('__')) {
                            const item = new vscode.CompletionItem(funcName, vscode.CompletionItemKind.Function);
                            item.detail = `function ${funcName}`;
                            items.push(item);
                        }
                    }
                }

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
            path.join(rootPath, '.venv', 'lib', 'quirk', 'packages'),
            path.join(rootPath, '.venv', 'lib', 'quirk'),
            // Fix:
            path.join(rootPath, 'quirk-compiler', '.venv', 'lib', 'quirk', 'packages'),
            path.join(rootPath, 'quirk-compiler', '.venv', 'lib', 'quirk'),
            
            path.join(rootPath, 'libs'),
            path.join(rootPath, 'src'),
            path.join(rootPath, 'quirk-compiler', 'libs')
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