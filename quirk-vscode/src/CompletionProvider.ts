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

        // Chained method call: expr.method(args).partial
        // Handles string literals, variables, and multi-level chains.
        // e.g. "true".to_bool().s  →  Bool methods
        //      word.distance(typo).  →  Int methods
        //      s.split(",").  →  List methods
        if (/\)\.[a-zA-Z0-9_]*$/.test(linePrefix)) {
            const lastDot = linePrefix.lastIndexOf('.');
            const exprStr = linePrefix.substring(0, lastDot);
            const tailExpr = this.extractTailExpr(exprStr);
            if (tailExpr !== null) {
                const chainedType = this.inferExpressionType(tailExpr, document, position);
                if (chainedType) return this.provideObjectMemberCompletions(document, position, '', chainedType);
            }
        }

        // Literal value followed by a dot: "hello"., true., false., 42.
        // Must come before memberMatch since that regex only handles identifiers.
        if (/(?:"[^"]*"|'[^']*')\.[a-zA-Z0-9_]*$/.test(linePrefix))
            return this.provideObjectMemberCompletions(document, position, '', 'String');
        if (/\b(true|false)\.[a-zA-Z0-9_]*$/.test(linePrefix))
            return this.provideObjectMemberCompletions(document, position, '', 'Bool');
        // \d+\.(?!\d) — int literal dot, but not a double like 3.14
        if (/\b\d+\.(?!\d)[a-zA-Z0-9_]*$/.test(linePrefix))
            return this.provideObjectMemberCompletions(document, position, '', 'Int');

        // someVar.  or  someVar.partial  — member completions
        const memberMatch = /([a-zA-Z0-9_]+)\.([a-zA-Z0-9_]*)$/.exec(linePrefix);
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
    public formatDocstring(docstring: string[]): { md: vscode.MarkdownString, deprecated: boolean } {
        const md = new vscode.MarkdownString();
        md.isTrusted = true;
        let description: string[] = [];
        let paramsList: string[] = [];
        let returnsText = "";
        let throwsList: string[] = [];
        let notes: string[] = [];
        let warnings: string[] = [];
        let deprecated = false;
        let deprecatedReason = "";
        let readingParamsList = false;
        let exampleLines: string[] = [];
        let readingExample = false;
        let readingNoteList = false;
        let readingWarningList = false;
        let readingReturns = false;

        const isListItem = (s: string) => s.startsWith('-') || s.startsWith('*') || /^\d+[\.\)]/.test(s);

        for (const line of docstring) {
            const trimmed = line.trim();

            // Collect list items appended to the current @note block
            if (readingNoteList) {
                if (isListItem(trimmed)) { notes[notes.length - 1] += '\n' + trimmed; continue; }
                if (trimmed === '') continue;
                readingNoteList = false;
            }

            // Collect list items appended to the current @warning block
            if (readingWarningList) {
                if (isListItem(trimmed)) { warnings[warnings.length - 1] += '\n' + trimmed; continue; }
                if (trimmed === '') continue;
                readingWarningList = false;
            }

            // @returns continuation — collect indented lines after a bare "@returns:"
            if (readingReturns) {
                if (trimmed.startsWith('@')) { readingReturns = false; }
                else if (trimmed !== '') { returnsText += (returnsText ? ' ' : '') + trimmed; continue; }
                else { continue; }
            }

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

            // @params:  (block-list form)
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
                returnsText = trimmed.replace(/^@returns?\s*:?\s*/, '').replace(/\*\*/g, '').trim();
                readingReturns = !returnsText;
                continue;
            }

            // @throws TypeName desc
            const throwsMatch = /^@throws?\s+([a-zA-Z_]\w*)(?:\s+(.*))?/.exec(trimmed);
            if (throwsMatch) {
                const desc = throwsMatch[2] ? ' — ' + throwsMatch[2] : '';
                throwsList.push(`* \`${throwsMatch[1]}\`${desc}`);
                continue;
            }

            // @deprecated reason
            const deprecatedMatch = /^@deprecated\s*(.*)/.exec(trimmed);
            if (deprecatedMatch) {
                deprecated = true;
                deprecatedReason = deprecatedMatch[1].trim();
                continue;
            }

            // @note [text] — optional inline text, then collects following list items
            const noteMatch = /^@note\s*(.*)/.exec(trimmed);
            if (noteMatch) {
                notes.push(noteMatch[1].trim());
                readingNoteList = true;
                readingWarningList = false;
                continue;
            }

            // @warning [text] — optional inline text, then collects following list items
            const warningMatch = /^@warning\s*(.*)/.exec(trimmed);
            if (warningMatch) {
                warnings.push(warningMatch[1].trim());
                readingNoteList = false;
                readingWarningList = true;
                continue;
            }

            description.push(line + '  ');
        }

        const renderBlock = (emoji: string, label: string, content: string) => {
            const [head, ...rest] = content.split('\n');
            if (rest.length === 0) {
                md.appendMarkdown(`${emoji} **${label}:** ${head}\n\n`);
            } else {
                const header = head ? `${emoji} **${label}:** ${head}\n` : `${emoji} **${label}:**\n`;
                md.appendMarkdown(header + rest.join('\n') + '\n\n');
            }
        };

        if (deprecated) {
            const reason = deprecatedReason ? ` — ${deprecatedReason}` : '';
            md.appendMarkdown(`~~**Deprecated**~~${reason}\n\n`);
        }
        if (description.length > 0) md.appendMarkdown(description.join('\n') + '\n\n');
        for (const n of notes)    renderBlock('📝', 'Note', n);
        for (const w of warnings) renderBlock('⚠️', 'Warning', w);
        if (paramsList.length > 0)  md.appendMarkdown('**Parameters:**\n\n' + paramsList.join('\n') + '\n\n');
        if (returnsText)            md.appendMarkdown(`**Returns:** ${returnsText}\n\n`);
        if (throwsList.length > 0)  md.appendMarkdown('**Throws:**\n\n' + throwsList.join('\n') + '\n\n');
        if (exampleLines.length > 0) {
            md.appendMarkdown('**Example:**\n');
            md.appendCodeblock(exampleLines.join('\n'), 'quirk');
        }

        return { md, deprecated };
    }

    // =========================================================
    // TYPE INFERENCE & OBJECT MEMBER COMPLETIONS
    // =========================================================

    private provideObjectMemberCompletions(
        document: vscode.TextDocument,
        position: vscode.Position,
        variableName: string,
        overrideType?: string
    ): vscode.CompletionItem[] {
        const typeName = overrideType ?? this.inferTypeOfVariable(document, position, variableName);
        if (!typeName) return [];
        const projectRoot = this.findProjectRoot(document.uri.fsPath);

        // Labels that will be superseded by richer hand-crafted completions below
        const overriddenByLambdaMethods = new Set<string>(
            typeName === 'List' ? ['map', 'filter', 'each', 'reduce', 'any', 'all', 'find'] :
            typeName === 'Map'  ? ['each', 'each_key', 'each_value'] : []
        );

        const rawItems = this.getStructMembersWithInheritance(projectRoot, document.uri.fsPath, typeName);
        const items = rawItems.filter(i => {
            const label = typeof i.label === 'string' ? i.label : (i.label as any).label;
            return !overriddenByLambdaMethods.has(label);
        });

        // Magic attributes available on every struct instance
        if (variableName === 'self') {
            const nameItem = new vscode.CompletionItem('__name', vscode.CompletionItemKind.Property);
            nameItem.detail = 'magic attribute → String';
            nameItem.documentation = new vscode.MarkdownString(
                '**`__name`** — the struct\'s name as a `String`.\n\n' +
                '```quirk\nprint(self.__name)           // "TypeError"\nprint(self.__class.__name)   // "TypeError"\n```'
            );
            nameItem.sortText = '3___name';
            items.push(nameItem);

            const classItem = new vscode.CompletionItem('__class', vscode.CompletionItemKind.Property);
            classItem.detail = 'magic attribute → Type';
            classItem.documentation = new vscode.MarkdownString(
                '**`__class`** — `Type` descriptor for the enclosing struct.\n\n' +
                'Access `.__name` and `.__parent` on the result.\n\n' +
                '```quirk\nprint(self.__class.__name)    // "TypeError"\n' +
                'print(self.__class.__parent)  // "Exception"\n```'
            );
            classItem.sortText = '3___class';
            items.push(classItem);
        }

        // List functional methods
        if (typeName === 'List') {
            const listMethods: { label: string; detail: string; doc: string; snippet: string }[] = [
                {
                    label: 'map',
                    detail: '(cb: Callable) → List',
                    doc: 'Transform each element and return a new List.\n\n```quirk\ndoubled := nums.map(fn(x: Int) => x * 2)\n```',
                    snippet: 'map(fn(${1:x}: ${2:Int}) => ${3:$1})$0',
                },
                {
                    label: 'filter',
                    detail: '(cb: Callable) → List',
                    doc: 'Keep elements where the predicate is true.\n\n```quirk\nbig := nums.filter(fn(x: Int) => x > 2)\n```',
                    snippet: 'filter(fn(${1:x}: ${2:Int}) => ${3:$1 > 0})$0',
                },
                {
                    label: 'each',
                    detail: '(cb: Callable)',
                    doc: 'Call callback for each element (no return value).\n\n```quirk\nnums.each(fn(x: Int) => print(x))\n// or with a block:\nnums.each(fn(x: Int) {\n    print(x)\n})\n```',
                    snippet: 'each(fn(${1:x}: ${2:Int}) => ${3:print($1)})$0',
                },
                {
                    label: 'reduce',
                    detail: '(initial: Any, cb: Callable) → Any',
                    doc: 'Fold elements into a single value.\n\n```quirk\nsum: Int = nums.reduce(0, fn(acc: Int, x: Int) => acc + x)\n```',
                    snippet: 'reduce(${1:0}, fn(${2:acc}: ${3:Int}, ${4:x}: ${5:Int}) => ${6:$2 + $4})$0',
                },
                {
                    label: 'any',
                    detail: '(cb: Callable) → Bool',
                    doc: 'Return `true` if at least one element matches the predicate.\n\n```quirk\nhas_big := nums.any(fn(x: Int) => x > 4)\n```',
                    snippet: 'any(fn(${1:x}: ${2:Int}) => ${3:$1 > 0})$0',
                },
                {
                    label: 'all',
                    detail: '(cb: Callable) → Bool',
                    doc: 'Return `true` if every element matches the predicate.\n\n```quirk\nall_pos := nums.all(fn(x: Int) => x > 0)\n```',
                    snippet: 'all(fn(${1:x}: ${2:Int}) => ${3:$1 > 0})$0',
                },
                {
                    label: 'find',
                    detail: '(cb: Callable) → Any',
                    doc: 'Return the first matching element, or `null` if none found.\n\n```quirk\nfound: Int = nums.find(fn(x: Int) => x > 3)\n```',
                    snippet: 'find(fn(${1:x}: ${2:Int}) => ${3:$1 > 0})$0',
                },
            ];
            for (const m of listMethods) {
                const item = new vscode.CompletionItem(m.label, vscode.CompletionItemKind.Method);
                item.detail = m.detail;
                item.documentation = new vscode.MarkdownString(`**\`List.${m.label}\`** — ${m.doc}`);
                item.insertText = new vscode.SnippetString(m.snippet);
                item.sortText = '0_' + m.label;
                items.push(item);
            }
        }

        // Map functional methods
        if (typeName === 'Map') {
            const mapMethods: { label: string; detail: string; doc: string; snippet: string }[] = [
                {
                    label: 'each',
                    detail: '(fn(key: String, value: Any))',
                    doc: 'Call callback for each key-value pair.\n\n```quirk\npeople.each(fn(id: String, person: Map) {\n    print(id + ": " + person.get("name"))\n})\n```',
                    snippet: 'each(fn(${1:key}: String, ${2:value}) {\n\t$0\n})',
                },
                {
                    label: 'each_key',
                    detail: '(fn(key: String))',
                    doc: 'Call callback for each key only.\n\n```quirk\nm.each_key(fn(k: String) => print(k))\n```',
                    snippet: 'each_key(fn(${1:key}: String) => ${2:print($1)})$0',
                },
                {
                    label: 'each_value',
                    detail: '(fn(value: Any))',
                    doc: 'Call callback for each value only.\n\n```quirk\nm.each_value(fn(v: Map) => print(v.get("name")))\n```',
                    snippet: 'each_value(fn(${1:value}) => ${2})$0',
                },
            ];
            for (const m of mapMethods) {
                const item = new vscode.CompletionItem(m.label, vscode.CompletionItemKind.Method);
                item.detail = m.detail;
                item.documentation = new vscode.MarkdownString(`**\`Map.${m.label}\`** — ${m.doc}`);
                item.insertText = new vscode.SnippetString(m.snippet);
                item.sortText = '0_' + m.label;
                items.push(item);
            }
        }

        // Tuple methods
        if (typeName === 'Tuple') {
            const lengthItem = new vscode.CompletionItem('length', vscode.CompletionItemKind.Method);
            lengthItem.detail = '() → Int';
            lengthItem.documentation = new vscode.MarkdownString('**`Tuple.length`** — returns the number of elements.\n\n```quirk\nt := (1, 2, 3)\nprint(t.length())  // 3\n```');
            lengthItem.insertText = new vscode.SnippetString('length()');
            lengthItem.sortText = '0_length';
            items.push(lengthItem);
        }

        // Type descriptor members (result of self.__class)
        if (typeName === 'Type') {
            const makeTypeAttr = (label: string, doc: string) => {
                const item = new vscode.CompletionItem(label, vscode.CompletionItemKind.Property);
                item.detail = 'Type attribute → String';
                item.documentation = new vscode.MarkdownString(doc);
                item.sortText = '1_' + label;
                return item;
            };
            items.push(makeTypeAttr('__name',
                '**`__name`** — the name of the struct this `Type` describes.\n\n' +
                '```quirk\nprint(self.__class.__name)  // "TypeError"\n```'));
            items.push(makeTypeAttr('__parent',
                '**`__parent`** — the parent struct name of the struct this `Type` describes.\n\n' +
                '```quirk\nprint(self.__class.__parent)  // "Exception"\n```'));
        }

        return items;
    }

    // Returns the name of the struct if the cursor is directly inside a struct body
    // (i.e. not nested inside a define/def/init within that struct).
    private getDirectStructContext(document: vscode.TextDocument, position: vscode.Position): string | null {
        const textBefore = document.getText(new vscode.Range(new vscode.Position(0, 0), position));
        let depth = 0;
        for (let i = textBefore.length - 1; i >= 0; i--) {
            const ch = textBefore[i];
            if (ch === '}') { depth++; }
            else if (ch === '{') {
                depth--;
                if (depth < 0) {
                    const preceding = textBefore.substring(0, i).trimEnd();
                    const m = /\bstruct\s+([a-zA-Z0-9_]+)(?:\s*:\s*[a-zA-Z0-9_]+)?\s*$/.exec(preceding);
                    return m ? m[1] : null;
                }
            }
        }
        return null;
    }

    public inferTypeOfVariable(
        document: vscode.TextDocument,
        position: vscode.Position,
        variableName: string
    ): string | null {
        const textBeforeCursor = document.getText(new vscode.Range(new vscode.Position(0, 0), position));
        const lines = textBeforeCursor.split(/\r?\n/).reverse();

        // Built-in type names used as static namespaces: Int.parse(), Double.parse(), etc.
        const primitiveTypes = new Set(['Int', 'Double', 'Bool', 'Char', 'String', 'List', 'Map']);
        if (primitiveTypes.has(variableName)) return variableName;

        // self → look up the enclosing struct definition
        if (variableName === 'self') {
            for (const line of lines) {
                let m = /\bstruct\s+([a-zA-Z0-9_]+)/.exec(line)
                    || /\bextend\s+([a-zA-Z0-9_]+)/.exec(line)
                    || /(?:define|def|init)\s+([A-Z][a-zA-Z0-9_]*)_/.exec(line);
                if (m) return m[1];
            }
        }

        // Methods that return List regardless of receiver type
        const listReturnMethods = new Set(['map', 'filter', 'keys', 'values']);

        const projectRoot = this.findProjectRoot(document.uri.fsPath);

        for (const line of lines) {
            const esc = variableName.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');

            // x := [...]  or  x = [...]  → List literal
            if (new RegExp(`\\b${esc}\\s*(?::=|=)\\s*\\[`).test(line)) return 'List';

            // x := { or x = {  → Map literal
            if (new RegExp(`\\b${esc}\\s*(?::=|=)\\s*\\{`).test(line)) return 'Map';

            // x := "..."  or  x = "..."  → String literal
            if (new RegExp(`\\b${esc}\\s*(?::=|=)\\s*"`).test(line)) return 'String';

            // x := 3.14  → Double literal (must come before Int check)
            if (new RegExp(`\\b${esc}\\s*(?::=|=)\\s*\\d+\\.\\d`).test(line)) return 'Double';

            // x := 42  → Int literal
            if (new RegExp(`\\b${esc}\\s*(?::=|=)\\s*\\d`).test(line)) return 'Int';

            // x := true / false  → Bool
            if (new RegExp(`\\b${esc}\\s*(?::=|=)\\s*(?:true|false)\\b`).test(line)) return 'Bool';

            // x := 'a'  → Char literal
            if (new RegExp(`\\b${esc}\\s*(?::=|=)\\s*'`).test(line)) return 'Char';

            // x := something.map(...) / .filter(...) / .keys() / .values()  → List
            const chainedMatch = new RegExp(`\\b${esc}\\s*:=\\s*.+\\.(${[...listReturnMethods].join('|')})\\s*\\(`).exec(line);
            if (chainedMatch) return 'List';

            // x := receiver.method(...)  → look up method return type
            const methodCallMatch = new RegExp(`\\b${esc}\\s*:=\\s*([a-zA-Z0-9_]+)\\.([a-zA-Z0-9_]+)\\s*\\(`).exec(line);
            if (methodCallMatch) {
                const receiverName = methodCallMatch[1];
                const methodName   = methodCallMatch[2];
                const receiverType = this.inferTypeOfVariable(document, position, receiverName);
                if (receiverType) {
                    const ret = this.inferMethodReturnType(projectRoot, document.uri.fsPath, receiverType, methodName);
                    if (ret) return ret;
                }
            }

            // x := someFunc(...)  → look up function return type
            const funcCallMatch = new RegExp(`\\b${esc}\\s*:=\\s*([a-z][a-zA-Z0-9_]*)\\s*\\(`).exec(line);
            if (funcCallMatch) {
                const ret = this.inferFunctionReturnType(projectRoot, document, funcCallMatch[1]);
                if (ret) return ret;
            }

            // x := expr + expr  — if either side contains a string literal or "+" with a known String, result is String
            const concatMatch = new RegExp(`\\b${esc}\\s*:=\\s*(.+)\\+(.+)`).exec(line);
            if (concatMatch && (/"/.test(concatMatch[1]) || /"/.test(concatMatch[2]))) return 'String';

            // x := a + b  — if a has a known numeric type, propagate it (Double wins over Int)
            const arithMatch = new RegExp(`\\b${esc}\\s*:=\\s*([a-zA-Z0-9_]+)\\s*[+\\-*\\/]`).exec(line);
            if (arithMatch) {
                const lhsType = this.inferTypeOfVariable(document, position, arithMatch[1]);
                if (lhsType === 'Double') return 'Double';
                if (lhsType === 'Int')    return 'Int';
            }

            // x := condition? thenExpr : elseExpr  — use the then-branch type
            const ternaryMatch = new RegExp(`\\b${esc}\\s*:=\\s*.+\\?\\s*([a-zA-Z0-9_"'\`[{]+)`).exec(line);
            if (ternaryMatch) {
                const thenToken = ternaryMatch[1];
                if (thenToken.startsWith('"')) return 'String';
                if (/^\d+\.\d/.test(thenToken))  return 'Double';
                if (/^\d/.test(thenToken))        return 'Int';
                if (thenToken === 'true' || thenToken === 'false') return 'Bool';
                if (thenToken.startsWith('['))    return 'List';
                if (thenToken.startsWith('{'))    return 'Map';
                const thenType = this.inferTypeOfVariable(document, position, thenToken);
                if (thenType) return thenType;
            }

            // lambda param:  fn(varName: Type)  or  fn(varName: Type, ...)
            const lambdaParamMatch = new RegExp(`\\bfn\\s*\\([^)]*\\b${esc}\\s*:\\s*([A-Za-z0-9_]+)`).exec(line);
            if (lambdaParamMatch) return lambdaParamMatch[1];

            // for varName in iterable  → infer element type from iterable
            const forMatch = new RegExp(`\\bfor\\s+(?:ref\\s+)?${esc}\\s+in\\s+([a-zA-Z0-9_"'\\[]+)`).exec(line);
            if (forMatch) {
                const iterable = forMatch[1];
                if (iterable.startsWith('"') || iterable.startsWith("'")) return 'Char'; // string literal → Char elements
                const iterType = this.inferTypeOfVariable(document, position, iterable);
                if (iterType === 'String') return 'Char';
                // List element type is unknown at static analysis time; return Any so members are offered
                if (iterType === 'List') return 'Any';
            }

            // catch (varName: ExceptionType)  → ExceptionType
            const catchParamMatch = new RegExp(`\\bcatch\\s*\\(\\s*${esc}\\s*:\\s*([A-Za-z0-9_]+)`).exec(line);
            if (catchParamMatch) return catchParamMatch[1];

            // x := SomeType(...)
            let match = new RegExp(`\\b${esc}\\s*:=\\s*(?:[a-zA-Z0-9_]+\\.)?([A-Z][A-Za-z0-9_]+)\\s*\\(`).exec(line);
            if (match) return match[1];

            // x: SomeType  or  x: SomeType =
            match = new RegExp(`\\b${esc}\\s*:\\s*(?:[a-zA-Z0-9_]+\\.)?([A-Za-z0-9_]+)`).exec(line);
            if (match) return match[1];

            // x := someIdentifier  (catch-all, might be a constructor alias)
            match = new RegExp(`\\b${esc}\\s*:=\\s*(?:[a-zA-Z0-9_]+\\.)?([A-Za-z0-9_]+)$`).exec(line);
            if (match) return match[1];
        }

        return null;
    }

    // Return type of `receiverType.methodName()` — built-in table first, then source scan.
    private inferMethodReturnType(projectRoot: string, currentFile: string, receiverType: string, methodName: string): string | null {
        // Built-in method return types for core types
        const builtinReturns: Record<string, Record<string, string>> = {
            String: {
                upper: 'String', lower: 'String', title: 'String', capitalize: 'String',
                sentence_case: 'String', swapcase: 'String', trim: 'String', lstrip: 'String',
                rstrip: 'String', replace: 'String', remove: 'String', repeat: 'String',
                reverse: 'String', encode: 'String', substring: 'String', zfill: 'String',
                ljust: 'String', rjust: 'String', center: 'String', join: 'String',
                format: 'String', format_map: 'String', format_list: 'String',
                split: 'List', lines: 'List',
                find: 'Int', index: 'Int', count: 'Int', distance: 'Int',
                to_int: 'Int', to_float: 'Double', to_bool: 'Bool', to_char: 'Char',
                startswith: 'Bool', endswith: 'Bool', contains: 'Bool', is_alpha: 'Bool',
                is_digit: 'Bool', is_space: 'Bool', empty: 'Bool',
                str: 'String',
            },
            List: {
                map: 'List', filter: 'List', keys: 'List', values: 'List', find: 'Any',
                join: 'String',
                length: 'Int', len: 'Int',
                any: 'Bool', all: 'Bool',
                pop: 'Any', get: 'Any',
            },
            Map: {
                get: 'Any', keys: 'List', values: 'List',
                len: 'Int',
                has: 'Bool',
            },
            Int: {
                str: 'String', is_even: 'Bool', is_odd: 'Bool',
            },
            Double: {
                str: 'String',
            },
            Bool: {
                str: 'String',
            },
            Char: {
                str: 'String', is_upper: 'Bool', is_lower: 'Bool',
                is_digit: 'Bool', is_alpha: 'Bool', is_space: 'Bool',
                to_int: 'Int',
            },
            Tuple: {
                length: 'Int',
            },
            Callable: {
                __str: 'String',
            },
        };

        if (builtinReturns[receiverType]?.[methodName]) {
            return builtinReturns[receiverType][methodName];
        }

        // Fall back to scanning the struct definition for the method's return type
        const structFile = this.findFileContainingStruct(projectRoot, currentFile, receiverType);
        if (!structFile) return null;
        const content = this.readFile(structFile);
        if (!content) return null;

        const methodRe = new RegExp(
            `(?:extern\\s+)?(?:define|def)\\s+${methodName}\\s*\\([^)]*\\)\\s*->\\s*([A-Za-z0-9_]+)`
        );
        const m = methodRe.exec(content);
        return m ? m[1] : null;
    }

    // Return type of a free function call — scans current file and imports.
    private inferFunctionReturnType(projectRoot: string, document: vscode.TextDocument, funcName: string): string | null {
        // Built-in functions with known return types
        if (funcName === 'type') return 'String';

        const defRe = new RegExp(
            `(?:extern\\s+)?define\\s+${funcName}\\s*\\([^)]*\\)\\s*->\\s*([A-Za-z0-9_]+)`
        );

        // Current file
        const src = document.getText();
        let m = defRe.exec(src);
        if (m) return m[1];

        // Imported files (shallow — one level of imports)
        const importRe = /^\s*(?:use|from)\s+([.a-zA-Z0-9_/]+)/gm;
        let im: RegExpExecArray | null;
        while ((im = importRe.exec(src)) !== null) {
            const resolved = this.resolvePath(projectRoot, document.uri.fsPath, im[1]);
            if (!resolved) continue;
            const content = this.readFile(resolved);
            if (!content) continue;
            m = defRe.exec(content);
            if (m) return m[1];
        }

        return null;
    }

    // Extract the last "complete" sub-expression from a string, respecting
    // balanced parens and quoted strings. Returns the extracted expression or null.
    // e.g. 'print("true".to_bool()' → '"true".to_bool()'
    private extractTailExpr(s: string): string | null {
        s = s.trimEnd();
        if (!s.length) return null;

        // Walk backward through balanced parens to find where the expression starts
        let i = s.length - 1;
        let depth = 0;

        // Each iteration steps backward one "unit" (paren group, string, or char)
        outer: while (i >= 0) {
            const c = s[i];
            if (c === ')') { depth++; i--; continue; }
            if (c === '(') {
                if (depth > 0) { depth--; i--; continue; }
                break; // unmatched '(' — this is a call-context boundary
            }
            if (depth === 0) {
                // Stop at operator / punctuation that isn't part of an expression
                if ('+-*/%,;=!<>&|^~@#'.includes(c)) break;
            }
            if ((c === '"' || c === "'") && depth === 0) {
                // Walk backward through the string literal
                const q = c;
                i--;
                while (i >= 0 && !(s[i] === q && s[i - 1] !== '\\')) i--;
                i--; // skip opening quote
                continue;
            }
            i--;
        }

        const tail = s.substring(i + 1).trimStart();
        return tail.length ? tail : null;
    }

    // Recursively infer the Quirk type of a simple expression string.
    // Handles literals, identifiers, and single/multi-level method chains.
    private inferExpressionType(expr: string, document: vscode.TextDocument, position: vscode.Position): string | null {
        expr = expr.trim();
        if (!expr) return null;

        // Method call chain MUST come first: receiver.method(args)
        // If the expression ends with ')', it's a call — check this before literal sniffing
        // so "true".to_bool() isn't short-circuited to String by the string-literal check.
        // Use lazy `.*?` so we peel off the OUTERMOST call last (matching from the end)
        const callRe = /^(.*?)\.([a-zA-Z_]\w*)\(([^()]*)\)$/.exec(expr);
        if (callRe) {
            const receiverExpr = callRe[1];
            const methodName   = callRe[2];
            const receiverType = this.inferExpressionType(receiverExpr, document, position);
            if (receiverType) {
                const projectRoot = this.findProjectRoot(document.uri.fsPath);
                return this.inferMethodReturnType(projectRoot, document.uri.fsPath, receiverType, methodName);
            }
        }

        // Literals (checked after method-call so "str".method() isn't short-circuited)
        if (/^["']/.test(expr))                     return 'String';
        if (/^\d+\.\d/.test(expr))                  return 'Double';
        if (/^\d/.test(expr))                       return 'Int';
        if (/^(true|false)$/.test(expr))            return 'Bool';
        if (expr.startsWith('['))                   return 'List';
        if (expr.startsWith('{'))                   return 'Map';

        // Simple identifier
        if (/^[a-zA-Z_]\w*$/.test(expr))
            return this.inferTypeOfVariable(document, position, expr);

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
            if (docstr.length > 0) {
                const parsed = this.formatDocstring(docstr);
                item.documentation = parsed.md;
                if (parsed.deprecated) item.tags = [vscode.CompletionItemTag.Deprecated];
            }
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

        const implicitCores = ['typing', 'typing.string', 'typing.collections.list', 'typing.collections.map', 'typing.primitives', 'typing.callable'];
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
            ['match',    'match ${1:value} {\n\tcase ${2:pattern} => $0\n\tcase _ => \n}', 'Match statement'],
            ['case',     'case ${1:pattern} => $0',                                'Match arm'],
            ['return'],  ['break'],   ['continue'],
            ['use',      'use ${1:module}',                                        'Import a module'],
            ['from',     'from ${1:module} use { ${2:symbol} }',                  'Destructure import'],
            ['with',     'with ${1:expr} as ${2:name} {\n\t$0\n}',               'Context manager'],
            ['where',    'where ${1:condition}',                                   'Precondition on a function (where clause)'],
            ['in'],      ['as'],      ['del'],
            ['true'],    ['false'],   ['null'],
            ['and'],     ['or'],      ['not'],
            ['trigger'], ['catch'],   ['super'],
            ['fn',       'fn(${1:x}) => ${2:x}',                                    'Lambda expression'],
        ];
        for (const [kw, snippet, doc] of keywords) {
            addItem(kw, vscode.CompletionItemKind.Keyword, 'keyword', snippet, doc);
        }

        // ---- Built-ins ----
        const builtins: [string, string, string?][] = [
            ['print',     '`print(value)` — print to stdout',                          'print(${1:value})'],
            ['printf',    '`printf(fmt, ...)` — formatted print',                   'printf(${1:fmt}${2:, args})'],
            ['type',      '`type(value) → String` — return the type name of a value', 'type(${1:value})'],
            ['exit',      '`exit(code)` — terminate program',                       'exit(${1:0})'],
            ['String',    'Built-in String type'],
            ['Int',       'Built-in Int type'],
            ['Double',    'Built-in Double type'],
            ['Bool',      'Built-in Bool type'],
            ['Char',      'Built-in Char type'],
            ['List',      'Built-in List<T> type'],
            ['Map',       'Built-in Map<K,V> type'],
            ['File',      'Built-in File type'],
            ['Any',       'Dynamic Any type'],
            ['Tuple',     'Immutable fixed-size sequence — (a, b, c)'],
            ['Callable',  'Lambda / function value (produced by fn(...) => ...)'],
            ['void',      'No return value'],
            ['Exception',           'Base exception class'],
            ['TypeError',           'Type mismatch'],
            ['ValueError',          'Invalid value'],
            ['IndexError',          'List/string index out of range'],
            ['KeyError',            'Map key not found'],
            ['IOError',             'File or I/O failure'],
            ['FileNotFoundError',   'File or directory not found'],
            ['RuntimeError',        'Generic runtime error'],
            ['NotImplementedError', 'Abstract method not implemented'],
            ['ZeroDivisionError',   'Division by zero'],
            ['AssertionError',      'Assertion failed'],
            ['NullError',           'Null value dereferenced'],
            ['SocketError',         'Socket or network failure'],
            ['WhereConditionError', 'where precondition violated'],
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

        // ---- Magic methods (only when cursor is directly inside a struct body) ----
        if (this.getDirectStructContext(document, position)) {
            const magicMethods: [string, string, string][] = [
                // Lifecycle
                ['__init',     'define __init(self$1) -> void {\n\t$0\n}',                               'Constructor — called on instantiation'],
                ['__del',      'define __del(self) -> void {\n\t$0\n}',                                  'Destructor — called on destruction'],
                // String conversion
                ['__str',      'define __str(self) -> String {\n\t$0\n}',                                'Human-readable string — used by print() and concatenation'],
                ['__repr',     'define __repr(self) -> String {\n\t$0\n}',                               'Developer representation — used as fallback when __str is absent'],
                // Boolean / length
                ['__bool',     'define __bool(self) -> Bool {\n\t$0\n}',                                 'Truthiness — enables if obj: / not obj / while obj:'],
                ['__len',      'define __len(self) -> Int {\n\t$0\n}',                                   'Length — called by .length on the struct'],
                // Indexing
                ['__get',      'define __get(self, index: ${1:Int}) -> ${2:Any} {\n\t$0\n}',             'Index read — obj[i]'],
                ['__set',      'define __set(self, index: ${1:Int}, value: ${2:Any}) -> void {\n\t$0\n}','Index write — obj[i] = v'],
                // Iteration
                ['__iter',     'define __iter(self) -> ${1:Iterator} {\n\t$0\n}',                        'Iterator — enables for-in loops'],
                ['__has_next', 'define __has_next(self) -> Bool {\n\t$0\n}',                             'Iterator protocol — true if more elements remain'],
                ['__next',     'define __next(self) -> ${1:Any} {\n\t$0\n}',                             'Iterator protocol — returns the next element'],
                // Arithmetic operators
                ['__add',      'define __add(self, other: ${1:Self}) -> ${2:Self} {\n\t$0\n}',           '+ operator'],
                ['__sub',      'define __sub(self, other: ${1:Self}) -> ${2:Self} {\n\t$0\n}',           '- operator'],
                ['__mul',      'define __mul(self, other: ${1:Self}) -> ${2:Self} {\n\t$0\n}',           '* operator'],
                ['__div',      'define __div(self, other: ${1:Self}) -> ${2:Self} {\n\t$0\n}',           '/ operator'],
                // Comparison operators
                ['__eq',       'define __eq(self, other: ${1:Self}) -> Bool {\n\t$0\n}',                 '== operator'],
                ['__ne',       'define __ne(self, other: ${1:Self}) -> Bool {\n\t$0\n}',                 '!= operator (falls back to !__eq if absent)'],
                ['__lt',       'define __lt(self, other: ${1:Self}) -> Bool {\n\t$0\n}',                 '< operator'],
                ['__le',       'define __le(self, other: ${1:Self}) -> Bool {\n\t$0\n}',                 '<= operator'],
                ['__gt',       'define __gt(self, other: ${1:Self}) -> Bool {\n\t$0\n}',                 '> operator'],
                ['__ge',       'define __ge(self, other: ${1:Self}) -> Bool {\n\t$0\n}',                 '>= operator'],
                // Context manager
                ['__enter',    'define __enter(self) -> void {\n\t$0\n}',                                'Context manager open — with obj as x { }'],
                ['__exit',     'define __exit(self) -> void {\n\t$0\n}',                                 'Context manager close — always runs'],
            ];
            for (const [name, snippet, doc] of magicMethods) {
                addItem(name, vscode.CompletionItemKind.Method, 'magic method', snippet, doc);
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
                        const parsed = this.formatDocstring(currentDocstring);
                        item.documentation = parsed.md;
                        if (parsed.deprecated) item.tags = [vscode.CompletionItemTag.Deprecated];
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

    // Read the --- docstring block that sits directly above `struct TypeName` in a .qk file.
    // Returns null if no file or docstring is found.
    public getStructDocHover(projectRoot: string, currentFile: string, typeName: string): vscode.MarkdownString | null {
        const filePath = this.findFileContainingStruct(projectRoot, currentFile, typeName);
        if (!filePath) return null;
        const content = this.readFile(filePath);
        if (!content) return null;

        const lines = content.split(/\r?\n/);
        // Find the `struct TypeName` line
        const structLineIdx = lines.findIndex(l => new RegExp(`^\\s*struct\\s+${typeName}\\b`).test(l));
        if (structLineIdx < 0) return null;

        // Backward scan for the closing --- of the docstring above
        const docLines: string[] = [];
        let i = structLineIdx - 1;
        // Skip blank lines between struct and docstring
        while (i >= 0 && lines[i].trim() === '') i--;
        if (i < 0 || lines[i].trim() !== '---') return null;
        i--; // step past closing ---
        while (i >= 0 && lines[i].trim() !== '---') {
            docLines.unshift(lines[i]);
            i--;
        }

        if (docLines.length === 0) return null;

        const md = new vscode.MarkdownString();
        md.isTrusted = true;
        md.appendMarkdown(`**\`${typeName}\`** — *${vscode.workspace.asRelativePath(filePath)}*\n\n---\n`);
        const formatted = this.formatDocstring(docLines);
        md.appendMarkdown(formatted.md.value);
        return md;
    }
}