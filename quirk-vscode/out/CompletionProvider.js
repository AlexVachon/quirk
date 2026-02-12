"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.QuirkCompletionProvider = void 0;
const vscode = require("vscode");
const fs = require("fs");
const path = require("path");
class QuirkCompletionProvider {
    constructor(outputChannel) {
        this.outputChannel = outputChannel;
    }
    provideCompletionItems(document, position) {
        const linePrefix = document.lineAt(position).text.substring(0, position.character);
        const moduleMatch = /^\s*(?:use|from)\s+([.a-zA-Z0-9_]*)$/.exec(linePrefix);
        if (moduleMatch) {
            return this.provideModuleCompletions(document, moduleMatch[1]);
        }
        return undefined;
    }
    findProjectRoot(currentFile) {
        let currentDir = path.dirname(currentFile);
        const workspaceFolder = vscode.workspace.getWorkspaceFolder(vscode.Uri.file(currentFile));
        const stopAt = workspaceFolder ? workspaceFolder.uri.fsPath : "/";
        while (currentDir.length >= stopAt.length) {
            if (fs.existsSync(path.join(currentDir, 'Makefile')) || fs.existsSync(path.join(currentDir, 'libs')))
                return currentDir;
            const parent = path.dirname(currentDir);
            if (parent === currentDir)
                break;
            currentDir = parent;
        }
        return stopAt;
    }
    provideModuleCompletions(document, typedPath) {
        const projectRoot = this.findProjectRoot(document.uri.fsPath);
        const searchRoots = [];
        if (process.env['QUIRK_HOME']) {
            const v = process.env['QUIRK_HOME'];
            searchRoots.push(path.join(v, 'lib', 'quirk', 'packages'), path.join(v, 'lib', 'quirk'));
        }
        try {
            const items = fs.readdirSync(projectRoot);
            for (const item of items) {
                const lp = path.join(projectRoot, item, 'lib', 'quirk');
                if (fs.existsSync(lp))
                    searchRoots.push(path.join(lp, 'packages'), lp);
            }
        }
        catch (e) { }
        searchRoots.push(path.join(projectRoot, 'libs'), path.join(projectRoot, 'src'));
        const completions = [];
        for (const root of searchRoots) {
            if (!fs.existsSync(root))
                continue;
            try {
                const files = fs.readdirSync(root);
                for (const f of files) {
                    let name = f;
                    let kind = vscode.CompletionItemKind.Module;
                    if (!fs.lstatSync(path.join(root, f)).isDirectory()) {
                        if (!f.endsWith('.qk') || f === '__init.qk')
                            continue;
                        name = f.substring(0, f.length - 3);
                        kind = vscode.CompletionItemKind.File;
                    }
                    if (!completions.some(c => c.label === name))
                        completions.push(new vscode.CompletionItem(name, kind));
                }
            }
            catch (e) { }
        }
        return completions;
    }
}
exports.QuirkCompletionProvider = QuirkCompletionProvider;
//# sourceMappingURL=CompletionProvider.js.map