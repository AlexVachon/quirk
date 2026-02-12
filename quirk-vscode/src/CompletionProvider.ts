import * as vscode from 'vscode';
import * as fs from 'fs';
import * as path from 'path';

export class QuirkCompletionProvider implements vscode.CompletionItemProvider {
    private outputChannel: vscode.OutputChannel;

    constructor(outputChannel: vscode.OutputChannel) {
        this.outputChannel = outputChannel;
    }

    public provideCompletionItems(document: vscode.TextDocument, position: vscode.Position): vscode.ProviderResult<vscode.CompletionItem[]> {
        const linePrefix = document.lineAt(position).text.substring(0, position.character);
        const moduleMatch = /^\s*(?:use|from)\s+([.a-zA-Z0-9_]*)$/.exec(linePrefix);
        
        if (moduleMatch) {
            return this.provideModuleCompletions(document, moduleMatch[1]);
        }
        return undefined;
    }

    private findProjectRoot(currentFile: string): string {
        let currentDir = path.dirname(currentFile);
        const workspaceFolder = vscode.workspace.getWorkspaceFolder(vscode.Uri.file(currentFile));
        const stopAt = workspaceFolder ? workspaceFolder.uri.fsPath : "/";
        while (currentDir.length >= stopAt.length) {
            if (fs.existsSync(path.join(currentDir, 'Makefile')) || fs.existsSync(path.join(currentDir, 'libs'))) return currentDir;
            const parent = path.dirname(currentDir);
            if (parent === currentDir) break;
            currentDir = parent;
        }
        return stopAt;
    }

    private provideModuleCompletions(document: vscode.TextDocument, typedPath: string): vscode.CompletionItem[] {
        const projectRoot = this.findProjectRoot(document.uri.fsPath);
        const searchRoots: string[] = [];

        if (process.env['QUIRK_HOME']) {
            const v = process.env['QUIRK_HOME']!;
            searchRoots.push(path.join(v, 'lib', 'quirk', 'packages'), path.join(v, 'lib', 'quirk'));
        }

        try {
            const items = fs.readdirSync(projectRoot);
            for (const item of items) {
                const lp = path.join(projectRoot, item, 'lib', 'quirk');
                if (fs.existsSync(lp)) searchRoots.push(path.join(lp, 'packages'), lp);
            }
        } catch (e) {}

        searchRoots.push(path.join(projectRoot, 'libs'), path.join(projectRoot, 'src'));

        const completions: vscode.CompletionItem[] = [];
        for (const root of searchRoots) {
            if (!fs.existsSync(root)) continue;
            try {
                const files = fs.readdirSync(root);
                for (const f of files) {
                    let name = f;
                    let kind = vscode.CompletionItemKind.Module;
                    if (!fs.lstatSync(path.join(root, f)).isDirectory()) {
                        if (!f.endsWith('.qk') || f === '__init.qk') continue;
                        name = f.substring(0, f.length - 3);
                        kind = vscode.CompletionItemKind.File;
                    }
                    if (!completions.some(c => c.label === name)) completions.push(new vscode.CompletionItem(name, kind));
                }
            } catch (e) {}
        }
        return completions;
    }
}