import * as vscode from 'vscode';

export class QuirkHoverProvider implements vscode.HoverProvider {
    public async provideHover(
        document: vscode.TextDocument,
        position: vscode.Position,
        token: vscode.CancellationToken
    ): Promise<vscode.Hover | null> {
        const range = document.getWordRangeAtPosition(position, /[a-zA-Z0-9_]+/);
        if (!range) return null;

        const word = document.getText(range);

        if (['use', 'from'].includes(word)) return new vscode.Hover('**Keyword**: Import a module.\n\nExample: `use core.sys`');
        if (word === 'define') return new vscode.Hover('**Keyword**: Define a function.\n\nExample: `define main() -> void { ... }`');
        if (word === 'struct') return new vscode.Hover('**Keyword**: Define a data structure.\n\nExample: `struct Vector { x: double }`');
        if (['int', 'double', 'bool', 'cstring'].includes(word)) return new vscode.Hover(`**Primitive Type**: \`${word}\``);

        try {
            const definitions = await vscode.commands.executeCommand<vscode.Location[]>(
                'vscode.executeDefinitionProvider',
                document.uri,
                position
            );

            if (definitions && definitions.length > 0) {
                const def = definitions[0];
                const targetDoc = await vscode.workspace.openTextDocument(def.uri);

                const defLineText = targetDoc.lineAt(def.range.start.line).text;
                let signature = defLineText.split('{')[0].trim();

                let docstring: string[] = [];
                let lineNum = def.range.start.line - 1;
                let readingDocBlock = false;

                while (lineNum >= 0) {
                    const lineText = targetDoc.lineAt(lineNum).text.trim();

                    if (!readingDocBlock) {
                        if (lineText === '---') readingDocBlock = true;
                        else if (lineText !== '') break;
                    } else {
                        if (lineText === '---') break;
                        else docstring.unshift(lineText);
                    }
                    lineNum--;
                }

                // ==========================================
                // ✨ Multi-Line List Formatter (Clean Layout)
                // ==========================================
                const md = new vscode.MarkdownString();
                md.appendCodeblock(signature, 'quirk');

                if (docstring.length > 0) {
                    md.appendMarkdown('\n---\n');

                    let description: string[] = [];
                    let paramsList: string[] = [];
                    let returnsText = "";
                    let readingParamsList = false;

                    for (const line of docstring) {
                        const trimmed = line.trim();

                        // 1. Single-line @param (e.g., @param url Description)
                        const singleParamMatch = /^@param\s+(?:\*\*)?([a-zA-Z0-9_]+)(?:\*\*)?\s*(.*)/.exec(trimmed);
                        if (singleParamMatch && singleParamMatch[1] !== ':') {
                            paramsList.push(`* \`${singleParamMatch[1]}\` — ${singleParamMatch[2]}`);
                            readingParamsList = false;
                            continue;
                        }

                        // 2. `@param :` Header
                        if (/^@params?\s*:?$/.test(trimmed)) {
                            readingParamsList = true;
                            continue;
                        }

                        // 3. Bullet points inside the `@param :` block
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

                        // 4. Extract @returns
                        if (trimmed.startsWith('@return')) {
                            readingParamsList = false;
                            returnsText = trimmed.replace(/^@returns?\s+/, '').replace(/\*\*/g, '').trim();
                            continue;
                        }

                        // 5. Standard Text
                        description.push(line + '  ');
                    }

                    // Assemble the final Markdown
                    if (description.length > 0) md.appendMarkdown(description.join('\n') + '\n\n');

                    if (paramsList.length > 0) {
                        md.appendMarkdown('**Parameters:**\n\n' + paramsList.join('\n') + '\n\n');
                    }

                    if (returnsText) {
                        md.appendMarkdown(`**Returns:** ${returnsText}\n`);
                    }
                }

                return new vscode.Hover(md);
            }
        } catch (e) { }

        return null;
    }
}