// QuirkInlineValuesProvider — drives VSCode's "inline values during debug"
// feature for Quirk. VSCode calls us with the visible viewport + the stopped
// location whenever the debug session pauses; we return an array of inline
// value markers and VSCode displays each variable's current value next to
// its source occurrence.
//
// We emit `InlineValueVariableLookup` items: VSCode then queries the active
// debug scope (our `variablesRequest`) to resolve each name. Names that
// don't match a live local are silently skipped — so we can be lenient
// about what we emit without producing visual junk.
//
// What we DON'T return:
//   - Keywords (`define`, `if`, `return`, ...) — never variables
//   - Method/field segments after `.` (those need expression eval)
//   - Identifiers immediately followed by `(` (call targets, not values)
//   - Identifiers inside string literals or `//`-comments

import * as vscode from 'vscode';

// Keep this list aligned with the lexer's keyword table. Out-of-date entries
// just mean a few false positives in the inline values; harmless.
const QUIRK_KEYWORDS = new Set([
    'define', 'fn', 'extern', 'return', 'if', 'elif', 'else', 'while', 'for',
    'in', 'break', 'continue', 'match', 'case', 'try', 'catch', 'throw',
    'with', 'use', 'from', 'as', 'struct', 'class', 'interface', 'enum',
    'type', 'const', 'static', 'nonlocal', 'global', 'self', 'where',
    'true', 'false', 'null', 'and', 'or', 'not', 'del',
    'Int', 'Double', 'String', 'Bool', 'List', 'Map', 'Set', 'Queue',
    'Tuple', 'Any', 'void',
]);

export class QuirkInlineValuesProvider implements vscode.InlineValuesProvider {
    provideInlineValues(
        document: vscode.TextDocument,
        viewport: vscode.Range,
        context: vscode.InlineValueContext,
    ): vscode.InlineValue[] {
        const items: vscode.InlineValue[] = [];

        // Only emit values for lines we've already executed in the current
        // frame — showing a value next to code that hasn't run yet would be
        // misleading. VSCode hands us `context.stoppedLocation` for this.
        const stopLine = context.stoppedLocation.end.line;
        const lastLine = Math.min(stopLine, viewport.end.line);
        const firstLine = Math.max(0, viewport.start.line);

        for (let lineNo = firstLine; lineNo <= lastLine; lineNo++) {
            const text = document.lineAt(lineNo).text;
            this.scanLine(text, lineNo, items);
        }
        return items;
    }

    // Tokenize one line, skipping comments and string contents, and emit
    // an InlineValueVariableLookup for each plausible variable reference.
    private scanLine(text: string, lineNo: number, out: vscode.InlineValue[]): void {
        let i = 0;
        const n = text.length;
        // Track names we've already emitted on this line so the user doesn't
        // see `x: 5  x: 5` when the same name appears twice.
        const seen = new Set<string>();

        while (i < n) {
            const c = text.charCodeAt(i);

            // Single-line comment — done with this line.
            if (c === 47 /* / */ && i + 1 < n && text.charCodeAt(i + 1) === 47) {
                return;
            }

            // String literal — skip to its closing quote, handling backslash
            // escapes. We only look at `"` and `'`; triple-quoted strings
            // would need more work but are rare in lines worth inlining.
            if (c === 34 /* " */ || c === 39 /* ' */) {
                const quote = c;
                i++;
                while (i < n) {
                    if (text.charCodeAt(i) === 92 /* \ */) { i += 2; continue; }
                    if (text.charCodeAt(i) === quote) { i++; break; }
                    i++;
                }
                continue;
            }

            // Identifier start: [A-Za-z_]. Once we have one, walk forward
            // through [A-Za-z0-9_]*.
            if (isIdentStart(c)) {
                const start = i;
                while (i < n && isIdentCont(text.charCodeAt(i))) i++;
                const word = text.slice(start, i);

                // Look back/forward to decide whether to emit.
                const prevCh = start > 0 ? text[start - 1] : '';
                const nextCh = i < n ? text[i] : '';

                if (QUIRK_KEYWORDS.has(word)) continue;
                // Property/method access — `obj.foo` — skip `foo` (needs eval).
                if (prevCh === '.') continue;
                // Looks like a call — `foo(` — skip the callee.
                if (nextCh === '(') continue;
                // Looks like a named-arg `foo=` inside a call — skip.
                if (nextCh === '=' && (i + 1 >= n || text[i + 1] !== '=')) {
                    // Could be assignment OR named arg. Be conservative and
                    // still emit on bare assignment (`x = 5` — we want x's
                    // pre-write value). Heuristic: if there's a `(` open
                    // earlier on the line without a matching `)`, treat as
                    // a named arg and skip.
                    if (insideUnclosedCall(text, start)) continue;
                }
                if (seen.has(word)) continue;
                seen.add(word);

                const range = new vscode.Range(lineNo, start, lineNo, i);
                out.push(new vscode.InlineValueVariableLookup(range, word, false));
                continue;
            }

            i++;
        }
    }
}

function isIdentStart(c: number): boolean {
    return (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || c === 95; // A-Z a-z _
}
function isIdentCont(c: number): boolean {
    return isIdentStart(c) || (c >= 48 && c <= 57); // + digits
}

// Cheap heuristic: is `pos` inside an unclosed `(` on this line? Used to
// distinguish `x = 5` (assignment) from `foo(x=5)` (named arg).
function insideUnclosedCall(text: string, pos: number): boolean {
    let depth = 0;
    for (let i = 0; i < pos; i++) {
        const c = text[i];
        if (c === '(') depth++;
        else if (c === ')') depth = Math.max(0, depth - 1);
        else if (c === '"' || c === "'") {
            const q = c;
            i++;
            while (i < text.length) {
                if (text[i] === '\\') { i++; continue; }
                if (text[i] === q) break;
                i++;
            }
        }
    }
    return depth > 0;
}
