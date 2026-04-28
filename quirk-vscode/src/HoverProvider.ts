import * as vscode from 'vscode';
import { QuirkCompletionProvider } from './CompletionProvider';

const _sharedFormatter = new QuirkCompletionProvider({ appendLine: () => {} } as any);

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
            'use':      '**`use`** — import a module.\n\n```quirk\nuse core.sys\n```',
            'from':     '**`from`** — destructuring import.\n\n```quirk\nfrom core.http use { request }\n```',
            'define':   '**`define`** — declare a function.\n\n```quirk\ndefine greet(name: String) -> void { ... }\n```',
            'struct':   '**`struct`** — declare a data structure.\n\n```quirk\nstruct Point { x: Int  y: Int }\n```',
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
            'trigger':  '**`trigger`** — register an event handler.',
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

        // ---- Magic method / attribute hovers ----
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
        if (word in magicHovers) {
            const md = new vscode.MarkdownString(magicHovers[word]);
            md.isTrusted = true;
            return new vscode.Hover(md);
        }

        // ---- Built-in type hovers ----
        const builtinHovers: Record<string, string> = {
            'String':    '**Built-in type** `String`\n\nUTF-8 string.\n\nMethods: `.length`, `.substring()`, `.split()`, `.trim()`, `.to_int()`, `.to_float()`, `.to_bool()`, `.to_char()`, etc.\n\nAll `.to_*()` methods throw `ValueError` on invalid input.',
            'Int':       '**Built-in type** `Int`\n\n32-bit signed integer.\n\nMethods: `.str()`, `.abs()`, `.pow()`, `.to_float()`, `.is_even()`, `.is_odd()`\n\nStatic: `Int.parse(s)` — parse a string, throws `ValueError` on failure.',
            'Double':    '**Built-in type** `Double`\n\n64-bit floating-point number.\n\nMethods: `.str()`, `.to_int()`, `.abs()`, `.ceil()`, `.floor()`, `.round()`, `.sqrt()`\n\nStatic: `Double.parse(s)` — parse a string, throws `ValueError` on failure.',
            'Bool':      '**Built-in type** `Bool`\n\n`true` or `false`.\n\nMethods: `.str()`\n\nStatic: `Bool.parse(s)` — accepts `"true"` or `"false"`, throws `ValueError` otherwise.',
            'Char':      '**Built-in type** `Char`\n\nA single character.\n\nMethods: `.str()`, `.to_upper()`, `.to_lower()`, `.is_alpha()`, `.is_digit()`, `.is_space()`\n\nStatic: `Char.parse(s)` — parse a single-character string, throws `ValueError` otherwise.',
            'List':      '**Built-in type** `List`\n\nDynamic array. Methods: `.append()`, `.pop()`, `.length`, etc.',
            'Map':       '**Built-in type** `Map`\n\nHash map. Methods: `.put()`, `.get()`, `.has()`, `.len()`, etc.',
            'File':      '**Built-in type** `File`\n\nFile handle. Methods: `.read()`, `.write()`, `.close()`.',
            'Any':       '**Built-in type** `Any`\n\nDynamic type — accepts any value.',
            'void':      '**Type** `void` — no return value.',
        };
        if (word in builtinHovers) {
            const md = new vscode.MarkdownString(builtinHovers[word]);
            md.isTrusted = true;
            return new vscode.Hover(md);
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
                    const docLines: string[] = [];
                    let inDocBlock = false;
                    for (let i = 0; i < Math.min(targetDoc.lineCount, 40); i++) {
                        const t = targetDoc.lineAt(i).text.trim();
                        if (!inDocBlock) {
                            if (t === '---') {
                                inDocBlock = true;
                            } else if (t !== '' && !t.startsWith('//')) {
                                break; // non-blank line before --- → no file docstring
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
                md.appendCodeblock(signature, 'quirk');

                if (def.uri.fsPath !== document.uri.fsPath) {
                    const relPath = vscode.workspace.asRelativePath(def.uri);
                    md.appendMarkdown(`\n*Defined in* \`${relPath}\`\n`);
                }

                // Backward scan for --- docstring above the definition
                const docstring: string[] = [];
                let lineNum = def.range.start.line - 1;
                let readingDocBlock = false;
                let docBlockOpenLine = -1;
                while (lineNum >= 0) {
                    const rawLine = targetDoc.lineAt(lineNum).text;
                    const t = rawLine.trim();
                    if (!readingDocBlock) {
                        if (t === '---') { readingDocBlock = true; }
                        else if (t !== '') { break; }
                    } else {
                        if (t === '---') { docBlockOpenLine = lineNum; break; }
                        else { docstring.unshift(rawLine); }
                    }
                    lineNum--;
                }

                // If the opening --- has only blank lines before it, this is a
                // module-level docstring, not a function docstring — don't steal it.
                if (docBlockOpenLine >= 0) {
                    let isModuleDoc = true;
                    for (let j = 0; j < docBlockOpenLine; j++) {
                        if (targetDoc.lineAt(j).text.trim() !== '') { isModuleDoc = false; break; }
                    }
                    if (isModuleDoc) docstring.length = 0;
                }

                if (docstring.length > 0) {
                    md.appendMarkdown('\n---\n');
                    const formatted = _sharedFormatter.formatDocstring(docstring);
                    md.appendMarkdown(formatted.md.value);
                }

                // For variable hovers (not define/struct lines) show inferred type
                const isDefLine = /^\s*(?:extern\s+)?(?:define|def|init|struct|enum)\b/.test(defLine);
                if (!isDefLine) {
                    const inferredType = _sharedFormatter.inferTypeOfVariable(document, position, word);
                    if (inferredType) {
                        md.appendMarkdown(`\n\n**Type:** \`${inferredType}\``);
                    }
                }

                return new vscode.Hover(md);
            }
        } catch { }

        return null;
    }
}