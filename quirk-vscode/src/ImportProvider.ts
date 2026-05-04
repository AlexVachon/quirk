import * as vscode from 'vscode';
import * as path from 'path';
import * as fs from 'fs';

const PRELUDE_MODULES = [
    'typing/index.qk',
    // primitives
    'typing/primitives/index.qk',
    'typing/primitives/string.qk',
    'typing/primitives/int.qk',
    'typing/primitives/double.qk',
    'typing/primitives/bool.qk',
    'typing/primitives/char.qk',
    // interfaces
    'typing/interfaces/index.qk',
    'typing/interfaces/printable.qk',
    'typing/interfaces/equatable.qk',
    'typing/interfaces/comparable.qk',
    'typing/interfaces/hashable.qk',
    'typing/interfaces/parseable.qk',
    'typing/interfaces/sizeable.qk',
    'typing/interfaces/iterable.qk',
    'typing/interfaces/iterator.qk',
    'typing/interfaces/representable.qk',
    'typing/interfaces/primitive.qk',
    'typing/interfaces/serializable.qk',
    // collections
    'typing/collections/list.qk',
    'typing/collections/map.qk',
    'typing/collections/tuple.qk',
    'typing/collections/set.qk',
    'typing/collections/queue.qk',
    // other
    'typing/callable.qk',
    'typing/exceptions/base.qk',
    'typing/exceptions/index.qk',
    'typing/exceptions/types.qk',
    'sys/index.qk'
];

export class QuirkDefinitionProvider implements vscode.DefinitionProvider {
    private outputChannel: vscode.OutputChannel;

    constructor(outputChannel: vscode.OutputChannel) {
        this.outputChannel = outputChannel;
    }

    public provideDefinition(
        document: vscode.TextDocument,
        position: vscode.Position,
        _token: vscode.CancellationToken
    ): vscode.ProviderResult<vscode.Definition> {
        const currentFilePath = document.uri.fsPath;
        const projectRoot = this.findProjectRoot(currentFilePath);
        const lineText = document.lineAt(position).text;

        // =========================================================
        // -1. INSIDE A DOCBLOCK → navigate to enclosing definition
        // =========================================================
        if (this.isInDocblock(document, position)) {
            return this.findEnclosingDefinition(document, position);
        }

        const range = document.getWordRangeAtPosition(position, /[a-zA-Z0-9_]+/);
        if (!range) return null;

        const symbol = document.getText(range);

        // Whether the word is immediately after a dot — member access, not an alias reference.
        // Exception: a dotted module path like `typing.collections.map` in a use/from line
        // has dots between segments but is NOT member access.
        let isMemberAccess = range.start.character > 0 && lineText.charAt(range.start.character - 1) === '.';
        if (isMemberAccess) {
            const importPathMatch = /^\s*(?:use|from)\s+([.a-zA-Z0-9_/]+)/.exec(lineText);
            if (importPathMatch) {
                const pathStart = lineText.indexOf(importPathMatch[1], importPathMatch.index);
                if (range.start.character >= pathStart && range.end.character <= pathStart + importPathMatch[1].length) {
                    isMemberAccess = false;
                }
            }
        }

        // =========================================================
        // 0. SUPER() METHOD CALL
        // =========================================================
        if (lineText.substring(0, range.start.character).trimEnd().endsWith('super().')) {
            const loc = this.findSuperMethod(document, position, symbol, projectRoot);
            if (loc) return loc;
        }

        // =========================================================
        // 1. IMPORT LINE — clicking on the module path or a named import,
        //    OR using an imported symbol elsewhere in the file.
        //    Skip when the cursor is on a member (e.g. the `greet` in
        //    `greet.greet(...)`) so it resolves to the function, not the module.
        // =========================================================
        const importLocation = !isMemberAccess && this.findImportLineInCurrentFile(document, symbol);
        if (importLocation) {
            // Resolve the actual definition regardless of where the cursor is
            const importLine = document.lineAt(importLocation.range.start.line).text;
            const importMatch = /^\s*(?:use|from)\s+([.a-zA-Z0-9_/]+)/.exec(importLine);
            if (importMatch) {
                // Named import:  from io.file use { File }
                const braceMatch = /\{([^}]*)\}/.exec(importLine);
                if (braceMatch && new RegExp(`\\b${symbol}\\b`).test(braceMatch[1])) {
                    const file = this.resolvePath(projectRoot, currentFilePath, importMatch[1]);
                    if (file) {
                        const loc = this.findSymbolInFile(projectRoot, file, symbol);
                        if (loc) return loc;
                    }
                }

                // Module alias:  use encoding.json  (clicking `json`)
                if (symbol === importMatch[1].split(/[./]/).pop()) {
                    const file = this.resolvePath(projectRoot, currentFilePath, importMatch[1]);
                    if (file) return new vscode.Location(vscode.Uri.file(file), new vscode.Position(0, 0));
                }

                // from .path as alias — resolve alias to the module file
                const asAliasImportMatch = /^\s*from\s+([.a-zA-Z0-9_/]+)\s+as\s+([a-zA-Z_]\w*)/.exec(importLine);
                if (asAliasImportMatch && asAliasImportMatch[2] === symbol) {
                    const file = this.resolvePath(projectRoot, currentFilePath, asAliasImportMatch[1]);
                    if (file) return new vscode.Location(vscode.Uri.file(file), new vscode.Position(0, 0));
                }
            }

            return importLocation;
        }

        // =========================================================
        // 2. MEMBER ACCESS  — prefix.symbol
        // =========================================================
        if (isMemberAccess) {
            const prefixRange = document.getWordRangeAtPosition(
                new vscode.Position(position.line, range.start.character - 2),
                /[a-zA-Z0-9_]+/
            );

            if (prefixRange) {
                const prefixWord = document.getText(prefixRange);

                if (prefixWord !== 'super') {
                    // (a) Module alias → function/struct exported from that module
                    const modulePath = this.resolveImportPathFromAlias(document, prefixWord);
                    if (modulePath) {
                        const file = this.resolvePath(projectRoot, currentFilePath, modulePath);
                        if (file) {
                            const loc = this.findSymbolInFile(projectRoot, file, symbol);
                            if (loc) return loc;
                        }
                    }

                    // (b) self → member of the enclosing struct
                    if (prefixWord === 'self') {
                        const structName = this.findEnclosingStruct(document, position);
                        if (structName) {
                            const loc = this.findStructMember(projectRoot, currentFilePath, structName, symbol);
                            if (loc) return loc;
                        }
                    } else {
                        // (c) Typed variable → infer struct type, navigate to its member
                        const typeName = this.inferType(document, position, prefixWord);
                        if (typeName) {
                            const loc = this.findStructMemberByType(projectRoot, currentFilePath, typeName, symbol);
                            if (loc) return loc;
                        }

                        // (d) Enum variant: Direction.North — PascalCase prefix
                        if (/^[A-Z]/.test(prefixWord)) {
                            const variantLoc = this.findEnumVariantInFiles(projectRoot, currentFilePath, prefixWord, symbol, document);
                            if (variantLoc) return variantLoc;
                        }
                    }
                }
            }
        }

        // =========================================================
        // 3. LOCAL VARIABLE OR PARAMETER
        // =========================================================
        const localLocation = this.findLocalDefinition(document, position, symbol);
        if (localLocation) return localLocation;

        // =========================================================
        // 4. GLOBAL: current file, then imports, then prelude
        // =========================================================
        let def = this.findSymbolInFile(projectRoot, currentFilePath, symbol);
        if (def) return def;

        def = this.findInImportedFiles(projectRoot, document, symbol);
        if (def) return def;

        def = this.findInPrelude(projectRoot, symbol);
        if (def) return def;

        return null;
    }

    // =========================================================
    // LOCAL VARIABLE / PARAMETER SEARCH
    // =========================================================

    private findLocalDefinition(document: vscode.TextDocument, position: vscode.Position, symbol: string): vscode.Location | null {
        const assignRe          = new RegExp(`^\\s*${symbol}\\b\\s*(?::\\s*[A-Za-z0-9_.]+\\s*)?(?:=|:=)`);
        const forRe             = new RegExp(`\\bfor\\s+(?:ref\\s+)?${symbol}\\s+in\\b`);
        const forParenDestructRe= new RegExp(`\\bfor\\s+\\([^)]*\\b${symbol}\\b[^)]*\\)\\s+in\\b`);
        const withAsRe          = new RegExp(`\\bwith\\b.*\\bas\\s+${symbol}\\b`);
        const catchRe           = new RegExp(`\\bcatch\\s*\\(\\s*${symbol}\\s*:`);
        const funcDefRe         = /^\s*(?:extern\s+)?(?:define|def|init)\s+[a-zA-Z0-9_]+\s*\(([^)]*)\)/;

        for (let i = position.line; i >= 0; i--) {
            const rawLine  = document.lineAt(i).text;
            const trimmed  = rawLine.trim();

            if (assignRe.test(trimmed)) {
                return new vscode.Location(document.uri, new vscode.Position(i, rawLine.indexOf(symbol)));
            }

            let m = forParenDestructRe.exec(trimmed);
            if (m) {
                return new vscode.Location(document.uri, new vscode.Position(i, rawLine.indexOf(symbol, m.index)));
            }

            m = forRe.exec(trimmed);
            if (m) {
                return new vscode.Location(document.uri, new vscode.Position(i, rawLine.indexOf(symbol, m.index)));
            }

            m = withAsRe.exec(trimmed);
            if (m) {
                const idx = rawLine.lastIndexOf(symbol);
                return new vscode.Location(document.uri, new vscode.Position(i, idx >= 0 ? idx : 0));
            }

            m = catchRe.exec(trimmed);
            if (m) {
                return new vscode.Location(document.uri, new vscode.Position(i, rawLine.indexOf(symbol, m.index)));
            }

            const funcMatch = funcDefRe.exec(rawLine);
            if (funcMatch) {
                for (const rawParam of funcMatch[1].split(',')) {
                    const p = rawParam.trim();
                    if (new RegExp(`^${symbol}\\b\\s*(?::|$)`).test(p)) {
                        const idx = rawLine.indexOf(symbol, funcMatch.index);
                        return new vscode.Location(document.uri, new vscode.Position(i, Math.max(0, idx)));
                    }
                }
                // Hit a function boundary — stop searching upward
                break;
            }
        }
        return null;
    }

    // =========================================================
    // STRUCT MEMBER NAVIGATION
    // =========================================================

    private findEnclosingStruct(document: vscode.TextDocument, position: vscode.Position): string | null {
        for (let i = position.line; i >= 0; i--) {
            const m = /^\s*struct\s+([a-zA-Z_]\w*)/.exec(document.lineAt(i).text);
            if (m) return m[1];
        }
        return null;
    }

    private inferType(document: vscode.TextDocument, position: vscode.Position, varName: string): string | null {
        const textBefore = document.getText(new vscode.Range(new vscode.Position(0, 0), position));
        for (const line of textBefore.split(/\r?\n/).reverse()) {
            // x := SomeType(...)  or  x := module.SomeType(...)
            let m = new RegExp(`\\b${varName}\\s*:=\\s*(?:[a-zA-Z0-9_]+\\.)?([A-Z][A-Za-z0-9_]*)\\s*\\(`).exec(line);
            if (m) return m[1];
            // x: SomeType  or  x: SomeType = ...
            m = new RegExp(`\\b${varName}\\s*:\\s*(?:[a-zA-Z0-9_]+\\.)?([A-Za-z0-9_]+)`).exec(line);
            if (m) return m[1];
        }
        return null;
    }

    // Find struct definition (any file) then look for the member inside it
    private findStructMemberByType(projectRoot: string, currentFile: string, typeName: string, member: string): vscode.Location | null {
        // Try current file and its imports first
        const structLoc = this.findSymbolInFile(projectRoot, currentFile, typeName);
        if (structLoc) {
            const loc = this.findMemberInStructBody(structLoc.uri.fsPath, typeName, member);
            if (loc) return loc;
        }
        // Try prelude (builtin types like Map, List, String, …)
        const preludeLoc = this.findInPrelude(projectRoot, typeName);
        if (preludeLoc) {
            const loc = this.findMemberInStructBody(preludeLoc.uri.fsPath, typeName, member);
            if (loc) return loc;
        }
        return null;
    }

    // Convenience: type is already known to be defined in currentFile's project
    private findStructMember(projectRoot: string, currentFile: string, structName: string, member: string): vscode.Location | null {
        return this.findStructMemberByType(projectRoot, currentFile, structName, member);
    }

    private findMemberInStructBody(filePath: string, structName: string, member: string): vscode.Location | null {
        const content = this.getFileContent(filePath);
        const lines = content.split(/\r?\n/);
        const structRe = new RegExp(`^\\s*(?:struct|interface)\\s+${structName}\\b`);
        const memberRe = new RegExp(
            `(?:(?:extern\\s+)?(?:define|def|init)\\s+${member}\\b)` +
            `|(?:^\\s*${member}\\s*:(?!=))`
        );

        let startLine = -1;
        for (let i = 0; i < lines.length; i++) {
            if (structRe.test(lines[i])) { startLine = i; break; }
        }
        if (startLine === -1) return null;

        let depth = 0;
        for (let i = startLine; i < lines.length; i++) {
            const line = lines[i];
            depth += (line.match(/\{/g) || []).length - (line.match(/\}/g) || []).length;
            if (i > startLine && depth <= 0) break;
            if (i === startLine) continue;
            if (memberRe.test(line)) {
                const charIdx = line.indexOf(member);
                return new vscode.Location(vscode.Uri.file(filePath), new vscode.Position(i, Math.max(0, charIdx)));
            }
        }
        return null;
    }

    // =========================================================
    // SYMBOL LOOKUP
    // =========================================================

    private findSymbolInFile(projectRoot: string, filePath: string, symbol: string, visited: Set<string> = new Set()): vscode.Location | null {
        if (visited.has(filePath)) return null;
        visited.add(filePath);

        const content = this.getFileContent(filePath);
        if (!content) return null;
        const lines = content.split(/\r?\n/);

        const defRe = new RegExp(`^\\s*(?:extern\\s+)?(?:struct|define|def|init|enum|interface)\\s+${symbol}\\b`);
        for (let i = 0; i < lines.length; i++) {
            if (defRe.test(lines[i])) {
                return new vscode.Location(vscode.Uri.file(filePath), new vscode.Position(i, Math.max(0, lines[i].indexOf(symbol))));
            }
        }

        // Follow re-exports
        const reExportRe = /from\s+([.a-zA-Z0-9_/]+)\s+use\s+\{([^}]*)\}/g;
        let match: RegExpExecArray | null;
        while ((match = reExportRe.exec(content)) !== null) {
            if (new RegExp(`\\b${symbol}\\b`).test(match[2])) {
                const next = this.resolvePath(projectRoot, filePath, match[1]);
                if (next) {
                    const res = this.findSymbolInFile(projectRoot, next, symbol, visited);
                    if (res) return res;
                }
            }
        }

        return null;
    }

    private findInImportedFiles(projectRoot: string, document: vscode.TextDocument, symbol: string): vscode.Location | null {
        const fullText = document.getText();
        const importRe = /^\s*(?:use|from)\s+([.a-zA-Z0-9_/]+)/gm;
        let m: RegExpExecArray | null;
        while ((m = importRe.exec(fullText)) !== null) {
            const file = this.resolvePath(projectRoot, document.uri.fsPath, m[1]);
            if (file) {
                const loc = this.findSymbolInFile(projectRoot, file, symbol);
                if (loc) return loc;
            }
        }
        return null;
    }

    private findInPrelude(projectRoot: string, symbol: string): vscode.Location | null {
        for (const root of this.getLibRoots(projectRoot)) {
            for (const mod of PRELUDE_MODULES) {
                const fullPath = path.join(root, mod);
                if (fs.existsSync(fullPath)) {
                    const loc = this.findSymbolInFile(projectRoot, fullPath, symbol, new Set());
                    if (loc) return loc;
                }
            }
        }
        return null;
    }

    private findSuperMethod(document: vscode.TextDocument, position: vscode.Position, methodName: string, projectRoot: string): vscode.Location | null {
        let parentStructName: string | null = null;
        for (let i = position.line; i >= 0; i--) {
            const m = /^\s*struct\s+[a-zA-Z_]\w*\s*:\s*([a-zA-Z_]\w*)/.exec(document.lineAt(i).text);
            if (m) { parentStructName = m[1]; break; }
        }
        if (!parentStructName) return null;

        const structLoc = this.findSymbolInFile(projectRoot, document.uri.fsPath, parentStructName)
            || this.findInPrelude(projectRoot, parentStructName);
        if (!structLoc) return null;

        return this.findMemberInStructBody(structLoc.uri.fsPath, parentStructName, methodName);
    }

    // =========================================================
    // DOCBLOCK HELPERS
    // =========================================================

    private isInDocblock(document: vscode.TextDocument, position: vscode.Position): boolean {
        let inDoc = false;
        for (let i = 0; i <= position.line; i++) {
            if (document.lineAt(i).text.trim() === '---') inDoc = !inDoc;
        }
        return inDoc;
    }

    private findEnclosingDefinition(document: vscode.TextDocument, position: vscode.Position): vscode.Location | null {
        for (let i = position.line; i < document.lineCount; i++) {
            const line = document.lineAt(i).text;
            const m = /^\s*(?:extern\s+)?(?:struct|define|def|init|enum|interface)\s+([a-zA-Z_]\w*)/.exec(line);
            if (m) {
                return new vscode.Location(document.uri, new vscode.Position(i, line.indexOf(m[1])));
            }
            // Hit closing --- without finding a definition
            if (i > position.line && line.trim() === '---') break;
        }
        return null;
    }

    /** Returns the line index of the opening `---` of a docblock immediately before defLine, or -1. */
    private findPrecedingDocstring(lines: string[], defLine: number): number {
        let i = defLine - 1;
        while (i >= 0 && lines[i].trim() === '') i--;
        if (i >= 0 && lines[i].trim() === '---') {
            i--;
            while (i >= 0 && lines[i].trim() !== '---') i--;
            return i >= 0 ? i : -1;
        }
        return -1;
    }

    // =========================================================
    // ENUM HELPERS
    // =========================================================

    private findEnumVariantInFiles(
        projectRoot: string, currentFile: string,
        enumName: string, variantName: string,
        document: vscode.TextDocument
    ): vscode.Location | null {
        // 1. Current file
        let loc = this.findEnumVariant(currentFile, enumName, variantName);
        if (loc) return loc;

        // 2. Imported files
        const importRe = /^\s*(?:use|from)\s+([.a-zA-Z0-9_/]+)/gm;
        let m: RegExpExecArray | null;
        while ((m = importRe.exec(document.getText())) !== null) {
            const file = this.resolvePath(projectRoot, currentFile, m[1]);
            if (file) {
                loc = this.findEnumVariant(file, enumName, variantName);
                if (loc) return loc;
            }
        }

        // 3. Prelude
        for (const root of this.getLibRoots(projectRoot)) {
            for (const mod of PRELUDE_MODULES) {
                const fullPath = path.join(root, mod);
                if (fs.existsSync(fullPath)) {
                    loc = this.findEnumVariant(fullPath, enumName, variantName);
                    if (loc) return loc;
                }
            }
        }
        return null;
    }

    private findEnumVariant(filePath: string, enumName: string, variantName: string): vscode.Location | null {
        const content = this.getFileContent(filePath);
        if (!content) return null;
        const lines = content.split(/\r?\n/);
        const enumRe = new RegExp(`^\\s*enum\\s+${enumName}\\b`);

        let startLine = -1;
        for (let i = 0; i < lines.length; i++) {
            if (enumRe.test(lines[i])) { startLine = i; break; }
        }
        if (startLine === -1) return null;

        // Inline: enum Color { Red Green Blue }
        const inlineMatch = /\{([^}]*)\}/.exec(lines[startLine]);
        if (inlineMatch) {
            const col = lines[startLine].indexOf(variantName, lines[startLine].indexOf('{'));
            if (col !== -1) return new vscode.Location(vscode.Uri.file(filePath), new vscode.Position(startLine, col));
            return null;
        }

        // Multi-line body (skip nested docblocks)
        let depth = 0;
        let inDocBlock = false;
        const variantRe = new RegExp(`^\\s*(${variantName})\\s*(?://.*)?$`);
        for (let i = startLine; i < lines.length; i++) {
            const line = lines[i];
            if (line.trim() === '---') { inDocBlock = !inDocBlock; continue; }
            if (inDocBlock) continue;
            depth += (line.match(/\{/g) || []).length - (line.match(/\}/g) || []).length;
            if (i > startLine && depth <= 0) break;
            if (i === startLine) continue;
            if (variantRe.test(line)) {
                return new vscode.Location(vscode.Uri.file(filePath), new vscode.Position(i, line.indexOf(variantName)));
            }
        }
        return null;
    }

    // =========================================================
    // IMPORT / ALIAS HELPERS
    // =========================================================

    private findImportLineInCurrentFile(document: vscode.TextDocument, symbol: string): vscode.Location | null {
        for (let i = 0; i < document.lineCount; i++) {
            const line = document.lineAt(i).text;

            // from .path as alias — alias is the declared name
            const asAliasMatch = /^\s*from\s+([.a-zA-Z0-9_/]+)\s+as\s+([a-zA-Z_]\w*)/.exec(line);
            if (asAliasMatch && asAliasMatch[2] === symbol) {
                const col = line.indexOf(symbol, line.indexOf(' as '));
                return new vscode.Location(document.uri, new vscode.Position(i, Math.max(0, col)));
            }

            const m = /^\s*(?:use|from)\s+([.a-zA-Z0-9_/]+)/.exec(line);
            if (!m) continue;
            if (m[1].split(/[./]/).pop() === symbol) {
                return new vscode.Location(document.uri, new vscode.Position(i, m.index));
            }
            if (line.includes('{') && new RegExp(`\\{[^}]*\\b${symbol}\\b`).test(line)) {
                return new vscode.Location(document.uri, new vscode.Position(i, line.indexOf(symbol)));
            }
        }
        return null;
    }

    private resolveImportPathFromAlias(document: vscode.TextDocument, alias: string): string | null {
        for (let i = 0; i < document.lineCount; i++) {
            const line = document.lineAt(i).text;
            // from .path as alias
            const asMatch = /^\s*from\s+([.a-zA-Z0-9_/]+)\s+as\s+([a-zA-Z_]\w*)/.exec(line);
            if (asMatch && asMatch[2] === alias) return asMatch[1];
            // use module.name  (last segment matches alias)
            const m = /^\s*(?:use|from)\s+([.a-zA-Z0-9_/]+)/.exec(line);
            if (m && m[1].split(/[./]/).pop() === alias) return m[1];
        }
        return null;
    }

    // =========================================================
    // PATH RESOLUTION
    // =========================================================

    public resolvePath(projectRoot: string, currentFile: string, modulePath: string): string | null {
        if (modulePath.startsWith('.')) return this.resolveRelative(currentFile, modulePath);
        const relPath = modulePath.replace(/\./g, '/');
        for (const root of this.getSearchRoots(projectRoot)) {
            const v1 = path.join(root, relPath + '.qk');
            const v2 = path.join(root, relPath, 'index.qk');
            const v3 = path.join(root, relPath, '__init.qk');
            if (fs.existsSync(v1)) return v1;
            if (fs.existsSync(v2)) return v2;
            if (fs.existsSync(v3)) return v3;
        }
        return null;
    }

    private resolveRelative(currentFile: string, modulePath: string): string | null {
        const m = /^(\.+)(.*)$/.exec(modulePath);
        if (!m) return null;
        let searchDir = path.dirname(currentFile);
        for (let i = 1; i < m[1].length; i++) searchDir = path.dirname(searchDir);
        const subPath = m[2].replace(/\./g, '/');
        const v1 = path.join(searchDir, subPath + '.qk');
        const v2 = path.join(searchDir, subPath, 'index.qk');
        const v3 = path.join(searchDir, subPath, '__init.qk');
        if (fs.existsSync(v1)) return v1;
        if (fs.existsSync(v2)) return v2;
        if (fs.existsSync(v3)) return v3;
        return null;
    }

    private getSearchRoots(projectRoot: string): string[] {
        const roots: string[] = [];
        const home = process.env['QUIRK_HOME'];
        if (home) {
            roots.push(
                path.join(home, 'lib', 'quirk', 'packages'),
                path.join(home, 'lib', 'quirk'),
                path.join(home, 'libs')
            );
        }
        roots.push(path.join(projectRoot, 'libs'), path.join(projectRoot, 'src'));
        return roots;
    }

    private getLibRoots(projectRoot: string): string[] {
        const roots: string[] = [];
        const home = process.env['QUIRK_HOME'];
        if (home) roots.push(path.join(home, 'libs'), path.join(home, 'lib', 'quirk'));
        roots.push(path.join(projectRoot, 'libs'), path.join(projectRoot, 'src'));
        return roots;
    }

    private findProjectRoot(currentFile: string): string {
        let dir = path.dirname(currentFile);
        while (dir.length > 3) {
            if (fs.existsSync(path.join(dir, 'Makefile')) || fs.existsSync(path.join(dir, 'libs'))) return dir;
            const parent = path.dirname(dir);
            if (parent === dir) break;
            dir = parent;
        }
        return dir;
    }

    private getFileContent(filePath: string): string {
        for (const doc of vscode.workspace.textDocuments) {
            if (doc.uri.fsPath === filePath) return doc.getText();
        }
        try { return fs.readFileSync(filePath, 'utf-8'); } catch { return ''; }
    }
}
