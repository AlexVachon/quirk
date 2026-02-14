import * as vscode from 'vscode';
import * as fs from 'fs';
import * as path from 'path';

export class QuirkCompletionProvider implements vscode.CompletionItemProvider {
    private outputChannel: vscode.OutputChannel;

    constructor(outputChannel: vscode.OutputChannel) {
        this.outputChannel = outputChannel;
    }

    public provideCompletionItems(
        document: vscode.TextDocument, 
        position: vscode.Position
    ): vscode.ProviderResult<vscode.CompletionItem[]> {
        
        const linePrefix = document.lineAt(position).text.substring(0, position.character);

        // 1. DESTRUCTURING IMPORT
        const destructureMatch = /^\s*from\s+([.a-zA-Z0-9_/]+)\s+use\s+\{([^}]*)$/.exec(linePrefix);
        if (destructureMatch) {
            return this.provideDestructureCompletions(document, destructureMatch[1]);
        }

        // 2. IMPORT PATHS
        const importMatch = /^\s*(?:use|from)\s+([.a-zA-Z0-9_/]*)$/.exec(linePrefix);
        if (importMatch) {
            return this.providePathCompletions(document, importMatch[1]);
        }

        // 3. DOT ACCESS (Module Member OR Object Property)
        const memberMatch = /([a-zA-Z0-9_]+)\.$/.exec(linePrefix);
        if (memberMatch) {
            const aliasOrVar = memberMatch[1];

            const modulePath = this.resolveImportPathFromAlias(document, aliasOrVar);
            if (modulePath) {
                return this.provideMemberCompletions(document, modulePath);
            }

            return this.provideObjectMemberCompletions(document, position, aliasOrVar);
        }

        // 4. GENERAL COMPLETIONS
        const wordMatch = /[a-zA-Z0-9_]+$/.exec(linePrefix);
        if (wordMatch || linePrefix.trim() === '') {
            return this.provideGeneralCompletions(document, position);
        }

        return undefined;
    }

    // =========================================================
    // TYPE INFERENCE & OBJECT COMPLETIONS
    // =========================================================

    private provideObjectMemberCompletions(document: vscode.TextDocument, position: vscode.Position, variableName: string): vscode.CompletionItem[] {
        const typeName = this.inferTypeOfVariable(document, position, variableName);
        if (!typeName) return [];

        const projectRoot = this.findProjectRoot(document.uri.fsPath);
        return this.getStructMembers(projectRoot, document.uri.fsPath, typeName);
    }

    private inferTypeOfVariable(document: vscode.TextDocument, position: vscode.Position, variableName: string): string | null {
        const textBeforeCursor = document.getText(new vscode.Range(new vscode.Position(0, 0), position));
        const lines = textBeforeCursor.split(/\r?\n/).reverse(); 

        if (variableName === 'self') {
            for (const line of lines) {
                let m = /extend\s+([a-zA-Z0-9_]+)/.exec(line);
                if (m) return m[1];
                m = /(?:define|def|init)\s+([a-zA-Z0-9_]+)_/.exec(line);
                if (m) return m[1];
            }
        }

        for (const line of lines) {
            let match = new RegExp(`\\b${variableName}\\s*:=\\s*(?:[a-zA-Z0-9_]+\\.)?([A-Za-z0-9_]+)`).exec(line);
            if (match) return match[1];

            match = new RegExp(`\\b${variableName}\\s*:\\s*(?:[a-zA-Z0-9_]+\\.)?([A-Za-z0-9_]+)`).exec(line);
            if (match) return match[1];
        }

        return null;
    }

    private getStructMembers(projectRoot: string, currentFile: string, structName: string): vscode.CompletionItem[] {
        const targetFile = this.findFileContainingStruct(projectRoot, currentFile, structName);
        if (!targetFile) return [];

        const items: vscode.CompletionItem[] = [];
        const seen = new Set<string>();
        const content = fs.readFileSync(targetFile, 'utf-8');

        const addCompletion = (label: string, kind: vscode.CompletionItemKind) => {
            if (!seen.has(label)) {
                seen.add(label);
                const item = new vscode.CompletionItem(label, kind);
                
                // --- NEW STRICT SORTING HIERARCHY ---
                if (label.startsWith('__')) {
                    item.sortText = '3_' + label; // Magic methods at the absolute bottom
                } else if (kind === vscode.CompletionItemKind.Field) {
                    item.sortText = '1_' + label; // Properties/Fields at the absolute top
                } else {
                    item.sortText = '2_' + label; // Normal methods in the middle
                }
                
                items.push(item);
            }
        };

        const extractBody = (keyword: string, name: string) => {
            const regex = new RegExp(`\\b${keyword}\\s+${name}\\b`);
            const matchIndex = content.search(regex);
            if (matchIndex !== -1) {
                const startIndex = content.indexOf('{', matchIndex);
                if (startIndex !== -1) {
                    let braceCount = 1;
                    let endIndex = startIndex + 1;
                    while (endIndex < content.length && braceCount > 0) {
                        if (content[endIndex] === '{') braceCount++;
                        else if (content[endIndex] === '}') braceCount--;
                        endIndex++;
                    }
                    return content.substring(startIndex + 1, endIndex - 1);
                }
            }
            return null;
        };

        // 1. Scan Nested Struct Body
        const structBody = extractBody('struct', structName);
        if (structBody) {
            const lines = structBody.split(/\r?\n/);
            for (const line of lines) {
                const fieldMatch = /^\s*([a-zA-Z0-9_]+)\s*:\s*[a-zA-Z0-9_]+/.exec(line);
                if (fieldMatch && !line.includes('(') && !line.includes('return') && !line.includes('=')) {
                    addCompletion(fieldMatch[1], vscode.CompletionItemKind.Field);
                }
                const methodMatch = /^\s*(?:extern\s+)?(?:define|def)\s+([a-zA-Z0-9_]+)/.exec(line);
                if (methodMatch) {
                    const methodName = methodMatch[1];
                    if (methodName !== '__init' && methodName !== 'init') { 
                        addCompletion(methodName, vscode.CompletionItemKind.Method);
                    }
                }
            }
        }

        // 2. Scan Nested Extend Body
        const extendBody = extractBody('extend', structName);
        if (extendBody) {
            const lines = extendBody.split(/\r?\n/);
            for (const line of lines) {
                const methodMatch = /^\s*(?:extern\s+)?(?:define|def)\s+([a-zA-Z0-9_]+)/.exec(line);
                if (methodMatch) {
                    const methodName = methodMatch[1];
                    if (methodName !== '__init' && methodName !== 'init') {
                        addCompletion(methodName, vscode.CompletionItemKind.Method);
                    }
                }
            }
        }

        // 3. Scan C-Style Methods
        const methodRegex = new RegExp(`(?:define|def|init)\\s+${structName}_([a-zA-Z0-9_]+)`, 'g');
        let methodMatch;
        while ((methodMatch = methodRegex.exec(content)) !== null) {
            let methodName = methodMatch[1];
            if (methodName !== '_init' && methodName !== 'init') {
                addCompletion(methodName, vscode.CompletionItemKind.Method);
            }
        }

        return items;
    }

    private findFileContainingStruct(projectRoot: string, currentFile: string, structName: string): string | null {
        let content = fs.readFileSync(currentFile, 'utf-8');
        if (new RegExp(`\\bstruct\\s+${structName}\\b`).test(content)) return currentFile;

        const lines = content.split(/\r?\n/);
        for (const line of lines) {
            const importMatch = /^\s*(?:use|from)\s+([.a-zA-Z0-9_/]+)/.exec(line);
            if (importMatch) {
                const resolvedFile = this.resolvePath(projectRoot, currentFile, importMatch[1]);
                if (resolvedFile) {
                    const deepFile = this.deepFindStruct(projectRoot, resolvedFile, structName);
                    if (deepFile) return deepFile;
                }
            }
        }

        const implicitCores = ['core', 'core.sys', 'core.string', 'core.collections.list', 'core.collections.map', 'core.primitives'];
        for (const coreMod of implicitCores) {
            const coreFile = this.resolvePath(projectRoot, currentFile, coreMod);
            if (coreFile) {
                const deepFile = this.deepFindStruct(projectRoot, coreFile, structName);
                if (deepFile) return deepFile;
            }
        }

        return null;
    }

    private deepFindStruct(projectRoot: string, filePath: string, structName: string, visited = new Set<string>()): string | null {
        if (visited.has(filePath)) return null;
        visited.add(filePath);
        
        try {
            const content = fs.readFileSync(filePath, 'utf-8');
            if (new RegExp(`\\bstruct\\s+${structName}\\b`).test(content)) return filePath;
            
            const reExportRegex = /from\s+([.a-zA-Z0-9_/]+)\s+use\s+\{([^}]*)\}/g;
            let match;
            while ((match = reExportRegex.exec(content)) !== null) {
                const symbols = match[2];
                if (new RegExp(`\\b${structName}\\b`).test(symbols)) {
                    const targetFile = this.resolvePath(projectRoot, filePath, match[1]);
                    if (targetFile) {
                        const found = this.deepFindStruct(projectRoot, targetFile, structName, visited);
                        if (found) return found;
                    }
                }
            }
        } catch (e) {}
        return null;
    }

    // =========================================================
    // STANDARD COMPLETIONS
    // =========================================================

    private provideGeneralCompletions(document: vscode.TextDocument, position: vscode.Position): vscode.CompletionItem[] {
        const items: vscode.CompletionItem[] = [];
        const seen = new Set<string>();

        const addCompletion = (label: string, kind: vscode.CompletionItemKind, detail?: string) => {
            if (!seen.has(label)) {
                seen.add(label);
                const item = new vscode.CompletionItem(label, kind);
                if (detail) item.detail = detail;
                
                if (label.startsWith('__')) {
                    item.sortText = '3_' + label;
                } else {
                    item.sortText = '2_' + label;
                }

                items.push(item);
            }
        };

        const fullText = document.getText();
        const textBeforeCursor = document.getText(new vscode.Range(new vscode.Position(0, 0), position));

        ['define', 'struct', 'if', 'else', 'elif', 'while', 'for', 'in', 'return', 'break', 'continue', 'use', 'from', 'with', 'as', 'extern', 'true', 'false', 'null'].forEach(kw => addCompletion(kw, vscode.CompletionItemKind.Keyword));
        ['print', 'printf', 'malloc', 'free', 'exit', 'String', 'List', 'Map', 'File', 'int', 'double', 'bool', 'cstring', 'any', 'void'].forEach(bi => addCompletion(bi, vscode.CompletionItemKind.Reference, 'Built-in'));

        let match;
        const varRegex = /([a-zA-Z0-9_]+)\s*:=/g;
        while ((match = varRegex.exec(textBeforeCursor)) !== null) addCompletion(match[1], vscode.CompletionItemKind.Variable, 'Local Variable');

        const defRegex = /^\s*(?:extern\s+)?(?:define|def|init|struct)\s+([a-zA-Z0-9_]+)/gm;
        while ((match = defRegex.exec(fullText)) !== null) addCompletion(match[1], match[0].includes('struct') ? vscode.CompletionItemKind.Struct : vscode.CompletionItemKind.Function, 'Local Definition');

        const projectRoot = this.findProjectRoot(document.uri.fsPath);
        const importRegex = /^\s*(?:use|from)\s+([.a-zA-Z0-9_/]+)/gm;
        while ((match = importRegex.exec(fullText)) !== null) {
            const modulePath = match[1];
            
            const filePath = this.resolvePath(projectRoot, document.uri.fsPath, modulePath);
            if (filePath) {
                this.scanFileForExports(projectRoot, filePath).forEach(exp => {
                    if (typeof exp.label === 'string') addCompletion(exp.label, exp.kind || vscode.CompletionItemKind.Reference, `from ${modulePath}`);
                });
            }
        }
        return items;
    }

    private provideDestructureCompletions(document: vscode.TextDocument, modulePath: string): vscode.CompletionItem[] {
        const projectRoot = this.findProjectRoot(document.uri.fsPath);
        const filePath = this.resolvePath(projectRoot, document.uri.fsPath, modulePath);
        if (!filePath) return [];
        return this.scanFileForExports(projectRoot, filePath);
    }

    private provideMemberCompletions(document: vscode.TextDocument, modulePath: string): vscode.CompletionItem[] {
        const projectRoot = this.findProjectRoot(document.uri.fsPath);
        const filePath = this.resolvePath(projectRoot, document.uri.fsPath, modulePath);
        if (!filePath) return [];
        return this.scanFileForExports(projectRoot, filePath);
    }

    private scanFileForExports(projectRoot: string, filePath: string, visited: Set<string> = new Set()): vscode.CompletionItem[] {
        if (visited.has(filePath)) return [];
        visited.add(filePath);
        const items: vscode.CompletionItem[] = [];

        try {
            const content = fs.readFileSync(filePath, 'utf-8');
            const lines = content.split(/\r?\n/);

            for (const line of lines) {
                const defMatch = /^\s*(?:extern\s+)?(?:struct|define|def|init)\s+([a-zA-Z0-9_]+)/.exec(line);
                if (defMatch) {
                    const name = defMatch[1];
                    if (name === 'init' || name === 'main' || name.startsWith('_')) continue;
                    
                    const item = new vscode.CompletionItem(name, line.includes('struct') ? vscode.CompletionItemKind.Struct : vscode.CompletionItemKind.Function);
                    item.sortText = '2_' + name;
                    items.push(item);
                }
            }

            const reExportRegex = /from\s+([.a-zA-Z0-9_/]+)\s+use\s+\{([^}]*)\}/g;
            let match;
            while ((match = reExportRegex.exec(content)) !== null) {
                const targetFile = this.resolvePath(projectRoot, filePath, match[1]);
                if (targetFile && fs.existsSync(targetFile)) {
                    const validSymbols = new Set(match[2].split(',').map(s => s.trim()));
                    this.scanFileForExports(projectRoot, targetFile, visited).forEach(expItem => {
                        if (typeof expItem.label === 'string' && validSymbols.has(expItem.label)) {
                            const newItem = new vscode.CompletionItem(expItem.label, expItem.kind);
                            newItem.detail = `(from ${path.basename(targetFile)})`;
                            newItem.sortText = '2_' + expItem.label;
                            items.push(newItem);
                        }
                    });
                }
            }
        } catch (e) {}
        return items;
    }

    private providePathCompletions(document: vscode.TextDocument, typedPath: string): vscode.CompletionItem[] {
        const projectRoot = this.findProjectRoot(document.uri.fsPath);
        let searchRoots: string[] = [];
        let searchDirRel = "";

        if (typedPath.startsWith('.')) {
            let dotCount = 0;
            while (dotCount < typedPath.length && typedPath[dotCount] === '.') dotCount++;
            let searchDir = path.dirname(document.uri.fsPath);
            for (let i = 1; i < dotCount; i++) searchDir = path.dirname(searchDir);
            searchRoots = [searchDir];
            const rest = typedPath.substring(dotCount);
            if (rest.includes('.')) {
                const parts = rest.split('.');
                parts.pop(); 
                searchDirRel = parts.join('/');
            }
        } else {
            searchRoots = this.getSearchRoots(projectRoot);
            if (typedPath.includes('.')) {
                const parts = typedPath.split('.');
                parts.pop();
                searchDirRel = parts.join('/');
            }
        }

        const completions: vscode.CompletionItem[] = [];
        for (const root of searchRoots) {
            const targetDir = searchDirRel ? path.join(root, searchDirRel) : root;
            if (fs.existsSync(targetDir) && fs.lstatSync(targetDir).isDirectory()) {
                try {
                    for (const f of fs.readdirSync(targetDir)) {
                        if (f.startsWith('.') || f === '__init.qk') continue;
                        let name = f, kind = vscode.CompletionItemKind.Folder;
                        const fullPath = path.join(targetDir, f);
                        if (!fs.lstatSync(fullPath).isDirectory()) {
                            if (!f.endsWith('.qk')) continue;
                            name = f.substring(0, f.length - 3);
                            kind = vscode.CompletionItemKind.Module;
                        }
                        if (!completions.some(c => c.label === name)) completions.push(new vscode.CompletionItem(name, kind));
                    }
                } catch (e) {}
            }
        }
        return completions;
    }

    private resolveImportPathFromAlias(document: vscode.TextDocument, alias: string): string | null {
        for (const line of document.getText().split(/\r?\n/)) {
            const useMatch = /^\s*use\s+([.a-zA-Z0-9_/]+)/.exec(line);
            if (useMatch && useMatch[1].split(/[\.\/]/).pop() === alias) return useMatch[1];
        }
        return null;
    }

    private resolvePath(projectRoot: string, currentFile: string, modulePath: string): string | null {
        if (modulePath.startsWith('.')) {
            let dotCount = 0;
            while (dotCount < modulePath.length && modulePath[dotCount] === '.') dotCount++;
            const subPath = modulePath.substring(dotCount).replace(/\./g, '/');
            let searchDir = path.dirname(currentFile);
            for (let i = 1; i < dotCount; i++) searchDir = path.dirname(searchDir);
            
            const targetBase = subPath ? path.join(searchDir, subPath) : searchDir; 
            if (fs.existsSync(targetBase + '.qk')) return targetBase + '.qk';
            if (fs.existsSync(path.join(targetBase, '__init.qk'))) return path.join(targetBase, '__init.qk');
            return null;
        }

        const searchRoots = this.getSearchRoots(projectRoot);
        const relPath = modulePath.replace(/\./g, '/');
        for (const root of searchRoots) {
            const fileVariant = path.join(root, relPath + '.qk');
            const folderVariant = path.join(root, relPath, '__init.qk');
            if (fs.existsSync(fileVariant)) return fileVariant;
            if (fs.existsSync(folderVariant)) return folderVariant;
        }
        return null;
    }

    private getSearchRoots(projectRoot: string): string[] {
        const searchRoots: string[] = [];
        if (process.env['QUIRK_HOME']) searchRoots.push(path.join(process.env['QUIRK_HOME'], 'lib', 'quirk', 'packages'), path.join(process.env['QUIRK_HOME'], 'lib', 'quirk'));
        
        try {
            for (const item of fs.readdirSync(projectRoot)) {
                const quirkLib = path.join(projectRoot, item, 'lib', 'quirk');
                if (fs.existsSync(quirkLib) && fs.lstatSync(path.join(projectRoot, item)).isDirectory()) {
                    searchRoots.push(path.join(quirkLib, 'packages'), quirkLib);
                }
            }
        } catch (e) {}

        searchRoots.push(path.join(projectRoot, 'libs'), path.join(projectRoot, 'src'));
        return searchRoots;
    }

    private findProjectRoot(currentFile: string): string {
        let currentDir = path.dirname(currentFile);
        const stopAt = vscode.workspace.getWorkspaceFolder(vscode.Uri.file(currentFile))?.uri.fsPath || "/";
        while (currentDir.length >= stopAt.length) {
            if (fs.existsSync(path.join(currentDir, 'Makefile')) || fs.existsSync(path.join(currentDir, 'libs'))) return currentDir;
            const parent = path.dirname(currentDir);
            if (parent === currentDir) break;
            currentDir = parent;
        }
        return stopAt;
    }
}