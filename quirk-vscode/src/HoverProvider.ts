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
            'throw':    '**`throw`** — raise an exception.\n\n```quirk\nthrow TypeError("Expected Int")\n```',
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

        // ---- Built-in type hovers ----
        const builtinHovers: Record<string, string> = {
            'String':    '**Built-in type** `String`\n\nUTF-8 string with methods: `.length`, `.substring()`, `.split()`, `.trim()`, etc.',
            'Int':       '**Built-in type** `Int`\n\n32-bit signed integer.',
            'Double':    '**Built-in type** `Double`\n\n64-bit floating-point number.',
            'Bool':      '**Built-in type** `Bool`\n\n`true` or `false`.',
            'Char':      '**Built-in type** `Char`\n\nA single character.',
            'List':      '**Built-in type** `List`\n\nDynamic array. Methods: `.append()`, `.pop()`, `.length`, etc.',
            'Map':       '**Built-in type** `Map`\n\nHash map. Methods: `.put()`, `.get()`, `.has()`, `.len()`, etc.',
            'File':      '**Built-in type** `File`\n\nFile handle. Methods: `.read()`, `.write()`, `.close()`.',
            'Any':       '**Built-in type** `Any`\n\nDynamic type — accepts any value.',
            'void':      '**Type** `void` — no return value.',
            'Exception': '**Built-in** `Exception`\n\nBase exception class. Fields: `.message`, `.type`.',
            'TypeError': '**Built-in** `TypeError` : `Exception`\n\nRaised for type mismatches.',
            'ValueError':'**Built-in** `ValueError` : `Exception`\n\nRaised for invalid values.',
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
                        md.appendMarkdown(formatted.value);
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
                while (lineNum >= 0) {
                    const t = targetDoc.lineAt(lineNum).text.trim();
                    if (!readingDocBlock) {
                        if (t === '---') { readingDocBlock = true; }
                        else if (t !== '') { break; }
                    } else {
                        if (t === '---') { break; }
                        else { docstring.unshift(t); }
                    }
                    lineNum--;
                }

                if (docstring.length > 0) {
                    md.appendMarkdown('\n---\n');
                    const formatted = _sharedFormatter.formatDocstring(docstring);
                    md.appendMarkdown(formatted.value);
                }

                // For variable hovers (not define/struct lines) show inferred type
                const isDefLine = /^\s*(?:extern\s+)?(?:define|def|init|struct)\b/.test(defLine);
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