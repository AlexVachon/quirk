// Quirk source extensions — mirrors quirk-compiler/include/source_ext.hpp.
// `.qk` (compiler 7.6+) is preferred and `.quirk` keeps working; wherever a
// module is looked up by name, `.qk` is tried first.
export const QUIRK_EXTS = ['.qk', '.quirk'];

// Glob for every Quirk source file (vscode.workspace.findFiles etc.).
export const QUIRK_GLOB = '**/*.{qk,quirk}';

// `pkg/src/index` → [`pkg/src/index.qk`, `pkg/src/index.quirk`].
export const withExts = (stem: string): string[] => QUIRK_EXTS.map(e => stem + e);

export const isQuirkSource = (file: string): boolean => QUIRK_EXTS.some(e => file.endsWith(e));

// `csv.qk` / `csv.quirk` → `csv`; anything else unchanged.
export const stripQuirkExt = (file: string): string => {
    for (const e of QUIRK_EXTS) if (file.endsWith(e)) return file.slice(0, -e.length);
    return file;
};

// A package's entry file (`index.qk`, `__init.quirk`, …), which isn't a module of its own.
export const isEntryFile = (file: string): boolean =>
    ['index', '__init'].some(base => QUIRK_EXTS.some(e => file === base + e));
