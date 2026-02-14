import * as vscode from 'vscode';

export class QuirkSignatureHelpProvider implements vscode.SignatureHelpProvider {
    public async provideSignatureHelp(
        document: vscode.TextDocument,
        position: vscode.Position,
        token: vscode.CancellationToken,
        context: vscode.SignatureHelpContext
    ): Promise<vscode.SignatureHelp | null> {
        
        // 1. Find the function name being called and the active parameter index
        const lineToCursor = document.getText(new vscode.Range(new vscode.Position(position.line, 0), position));
        
        // This regex looks backwards for `functionName(` optionally followed by arguments and commas.
        // It naturally grabs the innermost function if they are nested (e.g., foo(bar( )).
        const callMatch = /([a-zA-Z0-9_]+)\s*\(([^)]*)$/.exec(lineToCursor);
        
        if (!callMatch) return null;
        
        const funcName = callMatch[1];
        const argsString = callMatch[2];
        const activeParameter = argsString.split(',').length - 1; // Count commas to know which arg we are on

        try {
            // 2. Find the definition of the function using VS Code's built-in command
            // We fake a position just before the '(' to lookup the function name
            const funcPosition = new vscode.Position(position.line, callMatch.index + funcName.length - 1);
            
            const definitions = await vscode.commands.executeCommand<vscode.Location[]>(
                'vscode.executeDefinitionProvider',
                document.uri,
                funcPosition
            );

            if (definitions && definitions.length > 0) {
                const def = definitions[0];
                const targetDoc = await vscode.workspace.openTextDocument(def.uri);
                
                // 3. Extract Signature and Docstring
                const defLineText = targetDoc.lineAt(def.range.start.line).text;
                const signature = defLineText.split('{')[0].trim(); // e.g., "define process_link(url: String) -> void"
                
                let docstringLines: string[] = [];
                let lineNum = def.range.start.line - 1;
                let readingDocBlock = false;
                
                while (lineNum >= 0) {
                    const lineText = targetDoc.lineAt(lineNum).text.trim();
                    if (!readingDocBlock) {
                        if (lineText === '---') readingDocBlock = true;
                        else if (lineText !== '') break;
                    } else {
                        if (lineText === '---') break;
                        else docstringLines.unshift(lineText);
                    }
                    lineNum--;
                }

                // 4. Parse @param and @returns tags
                let description = "";
                const parameters: vscode.ParameterInformation[] = [];
                
                // Extract the raw parameters from the signature so VS Code can highlight them in the popup
                const sigParamMatch = /\(([^)]*)\)/.exec(signature);
                const sigParams = sigParamMatch ? sigParamMatch[1].split(',').map(p => p.trim()) : [];

                for (const line of docstringLines) {
                    const paramMatch = /@param\s+([a-zA-Z0-9_]+)\s+(.*)/.exec(line);
                    
                    if (paramMatch) {
                        const paramName = paramMatch[1];
                        const paramDesc = paramMatch[2];
                        
                        // Link the @param to the exact text in the signature (e.g., "url: String")
                        const matchingSigParam = sigParams.find(p => p.startsWith(paramName)) || paramName;
                        parameters.push(new vscode.ParameterInformation(matchingSigParam, new vscode.MarkdownString(paramDesc)));
                    } else if (line.startsWith('@return')) {
                        // Bold the return info and add it to the bottom of the description
                        description += `\n\n**Returns:** ${line.replace(/@returns?\s+/, '')}`;
                    } else {
                        // Normal description text
                        description += `\n${line}`;
                    }
                }

                // 5. Fallback: If the user didn't write @param tags, just show the signature parts
                if (parameters.length === 0 && sigParams.length > 0) {
                    for (const p of sigParams) {
                        if (p) parameters.push(new vscode.ParameterInformation(p));
                    }
                }

                // 6. Construct the Signature Help response
                const signatureInfo = new vscode.SignatureInformation(signature, new vscode.MarkdownString(description.trim()));
                signatureInfo.parameters = parameters;

                const signatureHelp = new vscode.SignatureHelp();
                signatureHelp.signatures = [signatureInfo];
                signatureHelp.activeSignature = 0;
                signatureHelp.activeParameter = activeParameter;

                return signatureHelp;
            }
        } catch (e) {}

        return null;
    }
}