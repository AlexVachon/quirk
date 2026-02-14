import * as vscode from 'vscode';

// Standard Quirk keywords and built-in types/functions
const KEYWORDS = new Set([
    'define', 'struct', 'if', 'else', 'elif', 'while', 'for', 'in', 
    'return', 'break', 'continue', 'use', 'from', 'with', 'as', 
    'extern', 'true', 'false', 'null', 'del', 'init', 'def', 'extend'
]);

const BUILTINS = new Set([
    'print', 'printf', 'malloc', 'free', 'exit', 'String', 'List', 'Map', 
    'File', 'int', 'double', 'bool', 'cstring', 'any', 'void', 'ptr', 'self'
]);

export function refreshDiagnostics(doc: vscode.TextDocument, quirkDiagnostics: vscode.DiagnosticCollection): void {
    if (doc.languageId !== 'quirk') {
        return;
    }

    const diagnostics: vscode.Diagnostic[] = [];
    const text = doc.getText();
    const lines = text.split(/\r?\n/);

    // ==========================================
    // PASS 1: Collect File-Level Globals & Imports
    // ==========================================
    const fileGlobals = new Set<string>();
    
    for (const line of lines) {
        // Mask strings and comments so they don't trigger false definitions
        let cleanLine = line.replace(/\/\/.*/, '').replace(/"(?:\\.|[^"\\])*"/g, '');

        // Structs & Functions
        let match = /^\s*(?:extern\s+)?(?:struct|define|def|init)\s+([a-zA-Z_]\w*)/.exec(cleanLine);
        if (match) fileGlobals.add(match[1]);

        // Imports: use path.to.module
        match = /^\s*use\s+([.a-zA-Z0-9_/]+)/.exec(cleanLine);
        if (match) {
            const alias = match[1].split(/[\.\/]/).pop();
            if (alias) fileGlobals.add(alias);
        }

        // Imports: from path use { A, B }
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

    for (let i = 0; i < lines.length; i++) {
        const originalLine = lines[i];
        
        // Create a "masked" line where strings and comments are replaced by spaces.
        // This preserves the exact character indices for accurate error underlining!
        let maskedLine = originalLine;
        maskedLine = maskedLine.replace(/\/\/.*/, (m) => ' '.repeat(m.length));
        maskedLine = maskedLine.replace(/"([^"\\]|\\.)*"/g, (m) => ' '.repeat(m.length));

        if (maskedLine.trim() === '') continue;

        // Skip import lines entirely
        if (/^\s*(use|from)\b/.test(maskedLine)) continue;

        // --- SCOPE TRACKING ---

        // Enter/Exit Struct Scope
        if (/^\s*struct\s+[a-zA-Z_]/.test(maskedLine)) inStruct = true;
        if (inStruct && maskedLine.includes('}')) inStruct = false;

        // Mask out struct field definitions (x: double) so 'x' isn't flagged
        if (inStruct) {
            maskedLine = maskedLine.replace(/^\s*([a-zA-Z_]\w*)\s*:/, (match, p1) => {
                return match.replace(p1, ' '.repeat(p1.length));
            });
        }

        // Enter Function Scope (Reset locals, add params)
        const funcMatch = /^\s*(?:extern\s+)?(?:define|def|init)\s+[a-zA-Z_]\w*\s*\(([^)]*)\)/.exec(maskedLine);
        if (funcMatch) {
            locals = new Set<string>(); // New function, clear old locals
            const paramsStr = funcMatch[1];
            const paramMatches = [...paramsStr.matchAll(/\b([a-zA-Z_]\w*)\s*(?::|$|,)/g)];
            for (const pm of paramMatches) {
                locals.add(pm[1]);
            }
        }

        // Local Assignments (v2 := ...)
        const assignMatch = /([a-zA-Z_]\w*)\s*(?::[a-zA-Z0-9_]+)?\s*:=/.exec(maskedLine);
        if (assignMatch) locals.add(assignMatch[1]);

        // For Loops (for x in ...)
        const forMatch = /^\s*for\s+(?:ref\s+)?([a-zA-Z_]\w*)\s+in\b/.exec(maskedLine);
        if (forMatch) locals.add(forMatch[1]);

        // With blocks (with x as y)
        const withMatch = /\bwith\s+.*\s+as\s+([a-zA-Z_]\w*)\b/.exec(maskedLine);
        if (withMatch) locals.add(withMatch[1]);


        // --- IDENTIFIER VALIDATION ---

        // Regex Explanation: Matches any word, BUT uses a lookbehind `(?<!\.)` 
        // to ignore words that follow a dot (e.g., ignores 'x' in 'v2.x')
        const identRegex = /(?<!\.)\b([a-zA-Z_]\w*)\b/g;
        let match;

        while ((match = identRegex.exec(maskedLine)) !== null) {
            const ident = match[1];

            // If it's a known word, it's valid.
            if (KEYWORDS.has(ident) || BUILTINS.has(ident) || fileGlobals.has(ident) || locals.has(ident)) {
                continue;
            }

            // It's undefined! Create a warning.
            const range = new vscode.Range(i, match.index, i, match.index + ident.length);
            const diagnostic = new vscode.Diagnostic(
                range, 
                `'${ident}' is not defined. (Missing import or assignment?)`, 
                vscode.DiagnosticSeverity.Warning
            );
            
            // Set error code for visual styling
            diagnostic.code = "undefined_symbol";
            diagnostics.push(diagnostic);
        }
    }

    quirkDiagnostics.set(doc.uri, diagnostics);
}

// Subscribes the linter to VS Code document events
export function subscribeToDocumentChanges(context: vscode.ExtensionContext, quirkDiagnostics: vscode.DiagnosticCollection): void {
    if (vscode.window.activeTextEditor) {
        refreshDiagnostics(vscode.window.activeTextEditor.document, quirkDiagnostics);
    }

    context.subscriptions.push(
        vscode.window.onDidChangeActiveTextEditor(editor => {
            if (editor) refreshDiagnostics(editor.document, quirkDiagnostics);
        })
    );

    context.subscriptions.push(
        vscode.workspace.onDidChangeTextDocument(e => refreshDiagnostics(e.document, quirkDiagnostics))
    );

    context.subscriptions.push(
        vscode.workspace.onDidCloseTextDocument(doc => quirkDiagnostics.delete(doc.uri))
    );
}