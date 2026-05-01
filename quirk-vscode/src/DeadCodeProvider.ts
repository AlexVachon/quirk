import * as vscode from 'vscode';

const deadDecoration = vscode.window.createTextEditorDecorationType({
    opacity: '0.45',
});

function countBraces(line: string): { opens: number; closes: number } {
    let opens = 0, closes = 0;
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
        }
        i++;
    }
    return { opens, closes };
}

function computeDeadLines(text: string): Set<number> {
    const lines = text.split('\n');
    const dead = new Set<number>();

    let depth = 0;
    let deadDepth = -1;
    let inDocstring = false;

    for (let i = 0; i < lines.length; i++) {
        const line = lines[i];
        const trimmed = line.trim();

        if (trimmed === '---') { inDocstring = !inDocstring; continue; }
        if (inDocstring) continue;

        const { opens, closes } = countBraces(line);
        const depthBefore = depth;

        if (
            deadDepth >= 0
            && depthBefore >= deadDepth
            && trimmed.length > 0
            && !trimmed.startsWith('//')
            && !/^\s*\}/.test(line)
        ) {
            dead.add(i);
        }

        depth = Math.max(0, depth + opens - closes);

        if (deadDepth >= 0 && depth < deadDepth) {
            deadDepth = -1;
        }

        if (deadDepth < 0 && /^\s*return\b/.test(line) && !trimmed.startsWith('//')) {
            deadDepth = depthBefore;
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
