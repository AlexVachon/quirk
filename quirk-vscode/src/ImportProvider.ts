import * as vscode from 'vscode';
import * as path from 'path';
import * as fs from 'fs';

// --- FILES AUTOMATICALLY LOADED BY THE COMPILER (PRELUDE) ---
const PRELUDE_MODULES = [
    'core/__init.qk',
    'core/string.qk',
    'core/primitives.qk',       
    'core/exceptions/base.qk',
    'core/exceptions/__init.qk',
    'core/types.qk',
    'core/collections/list.qk', 
    'core/collections/map.qk', 
    'sys/__init.qk'
];

export class QuirkDefinitionProvider implements vscode.DefinitionProvider {
    private outputChannel: vscode.OutputChannel;

    constructor(outputChannel: vscode.OutputChannel) {
        this.outputChannel = outputChannel;
    }

    public provideDefinition(
        document: vscode.TextDocument,
        position: vscode.Position,
        token: vscode.CancellationToken
    ): vscode.ProviderResult<vscode.Definition> {
        const currentFilePath = document.uri.fsPath;
        const projectRoot = this.findProjectRoot(currentFilePath);
        const lineText = document.lineAt(position).text;
        
        const range = document.getWordRangeAtPosition(position, /[a-zA-Z0-9_]+/);
        if (!range) return null;

        const symbol = document.getText(range);
        this.outputChannel.appendLine(`\n[Definition] Request for symbol: "${symbol}"`);

        // =========================================================
        // 0. SUPER() METHOD CALL
        // =========================================================
        const prefix = lineText.substring(0, range.start.character).trim();
        const superRegex = new RegExp(`super\\(\\)\\s*\\.\\s*${symbol}\\b`);
        
        if (prefix.endsWith('super().') || superRegex.test(lineText)) {
            this.outputChannel.appendLine(`[Definition] Detected super() call for method: "${symbol}"`);
            const superLoc = this.findSuperMethod(document, position, symbol, projectRoot);
            if (superLoc) return superLoc;
            this.outputChannel.appendLine(`[Definition] Failed to resolve super() parent.`);
        }

        // =========================================================
        // 1. IMPORT LINE
        // =========================================================
        const importLocation = this.findImportLineInCurrentFile(document, symbol);
        if (importLocation) {
            if (position.line === importLocation.range.start.line) {
                const importMatch = /^\s*(?:use|from)\s+([.a-zA-Z0-9_/]+)/.exec(lineText);
                if (importMatch && range.start.character >= lineText.indexOf(importMatch[1])) {
                     if (symbol === importMatch[1].split(/[\.\/]/).pop()) {
                         const file = this.resolvePath(projectRoot, currentFilePath, importMatch[1]);
                         if (file) return new vscode.Location(vscode.Uri.file(file), new vscode.Position(0, 0));
                     }
                }
            }
            return importLocation;
        }

        // =========================================================
        // 2. MEMBER ACCESS
        // =========================================================
        if (range.start.character > 0 && lineText.charAt(range.start.character - 1) === '.') {
            const prefixRange = document.getWordRangeAtPosition(
                new vscode.Position(position.line, range.start.character - 2),
                /[a-zA-Z0-9_]+/
            );

            if (prefixRange) {
                const moduleAlias = document.getText(prefixRange);
                if (moduleAlias !== 'super') {
                    const modulePath = this.resolveImportPathFromAlias(document, moduleAlias);
                    if (modulePath) {
                        const file = this.resolvePath(projectRoot, currentFilePath, modulePath);
                        if (file) return this.findSymbolInFile(projectRoot, file, symbol); 
                    }
                }
            }
        }

        // =========================================================
        // 3. LOCAL VARIABLE OR PARAMETER
        // =========================================================
        if (range.start.character === 0 || lineText.charAt(range.start.character - 1) !== '.') {
            const localLocation = this.findLocalVariableInCurrentFile(document, position, symbol);
            if (localLocation) {
                this.outputChannel.appendLine(`[Definition] Found local definition for "${symbol}"`);
                return localLocation;
            }
        }

        // =========================================================
        // 4. FALLBACK: Global Structs/Functions
        // =========================================================
        let def = this.findSymbolInFile(projectRoot, currentFilePath, symbol);
        if (def) return def;

        this.outputChannel.appendLine(`[Definition] Checking Prelude for "${symbol}"...`);
        def = this.findInPrelude(projectRoot, symbol);
        if (def) return def;

        return null;
    }

    // --- FIX: Strict Parameter Parsing ---
    private findLocalVariableInCurrentFile(document: vscode.TextDocument, position: vscode.Position, symbol: string): vscode.Location | null {
        // 1. Check Assignments:  x := ...  OR  x: Type = ...
        // We match symbol at the START of the assignment, not the type position
        const assignRegex = new RegExp(`^\\s*${symbol}\\b\\s*(?::\\s*[A-Za-z0-9_.]+\\s*)?(?:=|:=)`);
        
        const funcDefRegex = /^\s*(?:extern\s+)?(?:define|def|init)\s+[a-zA-Z0-9_]+\s*\(([^)]*)\)/;
        
        for (let i = position.line; i >= 0; i--) {
            const lineText = document.lineAt(i).text.trim();
            
            // Check assignment match
            if (assignRegex.test(lineText)) {
                return new vscode.Location(document.uri, new vscode.Position(i, lineText.indexOf(symbol)));
            }
            
            const funcDefMatch = funcDefRegex.exec(document.lineAt(i).text);
            if (funcDefMatch) {
                const params = funcDefMatch[1]; // e.g. "self, msg: String"
                
                // Split params and check each one
                const paramList = params.split(',');
                for (const rawParam of paramList) {
                    const p = rawParam.trim();
                    // Match "symbol" OR "symbol: Type"
                    // Ensure symbol is the NAME (starts with symbol, followed by colon or end of string)
                    const isParamName = new RegExp(`^${symbol}\\b\\s*(?::|$)`).test(p);
                    
                    if (isParamName) {
                        const symbolIdx = document.lineAt(i).text.indexOf(symbol, funcDefMatch.index);
                        return new vscode.Location(document.uri, new vscode.Position(i, Math.max(0, symbolIdx)));
                    }
                }
                
                // If we hit a function definition boundary, STOP searching upwards.
                // We don't want to find a variable with the same name in the function above us.
                break; 
            }
        }
        return null;
    }

    private findInPrelude(projectRoot: string, symbol: string): vscode.Location | null {
        let libsDir = path.join(projectRoot, 'libs');
        if (!fs.existsSync(libsDir)) libsDir = path.join(projectRoot, 'src'); 

        for (const mod of PRELUDE_MODULES) {
            const fullPath = path.join(libsDir, mod);
            if (fs.existsSync(fullPath)) {
                const loc = this.findSymbolInFile(projectRoot, fullPath, symbol, new Set());
                if (loc) {
                    this.outputChannel.appendLine(`[Definition] Found "${symbol}" in prelude: ${mod}`);
                    return loc;
                }
            }
        }
        return null;
    }

    private findSuperMethod(document: vscode.TextDocument, position: vscode.Position, methodName: string, projectRoot: string): vscode.Location | null {
        let parentStructName: string | null = null;
        
        for (let i = position.line; i >= 0; i--) {
            const match = /^\s*struct\s+[a-zA-Z_]\w*\s*:\s*([a-zA-Z_]\w*)/.exec(document.lineAt(i).text);
            if (match) {
                parentStructName = match[1];
                break;
            }
        }
        
        if (!parentStructName) return null;
        this.outputChannel.appendLine(`[Definition] Found parent struct: ${parentStructName}`);

        let structLocation = this.findSymbolInFile(projectRoot, document.uri.fsPath, parentStructName);
        if (!structLocation) structLocation = this.findInPrelude(projectRoot, parentStructName);

        if (!structLocation) return null;

        const targetFilePath = structLocation.uri.fsPath;
        const content = this.getFileContent(targetFilePath); 
        const lines = content.split(/\r?\n/);
        
        let inTargetStruct = false;
        
        for (let i = structLocation.range.start.line; i < lines.length; i++) {
            const line = lines[i];
            if (new RegExp(`^\\s*struct\\s+${parentStructName}\\b`).test(line)) {
                inTargetStruct = true;
                continue;
            }
            if (inTargetStruct && (/^}/.test(line.trim()) || /^\s*struct\s+/.test(line))) break; 

            if (inTargetStruct) {
                const methodRegex = new RegExp(`(?:extern\\s+)?(?:define|def|init)\\s+${methodName}\\b`);
                if (methodRegex.test(line)) {
                    return new vscode.Location(structLocation.uri, new vscode.Position(i, line.indexOf(methodName)));
                }
            }
        }
        return null;
    }

    private getFileContent(filePath: string): string {
        for (const doc of vscode.workspace.textDocuments) {
            if (doc.uri.fsPath === filePath) return doc.getText();
        }
        try {
            return fs.readFileSync(filePath, 'utf-8');
        } catch (e) {
            return "";
        }
    }

    private findSymbolInFile(projectRoot: string, filePath: string, symbol: string, visited: Set<string> = new Set()): vscode.Location | null {
        if (visited.has(filePath)) return null;
        visited.add(filePath);

        try {
            const content = this.getFileContent(filePath);
            const lines = content.split(/\r?\n/);

            // 1. Definition check
            const defRegex = new RegExp(`^\\s*(?:extern\\s+)?(?:struct|define|def|init)\\s+${symbol}\\b`);
            for (let i = 0; i < lines.length; i++) {
                if (defRegex.test(lines[i])) {
                    const charIndex = lines[i].indexOf(symbol);
                    return new vscode.Location(vscode.Uri.file(filePath), new vscode.Position(i, Math.max(0, charIndex)));
                }
            }

            // 2. Re-export check
            const reExportRegex = /from\s+([.a-zA-Z0-9_/]+)\s+use\s+\{([^}]*)\}/g;
            let match;
            while ((match = reExportRegex.exec(content)) !== null) {
                const importPath = match[1];
                const importedSymbols = match[2];
                if (new RegExp(`\\b${symbol}\\b`).test(importedSymbols)) {
                    const nextFile = this.resolvePath(projectRoot, filePath, importPath);
                    if (nextFile) {
                        const res = this.findSymbolInFile(projectRoot, nextFile, symbol, visited);
                        if (res) return res;
                    }
                }
            }
        } catch (e) { }
        return null;
    }

    private findImportLineInCurrentFile(document: vscode.TextDocument, symbol: string): vscode.Location | null {
        for (let i = 0; i < document.lineCount; i++) {
            const line = document.lineAt(i).text;
            const importMatch = /^\s*(?:use|from)\s+([.a-zA-Z0-9_/]+)/.exec(line);
            if (importMatch) {
                const alias = importMatch[1].split(/[\.\/]/).pop();
                if (alias === symbol) return new vscode.Location(document.uri, new vscode.Position(i, importMatch.index));
                if (line.includes(`{`) && line.includes(symbol)) {
                     if (new RegExp(`\\{\\s*[^}]*\\b${symbol}\\b`).test(line)) {
                         const idx = line.indexOf(symbol);
                         return new vscode.Location(document.uri, new vscode.Position(i, idx));
                     }
                }
            }
        }
        return null;
    }

    private resolveImportPathFromAlias(document: vscode.TextDocument, alias: string): string | null {
        for (let i = 0; i < document.lineCount; i++) {
            const line = document.lineAt(i).text;
            const useMatch = /^\s*(?:use|from)\s+([.a-zA-Z0-9_/]+)/.exec(line);
            if (useMatch && useMatch[1].split(/[\.\/]/).pop() === alias) return useMatch[1];
        }
        return null;
    }

    private findProjectRoot(currentFile: string): string {
        let currentDir = path.dirname(currentFile);
        while (currentDir.length > 3) {
            if (fs.existsSync(path.join(currentDir, 'Makefile')) || fs.existsSync(path.join(currentDir, 'libs'))) return currentDir;
            currentDir = path.dirname(currentDir);
        }
        return currentDir;
    }

    public resolvePath(projectRoot: string, currentFile: string, modulePath: string): string | null {
        if (modulePath.startsWith('.')) return this.resolveRelative(currentFile, modulePath);
        const searchRoots = [path.join(projectRoot, 'libs'), path.join(projectRoot, 'src')];
        const relPath = modulePath.replace(/\./g, '/');
        for (const root of searchRoots) {
            const v1 = path.join(root, relPath + '.qk');
            const v2 = path.join(root, relPath, '__init.qk');
            if (fs.existsSync(v1)) return v1;
            if (fs.existsSync(v2)) return v2;
        }
        return null;
    }

    private resolveRelative(currentFile: string, modulePath: string): string | null {
        const match = /^(\.+)(.*)$/.exec(modulePath);
        if (!match) return null;
        let searchDir = path.dirname(currentFile);
        for (let i = 1; i < match[1].length; i++) searchDir = path.dirname(searchDir);
        const subPath = match[2].replace(/\./g, '/');
        const v1 = path.join(searchDir, subPath + '.qk');
        const v2 = path.join(searchDir, subPath, '__init.qk');
        if (fs.existsSync(v1)) return v1;
        if (fs.existsSync(v2)) return v2;
        return null;
    }
}