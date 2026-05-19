import * as vscode from 'vscode';
import { QuirkCompletionProvider } from './CompletionProvider';

const _sharedFormatter = new QuirkCompletionProvider();

export class QuirkHoverProvider implements vscode.HoverProvider {
    public async provideHover(
        document: vscode.TextDocument,
        position: vscode.Position,
        token: vscode.CancellationToken
    ): Promise<vscode.Hover | null> {

        const range = document.getWordRangeAtPosition(position, /[a-zA-Z0-9_]+/);
        if (!range) return null;
        const word = document.getText(range);

        // ---- Keyword hovers ----
        const keywordHovers: Record<string, string> = {
            'use':      '**`use`** — import a module.\n\n```quirk\nuse sys\n```',
            'from':     '**`from`** — destructuring import.\n\n```quirk\nfrom net.http use { request }\n```',
            'define':   '**`define`** — declare a function.\n\n```quirk\ndefine greet(name: String) -> void { ... }\n```',
            'struct':    '**`struct`** — declare a data structure.\n\n```quirk\nstruct Point { x: Int  y: Int }\n```',
            'interface': '**`interface`** — declare an interface (abstract contract).\n\n```quirk\ninterface Printable {\n    define __str(self) -> String\n}\n```',
            'enum':      '**`enum`** — declare an enumeration.\n\n```quirk\nenum Direction { North South East West }\n```',
            'where':     '**`where`** — generic type constraint.\n\n```quirk\ndefine max[T](a: T, b: T) -> T where T: Comparable { ... }\n```',
            'try':      '**`try`** — begin an exception-safe block.',
            'catch':    '**`catch`** — handle a thrown exception.\n\n```quirk\ncatch (e: Exception) { print(e.message) }\n```',
            'throw':    '**`throw`** — raise an exception. Bare `throw` re-raises the current exception.\n\n```quirk\nthrow TypeError("Expected Int")\n```',
            'finally':  '**`finally`** — block that always runs after try/catch, whether or not an exception was thrown.\n\n```quirk\ntry { f := File("x.txt", "r") } catch (e: Exception) { ... } finally { f.close() }\n```',
            'return':   '**`return`** — return a value from a function.',
            'for':      '**`for`** — iterate over a collection.\n\n```quirk\nfor item in list { ... }\n```',
            'while':    '**`while`** — loop while a condition holds.',
            'if':       '**`if`** — conditional branch.',
            'elif':     '**`elif`** — else-if branch.',
            'else':     '**`else`** — fallback branch.',
            'with':     '**`with`** — context-managed block (auto-close).\n\n```quirk\nwith File("f.txt", "r") as f { ... }\n```',
            'const':    '**`const`** — declare a constant (immutable) variable.\n\n```quirk\nconst PI := 3.14159\n```',
            'super':    '**`super`** — reference the parent struct.\n\n```quirk\nsuper().__init("message")\n```',
            'self':     '**`self`** — reference the current struct instance.',
            'true':     '**`true`** — boolean literal `true`',
            'false':    '**`false`** — boolean literal `false`',
            'null':     '**`null`** — null / no value',
        };
        if (word in keywordHovers) {
            const md = new vscode.MarkdownString(keywordHovers[word]);
            md.isTrusted = true;
            return new vscode.Hover(md);
        }

        // ---- Magic method descriptions — used as fallback only, not an early return ----
        // Priority: specific docstring from the .quirk file > generic description below.
        const magicHovers: Record<string, string> = {
            '__init':     '**`__init`** — constructor, called when the struct is instantiated.\n\n```quirk\ndefine __init(self, message: String) -> void { ... }\n```',
            '__del':      '**`__del`** — destructor, called when the struct instance is destroyed.',
            '__str':      '**`__str`** — human-readable string conversion, used by `print()` and string concatenation.\n\n```quirk\ndefine __str(self) -> String { return "MyStruct(" + self.value.str() + ")" }\n```',
            '__repr':     '**`__repr`** — developer representation. Used as a fallback when `__str` is absent.\n\n```quirk\ndefine __repr(self) -> String { return "MyStruct{value=" + self.value.str() + "}" }\n```',
            '__bool':     '**`__bool`** — truthiness. Enables `if obj:`, `not obj`, and `while obj:`.\n\n```quirk\ndefine __bool(self) -> Bool { return self.size > 0 }\n```',
            '__len':      '**`__len`** — length protocol. Called when `.length` is accessed on the struct.\n\n```quirk\ndefine __len(self) -> Int { return self.items.length }\n```',
            '__get':      '**`__get`** — index read. Called for `obj[i]`.\n\n```quirk\ndefine __get(self, index: Int) -> Any { return self.data[index] }\n```',
            '__set':      '**`__set`** — index write. Called for `obj[i] = v`.\n\n```quirk\ndefine __set(self, index: Int, value: Any) -> void { self.data[index] = value }\n```',
            '__iter':     '**`__iter`** — iterator factory. Enables `for item in obj:`.\n\n```quirk\ndefine __iter(self) -> MyIterator { return MyIterator(self) }\n```',
            '__has_next': '**`__has_next`** — iterator protocol. Return `true` if more elements remain.',
            '__next':     '**`__next`** — iterator protocol. Advance and return the next element.',
            '__add':      '**`__add`** — `+` operator.\n\n```quirk\ndefine __add(self, other: Vec2) -> Vec2 { return Vec2(self.x + other.x, self.y + other.y) }\n```',
            '__sub':      '**`__sub`** — `-` operator.',
            '__mul':      '**`__mul`** — `*` operator.',
            '__div':      '**`__div`** — `/` operator.',
            '__eq':       '**`__eq`** — `==` operator. Also used for `!=` if `__ne` is not defined.\n\n```quirk\ndefine __eq(self, other: Point) -> Bool { return self.x == other.x and self.y == other.y }\n```',
            '__ne':       '**`__ne`** — `!=` operator. Falls back to `!__eq` if not defined.',
            '__lt':       '**`__lt`** — `<` operator.',
            '__le':       '**`__le`** — `<=` operator.',
            '__gt':       '**`__gt`** — `>` operator.',
            '__ge':       '**`__ge`** — `>=` operator.',
            '__enter':    '**`__enter`** — context manager open. Called at the start of `with obj as x { }`.',
            '__exit':     '**`__exit`** — context manager close. Always called at the end of `with`, even if an exception occurs.',
            '__name':     '**`__name`** — the struct\'s name as a `String`. When accessed on `self`, returns the compile-time class name. When accessed on a `Type` instance (`self.__class.__name`), reads the stored name.\n\n```quirk\nprint(self.__name)           // "TypeError"\nprint(self.__class.__name)   // "TypeError"\n```',
            '__parent':   '**`__parent`** — the parent struct\'s name as a `String`. Only meaningful on a `Type` instance (`self.__class.__parent`).\n\n```quirk\nprint(self.__class.__parent)  // "Exception"\n```',
            '__class':    '**`__class`** — magic attribute. Returns a `Type` descriptor for the enclosing struct.\n\nAccess `.__name` and `.__parent` on the result.\n\n```quirk\nprint(self.__class.__name)    // "TypeError"\nprint(self.__class.__parent)  // "Exception"\n```',
        };

        // ---- Built-in function / literal hovers (not struct types) ----
        const builtinFnHovers: Record<string, string> = {
            'Any':       '**Built-in type** `Any`\n\nDynamic type — accepts any value.',
            'void':      '**Type** `void` — no return value.',
            'print':     '**Built-in** `print(value)`\n\nPrint a value to stdout followed by a newline. Accepts any type — calls `.__str()` on structs automatically.',
            'printf':    '**Built-in** `printf(fmt, ...)`\n\nFormatted print using C-style format strings.\n\n```quirk\nprintf("%s is %d years old\\n", name, age)\n```',
            'type':      '**Built-in** `type(value) → String`\n\nReturn the type name of a value as a `String`.\n\n```quirk\ntype(42)        // "Int"\ntype("hello")   // "String"\ntype(true)      // "Bool"\ntype(3.14)      // "Double"\n```\n\nFor `Any`-typed variables the lookup is done at runtime via the tag in the boxed value.',
            'exit':      '**Built-in** `exit(code)`\n\nTerminate the program with the given exit code.\n\n```quirk\nexit(0)   // success\nexit(1)   // failure\n```',
        };
        if (word in builtinFnHovers) {
            const md = new vscode.MarkdownString(builtinFnHovers[word]);
            md.isTrusted = true;
            return new vscode.Hover(md);
        }

        // ---- Built-in struct types — read docstrings live from the .quirk lib files ----
        const builtinStructTypes = new Set([
            // Primitive types
            'String', 'Int', 'Double', 'Bool',
            // Collections
            'List', 'Map', 'Tuple', 'Set', 'Queue', 'Callable', 'File',
            // Typing interfaces
            'Printable', 'Equatable', 'Comparable', 'Hashable',
            'Parseable', 'Sizeable', 'Iterable', 'Iterator', 'Representable', 'Primitive',
            // Iterator types
            'ListIterator', 'MapIterator', 'MapPairIterator', 'TupleIterator', 'SetIterator', 'QueueIterator', 'StringIterator',
            // Exceptions
            'Exception', 'TypeError', 'ValueError', 'IndexError', 'KeyError',
            'IOError', 'FileNotFoundError', 'RuntimeError', 'NotImplementedError',
            'SocketError', 'ZeroDivisionError', 'AssertionError', 'NullError', 'WhereConditionError',
        ]);
        if (builtinStructTypes.has(word)) {
            const projectRoot = _sharedFormatter.findProjectRoot(document.uri.fsPath);
            const docMd = _sharedFormatter.getStructDocHover(projectRoot, document.uri.fsPath, word);
            if (docMd) return new vscode.Hover(docMd);
        }

        // ---- Definition-based hover ----
        try {
            const definitions = await vscode.commands.executeCommand<vscode.Location[]>(
                'vscode.executeDefinitionProvider',
                document.uri,
                position
            );

            if (definitions && definitions.length > 0) {
                const def = definitions[0];
                const targetDoc = await vscode.workspace.openTextDocument(def.uri);
                const defLine = targetDoc.lineAt(def.range.start.line).text;
                const md = new vscode.MarkdownString();
                md.isTrusted = true;

                // =====================================================
                // MODULE HOVER
                // ImportProvider resolves `use encoding.base64` to
                // Position(0, 0) of the target file.  Scan forward for
                // a file-level  ---  docstring block at the top.
                // =====================================================
                if (def.range.start.line === 0 && def.range.start.character === 0) {
                    const relPath = vscode.workspace.asRelativePath(def.uri);
                    md.appendMarkdown(`**Module** \`${word}\`\n\n*${relPath}*\n`);

                    // Forward scan for opening --- ... closing ---
                    // Skip blank lines, comments, and import lines (from/use) before the block.
                    const docLines: string[] = [];
                    let inDocBlock = false;
                    for (let i = 0; i < Math.min(targetDoc.lineCount, 60); i++) {
                        const t = targetDoc.lineAt(i).text.trim();
                        if (!inDocBlock) {
                            if (t === '---') {
                                inDocBlock = true;
                            } else if (t !== '' && !t.startsWith('//') && !t.startsWith('from ') && !t.startsWith('use ')) {
                                break; // non-blank, non-import line before --- → no file docstring
                            }
                        } else {
                            if (t === '---') { break; } // closing ---
                            docLines.push(targetDoc.lineAt(i).text);
                        }
                    }

                    if (docLines.length > 0) {
                        md.appendMarkdown('\n---\n');
                        const formatted = _sharedFormatter.formatDocstring(docLines);
                        md.appendMarkdown(formatted.md.value);
                    }

                    return new vscode.Hover(md);
                }

                // =====================================================
                // FUNCTION / STRUCT / VARIABLE HOVER
                // =====================================================
                const signature = defLine.split('{')[0].trim();

                // Backward scan for `@decorator` lines stacked above the
                // definition. Stops on the first non-decorator non-blank line
                // (or a `---` docstring boundary, which the docstring scanner
                // handles separately below). Decorators are prepended to the
                // signature code block in source order so hovers read like
                // the file:
                //   @cached
                //   @logged
                //   define square(x: Int) -> Int
                const decoratorLines: string[] = [];
                {
                    let dLine = def.range.start.line - 1;
                    const decRe = /^\s*(@[a-zA-Z_]\w*(?:\.[a-zA-Z_]\w*)*(?:\s*\([^)]*\))?)\s*$/;
                    while (dLine >= 0) {
                        const t = targetDoc.lineAt(dLine).text;
                        const trimmed = t.trim();
                        if (trimmed === '') { dLine--; continue; }
                        const m = decRe.exec(t);
                        if (!m) break;
                        decoratorLines.unshift(m[1]);
                        dLine--;
                    }
                }

                const codeBlock = decoratorLines.length
                    ? decoratorLines.join('\n') + '\n' + signature
                    : signature;
                md.appendCodeblock(codeBlock, 'quirk');

                if (def.uri.fsPath !== document.uri.fsPath) {
                    const relPath = vscode.workspace.asRelativePath(def.uri);
                    md.appendMarkdown(`\n*Defined in* \`${relPath}\`\n`);
                }

                // Backward scan for --- docstring above the definition
                const docstring: string[] = [];
                let lineNum = def.range.start.line - 1;
                let readingDocBlock = false;
                let docBlockOpenLine = -1;
                let docBlockCloseLine = -1;
                const decRe = /^\s*@[a-zA-Z_]\w*(?:\.[a-zA-Z_]\w*)*(?:\s*\([^)]*\))?\s*$/;
                while (lineNum >= 0) {
                    const rawLine = targetDoc.lineAt(lineNum).text;
                    const t = rawLine.trim();
                    if (!readingDocBlock) {
                        // Skip decorator lines between the def and a potential
                        // docstring above them — they're shown in the codeblock
                        // header by the decorator scan above, not in the doc.
                        if (decRe.test(rawLine)) { lineNum--; continue; }
                        if (t === '---') { readingDocBlock = true; docBlockCloseLine = lineNum; }
                        else if (t.startsWith('---') && t.endsWith('---') && t.length > 6) {
                            // Inline single-line docstring: --- text ---
                            docstring.unshift(t.slice(3, -3).trim());
                            docBlockOpenLine = lineNum;
                            docBlockCloseLine = lineNum;
                            break;
                        }
                        else if (t !== '') { break; }
                    } else {
                        if (t === '---') { docBlockOpenLine = lineNum; break; }
                        else { docstring.unshift(rawLine); }
                    }
                    lineNum--;
                }

                // Module-doc heuristic: a docstring is the *module's* doc only
                // when (a) nothing non-blank precedes its opening `---`, AND
                // (b) there's a blank-line gap between its closing `---` and
                // the definition. Without the gap, the block is touching the
                // definition and belongs to it — even when it sits at the
                // very top of the file.
                if (docBlockOpenLine >= 0) {
                    // "Gap" = non-blank non-decorator content between the
                    // closing `---` and the definition. Decorators stacked
                    // above the def don't count as a gap — the doc still
                    // belongs to the decorated function.
                    let hasGap = false;
                    for (let k = docBlockCloseLine + 1; k < def.range.start.line; k++) {
                        const ln = targetDoc.lineAt(k).text;
                        const tr = ln.trim();
                        if (tr === '' || decRe.test(ln)) continue;
                        hasGap = true;
                        break;
                    }
                    let isModuleDoc = hasGap;
                    if (isModuleDoc) {
                        for (let j = 0; j < docBlockOpenLine; j++) {
                            if (targetDoc.lineAt(j).text.trim() !== '') { isModuleDoc = false; break; }
                        }
                    }
                    if (isModuleDoc) docstring.length = 0;
                }

                if (docstring.length > 0) {
                    md.appendMarkdown('\n---\n');
                    const formatted = _sharedFormatter.formatDocstring(docstring);
                    md.appendMarkdown(formatted.md.value);
                } else if (word in magicHovers) {
                    // No specific docstring — fall back to the generic magic method description.
                    md.appendMarkdown('\n---\n');
                    md.appendMarkdown(magicHovers[word]);
                }

                // For variable hovers (not define/struct lines) show inferred type
                const isDefLine = /^\s*(?:extern\s+)?(?:define|def|init|struct|enum|interface)\b/.test(defLine);
                if (!isDefLine) {
                    const inferredType = _sharedFormatter.inferTypeOfVariable(document, position, word);
                    if (inferredType) {
                        md.appendMarkdown(`\n\n**Type:** \`${inferredType}\``);
                    }
                }

                return new vscode.Hover(md);
            }
        } catch { }

        // No definition found — last-resort fallback for magic methods.
        if (word in magicHovers) {
            const md = new vscode.MarkdownString(magicHovers[word]);
            md.isTrusted = true;
            return new vscode.Hover(md);
        }

        return null;
    }
}