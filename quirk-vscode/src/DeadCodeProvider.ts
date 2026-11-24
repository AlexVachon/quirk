import * as vscode from 'vscode';
import { maskLineWithState, TripleState } from './utils/maskLine';

const deadDecoration = vscode.window.createTextEditorDecorationType({
    opacity: '0.45',
});

function countBraces(line: string): { opens: number; closes: number; parens: number } {
    let opens = 0, closes = 0;
    let parens = 0;   // ( and [ opened minus closed: a call or list going on to the next line
    let inStr = false;
    let i = 0;
    while (i < line.length) {
        const ch = line[i];
        if (!inStr && ch === '"') { inStr = true; i++; continue; }
        if (inStr && ch === '\\') { i += 2; continue; } // skip escape
        if (inStr && ch === '"') { inStr = false; i++; continue; }
        if (!inStr) {
            if (ch === '/' && line[i + 1] === '/') break; // line comment
            if (ch === '{') opens++;
            else if (ch === '}') closes++;
            else if (ch === '(' || ch === '[') parens++;
            else if (ch === ')' || ch === ']') parens--;
        }
        i++;
    }
    return { opens, closes, parens };
}

export function computeDeadLines(text: string): Set<number> {
    const lines = text.split('\n');
    const dead = new Set<number>();

    let depth = 0;
    let deadDepth = -1;
    // Braces opened by a `return <expr>` whose expression is multi-line —
    // typically `return fn() => { ... }` or `return Struct { ... }`. While
    // this counter is > 0 we're inside the return *value*, not after it,
    // so dead-code marking is suspended until those braces close.
    let returnValueOpens = 0;
    // Same for ( and [: `return Config(a,` + more arguments on the next
    // lines is one return statement until its parentheses close.
    let returnParens = 0;
    let inDocstring = false;
    // A `"""…"""` string spanning lines: its lines are part of the statement
    // that opened it — `return r"""` + a template is one return, not a
    // return followed by dead code. `stringDead`: it opened on a dead line.
    let triple: TripleState = { active: false, quote: '' };
    let stringDead = false;

    for (let i = 0; i < lines.length; i++) {
        const line = lines[i];
        const trimmed = line.trim();

        if (!triple.active && trimmed === '---') { inDocstring = !inDocstring; continue; }
        if (inDocstring) continue;

        // Strings (and comments) masked out, so braces and `return` inside
        // them don't count.
        const inString = triple.active;
        const ms = maskLineWithState(line, triple);
        triple = ms.state;
        const { opens, closes, parens } = countBraces(ms.masked);
        const depthBefore = depth;

        if (inString) {
            if (stringDead && trimmed.length > 0) dead.add(i);
            depth = Math.max(0, depth + opens - closes);
            if (deadDepth >= 0 && depth < deadDepth) deadDepth = -1;
            continue;
        }

        if (
            deadDepth >= 0
            && returnValueOpens === 0          // not inside a return-value scope
            && returnParens === 0
            && depthBefore >= deadDepth
            && trimmed.length > 0
            && !trimmed.startsWith('//')
            && !/^\s*\}/.test(line)
        ) {
            dead.add(i);
        }
        stringDead = triple.active && dead.has(i);

        // If we're inside a return value (multi-line lambda body, etc.),
        // track its brace balance and unsuspend once it closes.
        if (returnValueOpens > 0) {
            returnValueOpens += opens - closes;
            if (returnValueOpens < 0) returnValueOpens = 0;
        }
        const continuing = returnParens > 0;
        if (continuing) returnParens = Math.max(0, returnParens + parens);

        depth = Math.max(0, depth + opens - closes);

        if (deadDepth >= 0 && depth < deadDepth) {
            deadDepth = -1;
        }

        if (deadDepth < 0 && /^\s*return\b/.test(ms.masked) && !trimmed.startsWith('//')) {
            deadDepth = depthBefore;
            // The return expression spans multiple lines if it opens more
            // braces than it closes (e.g. `return fn() => {`). Defer dead-
            // code marking until the corresponding `}` closes the value.
            if (opens > closes) returnValueOpens = opens - closes;
            if (parens > 0 && !continuing) returnParens = parens;
        }
    }

    return dead;
}

export function getDeadLines(document: vscode.TextDocument): Set<number> {
    return computeDeadLines(document.getText());
}

function getDeadRanges(document: vscode.TextDocument): vscode.Range[] {
    const lines = document.getText().split('\n');
    const dead = computeDeadLines(document.getText());
    return [...dead].map(i => new vscode.Range(i, 0, i, lines[i].length));
}

export function updateDeadCode(editor: vscode.TextEditor): void {
    if (editor.document.languageId !== 'quirk') {
        editor.setDecorations(deadDecoration, []);
        return;
    }
    editor.setDecorations(deadDecoration, getDeadRanges(editor.document));
}
