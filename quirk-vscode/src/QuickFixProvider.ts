import * as vscode from 'vscode';
import { compilerSuggestions } from './CompilerService';

// Quick fixes (Ctrl+.) for diagnostics:
//   • "Change to `name`" — the compiler's "did you mean" suggestions for an
//     undefined name, unknown member or unknown method.
//   • Q0209 / Q0210 type mismatches — convert the value (`.to_int()`,
//     `.to_float()`, `.str()`, …) or, for an annotated variable, change its
//     declared type.
//   • "Remove unused declaration" for a one-line declaration.
export class QuirkQuickFixProvider implements vscode.CodeActionProvider {
    public static readonly providedCodeActionKinds = [
        vscode.CodeActionKind.QuickFix
    ];

    public provideCodeActions(
        document: vscode.TextDocument,
        _range: vscode.Range | vscode.Selection,
        context: vscode.CodeActionContext,
        _token: vscode.CancellationToken
    ): vscode.CodeAction[] {
        const actions: vscode.CodeAction[] = [];
        for (const diag of context.diagnostics) {
            if (diag.source === 'quirk') {
                actions.push(...this.suggestionFixes(document, diag));
                actions.push(...this.typeMismatchFixes(document, diag));
            }
            if (diag.message.includes('never used')) {
                const fix = this.removeUnusedFix(document, diag);
                if (fix) actions.push(fix);
            }
        }
        return actions;
    }

    // ---- "did you mean" ------------------------------------------------

    private suggestionFixes(document: vscode.TextDocument, diag: vscode.Diagnostic): vscode.CodeAction[] {
        const names = compilerSuggestions(document.uri, diag);
        // Only when the squiggle is exactly one identifier — that's what
        // gets replaced.
        if (names.length === 0 || !diag.range.isSingleLine) return [];
        if (!/^[A-Za-z_]\w*$/.test(document.getText(diag.range))) return [];
        return names.map((name, i) => {
            const fix = new vscode.CodeAction(`Change to '${name}'`, vscode.CodeActionKind.QuickFix);
            fix.edit = new vscode.WorkspaceEdit();
            fix.edit.replace(document.uri, diag.range, name);
            fix.diagnostics = [diag];
            fix.isPreferred = i === 0;
            return fix;
        });
    }

    // ---- Q0209 / Q0210: wrong type -------------------------------------

    private typeMismatchFixes(document: vscode.TextDocument, diag: vscode.Diagnostic): vscode.CodeAction[] {
        const msg = diag.message;
        let actual: string, target: string, varName: string | undefined;
        let m = /^cannot assign '([^']+)' to (field )?'([^']+)' of type '([^']+)'/.exec(msg);
        const isReturn = !m;
        if (m) {
            actual = m[1]; target = m[4];
            if (!m[2]) varName = m[3];
        } else {
            m = /^cannot return '([^']+)' from '[^']+', which returns '([^']+)'/.exec(msg);
            if (!m) return [];
            actual = m[1]; target = m[2];
        }

        const line = document.lineAt(diag.range.start.line);
        const value = isReturn ? this.returnValueRange(line) : this.assignedValueRange(line, diag.range.start.character);
        const out: vscode.CodeAction[] = [];

        const conv = conversion(actual, target);
        if (conv && value) {
            const text = document.getText(value);
            const replaced = isSimpleExpression(text) ? `${text}${conv}` : `(${text})${conv}`;
            const fix = new vscode.CodeAction(`Convert with ${conv}`, vscode.CodeActionKind.QuickFix);
            fix.edit = new vscode.WorkspaceEdit();
            fix.edit.replace(document.uri, value, replaced);
            fix.diagnostics = [diag];
            fix.isPreferred = true;
            out.push(fix);
        }

        // `n: Int := "5"` → `n: String := "5"`.
        if (varName && actual !== 'Null' && actual !== 'void') {
            const ann = new RegExp(`\\b${escapeRe(varName)}\\s*:\\s*(${escapeRe(target)})\\b`).exec(line.text);
            if (ann) {
                const start = ann.index + ann[0].length - ann[1].length;
                const fix = new vscode.CodeAction(`Change the type of '${varName}' to ${actual}`, vscode.CodeActionKind.QuickFix);
                fix.edit = new vscode.WorkspaceEdit();
                fix.edit.replace(document.uri,
                    new vscode.Range(line.lineNumber, start, line.lineNumber, start + ann[1].length), actual);
                fix.diagnostics = [diag];
                out.push(fix);
            }
        }
        return out;
    }

    // The value after `:=` / `=` on an assignment line (from the target at
    // `fromCol`), without a trailing `// comment`.
    private assignedValueRange(line: vscode.TextLine, fromCol: number): vscode.Range | undefined {
        const text = line.text;
        const opRe = /:=|(?<![=!<>:])=(?!=)/g;
        opRe.lastIndex = fromCol;
        const op = opRe.exec(text);
        if (!op) return undefined;
        return trimmedRange(line, op.index + op[0].length, endOfCode(text));
    }

    private returnValueRange(line: vscode.TextLine): vscode.Range | undefined {
        const m = /\breturn\s+/.exec(line.text);
        if (!m) return undefined;
        let end = endOfCode(line.text);
        // `if x { return y }` on one line: stop before the closing brace.
        const close = line.text.lastIndexOf('}', end - 1);
        if (close > m.index) end = close;
        return trimmedRange(line, m.index + m[0].length, end);
    }

    // ---- unused declarations -------------------------------------------

    private removeUnusedFix(document: vscode.TextDocument, diagnostic: vscode.Diagnostic): vscode.CodeAction | undefined {
        const line = document.lineAt(diagnostic.range.start.line);
        // Deleting the first line of a multi-line `define` / `struct` / block
        // would leave its body behind — only offer it for one-line
        // declarations.
        if (/\{\s*(\/\/.*)?$/.test(line.text) || /^\s*(define|struct|enum|interface)\b/.test(line.text)) return undefined;
        const fix = new vscode.CodeAction('Remove unused declaration', vscode.CodeActionKind.QuickFix);
        fix.edit = new vscode.WorkspaceEdit();
        fix.edit.delete(document.uri, line.rangeIncludingLineBreak);
        fix.diagnostics = [diagnostic];
        fix.isPreferred = true;
        return fix;
    }
}

// The method that turns an `actual` value into a `target` one, if any.
export function conversion(actual: string, target: string): string | undefined {
    if (target === 'String' && actual !== 'String' && actual !== 'Null' && actual !== 'void') return '.str()';
    if (actual === 'String' && target === 'Int') return '.to_int()';
    if (actual === 'String' && target === 'Double') return '.to_float()';
    if (actual === 'String' && target === 'Bool') return '.to_bool()';
    if (actual === 'Double' && target === 'Int') return '.to_int()';
    if (actual === 'Int' && target === 'Double') return '.to_float()';
    return undefined;
}

// Can `.method()` be appended without parentheses? Literals, names,
// member chains, calls and index expressions with balanced brackets.
export function isSimpleExpression(text: string): boolean {
    const t = text.trim();
    if (/^"([^"\\]|\\.)*"$/.test(t) || /^'([^'\\]|\\.)*'$/.test(t)) return true;
    if (/^-?\d+(\.\d+)?$/.test(t)) return !t.startsWith('-');
    if (!/^[A-Za-z_]/.test(t)) return false;
    // A name followed by `.x`, `(...)`, `[...]` only — no operators at depth 0.
    let depth = 0;
    for (let i = 0; i < t.length; i++) {
        const c = t[i];
        if (c === '"' || c === "'") {   // skip string literals
            const q = c;
            for (i++; i < t.length && t[i] !== q; i++) if (t[i] === '\\') i++;
            continue;
        }
        if (c === '(' || c === '[') depth++;
        else if (c === ')' || c === ']') depth--;
        else if (depth === 0 && !/[\w.]/.test(c)) return false;
    }
    return depth === 0;
}

function escapeRe(s: string): string { return s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'); }

// Index where code ends on a line: before a `//` comment outside strings.
function endOfCode(text: string): number {
    let q: string | null = null;
    for (let i = 0; i < text.length; i++) {
        const c = text[i];
        if (q) { if (c === '\\') i++; else if (c === q) q = null; continue; }
        if (c === '"' || c === "'") q = c;
        else if (c === '/' && text[i + 1] === '/') return i;
    }
    return text.length;
}

function trimmedRange(line: vscode.TextLine, start: number, end: number): vscode.Range | undefined {
    const text = line.text;
    while (start < end && /\s/.test(text[start])) start++;
    while (end > start && /\s/.test(text[end - 1])) end--;
    return end > start ? new vscode.Range(line.lineNumber, start, line.lineNumber, end) : undefined;
}
