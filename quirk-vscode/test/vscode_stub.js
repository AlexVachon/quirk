// A stand-in for the `vscode` module, enough for the analyzers and the
// definition provider to run under plain Node (npm test).
class Position {
    constructor(line, character) { this.line = line; this.character = character; }
    isEqual(o) { return !!o && o.line === this.line && o.character === this.character; }
}
class Range {
    constructor(a, b, c, d) { this.start = new Position(a, b); this.end = new Position(c, d); }
    isEqual(o) { return !!o && this.start.isEqual(o.start) && this.end.isEqual(o.end); }
    contains() { return true; }
}
class Location {
    constructor(uri, posOrRange) {
        this.uri = uri;
        this.range = posOrRange && posOrRange.start ? posOrRange : new Range(posOrRange.line, posOrRange.character, posOrRange.line, posOrRange.character);
    }
}
class Diagnostic {
    constructor(range, message, severity) { this.range = range; this.message = message; this.severity = severity; }
}
const disposable = () => ({ dispose() {} });
const config = { get: (_k, d) => d, inspect: () => undefined, has: () => false };
module.exports = {
    Position, Range, Location, Diagnostic,
    DiagnosticSeverity: { Error: 0, Warning: 1, Information: 2, Hint: 3 },
    DiagnosticTag: { Unnecessary: 1, Deprecated: 2 },
    Uri: { file: (p) => ({ fsPath: p, toString: () => p }) },
    window: {
        activeTextEditor: undefined, visibleTextEditors: [],
        onDidChangeActiveTextEditor: disposable,
        createTextEditorDecorationType: () => ({ dispose() {} }),
    },
    workspace: {
        textDocuments: [], workspaceFolders: [],
        getWorkspaceFolder: () => undefined,
        getConfiguration: () => config,
        onDidChangeTextDocument: disposable, onDidCloseTextDocument: disposable,
    },
    languages: { createDiagnosticCollection: () => ({ set() {}, delete() {} }) },
};
