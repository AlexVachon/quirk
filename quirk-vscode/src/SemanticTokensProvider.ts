import * as vscode from 'vscode';
import * as fs from 'fs';
import { maskLineWithState, TripleState } from './utils/maskLine';
import { resolveModulePath, findProjectRootFor } from './ImportProvider';

// Define the types of tokens we will return. 
// Index 0 = 'namespace', which is the standard scope for modules.
const tokenTypes = ['namespace', 'class', 'function', 'variable'];
// 'readonly' (bit 2): constants — themes color `variable.readonly` as one.
const tokenModifiers = ['declaration', 'static', 'readonly'];
const VARIABLE = 3, DECLARATION = 1 << 0, READONLY = 1 << 2;

export const legend = new vscode.SemanticTokensLegend(tokenTypes, tokenModifiers);

export class QuirkSemanticTokensProvider implements vscode.DocumentSemanticTokensProvider {
    
    provideDocumentSemanticTokens(
        document: vscode.TextDocument, 
        token: vscode.CancellationToken
    ): vscode.ProviderResult<vscode.SemanticTokens> {
        
        const builder = new vscode.SemanticTokensBuilder(legend);
        // [line, start, length, type, modifiers], sorted before pushing.
        const toks: number[][] = [];
        const text = document.getText();
        const lines = text.split(/\r?\n/);

        // 1. Identify all Module Aliases in the file
        //    Scans for: "use path.to.alias" -> alias = "alias"
        const moduleAliases = new Set<string>();

        for (const line of lines) {
            // Regex for "use path" or "use path.to.module"
            const useMatch = /^\s*use\s+([.a-zA-Z0-9_/]+)/.exec(line);
            if (useMatch) {
                const fullPath = useMatch[1];
                // Extract last part: "typing.collections.list" -> "list"
                const alias = fullPath.split(/[\.\/]/).pop();
                if (alias) moduleAliases.add(alias);
            }
        }

        // 2. Scan for usages of these aliases
        //    Looking for: "alias.Member"
        // Track docstring state so we don't paint module references inside
        // `---` blocks. Semantic tokens override TextMate coloring, so
        // without this `print(itertools.foo)` in a docstring example would
        // still get the namespace highlight.
        let inDocBlock = false;
        for (let i = 0; i < lines.length; i++) {
            const line = lines[i];
            const trimmed = line.trim();

            // Single-line docstring (`--- text ---`): entire line is doc,
            // skip without touching `inDocBlock`.
            if (trimmed.startsWith('---') && trimmed.endsWith('---') && trimmed.length > 3) {
                continue;
            }
            // Lone `---` toggles a multi-line docstring block.
            if (trimmed === '---') {
                inDocBlock = !inDocBlock;
                continue;
            }
            if (inDocBlock) continue;

            // Skip `//` line comments — same reason: namespace coloring
            // shouldn't bleed into commentary that happens to mention a
            // module name with a dot.
            const commentIdx = line.indexOf('//');

            // Look for words followed immediately by a dot: "word."
            const regex = /([a-zA-Z_]\w*)\./g;
            let match;

            while ((match = regex.exec(line)) !== null) {
                if (commentIdx !== -1 && match.index >= commentIdx) break;
                const word = match[1];

                if (moduleAliases.has(word)) {
                    // It is a known module alias!
                    // Color it as a 'namespace' (Index 0 in tokenTypes)
                    const startPos = match.index;

                    toks.push([i, startPos, word.length, 0, 0]);   // 0 = namespace
                }
            }
        }

        // 3. Constants: names declared `const` here, or imported (`from x use
        //    { PORT }`) from a file that declares them `const` — every use.
        for (const t of constantTokens(document, lines)) toks.push(t);

        toks.sort((a, b) => a[0] - b[0] || a[1] - b[1]);
        let lastLine = -1, lastEnd = -1;
        for (const [l, c, len, type, mods] of toks) {
            if (l === lastLine && c < lastEnd) continue;   // overlapping: keep the first
            builder.push(l, c, len, type, mods);
            lastLine = l;
            lastEnd = c + len;
        }
        return builder.build();
    }
}

const CONST_DECL = /\bconst\s+([A-Za-z_]\w*)/g;

// Names declared `const` in `text`.
function constNames(text: string): Set<string> {
    const out = new Set<string>();
    let m: RegExpExecArray | null;
    CONST_DECL.lastIndex = 0;
    while ((m = CONST_DECL.exec(text)) !== null) out.add(m[1]);
    return out;
}

function constantTokens(document: vscode.TextDocument, lines: string[]): number[][] {
    const names = constNames(lines.join('\n'));
    // Imported constants: `from x use { A, B }` where x declares A `const`.
    const file = document.uri.fsPath;
    let root: string | undefined;
    for (const line of lines) {
        const m = /^\s*from\s+([.a-zA-Z0-9_/]+)\s+use\s+\{([^}]*)\}/.exec(line);
        if (!m) continue;
        try {
            root = root ?? findProjectRootFor(file);
            const target = resolveModulePath(root, file, m[1]);
            if (!target) continue;
            const declared = constNames(fs.readFileSync(target, 'utf8'));
            for (const part of m[2].split(',')) {
                const name = part.trim().split(/\s+as\s+/)[0].trim();
                const shown = part.includes(' as ') ? part.split(/\s+as\s+/)[1].trim() : name;
                if (declared.has(name)) names.add(shown);
            }
        } catch { /* unreadable module: no constants from it */ }
    }
    const out: number[][] = [];
    if (names.size === 0) return out;
    let state: TripleState = { active: false, quote: '' };
    let inDoc = false;
    for (let i = 0; i < lines.length; i++) {
        const line = lines[i];
        const trimmed = line.trim();
        if (!state.active && trimmed === '---') { inDoc = !inDoc; continue; }
        if (inDoc) continue;
        const ms = maskLineWithState(line, state);   // strings and comments blanked
        state = ms.state;
        const re = /[A-Za-z_]\w*/g;
        let m: RegExpExecArray | null;
        while ((m = re.exec(ms.masked)) !== null) {
            if (!names.has(m[0])) continue;
            const before = ms.masked.slice(0, m.index);
            if (/\.\s*$/.test(before)) continue;                 // obj.PORT: a field
            if (/^\s*=(?!=)/.test(ms.masked.slice(m.index + m[0].length)) && /[(,]\s*$/.test(before)) continue;  // f(PORT=1)
            const isDecl = /\bconst\s+$/.test(before);
            out.push([i, m.index, m[0].length, VARIABLE, READONLY | (isDecl ? DECLARATION : 0)]);
        }
    }
    return out;
}
