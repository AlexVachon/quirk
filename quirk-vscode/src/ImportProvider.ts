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
        const wordRange = document.getWordRangeAtPosition(position, /[a-zA-Z0-9_.]+/);
        if (!wordRange) return null;

        const symbol = document.getText(wordRange);
        this.outputChannel.appendLine(`\n[Definition] Request for symbol: "${symbol}"`);

        const importMatch = /^\s*(?:use|from)\s+([.a-zA-Z0-9_]+)/.exec(lineText);
        let targetModule = "";

        if (importMatch) {
            targetModule = importMatch[1];
        } else {
            const fromMatch = /^\s*from\s+([.a-zA-Z0-9_]+)\s+use/.exec(lineText);
            if (fromMatch) targetModule = fromMatch[1];
        }

        if (targetModule) {
            const file = this.resolvePath(projectRoot, currentFilePath, targetModule);
            if (file) {
                if (!importMatch) {
                    return this.findSymbolInFile(file, symbol);
                }
                return new vscode.Location(vscode.Uri.file(file), new vscode.Position(0, 0));
            }
        }

        return this.findSymbolInFile(currentFilePath, symbol);
    }

    /**
     * Walks up from the current file to find the folder containing a Makefile or libs folder.
     */
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
        this.outputChannel.appendLine(`[Path] Project Root detected: ${projectRoot}`);
        this.outputChannel.appendLine(`[Path] Resolving: "${modulePath}"`);

        // 1. Handle Relative Imports (.utils)
        if (modulePath.startsWith('.')) {
            const resolved = this.resolveRelative(currentFile, modulePath);
            if (resolved) {
                this.outputChannel.appendLine(`[Path] SUCCESS: Relative found at ${resolved}`);
                return resolved;
            }
        }

        const searchRoots: string[] = [];
        
        // 2. Add Active Environment (QUIRK_HOME)
        if (process.env['QUIRK_HOME']) {
            const v = process.env['QUIRK_HOME']!;
            searchRoots.push(path.join(v, 'lib', 'quirk', 'packages'), path.join(v, 'lib', 'quirk'));
        }

        // 3. Python-style Scan: Check folders inside the detected Project Root
        try {
            const items = fs.readdirSync(projectRoot);
            for (const item of items) {
                const fullItemPath = path.join(projectRoot, item);
                const quirkLib = path.join(fullItemPath, 'lib', 'quirk');
                if (fs.existsSync(quirkLib) && fs.lstatSync(fullItemPath).isDirectory()) {
                    this.outputChannel.appendLine(`[Path] Detected Environment: ${item}`);
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
                this.outputChannel.appendLine(`[Path] Checking: ${full}`);
                if (fs.existsSync(full)) {
                    this.outputChannel.appendLine(`[Path] SUCCESS: Found ${full}`);
                    return full;
                }
            }
        }

        this.outputChannel.appendLine(`[Path] FAILED: Could not resolve ${modulePath}`);
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

    private findSymbolInFile(filePath: string, symbol: string): vscode.Location | null {
        try {
            const content = fs.readFileSync(filePath, 'utf-8');
            const lines = content.split(/\r?\n/);
            for (let i = 0; i < lines.length; i++) {
                if (new RegExp(`\\b(struct|define|def|init)\\s+${symbol}\\b`).test(lines[i])) {
                    return new vscode.Location(vscode.Uri.file(filePath), new vscode.Position(i, 0));
                }
            }
        } catch (e) { }
        return null;
    }
}