import * as vscode from 'vscode';
import { getDeadLines } from './DeadCodeProvider';
import { maskLine, maskLineWithState, TripleState } from './utils/maskLine';
import { resolveModulePath, findProjectRootFor, resolveQuirkHome } from './ImportProvider';

const KEYWORDS = new Set([
    'define', 'struct', 'if', 'else', 'elif', 'while', 'for', 'in',
    'return', 'break', 'continue', 'use', 'from', 'with', 'as',
    'extern', 'true', 'false', 'null', 'del', 'init', 'def',
    'const', 'ref', 'try', 'catch', 'throw', 'finally', 'and', 'or', 'not', 'super', 'enum',
    'match', 'case', '_', 'where', 'is',
    'fn', 'nonlocal', 'global', 'interface'
]);

const BUILTINS = new Set([
    'print', 'printf', 'type', 'exit', 'String', 'List', 'Map',
    'File', 'Int', 'Double', 'Bool', 'Any', 'void', 'Callable', 'Tuple',
    'Self',  // type placeholder in interface methods
    'Printable', 'Equatable', 'Comparable', 'Hashable',
    'Parseable', 'Sizeable', 'Iterable', 'Iterator', 'Representable', 'Primitive', // built-in typing interfaces
    'ListIterator', 'MapIterator', 'MapPairIterator', 'TupleIterator', 'SetIterator', 'QueueIterator', 'StringIterator',
    'Set', 'Queue',
    'true', 'false', 'null',
    'Exception', 'TypeError', 'ValueError', 'IndexError', 'KeyError',
    'IOError', 'FileNotFoundError', 'RuntimeError', 'NotImplementedError',
    'SocketError', 'ZeroDivisionError', 'AssertionError', 'NullError', 'WhereConditionError'
]);

export function refreshDiagnostics(doc: vscode.TextDocument, quirkDiagnostics: vscode.DiagnosticCollection): void {
    if (doc.languageId !== 'quirk') return;

    const deadLines = getDeadLines(doc);
    const diagnostics: vscode.Diagnostic[] = [];
    const text = doc.getText();
    const lines = text.split(/\r?\n/);

    // Import resolution context — cached for the whole pass over the file.
    const docPath = doc.uri.fsPath;
    const projectRootForImports = findProjectRootFor(docPath);
    const homeForImports = resolveQuirkHome(projectRootForImports);
    const reportUnresolvedImport = (lineIdx: number, line: string, modulePath: string) => {
        if (resolveModulePath(projectRootForImports, docPath, modulePath)) return;
        const pos = line.indexOf(modulePath);
        if (pos < 0) return;
        const range = new vscode.Range(lineIdx, pos, lineIdx, pos + modulePath.length);
        const hint = homeForImports
            ? ` (checked QUIRK_HOME=${homeForImports})`
            : ` (no venv detected — activate one or run \`quirk install\`)`;
        diagnostics.push(new vscode.Diagnostic(
            range,
            `Cannot resolve module '${modulePath}'${hint}`,
            vscode.DiagnosticSeverity.Warning,
        ));
    };

    const declarations = new Map<string, vscode.Range>();
    // Parallel index: for each declared identifier, a sorted-ascending list
    // of line numbers where it's declared. Lets the usage-tracking pass find
    // the nearest predecessor declaration via binary search in O(log K)
    // instead of an O(N) backward line scan per identifier reference.
    const declarationLinesByName = new Map<string, number[]>();
    const recordDeclaration = (name: string, line: number, range: vscode.Range) => {
        declarations.set(`${line}_${name}`, range);
        let arr = declarationLinesByName.get(name);
        if (!arr) { arr = []; declarationLinesByName.set(name, arr); }
        // Pass-1 walks lines in ascending order so the array stays sorted
        // without an explicit insertion sort.
        if (arr.length === 0 || arr[arr.length - 1] < line) arr.push(line);
    };
    // Find the largest declared line `<= usageLine` for `name`, or -1.
    const nearestDeclLine = (name: string, usageLine: number): number => {
        const arr = declarationLinesByName.get(name);
        if (!arr || arr.length === 0) return -1;
        // Binary search for rightmost entry <= usageLine
        let lo = 0, hi = arr.length - 1, best = -1;
        while (lo <= hi) {
            const mid = (lo + hi) >> 1;
            if (arr[mid] <= usageLine) { best = arr[mid]; lo = mid + 1; }
            else hi = mid - 1;
        }
        return best;
    };
    const usages = new Set<string>();
    const fileGlobals = new Set<string>();
    // Module aliases brought into scope by `use X` (allow `X.foo(...)` only).
    const useAliases = new Set<string>();
    // Symbols brought in by `from X use { a, b }` (allow bare `a(...)`).
    const explicitImports = new Set<string>();
    const interfaceNames = new Set<string>([
        'Any', 'Printable', 'Equatable', 'Comparable', 'Hashable',
        'Parseable', 'Sizeable', 'Iterable', 'Iterator', 'Representable', 'Primitive',
    ]); // built-ins + names declared with `interface`
    const structNames = new Set<string>();    // names declared with `struct`

    let inDocBlock = false;

    // ==========================================
    // PASS 1: Collect Declarations & Globals
    // ==========================================
    let multiLineImport = "";
    let isReadingImport = false;
    let isReadingEnum = false;
    // Track brace depth so the top-level VarDecl detector below only
    // promotes names declared at module scope (depth 0) into fileGlobals.
    let pass1Depth = 0;
    // Track whether we're currently inside a `"""..."""` block. State
    // threads across lines so identifiers buried in multi-line string
    // content don't leak out as undefined-variable warnings.
    let pass1Triple: TripleState = { active: false, quote: '' };

    for (let i = 0; i < lines.length; i++) {
        const line = lines[i];
        const trimmed = line.trim();
        if (trimmed === '---') { inDocBlock = !inDocBlock; continue; }
        if (trimmed.startsWith('---') && trimmed.endsWith('---') && trimmed !== '---') continue;
        if (inDocBlock) continue;

        const ms = maskLineWithState(line, pass1Triple);
        pass1Triple = ms.state;
        const cleanLine = ms.masked;
        const lineStartDepth = pass1Depth;
        for (const c of cleanLine) {
            if (c === '{') pass1Depth++;
            else if (c === '}') pass1Depth--;
        }

        if (isReadingImport) {
            multiLineImport += " " + cleanLine;
            if (cleanLine.includes('}')) {
                const symbolsMatch = /\{([^}]*)\}/.exec(multiLineImport);
                if (symbolsMatch) {
                    symbolsMatch[1].split(',').forEach(s => {
                        const trimmed = s.trim();
                        if (trimmed) { fileGlobals.add(trimmed); explicitImports.add(trimmed); }
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
            const fromMatch = /^\s*from\s+([.a-zA-Z0-9_/]+)/.exec(cleanLine);
            if (fromMatch) reportUnresolvedImport(i, line, fromMatch[1]);
            continue;
        }

        // Single-line `from X use { ... }` — validate the module path
        const fromInlineMatch = /^\s*from\s+([.a-zA-Z0-9_/]+)\s+(?:use|as)\b/.exec(cleanLine);
        if (fromInlineMatch) reportUnresolvedImport(i, line, fromInlineMatch[1]);

        let match = /^\s*use\s+([.a-zA-Z0-9_/]+)/.exec(cleanLine);
        if (match) {
            const alias = match[1].split(/[\.\/]/).pop();
            if (alias) { fileGlobals.add(alias); useAliases.add(alias); }
            reportUnresolvedImport(i, line, match[1]);
        }

        // from .path as alias — register the alias as a known global
        const fromAsMatch = /^\s*from\s+[.a-zA-Z0-9_/]+\s+as\s+([a-zA-Z_]\w*)/.exec(cleanLine);
        if (fromAsMatch) { fileGlobals.add(fromAsMatch[1]); useAliases.add(fromAsMatch[1]); }

        match = /^\s*from\s+[.a-zA-Z0-9_/]+\s+use\s+\{([^}]*)\}/.exec(cleanLine);
        if (match) {
            match[1].split(',').forEach(s => {
                const trimmed = s.trim();
                if (trimmed) { fileGlobals.add(trimmed); explicitImports.add(trimmed); }
            });
        }

        // Type alias: type Name = T — register as global so usage doesn't warn "not defined"
        const typeAliasMatch1 = /^\s*type\s+([a-zA-Z_]\w*)\s*=/.exec(cleanLine);
        if (typeAliasMatch1) { fileGlobals.add(typeAliasMatch1[1]); continue; }

        // Interface declaration: register the name + collect type params from extends clause
        const ifaceMatch = /^\s*interface\s+([a-zA-Z_]\w*)/.exec(cleanLine);
        if (ifaceMatch) { fileGlobals.add(ifaceMatch[1]); interfaceNames.add(ifaceMatch[1]); continue; }

        // Top-level mutable state: `counter := 0`, `name: Type = value`
        // at brace depth 0 becomes a module-level GlobalVariable and is
        // visible from every function. Only promote when the line starts
        // at module scope — inside a function body these are locals.
        if (lineStartDepth === 0) {
            const topLevelDeclMatch = /^\s*([a-zA-Z_]\w*)\s*(?::\s*[a-zA-Z_][\w\[\], ]*)?\s*:?=/.exec(cleanLine);
            if (topLevelDeclMatch) {
                const name = topLevelDeclMatch[1];
                if (!KEYWORDS.has(name) && !cleanLine.trimStart().startsWith('return')) {
                    fileGlobals.add(name);
                }
            }
        }

        match = /^\s*(?:extern\s+)?(?:struct|define|def|init)\s+([a-zA-Z_]\w*)/.exec(cleanLine);
        if (match) {
            const name = match[1];
            fileGlobals.add(name);
            if (/^\s*(?:extern\s+)?struct\b/.test(cleanLine)) structNames.add(name);
            // Collect generic type params [T, U] so they're not flagged as "not defined"
            const typeParamMatch = /^\s*(?:extern\s+)?(?:struct|define|def|init)\s+[a-zA-Z_]\w*\s*\[([^\]]+)\]/.exec(cleanLine);
            if (typeParamMatch) {
                typeParamMatch[1].split(',').forEach(tp => {
                    const t = tp.trim();
                    if (t) fileGlobals.add(t);
                });
            }
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
    const localTypes = new Map<string, string>(); // varName → inferred Quirk type
    inDocBlock = false;

    // Precompile regexes used inside the per-line loop
    const memberRegex = /\.\s*([a-zA-Z_]\w*)\b/g;
    const identRegex = /(?<!\.)\b([a-zA-Z_]\w*)\b/g;
    const restOfLineColonRe = /^:(?!=)/;

    // Robust brace-depth tracking replaces the buggy 'inStruct' regex logic
    let braceDepth = 0;
    let currentFuncDepth = -1;
    // Stack of outer (locals, funcDepth) pairs — pushed when we enter a
    // nested `define` inside another function body so the inner define's
    // params don't clobber the outer scope's locals. Popped when the inner
    // function's braces close. Without this, code like
    //     define main() {
    //         t1 := ...
    //         define helper() { ... }
    //         t2 := ...        // would falsely warn "t1 not defined"
    //     }
    // breaks scope tracking because we reset `locals` on every funcMatch.
    const scopeStack: { locals: Set<string>; depth: number }[] = [];
    let inMultiLineImport = false;
    // Same TripleState pattern as Pass 1, run on each line of Pass 2 to
    // hide triple-quoted content from the identifier scanner.
    let pass2Triple: TripleState = { active: false, quote: '' };

    for (let i = 0; i < lines.length; i++) {
        const originalLine = lines[i];
        const trimmedOrig = originalLine.trim();
        if (trimmedOrig === '---') { inDocBlock = !inDocBlock; continue; }
        if (trimmedOrig.startsWith('---') && trimmedOrig.endsWith('---') && trimmedOrig !== '---') continue;
        if (inDocBlock) continue;

        const ms2 = maskLineWithState(originalLine, pass2Triple);
        pass2Triple = ms2.state;
        let maskedLine = ms2.masked;
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

        // Reset scope if we enter a new function (optional [T, U] generic params before `(`)
        const funcMatch = /^\s*(?:extern\s+)?(?:define|def|init)\s+([a-zA-Z_]\w*)\s*(?:\[[^\]]*\]\s*)?\(([^)]*)\)/.exec(maskedLine);
        if (funcMatch) {
            // Nested define inside another function body: the inner define
            // desugars to a local Callable binding (`name := fn(...) {...}`),
            // so its NAME is visible to the outer scope. Stash the outer
            // (locals, funcDepth) so we can restore them when the inner
            // body closes; meanwhile expose the inner function's name to
            // the outer scope's locals.
            if (currentFuncDepth !== -1 && !maskedLine.includes('extern')) {
                const innerName = funcMatch[1];
                locals.add(innerName);
                scopeStack.push({ locals, depth: currentFuncDepth });
            }
            locals = new Set<string>();
            if (!maskedLine.includes('extern')) {
                currentFuncDepth = braceDepth;
            }

            // Seed locals with generic type params [T, U] so they're valid inside body
            const typeParamBlock = /^\s*(?:extern\s+)?(?:define|def|init)\s+[a-zA-Z_]\w*\s*\[([^\]]*)\]/.exec(maskedLine);
            if (typeParamBlock) {
                typeParamBlock[1].split(',').forEach(tp => {
                    const t = tp.trim();
                    if (t) locals.add(t);
                });
            }

            const isExtern = maskedLine.includes('extern');
            // Bodyless signatures (interface methods, forward decls) have no { on the line.
            // Params of bodyless functions are never "used" in Quirk source — don't track them.
            const isBodyless = !maskedLine.includes('{');
            const paramsStr = funcMatch[2];
            // Type annotation allows generic params: List[T], Map[K, V], etc.
            // Optional `= <default>` between the type annotation and the
            // comma — without consuming it, only the first parameter of a
            // signature like `(a, b: Int = 0, c: Int = 1)` matched, and
            // every subsequent name was flagged "not defined".
            const paramMatches = [...paramsStr.matchAll(/(?:\.\.\.)?([a-zA-Z_]\w*)\s*(?::\s*[a-zA-Z_][\w.?]*(?:\[[^\]]*\])?)?(?:\s*=\s*(?:"[^"]*"|'[^']*'|\[[^\]]*\]|\([^)]*\)|[^,)]+))?(?:\s*,|\s*$)/g)];
            for (const pm of paramMatches) {
                const pName = pm[1];
                locals.add(pName);

                // extern define and bodyless signatures are implemented outside Quirk source
                if (!isExtern && !isBodyless && pName !== 'self' && !BUILTINS.has(pName)) {
                    const startIdx = originalLine.indexOf(pName, funcMatch.index);
                    recordDeclaration(pName, i, new vscode.Range(i, startIdx, i, startIdx + pName.length));
                }
            }
        }

        // Allow declaration tracking inside a function body OR at the top level (braceDepth === 0).
        // braceDepth > 0 outside a function means we're inside a struct body — skip to avoid
        // treating type-annotated fields like `data: Any` as variable declarations.
        const isInsideFunc = currentFuncDepth !== -1;
        const isTopLevel = braceDepth === 0;

        // Collect named argument names: `ident =` patterns at paren-depth > 0.
        // These are keyword arguments to function calls, not variable declarations.
        const namedArgNames = new Set<string>();
        {
            let pd = 0;
            for (let ci = 0; ci < maskedLine.length; ci++) {
                const ch = maskedLine[ci];
                if (ch === '(') { pd++; }
                else if (ch === ')') { pd--; }
                else if (pd > 0) {
                    const m = /^([a-zA-Z_]\w*)\s*=(?![=>])/.exec(maskedLine.slice(ci));
                    if (m) { namedArgNames.add(m[1]); }
                }
            }
        }

        // Collect comprehension variable names from ALL `for var1 (,var2) in` patterns on the line.
        // These are defined by the for-clause and are valid identifiers throughout the expression.
        const compVarNames = new Set<string>();
        {
            const cvRe = /\bfor\s+(?:ref\s+)?([a-zA-Z_]\w*)(?:\s*,\s*([a-zA-Z_]\w*))?\s+in\b/g;
            let cvm;
            while ((cvm = cvRe.exec(maskedLine)) !== null) {
                compVarNames.add(cvm[1]);
                if (cvm[2]) compVarNames.add(cvm[2]);
            }
            // Also capture parenthesized for-destructuring: for (n, s) in
            const cvParenRe = /\bfor\s+\(([^)]+)\)\s+in\b/g;
            let cvpm;
            while ((cvpm = cvParenRe.exec(maskedLine)) !== null) {
                cvpm[1].split(',').forEach(part => {
                    const vn = part.trim();
                    if (vn && /^[a-zA-Z_]\w*$/.test(vn)) compVarNames.add(vn);
                });
            }
        }

        // Type alias lines don't declare runtime variables — skip assignment matching
        const isTypeAliasLine = /^\s*type\s+[a-zA-Z_]\w*\s*=/.test(maskedLine);

        if (isInsideFunc || isTopLevel) {
            // Parenthesized for-destructuring: for (n, s) in pairs
            const forParenDestructMatch = /\bfor\s+\(([^)]+)\)\s+in\b/.exec(maskedLine);
            if (forParenDestructMatch) {
                forParenDestructMatch[1].split(',').forEach(part => {
                    const vName = part.trim();
                    if (vName && /^[a-zA-Z_]\w*$/.test(vName) && !locals.has(vName) && !KEYWORDS.has(vName) && !BUILTINS.has(vName)) {
                        locals.add(vName);
                        const startIdx = originalLine.indexOf(vName, forParenDestructMatch.index);
                        recordDeclaration(vName, i, new vscode.Range(i, startIdx, i, startIdx + vName.length));
                    }
                });
            }

            // Parenthesized tuple destructuring: (x, y) := expr
            const parenDestructMatch = /^\s*\(\s*([a-zA-Z_]\w*(?:\s*,\s*[a-zA-Z_]\w*)*)\s*\)\s*:=/.exec(maskedLine);
            if (parenDestructMatch) {
                parenDestructMatch[1].split(',').forEach(part => {
                    const vName = part.trim();
                    if (vName && !locals.has(vName) && !KEYWORDS.has(vName) && !BUILTINS.has(vName)) {
                        locals.add(vName);
                        const startIdx = originalLine.indexOf(vName);
                        recordDeclaration(vName, i, new vscode.Range(i, startIdx, i, startIdx + vName.length));
                    }
                });
            }

            // Bare tuple destructuring: x, y := expr  (two or more names before :=)
            const bareDestructMatch = /^\s*([a-zA-Z_]\w*(?:\s*,\s*[a-zA-Z_]\w*)+)\s*:=/.exec(maskedLine);
            if (bareDestructMatch && !parenDestructMatch) {
                bareDestructMatch[1].split(',').forEach(part => {
                    const vName = part.trim();
                    if (vName && !locals.has(vName) && !KEYWORDS.has(vName) && !BUILTINS.has(vName)) {
                        locals.add(vName);
                        const startIdx = originalLine.indexOf(vName);
                        recordDeclaration(vName, i, new vscode.Range(i, startIdx, i, startIdx + vName.length));
                    }
                });
            }

            const assignMatch = !isTypeAliasLine ? /(?<!\.)\b([a-zA-Z_]\w*)\s*(?::\s*([a-zA-Z0-9_.?]+))?\s*(?::=|=(?!>)|\+=|-=|\*=|\/=)/.exec(maskedLine) : null;
            if (assignMatch && !parenDestructMatch && !bareDestructMatch) {
                const vName = assignMatch[1];
                // If `name` is already a module-level global, this is a
                // reassignment (`counter = counter + 1`), not a fresh
                // declaration. Without this guard the global's original
                // declaration is flagged as "unused" because every read
                // gets attributed to the newly-recorded local at this line.
                const assignOp = assignMatch[0].includes(':=') ? ':=' : '=';
                const isReassignmentOfGlobal = assignOp === '=' && fileGlobals.has(vName);
                if (!isReassignmentOfGlobal &&
                    !namedArgNames.has(vName) && !locals.has(vName) &&
                    !KEYWORDS.has(vName) && !BUILTINS.has(vName)) {
                    locals.add(vName);
                    const startIdx = originalLine.indexOf(vName);
                    recordDeclaration(vName, i, new vscode.Range(i, startIdx, i, startIdx + vName.length));
                }
                // Infer type from explicit annotation or RHS
                if (!localTypes.has(vName)) {
                    const rhs = maskedLine.slice((assignMatch.index ?? 0) + assignMatch[0].length).trim();
                    const explicitType = assignMatch[2];
                    if (explicitType && explicitType !== 'void') {
                        localTypes.set(vName, explicitType);
                    } else if (rhs.startsWith('"'))                  { localTypes.set(vName, 'String'); }
                    else if (/^\d+\.\d/.test(rhs))                   { localTypes.set(vName, 'Double'); }
                    else if (/^\d/.test(rhs))                        { localTypes.set(vName, 'Int'); }
                    else if (/^(?:true|false)\b/.test(rhs))          { localTypes.set(vName, 'Bool'); }
                    else if (rhs.startsWith("'"))                    { localTypes.set(vName, 'String'); }
                    else if (rhs.startsWith('['))                     { localTypes.set(vName, 'List'); }
                    else if (rhs.startsWith('{'))                    { localTypes.set(vName, 'Map'); }
                    else if (rhs.startsWith('('))                    { localTypes.set(vName, 'Tuple'); }
                    else {
                        // x := receiver.method(...)
                        const mCall = /^([a-zA-Z0-9_]+)\.([a-zA-Z0-9_]+)\s*\(/.exec(rhs);
                        if (mCall) {
                            const recvType = localTypes.get(mCall[1]);
                            if (recvType) {
                                const methodReturnTypes: Record<string, Record<string, string>> = {
                                    String: { upper:'String', lower:'String', trim:'String', replace:'String',
                                              split:'List', lines:'List', to_int:'Int', to_float:'Double',
                                              to_bool:'Bool', find:'Int', count:'Int',
                                              contains:'Bool', startswith:'Bool', endswith:'Bool', distance:'Int',
                                              substring:'String', join:'String', reverse:'String', encode:'String' },
                                    List:   { join:'String', find:'Any', any:'Bool', all:'Bool', get:'Any' },
                                    Map:    { get:'Any', keys:'List', values:'List', has:'Bool' },
                                    Tuple:  { length:'Int', get:'Any' },
                                };
                                const ret = methodReturnTypes[recvType]?.[mCall[2]];
                                if (ret) localTypes.set(vName, ret);
                            }
                        }
                    }
                }
            }

            const withMatch = /\bwith\b.*\bas\s+([a-zA-Z_]\w*)/.exec(maskedLine);
            if (withMatch) {
                const vName = withMatch[1];
                if (!locals.has(vName)) {
                    locals.add(vName);
                    const startIdx = originalLine.indexOf(vName, withMatch.index + 4);
                    recordDeclaration(vName, i, new vscode.Range(i, startIdx, i, startIdx + vName.length));
                }
            }

            const forMatch = /\bfor\s+(?:ref\s+)?([a-zA-Z_]\w*)(?:\s*,\s*([a-zA-Z_]\w*))?\s+in\s+([a-zA-Z0-9_"'\[]+)/.exec(maskedLine);
            if (forMatch) {
                const vName = forMatch[1];
                if (!locals.has(vName)) {
                    locals.add(vName);
                    const startIdx = originalLine.indexOf(vName, forMatch.index);
                    recordDeclaration(vName, i, new vscode.Range(i, startIdx, i, startIdx + vName.length));
                }
                if (forMatch[2]) {
                    const vName2 = forMatch[2];
                    if (!locals.has(vName2)) {
                        locals.add(vName2);
                        const startIdx2 = originalLine.indexOf(vName2, forMatch.index + forMatch[1].length);
                        recordDeclaration(vName2, i, new vscode.Range(i, startIdx2, i, startIdx2 + vName2.length));
                    }
                }
                // Infer element type from the iterable
                const iterable = forMatch[3];
                if (iterable.startsWith('"') || iterable.startsWith("'")) {
                    localTypes.set(vName, 'String');
                } else {
                    const iterType = localTypes.get(iterable);
                    if (iterType === 'String') localTypes.set(vName, 'String');
                    else if (iterType === 'List') localTypes.set(vName, 'Any');
                }
            }

            const catchMatch = /\bcatch\s*\(\s*([a-zA-Z_]\w*)\s*:\s*([a-zA-Z_]\w*)/.exec(maskedLine);
            if (catchMatch) {
                const vName = catchMatch[1];
                if (!locals.has(vName)) {
                    locals.add(vName);
                    const startIdx = originalLine.indexOf(vName, catchMatch.index);
                    recordDeclaration(vName, i, new vscode.Range(i, startIdx, i, startIdx + vName.length));
                }
                localTypes.set(vName, catchMatch[2]); // e: WhereConditionError → WhereConditionError
            }

            // Lambda params: fn(x: Int, y) => ... or fn(x) { ... }
            // The `...args` variadic prefix needs to be stripped before the
            // identifier regex sees it — otherwise the param name leaks out
            // as "not defined" everywhere it's referenced inside the body.
            const lambdaParamRegex = /\bfn\s*\(([^)]*)\)/g;
            let lambdaParamMatch;
            while ((lambdaParamMatch = lambdaParamRegex.exec(maskedLine)) !== null) {
                lambdaParamMatch[1].split(',').forEach(part => {
                    let pName = part.trim().split(':')[0].trim();
                    if (pName.startsWith('...')) pName = pName.slice(3).trim();
                    if (pName && /^[a-zA-Z_]\w*$/.test(pName)) {
                        locals.add(pName);
                    }
                });
            }

            // Match-arm bindings introduced by `case x =>`, `case x if …`,
            // or `case (a, b) =>` (tuple destructure). These names live for
            // the rest of the arm — registering them as locals keeps the
            // guard expression and body free of phantom "not defined" warnings.
            // Single lowercase identifier:
            const caseBindRe = /\bcase\s+([a-z_]\w*)\s*(?:if\b|=>|\{)/g;
            let cbm;
            while ((cbm = caseBindRe.exec(maskedLine)) !== null) {
                const n = cbm[1];
                if (KEYWORDS.has(n) || BUILTINS.has(n)) continue;
                if (n === '_' || n === 'true' || n === 'false' || n === 'null') continue;
                locals.add(n);
            }
            // Tuple destructure:
            const caseTupleRe = /\bcase\s*\(([^)]+)\)\s*(?:if\b|=>|\{)/g;
            let ctm;
            while ((ctm = caseTupleRe.exec(maskedLine)) !== null) {
                ctm[1].split(',').forEach(part => {
                    const n = part.trim();
                    if (n && /^[a-z_]\w*$/.test(n) && !KEYWORDS.has(n)) locals.add(n);
                });
            }
        }

        // where-clause generic constraint check: warn if bound is a concrete struct, not an interface
        if (/^\s*(?:extern\s+)?(?:define|def|struct)\b/.test(maskedLine)) {
            const whereIdx = maskedLine.indexOf(' where ');
            if (whereIdx !== -1) {
                const whereClause = maskedLine.slice(whereIdx + 7);
                const constraintRe = /[a-zA-Z_]\w*\s*:\s*([a-zA-Z_]\w*(?:\s*&\s*[a-zA-Z_]\w*)*)/g;
                let cm;
                while ((cm = constraintRe.exec(whereClause)) !== null) {
                    const bounds = cm[1].split(/\s*&\s*/);
                    for (const b of bounds) {
                        const bound = b.trim();
                        if (structNames.has(bound) && !interfaceNames.has(bound)) {
                            const boundIdx = originalLine.indexOf(bound, whereIdx);
                            if (boundIdx !== -1) {
                                diagnostics.push(new vscode.Diagnostic(
                                    new vscode.Range(i, boundIdx, i, boundIdx + bound.length),
                                    `'${bound}' is a concrete type, not an interface. Generic constraints should be interfaces.`,
                                    vscode.DiagnosticSeverity.Warning
                                ));
                            }
                        }
                    }
                }
            }
        }

        // Spread usage: ...varName counts as a reference (identRegex misses it due to dot lookbehind)
        {
            const spreadRe = /\.\.\.([a-zA-Z_]\w*)/g;
            let sm;
            while ((sm = spreadRe.exec(maskedLine)) !== null) {
                const ident = sm[1];
                if (!KEYWORDS.has(ident) && !BUILTINS.has(ident)) {
                    usages.add(ident);
                    const k = nearestDeclLine(ident, i);
                    if (k !== -1) usages.add(`${k}_${ident}`);
                }
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
            if (namedArgNames.has(ident)) continue; // named argument — not a variable reference
            if (compVarNames.has(ident)) {
                if (!locals.has(ident)) locals.add(ident);
                usages.add(ident);
                const k = nearestDeclLine(ident, i);
                if (k !== -1) usages.add(`${k}_${ident}`);
                continue;
            }

            // Prevent struct properties like "file: String" from triggering a warning
            const restOfLine = maskedLine.substring(match.index + ident.length);
            if (restOfLineColonRe.test(restOfLine)) continue;

            // Strict-import warning: calling `name(...)` where `name` is only
            // imported as a module via `use X` and not explicitly via
            // `from X use { name }`. Mirrors the compiler's hard error.
            if (useAliases.has(ident) && !explicitImports.has(ident) && /^\s*\(/.test(restOfLine)) {
                const range = new vscode.Range(i, match.index, i, match.index + ident.length);
                diagnostics.push(new vscode.Diagnostic(
                    range,
                    `Cannot call module '${ident}' directly. Use '${ident}.foo(...)' or import explicitly with 'from ${ident} use { ${ident} }'.`,
                    vscode.DiagnosticSeverity.Warning,
                ));
            }

            const range = new vscode.Range(i, match.index, i, match.index + ident.length);

            if (declarations.get(ident)?.isEqual(range) || declarations.get(`${i}_${ident}`)?.isEqual(range)) {
                continue;
            }

            if (locals.has(ident) || fileGlobals.has(ident)) {
                usages.add(ident);
                const k = nearestDeclLine(ident, i);
                if (k !== -1) usages.add(`${k}_${ident}`);
            } else if (!deadLines.has(i)) {
                diagnostics.push(new vscode.Diagnostic(range, `'${ident}' is not defined.`, vscode.DiagnosticSeverity.Warning));
            }
        }

        // Adjust scope level
        braceDepth += openBraces - closeBraces;
        
        // If we drop back down to the brace level where the function started,
        // we have exited the function body. Pop the outer scope if one was
        // stashed (i.e. this was a nested define inside another function);
        // otherwise return to "no function" state.
        if (currentFuncDepth !== -1 && braceDepth <= currentFuncDepth && closeBraces > 0) {
            const outer = scopeStack.pop();
            if (outer) {
                locals             = outer.locals;
                currentFuncDepth   = outer.depth;
            } else {
                currentFuncDepth   = -1;
            }
        }
    }

    // ==========================================
    // PASS 3: Generate "Unused" Diagnostics 
    // ==========================================
    declarations.forEach((range, key) => {
        // Only warn on unused LOCAL variables.
        if (/^\d+_/.test(key) && !usages.has(key)) {
            const cleanKey = key.replace(/^\d+_/, '');

            if (cleanKey.startsWith('_')) return;

            // Suppress warnings for declarations on dead code lines
            if (deadLines.has(range.start.line)) return;

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