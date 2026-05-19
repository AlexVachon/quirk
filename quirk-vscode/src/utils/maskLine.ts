/**
 * Replaces string literals (single, double, and the contents of `${...}`
 * interpolations are preserved as code) and `//` line comments with spaces,
 * preserving line length so column offsets stay accurate. Used by providers
 * that scan for identifier usages to avoid matching inside strings/comments.
 *
 * `tripleState` tracks whether we entered the line already inside a
 * `"""..."""` (or `'''...'''`) multi-line string. Callers can thread this
 * across lines so identifiers buried in triple-quoted content don't leak
 * out as undefined-variable warnings. Returns the updated state.
 */
export type TripleState = { active: boolean; quote: '"' | "'" | '' };

// Mask a slice of string content: blank everything *except* the contents
// of `${…}` interpolations, which stay as code so identifier references
// inside them register as usages. Mirrors the single-line maskLine
// in-string branch but operates on a bare string slice with no enclosing
// quotes. Format specs after a top-level `:` / `%` / `|` are blanked.
function maskStringContent(s: string): string {
    let out = "";
    let i = 0;
    while (i < s.length) {
        const c = s[i];
        const nxt = s[i + 1];
        if (c === '\\' && i + 1 < s.length) { out += '  '; i += 2; continue; }
        if (c === '$' && nxt === '{') {
            out += '  ';
            i += 2;
            let depth = 1;
            while (i < s.length && depth > 0) {
                const ch = s[i];
                if (ch === '{') { depth++; out += ch; i++; continue; }
                if (ch === '}') {
                    depth--;
                    if (depth === 0) { out += ' '; i++; break; }
                    out += ch; i++; continue;
                }
                if (depth === 1 && (ch === ':' || ch === '%' || ch === '|')) {
                    while (i < s.length && s[i] !== '}') { out += ' '; i++; }
                    continue;
                }
                out += ch;
                i++;
            }
            continue;
        }
        out += ' ';
        i++;
    }
    return out;
}

export function maskLineWithState(line: string, state: TripleState): { masked: string; state: TripleState } {
    // Continuation line of a `"""..."""` block opened earlier. Scan for
    // the closing triple-quote on this line; everything up to it is
    // string content (with interpolations preserved as code).
    if (state.active) {
        const q = state.quote;
        const idx = line.indexOf(q + q + q);
        if (idx === -1) {
            // Whole line is content — preserve any `${…}` expressions.
            return { masked: maskStringContent(line), state };
        }
        const closedAt = idx + 3;
        const head = maskStringContent(line.slice(0, idx)) + '   '; // 3 spaces for the closing """
        const tail = maskLine(line.slice(closedAt));
        return {
            masked: head + tail,
            state: { active: false, quote: '' },
        };
    }

    // First-line case: does the line open a triple-quoted string that
    // doesn't close on the same line? If so, mask from the opener to
    // end-of-line (preserving interpolations) and flip the flag.
    for (let i = 0; i + 2 < line.length; i++) {
        const c = line[i];
        if ((c !== '"' && c !== "'") || line[i + 1] !== c || line[i + 2] !== c) continue;
        // Skip openers that are inside a comment we'd be masking anyway.
        if (line.slice(0, i).includes('//')) continue;
        const restStart = i + 3;
        const closeIdx = line.indexOf(c + c + c, restStart);
        if (closeIdx !== -1) {
            // Same-line """...""" — handle inline. Three spaces for the
            // opener + masked content + three spaces for the close + tail.
            const head = line.slice(0, i);
            const content = maskStringContent(line.slice(restStart, closeIdx));
            const tail = maskLine(line.slice(closeIdx + 3));
            return {
                masked: head + '   ' + content + '   ' + tail,
                state,
            };
        }
        // Spans lines: mask from the opener to EOL, flip state.
        const head = line.slice(0, i);
        const content = maskStringContent(line.slice(restStart));
        return {
            masked: head + '   ' + content,
            state: { active: true, quote: c as '"' | "'" },
        };
    }
    return { masked: maskLine(line), state };
}

export function maskLine(line: string): string {
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
                    } else if ((char === ':' || char === '%' || char === '|') && braceDepth === 1) {
                        // Format-spec separator inside `${expr : fmt}` (or
                        // legacy `${expr % fmt}` / `${expr | fmt}`). Everything
                        // from here to the closing `}` is a format string,
                        // not code — blank it so `${n:x}` doesn't flag `x`
                        // as an undefined identifier.
                        while (j < line.length) {
                            const c = line[j];
                            if (c === '}') {
                                braceDepth = 0;
                                inInterpolation = false;
                                masked += ' ';
                                break;
                            }
                            masked += ' ';
                            j++;
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
