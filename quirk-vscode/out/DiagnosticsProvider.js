"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.subscribeToDocumentChanges = exports.refreshDiagnostics = void 0;
const vscode = require("vscode");
const KEYWORDS = new Set([
    'define', 'struct', 'if', 'else', 'elif', 'while', 'for', 'in',
    'return', 'break', 'continue', 'use', 'from', 'with', 'as',
    'extern', 'true', 'false', 'null', 'del', 'init', 'def', 'extend'
]);
const BUILTINS = new Set([
    'print', 'exit', 'String', 'List', 'Map',
    'File', 'Int', 'Double', 'Bool', 'any', 'void'
]);
function maskLine(line) {
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
            }
            else if (char === '"') {
                inString = true;
                masked += ' ';
            }
            else {
                masked += char;
            }
        }
        else {
            if (char === '\\') {
                masked += '  ';
                j++;
            }
            else if (!inInterpolation && char === '$' && nextChar === '{') {
                inInterpolation = true;
                braceDepth = 1;
                masked += '  ';
                j++;
            }
            else if (inInterpolation) {
                if (char === '{')
                    braceDepth++;
                if (char === '}')
                    braceDepth--;
                if (braceDepth === 0) {
                    inInterpolation = false;
                    masked += ' ';
                }
                else {
                    masked += char;
                }
            }
            else if (char === '"') {
                inString = false;
                masked += ' ';
            }
            else {
                masked += ' ';
            }
        }
    }
    return masked;
}
function refreshDiagnostics(doc, quirkDiagnostics) {
    var _a, _b;
    if (doc.languageId !== 'quirk')
        return;
    const diagnostics = [];
    const text = doc.getText();
    const lines = text.split(/\r?\n/);
    const declarations = new Map();
    const usages = new Set();
    const fileGlobals = new Set();
    let inDocBlock = false;
    // ==========================================
    // PASS 1: Collect Declarations & Globals
    // ==========================================
    for (let i = 0; i < lines.length; i++) {
        const line = lines[i];
        if (line.trim() === '---') {
            inDocBlock = !inDocBlock;
            continue;
        }
        if (inDocBlock)
            continue;
        const cleanLine = maskLine(line);
        let match = /^\s*(?:extern\s+)?(?:struct|define|def|init)\s+([a-zA-Z_]\w*)/.exec(cleanLine);
        if (match) {
            const name = match[1];
            fileGlobals.add(name);
            if (name !== 'main' && !name.startsWith('__')) {
                const startIdx = cleanLine.indexOf(name, match.index);
                declarations.set(name, new vscode.Range(i, startIdx, i, startIdx + name.length));
            }
        }
        match = /^\s*use\s+([.a-zA-Z0-9_/]+)/.exec(cleanLine);
        if (match) {
            const alias = match[1].split(/[\.\/]/).pop();
            if (alias)
                fileGlobals.add(alias);
        }
        match = /^\s*from\s+[.a-zA-Z0-9_/]+\s+use\s+\{([^}]*)\}/.exec(cleanLine);
        if (match) {
            const symbols = match[1].split(',').map(s => s.trim());
            symbols.forEach(s => { if (s)
                fileGlobals.add(s); });
        }
    }
    // ==========================================
    // PASS 2: Detailed Scope & Usage Scan
    // ==========================================
    let locals = new Set();
    let inStruct = false;
    inDocBlock = false;
    for (let i = 0; i < lines.length; i++) {
        const originalLine = lines[i];
        if (originalLine.trim() === '---') {
            inDocBlock = !inDocBlock;
            continue;
        }
        if (inDocBlock)
            continue;
        let maskedLine = maskLine(originalLine);
        if (maskedLine.trim() === '')
            continue;
        if (/^\s*(use|from)\b/.test(maskedLine))
            continue;
        if (/^\s*struct\s+[a-zA-Z_]/.test(maskedLine))
            inStruct = true;
        if (inStruct && maskedLine.includes('}'))
            inStruct = false;
        if (inStruct) {
            maskedLine = maskedLine.replace(/^\s*([a-zA-Z_]\w*)\s*:/, (match, p1) => match.replace(p1, ' '.repeat(p1.length)));
        }
        // --- DECLARATION TRACKING (Local) ---
        const funcMatch = /^\s*(?:extern\s+)?(?:define|def|init)\s+[a-zA-Z_]\w*\s*\(([^)]*)\)/.exec(maskedLine);
        if (funcMatch) {
            locals = new Set();
            const paramsStr = funcMatch[1];
            // Updated Regex: Specifically captures the name before the colon
            // and ignores the type part (e.g., captures 'url' from 'url: String')
            const paramMatches = [...paramsStr.matchAll(/\b([a-zA-Z_]\w*)\s*(?::\s*[a-zA-Z_]\w*)?(?::|$|,)/g)];
            for (const pm of paramMatches) {
                const pName = pm[1];
                locals.add(pName);
                // Skip 'self' and ensure we only flag the parameter name, not the type
                if (pName !== 'self' && !BUILTINS.has(pName)) {
                    const startIdx = originalLine.indexOf(pName, funcMatch.index);
                    declarations.set(`${i}_${pName}`, new vscode.Range(i, startIdx, i, startIdx + pName.length));
                }
            }
        }
        const assignMatch = /([a-zA-Z_]\w*)\s*(?::[a-zA-Z0-9_]+)?\s*:=/.exec(maskedLine);
        if (assignMatch) {
            const vName = assignMatch[1];
            if (!locals.has(vName)) {
                locals.add(vName);
                const startIdx = originalLine.indexOf(vName);
                declarations.set(`${i}_${vName}`, new vscode.Range(i, startIdx, i, startIdx + vName.length));
            }
        }
        // --- USAGE SCAN ---
        const identRegex = /(?<!\.)\b([a-zA-Z_]\w*)\b/g;
        let match;
        while ((match = identRegex.exec(maskedLine)) !== null) {
            const ident = match[1];
            if (KEYWORDS.has(ident) || BUILTINS.has(ident))
                continue;
            const restOfLine = maskedLine.substring(match.index + ident.length);
            if (/^\s*:(?!=)/.test(restOfLine))
                continue;
            const range = new vscode.Range(i, match.index, i, match.index + ident.length);
            // ✨ NEW FIX: Skip if this specific occurrence is a declaration 
            if (((_a = declarations.get(ident)) === null || _a === void 0 ? void 0 : _a.isEqual(range)) || ((_b = declarations.get(`${i}_${ident}`)) === null || _b === void 0 ? void 0 : _b.isEqual(range))) {
                continue;
            }
            if (locals.has(ident) || fileGlobals.has(ident)) {
                usages.add(ident); // Global usage
                for (let k = i; k >= 0; k--) {
                    if (declarations.has(`${k}_${ident}`)) {
                        usages.add(`${k}_${ident}`); // Local instance usage 
                        break;
                    }
                }
            }
            else {
                diagnostics.push(new vscode.Diagnostic(range, `'${ident}' is not defined.`, vscode.DiagnosticSeverity.Warning));
            }
        }
    }
    // ==========================================
    // PASS 3: Generate "Unused" Diagnostics 
    // ==========================================
    declarations.forEach((range, key) => {
        if (!usages.has(key)) {
            const cleanKey = key.includes('_') ? key.split('_')[1] : key;
            // 1. Change Severity to Warning for the yellow wavy underline
            const diagnostic = new vscode.Diagnostic(range, `'${cleanKey}' is declared but never used.`, vscode.DiagnosticSeverity.Warning // 
            );
            // 2. Keep the Unnecessary tag to keep the variable "faded"
            diagnostic.tags = [vscode.DiagnosticTag.Unnecessary]; // 
            // 3. Optional: Add a custom code for your Quick Fix to target
            diagnostic.code = "unused_symbol";
            diagnostics.push(diagnostic);
        }
    });
    quirkDiagnostics.set(doc.uri, diagnostics);
}
exports.refreshDiagnostics = refreshDiagnostics;
function subscribeToDocumentChanges(context, quirkDiagnostics) {
    if (vscode.window.activeTextEditor) {
        refreshDiagnostics(vscode.window.activeTextEditor.document, quirkDiagnostics);
    }
    context.subscriptions.push(vscode.window.onDidChangeActiveTextEditor(editor => {
        if (editor)
            refreshDiagnostics(editor.document, quirkDiagnostics);
    }));
    context.subscriptions.push(vscode.workspace.onDidChangeTextDocument(e => refreshDiagnostics(e.document, quirkDiagnostics)));
    context.subscriptions.push(vscode.workspace.onDidCloseTextDocument(doc => quirkDiagnostics.delete(doc.uri)));
}
exports.subscribeToDocumentChanges = subscribeToDocumentChanges;
//# sourceMappingURL=DiagnosticsProvider.js.map