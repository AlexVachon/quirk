import * as vscode from 'vscode';

const KEYWORDS = new Set([
    'define', 'struct', 'if', 'else', 'elif', 'while', 'for', 'in',
    'return', 'break', 'continue', 'use', 'from', 'with', 'as',
    'extern', 'true', 'false', 'null', 'del', 'init', 'def', 'extend'
]);

const BUILTINS = new Set([
    'print', 'printf', 'malloc', 'free', 'exit', 'String', 'List', 'Map',
    'File', 'int', 'double', 'bool', 'cstring', 'any', 'void', 'ptr', 'self'
]);

function maskLine(line: string): string {
    let masked = "";
    let inString = false;
    let inInterpolation = false;
    let braceDepth = 0;

    for (let j = 0; j < line.length; j++) {
        const char = line[j];
        const nextChar = line[j + 1];

        if (!inString) {
            if (char === '/' && nextChar === '/') {
                masked += ' '.repeat(line.length - j);
                break;
            } else if (char === '"') {
                inString = true;
                masked += ' ';
            } else {
                masked += char;
            }
        } else {
            if (char === '\\') {
                masked += '  ';
                j++;
            } else if (!inInterpolation && char === '$' && nextChar === '{') {
                inInterpolation = true;
                braceDepth = 1;
                masked += '  ';
                j++;
            } else if (inInterpolation) {
                if (char === '{') braceDepth++;
                if (char === '}') braceDepth--;

                if (braceDepth === 0) {
                    inInterpolation = false;
                    masked += ' ';
                } else {
                    masked += char;
                }
            } else if (char === '"') {
                inString = false;
                masked += ' ';
            } else {
                masked += ' ';
            }
        }
    }
    return masked;
}

export function refreshDiagnostics(doc: vscode.TextDocument, quirkDiagnostics: vscode.DiagnosticCollection): void {
    if (doc.languageId !== 'quirk') return;

    const diagnostics: vscode.Diagnostic[] = [];
    const text = doc.getText();
    const lines = text.split(/\r?\n/);

    // ==========================================
    // PASS 1: Collect File-Level Globals & Imports
    // ==========================================
    const fileGlobals = new Set<string>();
    let inDocBlock = false;

    for (const line of lines) {
        // --- SKIP DOC BLOCKS ---
        if (line.trim() === '---') { inDocBlock = !inDocBlock; continue; }
        if (inDocBlock) continue;

        const cleanLine = maskLine(line);

        let match = /^\s*(?:extern\s+)?(?:struct|define|def|init)\s+([a-zA-Z_]\w*)/.exec(cleanLine);
        if (match) fileGlobals.add(match[1]);

        match = /^\s*use\s+([.a-zA-Z0-9_/]+)/.exec(cleanLine);
        if (match) {
            const alias = match[1].split(/[\.\/]/).pop();
            if (alias) fileGlobals.add(alias);
        }

        match = /^\s*from\s+[.a-zA-Z0-9_/]+\s+use\s+\{([^}]*)\}/.exec(cleanLine);
        if (match) {
            const symbols = match[1].split(',').map(s => s.trim());
            symbols.forEach(s => { if (s) fileGlobals.add(s); });
        }
    }

    // ==========================================
    // PASS 2: Line-by-Line Scope Analysis
    // ==========================================
    let locals = new Set<string>();
    let inStruct = false;
    inDocBlock = false; // Reset for pass 2

    for (let i = 0; i < lines.length; i++) {
        const originalLine = lines[i];

        // --- SKIP DOC BLOCKS ---
        if (originalLine.trim() === '---') { inDocBlock = !inDocBlock; continue; }
        if (inDocBlock) continue;

        let maskedLine = maskLine(originalLine);

        if (maskedLine.trim() === '') continue;
        if (/^\s*(use|from)\b/.test(maskedLine)) continue;

        if (/^\s*struct\s+[a-zA-Z_]/.test(maskedLine)) inStruct = true;
        if (inStruct && maskedLine.includes('}')) inStruct = false;

        if (inStruct) {
            maskedLine = maskedLine.replace(/^\s*([a-zA-Z_]\w*)\s*:/, (match, p1) => match.replace(p1, ' '.repeat(p1.length)));
        }

        const funcMatch = /^\s*(?:extern\s+)?(?:define|def|init)\s+[a-zA-Z_]\w*\s*\(([^)]*)\)/.exec(maskedLine);
        if (funcMatch) {
            locals = new Set<string>();
            const paramsStr = funcMatch[1];
            const paramMatches = [...paramsStr.matchAll(/\b([a-zA-Z_]\w*)\s*(?::|$|,)/g)];
            for (const pm of paramMatches) locals.add(pm[1]);
        }

        const assignMatch = /([a-zA-Z_]\w*)\s*(?::[a-zA-Z0-9_]+)?\s*:=/.exec(maskedLine);
        if (assignMatch) locals.add(assignMatch[1]);

        const forMatch = /^\s*for\s+(?:ref\s+)?([a-zA-Z_]\w*)\s+in\b/.exec(maskedLine);
        if (forMatch) locals.add(forMatch[1]);

        const withMatch = /\bwith\s+.*\s+as\s+([a-zA-Z_]\w*)\b/.exec(maskedLine);
        if (withMatch) locals.add(withMatch[1]);

        const identRegex = /(?<!\.)\b([a-zA-Z_]\w*)\b/g;
        let match;

        while ((match = identRegex.exec(maskedLine)) !== null) {
            const ident = match[1];

            if (KEYWORDS.has(ident) || BUILTINS.has(ident) || fileGlobals.has(ident) || locals.has(ident)) {
                continue;
            }

            const restOfLine = maskedLine.substring(match.index + ident.length);
            if (/^\s*:(?!=)/.test(restOfLine)) {
                continue;
            }

            const range = new vscode.Range(i, match.index, i, match.index + ident.length);
            const diagnostic = new vscode.Diagnostic(
                range,
                `'${ident}' is not defined. (Missing import or assignment?)`,
                vscode.DiagnosticSeverity.Warning
            );

            diagnostic.code = "undefined_symbol";
            diagnostics.push(diagnostic);
        }
    }

    quirkDiagnostics.set(doc.uri, diagnostics);
}

export function subscribeToDocumentChanges(context: vscode.ExtensionContext, quirkDiagnostics: vscode.DiagnosticCollection): void {
    if (vscode.window.activeTextEditor) refreshDiagnostics(vscode.window.activeTextEditor.document, quirkDiagnostics);
    context.subscriptions.push(vscode.window.onDidChangeActiveTextEditor(editor => { if (editor) refreshDiagnostics(editor.document, quirkDiagnostics); }));
    context.subscriptions.push(vscode.workspace.onDidChangeTextDocument(e => refreshDiagnostics(e.document, quirkDiagnostics)));
    context.subscriptions.push(vscode.workspace.onDidCloseTextDocument(doc => quirkDiagnostics.delete(doc.uri)));
}