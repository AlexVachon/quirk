import * as vscode from 'vscode';

const KEYWORDS = new Set([
    'define', 'struct', 'if', 'else', 'elif', 'while', 'for', 'in',
    'return', 'break', 'continue', 'use', 'from', 'with', 'as',
    'extern', 'true', 'false', 'null', 'del', 'init', 'def',
    'trigger', 'try', 'catch', 'throw', 'finally', 'and', 'or', 'not', 'super', 'enum',
    'fn'
]);

const BUILTINS = new Set([
    'print', 'exit', 'Char', 'String', 'List', 'Map',
    'File', 'Int', 'Double', 'Bool', 'Any', 'void', 'Callable',
    'true', 'false', 'null',
    'Exception', 'TypeError', 'ValueError', 'IndexError', 'KeyError',
    'IOError', 'FileNotFoundError', 'RuntimeError', 'NotImplementedError',
    'SocketError', 'ZeroDivisionError', 'AssertionError', 'NullError'
]);

function maskLine(line: string): string {
    let masked = "";
    let inString = false;
    let quoteChar = ''; 
    let inInterpolation = false;
    let braceDepth = 0;
    let nestedQuoteChar = ''; 

    for (let j = 0; j < line.length; j++) {
        const char = line[j];
        const nextChar = line[j + 1];

        if (!inString) {
            if (char === '/' && nextChar === '/') {
                masked += ' '.repeat(line.length - j);
                break;
            } else if (char === '"' || char === "'") { 
                inString = true;
                quoteChar = char; 
                masked += ' ';
            } else {
                masked += char;
            }
        } else {
            if (!inInterpolation) {
                if (char === '\\') {
                    masked += '  ';
                    j++;
                } else if (char === '$' && nextChar === '{') {
                    inInterpolation = true;
                    braceDepth = 1;
                    masked += '  ';
                    j++;
                } else if (char === quoteChar) { 
                    inString = false;
                    quoteChar = '';
                    masked += ' ';
                } else {
                    masked += ' ';
                }
            } else {
                if (nestedQuoteChar) {
                    if (char === '\\') {
                        masked += '  ';
                        j++;
                    } else if (char === nestedQuoteChar) {
                        nestedQuoteChar = ''; 
                        masked += ' ';
                    } else {
                        masked += ' '; 
                    }
                } else {
                    if (char === '"' || char === "'") {
                        nestedQuoteChar = char; 
                        masked += ' ';
                    } else if (char === '{') {
                        braceDepth++;
                        masked += char;
                    } else if (char === '}') {
                        braceDepth--;
                        if (braceDepth === 0) {
                            inInterpolation = false;
                            masked += ' ';
                        } else {
                            masked += char;
                        }
                    } else {
                        masked += char; 
                    }
                }
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

    const declarations = new Map<string, vscode.Range>();
    const usages = new Set<string>();
    const fileGlobals = new Set<string>();

    let inDocBlock = false;

    // ==========================================
    // PASS 1: Collect Declarations & Globals
    // ==========================================
    let multiLineImport = "";
    let isReadingImport = false;
    let isReadingEnum = false;

    for (let i = 0; i < lines.length; i++) {
        const line = lines[i];
        if (line.trim() === '---') { inDocBlock = !inDocBlock; continue; }
        if (inDocBlock) continue;

        const cleanLine = maskLine(line);

        if (isReadingImport) {
            multiLineImport += " " + cleanLine;
            if (cleanLine.includes('}')) {
                const symbolsMatch = /\{([^}]*)\}/.exec(multiLineImport);
                if (symbolsMatch) {
                    symbolsMatch[1].split(',').forEach(s => {
                        const trimmed = s.trim();
                        if (trimmed) fileGlobals.add(trimmed);
                    });
                }
                isReadingImport = false;
                multiLineImport = "";
            }
            continue;
        }

        // Enum block: collect name + all variants into fileGlobals
        if (isReadingEnum) {
            if (cleanLine.includes('}')) { isReadingEnum = false; }
            else {
                const variantMatch = /^\s*([a-zA-Z_]\w*)\s*$/.exec(cleanLine);
                if (variantMatch) fileGlobals.add(variantMatch[1]);
            }
            continue;
        }

        const enumMatch = /^\s*enum\s+([a-zA-Z_]\w*)/.exec(cleanLine);
        if (enumMatch) {
            fileGlobals.add(enumMatch[1]);
            // Collect inline variants (e.g. enum Small { A B C })
            const inlineBody = /\{([^}]*)\}/.exec(cleanLine);
            if (inlineBody) {
                inlineBody[1].split(/\s+/).forEach(v => { if (v) fileGlobals.add(v); });
            } else {
                isReadingEnum = true;
            }
            continue;
        }

        if (/^\s*from\s+.*use\s+\{/.test(cleanLine) && !cleanLine.includes('}')) {
            isReadingImport = true;
            multiLineImport = cleanLine;
            continue;
        }

        let match = /^\s*use\s+([.a-zA-Z0-9_/]+)/.exec(cleanLine);
        if (match) {
            const alias = match[1].split(/[\.\/]/).pop();
            if (alias) fileGlobals.add(alias);
        }

        match = /^\s*from\s+[.a-zA-Z0-9_/]+\s+use\s+\{([^}]*)\}/.exec(cleanLine);
        if (match) {
            match[1].split(',').forEach(s => {
                const trimmed = s.trim();
                if (trimmed) fileGlobals.add(trimmed);
            });
        }

        match = /^\s*(?:extern\s+)?(?:struct|define|def|init)\s+([a-zA-Z_]\w*)/.exec(cleanLine);
        if (match) {
            const name = match[1];
            fileGlobals.add(name);
            if (name !== 'main' && !name.startsWith('__')) {
                const startIdx = cleanLine.indexOf(name, match.index);
                declarations.set(name, new vscode.Range(i, startIdx, i, startIdx + name.length));
            }
        }
    }

    // ==========================================
    // PASS 2: Detailed Scope & Usage Scan
    // ==========================================
    let locals = new Set<string>();
    inDocBlock = false;

    // Precompile regexes used inside the per-line loop
    const memberRegex = /\.\s*([a-zA-Z_]\w*)\b/g;
    const identRegex = /(?<!\.)\b([a-zA-Z_]\w*)\b/g;
    const restOfLineColonRe = /^\s*:(?!=)/;

    // Robust brace-depth tracking replaces the buggy 'inStruct' regex logic
    let braceDepth = 0;
    let currentFuncDepth = -1;
    let inMultiLineImport = false;

    for (let i = 0; i < lines.length; i++) {
        const originalLine = lines[i];
        if (originalLine.trim() === '---') { inDocBlock = !inDocBlock; continue; }
        if (inDocBlock) continue;

        let maskedLine = maskLine(originalLine);
        if (maskedLine.trim() === '') continue;

        // Skip import lines, tracking multi-line { } blocks so their braces don't skew braceDepth
        if (/^\s*(use|from)\b/.test(maskedLine)) {
            if (maskedLine.includes('{') && !maskedLine.includes('}')) inMultiLineImport = true;
            continue;
        }
        if (inMultiLineImport) {
            if (maskedLine.includes('}')) inMultiLineImport = false;
            continue;
        }

        const openBraces = (maskedLine.match(/\{/g) || []).length;
        const closeBraces = (maskedLine.match(/\}/g) || []).length;

        // Reset scope if we enter a new function
        const funcMatch = /^\s*(?:extern\s+)?(?:define|def|init)\s+[a-zA-Z_]\w*\s*\(([^)]*)\)/.exec(maskedLine);
        if (funcMatch) {
            locals = new Set<string>();
            if (!maskedLine.includes('extern')) {
                currentFuncDepth = braceDepth;
            }
            
            const isExtern = maskedLine.includes('extern');
            const paramsStr = funcMatch[1];
            const paramMatches = [...paramsStr.matchAll(/\b([a-zA-Z_]\w*)\s*(?::\s*[a-zA-Z_]\w*)?(?::|$|,)/g)];
            for (const pm of paramMatches) {
                const pName = pm[1];
                locals.add(pName);

                // extern define params are implemented in C — never "used" in Quirk source
                if (!isExtern && pName !== 'self' && !BUILTINS.has(pName)) {
                    const startIdx = originalLine.indexOf(pName, funcMatch.index);
                    declarations.set(`${i}_${pName}`, new vscode.Range(i, startIdx, i, startIdx + pName.length));
                }
            }
        }

        // Reset scope if we enter a lambda trigger
        const triggerMatch = /^\s*trigger\s+[a-zA-Z0-9_.]+(?:\s*\(([^)]*)\))?/.exec(maskedLine);
        if (triggerMatch) {
            locals = new Set<string>();
            currentFuncDepth = braceDepth;
            
            const paramsStr = triggerMatch[1];
            if (paramsStr) {
                paramsStr.split(',').forEach(p => {
                    const pName = p.trim();
                    if (pName) {
                        locals.add(pName);
                        const startIdx = originalLine.indexOf(pName, triggerMatch.index);
                        declarations.set(`${i}_${pName}`, new vscode.Range(i, startIdx, i, startIdx + pName.length));
                    }
                });
            } else {
                locals.add('it');
                locals.add('was');
            }
        }

        // Only search for local variable declarations if we are INSIDE a function block.
        // This prevents the linter from thinking struct property fields are local variables.
        const isInsideFunc = currentFuncDepth !== -1;

        if (isInsideFunc) {
            const assignMatch = /(?<!\.)\b([a-zA-Z_]\w*)\s*(?::\s*[a-zA-Z0-9_.]+)?\s*(?::=|=|\+=|-=|\*=|\/=)/.exec(maskedLine);
            if (assignMatch) {
                const vName = assignMatch[1];
                if (!locals.has(vName)) {
                    locals.add(vName);
                    const startIdx = originalLine.indexOf(vName);
                    declarations.set(`${i}_${vName}`, new vscode.Range(i, startIdx, i, startIdx + vName.length));
                }
            }

            const withMatch = /\bwith\b.*\bas\s+([a-zA-Z_]\w*)/.exec(maskedLine);
            if (withMatch) {
                const vName = withMatch[1];
                if (!locals.has(vName)) {
                    locals.add(vName);
                    const startIdx = originalLine.indexOf(vName, withMatch.index + 4);
                    declarations.set(`${i}_${vName}`, new vscode.Range(i, startIdx, i, startIdx + vName.length));
                }
            }

            const forMatch = /\bfor\s+(?:ref\s+)?([a-zA-Z_]\w*)\s+in\b/.exec(maskedLine);
            if (forMatch) {
                const vName = forMatch[1];
                if (!locals.has(vName)) {
                    locals.add(vName);
                    const startIdx = originalLine.indexOf(vName, forMatch.index);
                    declarations.set(`${i}_${vName}`, new vscode.Range(i, startIdx, i, startIdx + vName.length));
                }
            }

            const catchMatch = /\bcatch\s*\(\s*([a-zA-Z_]\w*)\s*:/.exec(maskedLine);
            if (catchMatch) {
                const vName = catchMatch[1];
                if (!locals.has(vName)) {
                    locals.add(vName);
                    const startIdx = originalLine.indexOf(vName, catchMatch.index);
                    declarations.set(`${i}_${vName}`, new vscode.Range(i, startIdx, i, startIdx + vName.length));
                }
            }

            // Lambda params: fn(x: Int, y) => ... or fn(x) { ... }
            const lambdaParamRegex = /\bfn\s*\(([^)]*)\)/g;
            let lambdaParamMatch;
            while ((lambdaParamMatch = lambdaParamRegex.exec(maskedLine)) !== null) {
                lambdaParamMatch[1].split(',').forEach(part => {
                    const pName = part.trim().split(':')[0].trim();
                    if (pName && /^[a-zA-Z_]\w*$/.test(pName)) {
                        locals.add(pName);
                    }
                });
            }
        }

        memberRegex.lastIndex = 0;
        let memMatch;
        while ((memMatch = memberRegex.exec(maskedLine)) !== null) {
            usages.add(memMatch[1]);
        }

        identRegex.lastIndex = 0;
        let match;

        while ((match = identRegex.exec(maskedLine)) !== null) {
            const ident = match[1];
            if (KEYWORDS.has(ident) || BUILTINS.has(ident)) continue;

            // Prevent struct properties like "file: String" from triggering a warning
            const restOfLine = maskedLine.substring(match.index + ident.length);
            if (restOfLineColonRe.test(restOfLine)) continue;

            const range = new vscode.Range(i, match.index, i, match.index + ident.length);

            if (declarations.get(ident)?.isEqual(range) || declarations.get(`${i}_${ident}`)?.isEqual(range)) {
                continue;
            }

            if (locals.has(ident) || fileGlobals.has(ident)) {
                usages.add(ident); 
                for (let k = i; k >= 0; k--) {
                    if (declarations.has(`${k}_${ident}`)) {
                        usages.add(`${k}_${ident}`);  
                        break;
                    }
                }
            } else {
                diagnostics.push(new vscode.Diagnostic(range, `'${ident}' is not defined.`, vscode.DiagnosticSeverity.Warning));
            }
        }

        // Adjust scope level
        braceDepth += openBraces - closeBraces;
        
        // If we drop back down to the brace level where the function started, we have exited the function body
        if (currentFuncDepth !== -1 && braceDepth <= currentFuncDepth && closeBraces > 0) {
            currentFuncDepth = -1;
        }
    }

    // ==========================================
    // PASS 3: Generate "Unused" Diagnostics 
    // ==========================================
    declarations.forEach((range, key) => {
        // Only warn on unused LOCAL variables. 
        if (/^\d+_/.test(key) && !usages.has(key)) {
            const cleanKey = key.replace(/^\d+_/, '');

            // --- NEW: Ignore variables prefixed with an underscore ---
            if (cleanKey.startsWith('_')) return;

            const diagnostic = new vscode.Diagnostic(
                range,
                `'${cleanKey}' is declared but never used.`,
                vscode.DiagnosticSeverity.Warning
            );

            diagnostic.tags = [vscode.DiagnosticTag.Unnecessary];
            diagnostic.code = "unused_symbol";

            diagnostics.push(diagnostic);
        }
    });

    quirkDiagnostics.set(doc.uri, diagnostics);
}

export function subscribeToDocumentChanges(context: vscode.ExtensionContext, quirkDiagnostics: vscode.DiagnosticCollection): void {
    if (vscode.window.activeTextEditor) {
        refreshDiagnostics(vscode.window.activeTextEditor.document, quirkDiagnostics);
    }

    context.subscriptions.push(
        vscode.window.onDidChangeActiveTextEditor(editor => {
            if (editor) refreshDiagnostics(editor.document, quirkDiagnostics);
        })
    );

    let debounceTimer: ReturnType<typeof setTimeout> | undefined;
    context.subscriptions.push(
        vscode.workspace.onDidChangeTextDocument(e => {
            if (debounceTimer) clearTimeout(debounceTimer);
            debounceTimer = setTimeout(() => refreshDiagnostics(e.document, quirkDiagnostics), 300);
        })
    );

    context.subscriptions.push(
        vscode.workspace.onDidCloseTextDocument(doc => quirkDiagnostics.delete(doc.uri))
    );
}