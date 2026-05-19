/**
 * Replaces string literals (single, double, and the contents of `${...}`
 * interpolations are preserved as code) and `//` line comments with spaces,
 * preserving line length so column offsets stay accurate. Used by providers
 * that scan for identifier usages to avoid matching inside strings/comments.
 */
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
                    } else {
                        masked += char;
                    }
                }
            }
        }
    }
    return masked;
}
