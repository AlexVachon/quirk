import * as vscode from 'vscode';

export class QuirkSignatureHelpProvider implements vscode.SignatureHelpProvider {
    public async provideSignatureHelp(
        document: vscode.TextDocument,
        position: vscode.Position,
        token: vscode.CancellationToken,
        context: vscode.SignatureHelpContext
    ): Promise<vscode.SignatureHelp | null> {

        const lineToCursor = document.getText(new vscode.Range(new vscode.Position(position.line, 0), position));

        // Match the innermost open call — handles nested calls and method chains
        // Supports both:  funcName(   and   obj.method(
        const callMatch = /(?:([a-zA-Z0-9_]+)\.)?([a-zA-Z0-9_]+)\s*\(([^)]*)$/.exec(lineToCursor);
        if (!callMatch) return null;

        const funcName = callMatch[2];
        const argsString = callMatch[3];
        // Count commas, but skip commas inside nested parentheses
        const activeParameter = this.countArgs(argsString) - 1;

        try {
            // Compute the position of the function name so go-to-definition finds it
            const callStart = callMatch.index + (callMatch[1] ? callMatch[1].length + 1 : 0);
            const funcPosition = new vscode.Position(position.line, callStart + funcName.length - 1);

            const definitions = await vscode.commands.executeCommand<vscode.Location[]>(
                'vscode.executeDefinitionProvider',
                document.uri,
                funcPosition
            );

            if (!definitions || definitions.length === 0) return null;

            const def = definitions[0];
            const targetDoc = await vscode.workspace.openTextDocument(def.uri);
            const defLineText = targetDoc.lineAt(def.range.start.line).text;
            const signature = defLineText.split('{')[0].trim();

            // Read docstring backwards
            let docstringLines: string[] = [];
            let lineNum = def.range.start.line - 1;
            let readingDocBlock = false;

            while (lineNum >= 0) {
                const lineText = targetDoc.lineAt(lineNum).text.trim();
                if (!readingDocBlock) {
                    if (lineText === '---') { readingDocBlock = true; }
                    else if (lineText !== '') { break; }
                } else {
                    if (lineText === '---') { break; }
                    else { docstringLines.unshift(lineText); }
                }
                lineNum--;
            }

            // Parse signature parameters (skip 'self')
            const sigParamMatch = /\(([^)]*)\)/.exec(signature);
            const sigParams = sigParamMatch
                ? sigParamMatch[1].split(',').map(p => p.trim()).filter(p => p && p !== 'self')
                : [];

            // Build parameter info from @param docstring tags, or fall back to raw param names
            const paramDocs: Map<string, string> = new Map();
            let description = "";
            let returnsText = "";

            for (const line of docstringLines) {
                const paramMatch = /@param\s+([a-zA-Z0-9_]+)\s+(.*)/.exec(line);
                if (paramMatch) {
                    paramDocs.set(paramMatch[1], paramMatch[2]);
                } else if (line.startsWith('@return')) {
                    returnsText = line.replace(/@returns?\s+/, '').replace(/\*\*/g, '').trim();
                } else if (!line.startsWith('@')) {
                    description += `${line}\n`;
                }
            }

            const parameters: vscode.ParameterInformation[] = sigParams.map(p => {
                const pName = p.split(':')[0].trim();
                const pDoc = paramDocs.get(pName);
                return new vscode.ParameterInformation(p, pDoc ? new vscode.MarkdownString(pDoc) : undefined);
            });

            let fullDescription = description.trim();
            if (returnsText) fullDescription += `\n\n**Returns:** ${returnsText}`;

            const signatureInfo = new vscode.SignatureInformation(
                signature,
                new vscode.MarkdownString(fullDescription)
            );
            signatureInfo.parameters = parameters;

            const help = new vscode.SignatureHelp();
            help.signatures = [signatureInfo];
            help.activeSignature = 0;
            help.activeParameter = Math.min(activeParameter, Math.max(0, parameters.length - 1));

            return help;
        } catch { }

        return null;
    }

    /**
     * Count argument slots up to the cursor, respecting nested parentheses.
     * "foo, bar(x, y), " → 3 slots
     */
    private countArgs(argsString: string): number {
        let count = 1;
        let depth = 0;
        for (const ch of argsString) {
            if (ch === '(') depth++;
            else if (ch === ')') depth--;
            else if (ch === ',' && depth === 0) count++;
        }
        return count;
    }
}