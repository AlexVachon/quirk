import * as vscode from 'vscode';
import * as fs from 'fs';
import * as path from 'path';

const FILE_CACHE_TTL_MS = 5000;

interface CachedFile {
    content: string;
    ts: number;
}

export class QuirkCompletionProvider implements vscode.CompletionItemProvider {
    private outputChannel: vscode.OutputChannel;
    private fileCache = new Map<string, CachedFile>();
    private searchRootsCache: string[] | null = null;
    private searchRootsCacheKey = '';

    constructor(outputChannel: vscode.OutputChannel) {
        this.outputChannel = outputChannel;
    }

    private readFile(filePath: string): string | null {
        const now = Date.now();
        const cached = this.fileCache.get(filePath);
        if (cached && now - cached.ts < FILE_CACHE_TTL_MS) return cached.content;
        try {
            const content = fs.readFileSync(filePath, 'utf-8');
            this.fileCache.set(filePath, { content, ts: now });
            return content;
        } catch { return null; }
    }

    public provideCompletionItems(
        document: vscode.TextDocument,
        position: vscode.Position
    ): vscode.ProviderResult<vscode.CompletionItem[]> {

        const linePrefix = document.lineAt(position).text.substring(0, position.character);

        // from module use { ... }  — destructure completions
        const destructureMatch = /^\s*from\s+([.a-zA-Z0-9_/]+)\s+use\s+\{([^}]*)$/.exec(linePrefix);
        if (destructureMatch) return this.provideDestructureCompletions(document, destructureMatch[1]);

        // use / from  — path completions
        const importMatch = /^\s*(?:use|from)\s+([.a-zA-Z0-9_/]*)$/.exec(linePrefix);
        if (importMatch) return this.providePathCompletions(document, importMatch[1]);

        // someVar.  — member completions
        const memberMatch = /([a-zA-Z0-9_]+)\.$/.exec(linePrefix);
        if (memberMatch) {
            const aliasOrVar = memberMatch[1];
            const modulePath = this.resolveImportPathFromAlias(document, aliasOrVar);
            if (modulePath) return this.provideMemberCompletions(document, modulePath);
            return this.provideObjectMemberCompletions(document, position, aliasOrVar);
        }

        const wordMatch = /[a-zA-Z0-9_]+$/.exec(linePrefix);
        if (wordMatch || linePrefix.trim() === '') return this.provideGeneralCompletions(document, position);

        return undefined;
    }

    // =========================================================
    // DOCSTRING PARSER — shared utility, also used by HoverProvider
    // =========================================================
    public formatDocstring(docstring: string[]): vscode.MarkdownString {
        const md = new vscode.MarkdownString();
        md.isTrusted = true;
        let description: string[] = [];
        let paramsList: string[] = [];
        let returnsText = "";
        let readingParamsList = false;
        let exampleLines: string[] = [];
        let readingExample = false;

        for (const line of docstring) {
            const trimmed = line.trim();

            // @example block — collect until next @ tag
            if (/^@example\s*:?\s*$/.test(trimmed)) { readingExample = true; continue; }
            if (readingExample) {
                if (trimmed.startsWith('@')) { readingExample = false; }
                else { exampleLines.push(line); continue; }
            }

            // @param name desc  (single-line form)
            const singleParamMatch = /^@param\s+(?:\*\*)?([a-zA-Z0-9_]+)(?:\*\*)?\s*(.*)/.exec(trimmed);
            if (singleParamMatch && singleParamMatch[1] !== ':') {
                paramsList.push(`* \`${singleParamMatch[1]}\` — ${singleParamMatch[2]}`);
                readingParamsList = false;
                continue;
            }

            // @param :  (block-list form)
            if (/^@params?\s*:?$/.test(trimmed)) { readingParamsList = true; continue; }

            if (readingParamsList) {
                const bulletMatch = /^[-*]\s+(?:\*\*)?([a-zA-Z0-9_]+)(?:\*\*)?[\s:]*(.*)/.exec(trimmed);
                if (bulletMatch) {
                    paramsList.push(`* \`${bulletMatch[1]}\` — ${bulletMatch[2]}`);
                    continue;
                } else if (trimmed === '') {
                    continue;
                } else if (!trimmed.startsWith('@return')) {
                    readingParamsList = false;
                }
            }

            // @returns / @return
            if (trimmed.startsWith('@return')) {
                readingParamsList = false;
                returnsText = trimmed.replace(/^@returns?\s+/, '').replace(/\*\*/g, '').trim();
                continue;
            }

            description.push(line + '  ');
        }

        if (description.length > 0) md.appendMarkdown(description.join('\n') + '\n\n');
        if (paramsList.length > 0) md.appendMarkdown('**Parameters:**\n\n' + paramsList.join('\n') + '\n\n');
        if (returnsText) md.appendMarkdown(`**Returns:** ${returnsText}\n\n`);
        if (exampleLines.length > 0) {
            md.appendMarkdown('**Example:**\n');
            md.appendCodeblock(exampleLines.join('\n'), 'quirk');
        }

        return md;
    }

    // =========================================================
    // TYPE INFERENCE & OBJECT MEMBER COMPLETIONS
    // =========================================================

    private provideObjectMemberCompletions(
        document: vscode.TextDocument,
        position: vscode.Position,
        variableName: string
    ): vscode.CompletionItem[] {
        const typeName = this.inferTypeOfVariable(document, position, variableName);
        if (!typeName) return [];
        const projectRoot = this.findProjectRoot(document.uri.fsPath);
        return this.getStructMembersWithInheritance(projectRoot, document.uri.fsPath, typeName);
    }

    public inferTypeOfVariable(
        document: vscode.TextDocument,
        position: vscode.Position,
        variableName: string
    ): string | null {
        const textBeforeCursor = document.getText(new vscode.Range(new vscode.Position(0, 0), position));
        const lines = textBeforeCursor.split(/\r?\n/).reverse();

        // self → look up the enclosing struct definition
        if (variableName === 'self') {
            for (const line of lines) {
                let m = /\bstruct\s+([a-zA-Z0-9_]+)/.exec(line)
                    || /\bextend\s+([a-zA-Z0-9_]+)/.exec(line)
                    || /(?:define|def|init)\s+([a-zA-Z0-9_]+)_/.exec(line);
                if (m) return m[1];
            }
        }

        for (const line of lines) {
            // x := SomeType(...)
            let match = new RegExp(`\\b${variableName}\\s*:=\\s*(?:[a-zA-Z0-9_]+\\.)?([A-Z][A-Za-z0-9_]+)\\s*\\(`).exec(line);
            if (match) return match[1];

            // x: SomeType  or  x: SomeType =
            match = new RegExp(`\\b${variableName}\\s*:\\s*(?:[a-zA-Z0-9_]+\\.)?([A-Za-z0-9_]+)`).exec(line);
            if (match) return match[1];

            // x := someIdentifier  (catch-all, might be a constructor alias)
            match = new RegExp(`\\b${variableName}\\s*:=\\s*(?:[a-zA-Z0-9_]+\\.)?([A-Za-z0-9_]+)$`).exec(line);
            if (match) return match[1];
        }

        return null;
    }

    // Walk up the inheritance chain and merge members
    private getStructMembersWithInheritance(
        projectRoot: string,
        currentFile: string,
        structName: string,
        visited = new Set<string>()
    ): vscode.CompletionItem[] {
        if (visited.has(structName)) return [];
        visited.add(structName);

        const items = this.getStructMembers(projectRoot, currentFile, structName);
        const parentName = this.findParentStruct(projectRoot, currentFile, structName);

        if (parentName) {
            const parentItems = this.getStructMembersWithInheritance(projectRoot, currentFile, parentName, visited);
            const defined = new Set(items.map(i => typeof i.label === 'string' ? i.label : i.label.label));
            for (const pItem of parentItems) {
                const label = typeof pItem.label === 'string' ? pItem.label : pItem.label.label;
                if (!defined.has(label)) {
                    pItem.detail = (pItem.detail ? pItem.detail + ' ' : '') + `(from ${parentName})`;
                    pItem.sortText = '4_' + label;
                    items.push(pItem);
                }
            }
        }

        return items;
    }

    private findParentStruct(projectRoot: string, currentFile: string, structName: string): string | null {
        const targetFile = this.findFileContainingStruct(projectRoot, currentFile, structName);
        if (!targetFile) return null;
        const content = this.readFile(targetFile);
        if (!content) return null;
        const match = new RegExp(`\\bstruct\\s+${structName}\\s*:\\s*([A-Za-z0-9_]+)`).exec(content);
        return match ? match[1] : null;
    }

    private getStructMembers(
        projectRoot: string,
        currentFile: string,
        structName: string
    ): vscode.CompletionItem[] {
        const targetFile = this.findFileContainingStruct(projectRoot, currentFile, structName);
        if (!targetFile) return [];

        const items: vscode.CompletionItem[] = [];
        const seen = new Set<string>();
        const content = this.readFile(targetFile);
        if (!content) return items;

        const addCompletion = (
            label: string,
            kind: vscode.CompletionItemKind,
            docstr: string[] = [],
            insertText?: string,
            detail?: string
        ) => {
            if (seen.has(label)) return;
            seen.add(label);
            const item = new vscode.CompletionItem(label, kind);
            if (docstr.length > 0) item.documentation = this.formatDocstring(docstr);
            if (insertText) item.insertText = new vscode.SnippetString(insertText);
            if (detail) item.detail = detail;
            item.sortText = label.startsWith('__')
                ? '3_' + label
                : kind === vscode.CompletionItemKind.Field ? '1_' + label : '2_' + label;
            items.push(item);
        };

        const structMatch = new RegExp(`\\bstruct\\s+${structName}\\b`).exec(content);
        if (structMatch) {
            const startIndex = content.indexOf('{', structMatch.index);
            if (startIndex !== -1) {
                let braceCount = 1, endIndex = startIndex + 1;
                while (endIndex < content.length && braceCount > 0) {
                    if (content[endIndex] === '{') braceCount++;
                    else if (content[endIndex] === '}') braceCount--;
                    endIndex++;
                }

                const structBody = content.substring(startIndex + 1, endIndex - 1);
                const lines = structBody.split(/\r?\n/);
                let currentDocstring: string[] = [];
                let inDocBlock = false;

                for (const line of lines) {
                    const trimmed = line.trim();

                    if (trimmed === '---') {
                        inDocBlock = !inDocBlock;
                        if (inDocBlock) currentDocstring = [];
                        continue;
                    }
                    if (inDocBlock) { currentDocstring.push(trimmed); continue; }

                    if (trimmed !== '' && !trimmed.startsWith('//') && !/^\s*(?:extern\s+)?(?:define|def|init)/.test(line)) {
                        currentDocstring = [];
                    }

                    // Field:  name: Type
                    const fieldMatch = /^\s*([a-zA-Z0-9_]+)\s*:\s*([a-zA-Z0-9_]+)/.exec(line);
                    if (fieldMatch && !line.includes('(') && !line.includes('return') && !line.includes('=')) {
                        addCompletion(fieldMatch[1], vscode.CompletionItemKind.Field, currentDocstring, undefined, fieldMatch[2]);
                        currentDocstring = [];
                        continue;
                    }

                    // Method:  define/def name(params) -> RetType
                    const methodMatch = /^\s*(?:extern\s+)?(?:define|def)\s+([a-zA-Z0-9_]+)\s*\(([^)]*)\)(?:\s*->\s*([a-zA-Z0-9_]+))?/.exec(line);
                    if (methodMatch) {
                        const methodName = methodMatch[1];
                        const rawParams = methodMatch[2];
                        const returnType = methodMatch[3] || 'void';

                        if (methodName === '__init' || methodName === 'init') {
                            currentDocstring = [];
                            continue;
                        }

                        // Build snippet from params, skipping 'self'
                        const params = rawParams.split(',').map(p => p.trim()).filter(p => p && p !== 'self');
                        const snippetArgs = params.map((p, i) => `\${${i + 1}:${p.split(':')[0].trim()}}`).join(', ');
                        const insertText = `${methodName}(${snippetArgs})$0`;

                        addCompletion(
                            methodName,
                            vscode.CompletionItemKind.Method,
                            currentDocstring,
                            insertText,
                            `→ ${returnType}`
                        );
                        currentDocstring = [];
                    }
                }
            }
        }
        return items;
    }

    private findFileContainingStruct(projectRoot: string, currentFile: string, structName: string): string | null {
        const content = this.readFile(currentFile);
        if (content) {
            if (new RegExp(`\\bstruct\\s+${structName}\\b`).test(content)) return currentFile;

            for (const line of content.split(/\r?\n/)) {
                const importMatch = /^\s*(?:use|from)\s+([.a-zA-Z0-9_/]+)/.exec(line);
                if (importMatch) {
                    const resolvedFile = this.resolvePath(projectRoot, currentFile, importMatch[1]);
                    if (resolvedFile) {
                        const deepFile = this.deepFindStruct(projectRoot, resolvedFile, structName);
                        if (deepFile) return deepFile;
                    }
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
        const content = this.readFile(filePath);
        if (!content) return null;
        if (new RegExp(`\\bstruct\\s+${structName}\\b`).test(content)) return filePath;

        const reExportRegex = /from\s+([.a-zA-Z0-9_/]+)\s+use\s+\{([^}]*)\}/g;
        let match;
        while ((match = reExportRegex.exec(content)) !== null) {
            if (new RegExp(`\\b${structName}\\b`).test(match[2])) {
                const targetFile = this.resolvePath(projectRoot, filePath, match[1]);
                if (targetFile) {
                    const found = this.deepFindStruct(projectRoot, targetFile, structName, visited);
                    if (found) return found;
                }
            }
        }
        return null;
    }

    // =========================================================
    // GENERAL COMPLETIONS
    // =========================================================

    private provideGeneralCompletions(document: vscode.TextDocument, position: vscode.Position): vscode.CompletionItem[] {
        const items: vscode.CompletionItem[] = [];
        const seen = new Set<string>();

        const addItem = (
            label: string,
            kind: vscode.CompletionItemKind,
            detail?: string,
            insertText?: string,
            docs?: string | vscode.MarkdownString
        ) => {
            if (seen.has(label)) return;
            seen.add(label);
            const item = new vscode.CompletionItem(label, kind);
            if (detail) item.detail = detail;
            if (insertText) item.insertText = new vscode.SnippetString(insertText);
            if (docs) item.documentation = typeof docs === 'string' ? new vscode.MarkdownString(docs) : docs;
            item.sortText = label.startsWith('__') ? '3_' + label : '2_' + label;
            items.push(item);
        };

        // ---- Keywords with block snippets ----
        const keywords: [string, string?, string?][] = [
            ['define',   'define ${1:name}(${2:args}) -> ${3:void} {\n\t$0\n}',   'Define a function'],
            ['struct',   'struct ${1:Name} {\n\t${2:field}: ${3:Type}\n}',         'Define a struct'],
            ['enum', 'enum ${1:Name} {\n\t${2:Variant1}\n\t${3:Variant2}\n}', 'Define an enum'],
            ['if',       'if ${1:condition} {\n\t$0\n}',                           'If statement'],
            ['else',     'else {\n\t$0\n}',                                        'Else block'],
            ['elif',     'elif ${1:condition} {\n\t$0\n}',                         'Else-if branch'],
            ['while',    'while ${1:condition} {\n\t$0\n}',                        'While loop'],
            ['for',      'for ${1:item} in ${2:iterable} {\n\t$0\n}',             'For-in loop'],
            ['try',      'try {\n\t$0\n} catch (${1:e}: ${2:Exception}) {\n\t\n}','Try-catch block'],
            ['throw',    'throw ${1:Exception}("${2:message}")',                   'Throw an exception'],
            ['return'],  ['break'],   ['continue'],
            ['use',      'use ${1:module}',                                        'Import a module'],
            ['from',     'from ${1:module} use { ${2:symbol} }',                  'Destructure import'],
            ['with',     'with ${1:expr} as ${2:name} {\n\t$0\n}',               'Context manager'],
            ['in'],      ['as'],      ['del'],
            ['true'],    ['false'],   ['null'],
            ['and'],     ['or'],      ['not'],
            ['trigger'], ['catch'],   ['super'],
        ];
        for (const [kw, snippet, doc] of keywords) {
            addItem(kw, vscode.CompletionItemKind.Keyword, 'keyword', snippet, doc);
        }

        // ---- Built-ins ----
        const builtins: [string, string, string?][] = [
            ['print',     '`print(value)` — print to stdout',       'print(${1:value})'],
            ['printf',    '`printf(fmt, ...)` — formatted print',    'printf(${1:fmt}${2:, args})'],
            ['exit',      '`exit(code)` — terminate program',        'exit(${1:0})'],
            ['String',    'Built-in String type'],
            ['Int',       'Built-in Int type'],
            ['Double',    'Built-in Double type'],
            ['Bool',      'Built-in Bool type'],
            ['Char',      'Built-in Char type'],
            ['List',      'Built-in List<T> type'],
            ['Map',       'Built-in Map<K,V> type'],
            ['File',      'Built-in File type'],
            ['Any',       'Dynamic Any type'],
            ['void',      'No return value'],
            ['Exception', 'Base exception class',  'Exception(${1:message})'],
            ['TypeError', 'Type mismatch exception','TypeError(${1:message})'],
            ['ValueError','Invalid value exception','ValueError(${1:message})'],
        ];
        for (const [name, doc, snippet] of builtins) {
            addItem(name, vscode.CompletionItemKind.Class, 'built-in', snippet, doc);
        }

        const fullText = document.getText();
        const textBeforeCursor = document.getText(new vscode.Range(new vscode.Position(0, 0), position));

        // ---- Local variables (assigned before cursor) ----
        const varRegex = /\b([a-zA-Z_][a-zA-Z0-9_]*)\s*(?::=|=|\+=|-=|\*=|\/=)/g;
        let match: RegExpExecArray | null;
        while ((match = varRegex.exec(textBeforeCursor)) !== null) {
            addItem(match[1], vscode.CompletionItemKind.Variable, 'local variable');
        }

        // ---- Parameters of the enclosing function ----
        const funcBoundary = /^\s*(?:extern\s+)?(?:define|def|init)\s+[a-zA-Z0-9_]+\s*\(([^)]*)\)/gm;
        while ((match = funcBoundary.exec(textBeforeCursor)) !== null) {
            for (const p of match[1].split(',')) {
                const pName = p.trim().split(/[\s:]/)[0];
                if (pName && pName !== 'self') addItem(pName, vscode.CompletionItemKind.Variable, 'parameter');
            }
        }

        // ---- This-file definitions (functions & structs) ----
        const defRegex = /^\s*(?:extern\s+)?(?:define|def|init|struct)\s+([a-zA-Z0-9_]+)\s*(?:\(([^)]*)\))?(?:\s*->\s*([a-zA-Z0-9_]+))?/gm;
        while ((match = defRegex.exec(fullText)) !== null) {
            const isStruct = /\bstruct\b/.test(match[0]);
            const name = match[1];
            if (name === 'main') continue;
            if (isStruct) {
                addItem(name, vscode.CompletionItemKind.Struct, 'struct', `${name}($0)`);
            } else {
                const rawParams = (match[2] || '').split(',').map(p => p.trim()).filter(p => p && p !== 'self');
                const retType = match[3] || 'void';
                const snippetArgs = rawParams.map((p, i) => `\${${i + 1}:${p.split(':')[0].trim()}}`).join(', ');
                addItem(name, vscode.CompletionItemKind.Function, `→ ${retType}`, `${name}(${snippetArgs})$0`);
            }
        }

        // ---- Symbols from imported modules ----
        const projectRoot = this.findProjectRoot(document.uri.fsPath);
        const importRegex = /^\s*(?:use|from)\s+([.a-zA-Z0-9_/]+)/gm;
        while ((match = importRegex.exec(fullText)) !== null) {
            const modulePath = match[1];
            const filePath = this.resolvePath(projectRoot, document.uri.fsPath, modulePath);
            if (filePath) {
                this.scanFileForExports(projectRoot, filePath).forEach(exp => {
                    if (typeof exp.label === 'string') {
                        addItem(exp.label, exp.kind || vscode.CompletionItemKind.Reference, `from ${modulePath}`);
                    }
                });
            }
        }

        return items;
    }

    // =========================================================
    // MODULE / PATH / EXPORT COMPLETIONS
    // =========================================================

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

        const content = this.readFile(filePath);
        if (content) {
            const lines = content.split(/\r?\n/);
            let currentDocstring: string[] = [];
            let inDocBlock = false;

            for (const line of lines) {
                const trimmed = line.trim();

                if (trimmed === '---') {
                    inDocBlock = !inDocBlock;
                    if (inDocBlock) currentDocstring = [];
                    continue;
                }
                if (inDocBlock) { currentDocstring.push(trimmed); continue; }

                if (trimmed !== '' && !trimmed.startsWith('//') && !/^\s*(?:extern\s+)?(?:struct|define|def|init)/.test(line)) {
                    currentDocstring = [];
                }

                const defMatch = /^\s*(?:extern\s+)?(?:struct|define|def|init)\s+([a-zA-Z0-9_]+)\s*(?:\(([^)]*)\))?(?:\s*->\s*([a-zA-Z0-9_]+))?/.exec(line);
                if (defMatch) {
                    const name = defMatch[1];
                    if (name === 'init' || name === 'main' || name.startsWith('_')) continue;

                    const isStruct = /\bstruct\b/.test(line.trimStart());
                    const rawParams = (defMatch[2] || '').split(',').map(p => p.trim()).filter(p => p && p !== 'self');
                    const retType = defMatch[3];

                    const item = new vscode.CompletionItem(name, isStruct ? vscode.CompletionItemKind.Struct : vscode.CompletionItemKind.Function);
                    item.sortText = '2_' + name;
                    if (retType) item.detail = `→ ${retType}`;

                    if (!isStruct) {
                        const snippetArgs = rawParams.map((p, i) => `\${${i + 1}:${p.split(':')[0].trim()}}`).join(', ');
                        item.insertText = new vscode.SnippetString(`${name}(${snippetArgs})$0`);
                    }

                    if (currentDocstring.length > 0) {
                        item.documentation = this.formatDocstring(currentDocstring);
                        currentDocstring = [];
                    }

                    items.push(item);
                }
            }

            // Follow re-exports:  from x use { y, z }
            const reExportRegex = /from\s+([.a-zA-Z0-9_/]+)\s+use\s+\{([^}]*)\}/g;
            let match: RegExpExecArray | null;
            while ((match = reExportRegex.exec(content)) !== null) {
                const targetFile = this.resolvePath(projectRoot, filePath, match[1]);
                if (targetFile && fs.existsSync(targetFile)) {
                    const valid = new Set(match[2].split(',').map(s => s.trim()));
                    this.scanFileForExports(projectRoot, targetFile, visited).forEach(expItem => {
                        if (typeof expItem.label === 'string' && valid.has(expItem.label)) {
                            const newItem = new vscode.CompletionItem(expItem.label, expItem.kind);
                            newItem.detail = `(from ${path.basename(targetFile)})`;
                            newItem.sortText = '2_' + expItem.label;
                            if (expItem.documentation) newItem.documentation = expItem.documentation;
                            if (expItem.insertText) newItem.insertText = expItem.insertText;
                            items.push(newItem);
                        }
                    });
                }
            }
        }
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
                        if (!completions.some(c => c.label === name)) {
                            completions.push(new vscode.CompletionItem(name, kind));
                        }
                    }
                } catch { }
            }
        }
        return completions;
    }

    // =========================================================
    // HELPERS
    // =========================================================

    private resolveImportPathFromAlias(document: vscode.TextDocument, alias: string): string | null {
        for (const line of document.getText().split(/\r?\n/)) {
            const useMatch = /^\s*use\s+([.a-zA-Z0-9_/]+)/.exec(line);
            if (useMatch && useMatch[1].split(/[\.\/]/).pop() === alias) return useMatch[1];
        }
        return null;
    }

    public resolvePath(projectRoot: string, currentFile: string, modulePath: string): string | null {
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
            if (fs.existsSync(path.join(root, relPath + '.qk'))) return path.join(root, relPath + '.qk');
            if (fs.existsSync(path.join(root, relPath, '__init.qk'))) return path.join(root, relPath, '__init.qk');
        }
        return null;
    }

    private getSearchRoots(projectRoot: string): string[] {
        const cacheKey = projectRoot + '|' + (process.env['QUIRK_HOME'] || '');
        if (this.searchRootsCache && this.searchRootsCacheKey === cacheKey) {
            return this.searchRootsCache;
        }

        const roots: string[] = [];
        if (process.env['QUIRK_HOME']) {
            roots.push(
                path.join(process.env['QUIRK_HOME'], 'lib', 'quirk', 'packages'),
                path.join(process.env['QUIRK_HOME'], 'lib', 'quirk')
            );
        }
        try {
            for (const item of fs.readdirSync(projectRoot)) {
                const quirkLib = path.join(projectRoot, item, 'lib', 'quirk');
                if (fs.existsSync(quirkLib) && fs.lstatSync(path.join(projectRoot, item)).isDirectory()) {
                    roots.push(path.join(quirkLib, 'packages'), quirkLib);
                }
            }
        } catch { }
        roots.push(path.join(projectRoot, 'libs'), path.join(projectRoot, 'src'));

        this.searchRootsCache = roots;
        this.searchRootsCacheKey = cacheKey;
        return roots;
    }

    public findProjectRoot(currentFile: string): string {
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