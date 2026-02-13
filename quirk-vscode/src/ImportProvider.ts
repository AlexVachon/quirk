import * as vscode from 'vscode';
import * as path from 'path';
import * as fs from 'fs';

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
        // 1. MODULE ALIAS (math.Vector2 -> jump to "use math")
        // =========================================================
        const importLocation = this.findImportLineInCurrentFile(document, symbol);
        if (importLocation) {
            // If clicking the definition line itself, jump to file
            if (position.line === importLocation.range.start.line) {
                const importMatch = /^\s*(?:use|from)\s+([.a-zA-Z0-9_/]+)/.exec(lineText);
                if (importMatch) {
                    const targetModule = importMatch[1];
                    const file = this.resolvePath(projectRoot, currentFilePath, targetModule);
                    if (file) return new vscode.Location(vscode.Uri.file(file), new vscode.Position(0, 0));
                }
            } else {
                return importLocation;
            }
        }

        // =========================================================
        // 2. MEMBER ACCESS (math.Vector2 -> resolve math -> find Vector2)
        // =========================================================
        if (range.start.character > 1 && lineText.charAt(range.start.character - 1) === '.') {
            const prefixRange = document.getWordRangeAtPosition(
                new vscode.Position(position.line, range.start.character - 2),
                /[a-zA-Z0-9_]+/
            );

            if (prefixRange) {
                const moduleAlias = document.getText(prefixRange);
                this.outputChannel.appendLine(`[Definition] Detected member access via alias: "${moduleAlias}"`);

                const modulePath = this.resolveImportPathFromAlias(document, moduleAlias);
                
                if (modulePath) {
                    const file = this.resolvePath(projectRoot, currentFilePath, modulePath);
                    if (file) {
                        this.outputChannel.appendLine(`[Definition] searching for "${symbol}" inside ${file}`);
                        return this.findSymbolInFile(projectRoot, file, symbol); 
                    }
                }
            }
        }

        // Fallback: Look in current file
        return this.findSymbolInFile(projectRoot, currentFilePath, symbol);
    }

    private findImportLineInCurrentFile(document: vscode.TextDocument, symbol: string): vscode.Location | null {
        const text = document.getText();
        const lines = text.split(/\r?\n/);

        for (let i = 0; i < lines.length; i++) {
            const line = lines[i];
            const useMatch = /^\s*use\s+([.a-zA-Z0-9_/]+)/.exec(line);
            if (useMatch) {
                const fullPath = useMatch[1];
                const alias = fullPath.split(/[\.\/]/).pop(); 
                if (alias === symbol) {
                    return new vscode.Location(document.uri, new vscode.Position(i, useMatch.index));
                }
            }
            const fromMatch = /^\s*from\s+([.a-zA-Z0-9_/]+)/.exec(line);
            if (fromMatch) {
                const fullPath = fromMatch[1];
                const alias = fullPath.split(/[\.\/]/).pop();
                if (alias === symbol) {
                    return new vscode.Location(document.uri, new vscode.Position(i, fromMatch.index));
                }
            }
        }
        return null;
    }

    private resolveImportPathFromAlias(document: vscode.TextDocument, alias: string): string | null {
        const text = document.getText();
        const lines = text.split(/\r?\n/);

        for (const line of lines) {
            const useMatch = /^\s*use\s+([.a-zA-Z0-9_/]+)/.exec(line);
            if (useMatch) {
                const fullPath = useMatch[1];
                const implicitAlias = fullPath.split(/[\.\/]/).pop();
                if (implicitAlias === alias) return fullPath;
            }
        }
        return null;
    }

    private findProjectRoot(currentFile: string): string {
        let currentDir = path.dirname(currentFile);
        const workspaceFolder = vscode.workspace.getWorkspaceFolder(vscode.Uri.file(currentFile));
        const stopAt = workspaceFolder ? workspaceFolder.uri.fsPath : "/";

        while (currentDir.length >= stopAt.length) {
            if (fs.existsSync(path.join(currentDir, 'Makefile')) || fs.existsSync(path.join(currentDir, 'libs'))) {
                return currentDir;
            }
            const parent = path.dirname(currentDir);
            if (parent === currentDir) break;
            currentDir = parent;
        }
        return stopAt;
    }

    public resolvePath(projectRoot: string, currentFile: string, modulePath: string): string | null {
        // 1. Handle Relative Imports (.utils)
        if (modulePath.startsWith('.')) {
            return this.resolveRelative(currentFile, modulePath);
        }

        const searchRoots: string[] = [];
        
        // 2. Add Active Environment (QUIRK_HOME)
        if (process.env['QUIRK_HOME']) {
            const v = process.env['QUIRK_HOME']!;
            searchRoots.push(path.join(v, 'lib', 'quirk', 'packages'), path.join(v, 'lib', 'quirk'));
        }

        // 3. Virtual Environment Scan (Restore .venv lookup)
        try {
            const items = fs.readdirSync(projectRoot);
            for (const item of items) {
                const fullItemPath = path.join(projectRoot, item);
                // Check if directory has 'lib/quirk' (standard virtual env structure)
                const quirkLib = path.join(fullItemPath, 'lib', 'quirk');
                
                if (fs.existsSync(quirkLib) && fs.lstatSync(fullItemPath).isDirectory()) {
                    this.outputChannel.appendLine(`[Path] Detected Virtual Environment: ${item}`);
                    // Add 'packages' and base lib to search path
                    searchRoots.push(path.join(quirkLib, 'packages'), quirkLib);
                }
            }
        } catch (e) {}

        // 4. Default Project Fallbacks
        searchRoots.push(path.join(projectRoot, 'libs'), path.join(projectRoot, 'src'));

        const relPath = modulePath.replace(/\./g, '/');
        const variants = [relPath + '.qk', path.join(relPath, '__init.qk')];

        for (const root of searchRoots) {
            for (const v of variants) {
                const full = path.join(root, v);
                if (fs.existsSync(full)) return full;
            }
        }
        return null;
    }

    private resolveRelative(currentFile: string, modulePath: string): string | null {
        let dotCount = 0;
        while (dotCount < modulePath.length && modulePath[dotCount] === '.') dotCount++;
        const subPath = modulePath.substring(dotCount).replace(/\./g, '/');
        let searchDir = path.dirname(currentFile);
        for (let i = 1; i < dotCount; i++) searchDir = path.dirname(searchDir);

        const v1 = path.join(searchDir, subPath + '.qk');
        const v2 = path.join(searchDir, subPath, '__init.qk');
        if (fs.existsSync(v1)) return v1;
        if (fs.existsSync(v2)) return v2;
        return null;
    }

    private findSymbolInFile(projectRoot: string, filePath: string, symbol: string, visited: Set<string> = new Set()): vscode.Location | null {
        if (visited.has(filePath)) return null;
        visited.add(filePath);

        try {
            const content = fs.readFileSync(filePath, 'utf-8');
            const lines = content.split(/\r?\n/);

            // 1. Check for Definitions
            const defRegex = new RegExp(`\\b(struct|define|def|init)\\s+${symbol}\\b`);
            for (let i = 0; i < lines.length; i++) {
                if (defRegex.test(lines[i])) {
                    return new vscode.Location(vscode.Uri.file(filePath), new vscode.Position(i, 0));
                }
            }

            // 2. Check for Re-exports
            const reExportRegex = new RegExp(`from\\s+([.a-zA-Z0-9_/]+)\\s+use\\s+\\{([^}]*)\\}`);
            let match;
            const fullContent = content;
            const globalRegex = new RegExp(reExportRegex, 'g');
            
            while ((match = globalRegex.exec(fullContent)) !== null) {
                const importPath = match[1];
                const importedSymbols = match[2];
                
                if (new RegExp(`\\b${symbol}\\b`).test(importedSymbols)) {
                    this.outputChannel.appendLine(`[Definition] Found re-export of "${symbol}" from "${importPath}" in ${path.basename(filePath)}`);
                    
                    const nextFile = this.resolvePath(projectRoot, filePath, importPath);
                    if (nextFile) {
                        return this.findSymbolInFile(projectRoot, nextFile, symbol, visited);
                    }
                }
            }

        } catch (e) { 
            this.outputChannel.appendLine(`[Definition] Error reading ${filePath}: ${e}`);
        }
        return null;
    }
}