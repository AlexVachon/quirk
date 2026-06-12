#!/usr/bin/env python3
"""
Extract `---` docstrings + signatures from the stdlib packages and emit
per-package markdown to `docs/stdlib/`.

Quirk docstring convention:

    ---
    Short description.

    @param name: …
    @returns …
    @example:
    foo(42)
    ---
    define foo(x: Int) -> Int { … }

We walk each `packages/*/*.quirk` file, find every `---…---` block
followed by a `(extern )?define …` or `(extern )?struct …` line,
and emit one markdown section per definition.

Run:
    python3 tools/gen_stdlib_docs.py
    # writes docs/stdlib/<pkg>.md
"""

from __future__ import annotations

import json
import os
import re
import sys
from pathlib import Path
from dataclasses import dataclass, field
from typing import Optional

REPO_ROOT = Path(__file__).resolve().parents[1]
PACKAGES_DIR = REPO_ROOT / "packages"
DOCS_DIR = REPO_ROOT / "docs" / "stdlib"
# JSON symbol index consumed by the VSCode extension's hover provider.
# Lives next to the extension source so the build pipeline can bundle it.
VSCODE_DATA = REPO_ROOT.parent / "quirk-vscode" / "data"

# Header line for a struct or define, capturing the signature.
RE_DEFINE = re.compile(
    r"^(\s*)(extern\s+)?(define|init)\s+([A-Za-z_]\w*)\s*(\[[^\]]*\])?\s*\("
)
RE_STRUCT = re.compile(
    r"^(\s*)(extern\s+)?struct\s+([A-Za-z_]\w*)\b"
)
RE_ENUM = re.compile(
    r"^(\s*)enum\s+([A-Za-z_]\w*)\s*(?:\([^)]*\))?\s*\{?"
)
RE_INTERFACE = re.compile(
    r"^(\s*)interface\s+([A-Za-z_]\w*)\b"
)


@dataclass
class Entry:
    kind: str            # "struct" | "define" | "enum" | "interface" | "method"
    name: str            # bare name (no struct prefix)
    qualified: str       # "StructName.method" or just "name"
    signature: str       # the full first-line signature (trimmed)
    docstring: str       # markdown body between the --- fences
    indent: int          # indentation level of the declaration


def parse_file(path: Path) -> list[Entry]:
    """Walk one .quirk file and return every documented entry found."""
    src = path.read_text(encoding="utf-8", errors="replace")
    lines = src.splitlines()

    entries: list[Entry] = []
    # Track the enclosing struct/enum so methods get qualified names.
    # We use brace-depth heuristics: when a `struct X {` opens at indent
    # I, every define inside the matching block is a method on X.
    scope_stack: list[tuple[str, int]] = []  # (name, opening-indent)
    brace_depth = 0

    i = 0
    while i < len(lines):
        line = lines[i]
        stripped = line.strip()

        # Brace tracking — naive, but the stdlib doesn't embed `{` / `}`
        # inside strings except for interpolation which is rare enough
        # to ignore at this fidelity.
        brace_depth += line.count("{") - line.count("}")

        # Pop scope when we drop below its indent level (best-effort).
        while scope_stack and brace_depth <= scope_stack[-1][1]:
            scope_stack.pop()

        # Docstring block: `---` on its own line, until the next `---`.
        if stripped == "---":
            doc_lines: list[str] = []
            j = i + 1
            while j < len(lines) and lines[j].strip() != "---":
                doc_lines.append(lines[j])
                j += 1
            if j >= len(lines):
                # Unterminated — bail out of doc-block parsing.
                i += 1
                continue
            # The line after the closing `---` should be a declaration.
            k = j + 1
            while k < len(lines) and lines[k].strip() == "":
                k += 1
            if k >= len(lines):
                i = j + 1
                continue
            decl_line = lines[k]
            entry = _classify_decl(decl_line, scope_stack)
            if entry is not None:
                entry.docstring = _clean_docstring(doc_lines)
                entries.append(entry)
            i = j + 1
            continue

        # Even without a docstring, struct/enum/interface decls are
        # worth listing (they anchor sections). Module-level only —
        # nested defines without docstrings stay out of the index.
        m = RE_STRUCT.match(line)
        if m and not scope_stack:
            indent = len(m.group(1))
            scope_stack.append((m.group(3), brace_depth - line.count("{") + line.count("}")))
            # If the next line(s) start a new scope, we already pushed.
            continue
        m = RE_ENUM.match(line)
        if m and not scope_stack:
            scope_stack.append((m.group(2), brace_depth - line.count("{") + line.count("}")))
            continue
        i += 1

    return entries


def _classify_decl(line: str, scope_stack: list[tuple[str, int]]) -> Optional[Entry]:
    """Identify the kind of declaration on `line` and build an Entry."""
    stripped = line.strip()
    indent = len(line) - len(line.lstrip())

    m = RE_STRUCT.match(line)
    if m:
        name = m.group(3)
        return Entry("struct", name, name, stripped, "", indent)

    m = RE_INTERFACE.match(line)
    if m:
        name = m.group(2)
        return Entry("interface", name, name, stripped, "", indent)

    m = RE_ENUM.match(line)
    if m:
        name = m.group(2)
        return Entry("enum", name, name, stripped, "", indent)

    m = RE_DEFINE.match(line)
    if m:
        name = m.group(4)
        parent = scope_stack[-1][0] if scope_stack else None
        qualified = f"{parent}.{name}" if parent else name
        kind = "method" if parent else "define"
        return Entry(kind, name, qualified, stripped, "", indent)

    return None


def _clean_docstring(lines: list[str]) -> str:
    """Normalise a docstring body — strip common leading indent."""
    if not lines:
        return ""
    non_empty = [l for l in lines if l.strip()]
    if not non_empty:
        return ""
    indents = [len(l) - len(l.lstrip()) for l in non_empty]
    common = min(indents)
    out = []
    for l in lines:
        out.append(l[common:] if len(l) >= common else l)
    return "\n".join(out).rstrip()


def emit_package(pkg_name: str, files: list[Path]) -> str:
    """Render markdown for one package."""
    out: list[str] = []
    out.append(f"# `{pkg_name}` — API reference\n")
    out.append("> Generated from in-source `---` docstrings by")
    out.append("> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).")
    out.append("> Do not hand-edit — re-run `make docs` to refresh.\n")

    by_file: dict[Path, list[Entry]] = {}
    for f in sorted(files):
        entries = parse_file(f)
        if entries:
            by_file[f] = entries

    if not by_file:
        out.append("*(No documented declarations in this package yet.)*\n")
        return "\n".join(out)

    for f, entries in by_file.items():
        rel = f.relative_to(PACKAGES_DIR)
        out.append(f"\n## `{rel}`\n")
        # Group by struct/enum if any; orphan defines list directly.
        seen_orphans = False
        # First pass: top-level structs/enums/interfaces
        for e in entries:
            if e.kind in ("struct", "enum", "interface"):
                out.append(f"### `{e.signature.rstrip(' {')}`\n")
                if e.docstring:
                    out.append(f"{e.docstring}\n")
                # Methods belonging to this scope (qualified prefix match)
                children = [m for m in entries
                            if m.kind == "method" and m.qualified.startswith(e.name + ".")]
                if children:
                    for c in children:
                        out.append(f"#### `{c.signature.rstrip(' {')}`\n")
                        if c.docstring:
                            out.append(f"{c.docstring}\n")
        # Second pass: module-level defines (not methods)
        for e in entries:
            if e.kind == "define":
                if not seen_orphans:
                    out.append("\n### Module-level functions\n")
                    seen_orphans = True
                out.append(f"#### `{e.signature.rstrip(' {')}`\n")
                if e.docstring:
                    out.append(f"{e.docstring}\n")

    return "\n".join(out)


def build_symbol_index(all_entries: dict[str, list[Entry]]) -> dict:
    """Bare-name symbol map for the VSCode extension's hover fallback.

    Keys are the bare identifier the user types (`argv`, `exists`,
    `length`). Values are arrays of candidates — the same name can
    legitimately exist in multiple packages (e.g. `length` is on
    many structs), so hover shows them all rather than picking one.

    Each candidate carries `package`, `qualified` (StructName.method
    or bare), `kind`, `signature`, and `doc` so the extension can
    render a tooltip without re-reading source.
    """
    out: dict[str, list[dict]] = {}
    for pkg_name, entries in all_entries.items():
        for e in entries:
            if not e.docstring and e.kind == "method":
                # Skip undocumented methods to keep the index small;
                # explicit module-level structs/funcs stay even
                # without docstrings (the signature alone is useful).
                continue
            out.setdefault(e.name, []).append({
                "package": pkg_name,
                "qualified": e.qualified,
                "kind": e.kind,
                "signature": e.signature,
                "doc": e.docstring,
            })
    # Deterministic ordering for clean diffs.
    return {k: out[k] for k in sorted(out)}


def main() -> int:
    if not PACKAGES_DIR.is_dir():
        print(f"ERR: packages dir not found at {PACKAGES_DIR}", file=sys.stderr)
        return 1
    DOCS_DIR.mkdir(parents=True, exist_ok=True)

    pkg_count = 0
    entry_count = 0
    all_entries: dict[str, list[Entry]] = {}
    for pkg_path in sorted(PACKAGES_DIR.iterdir()):
        if not pkg_path.is_dir():
            continue
        files = list(pkg_path.rglob("*.quirk"))
        if not files:
            continue
        md = emit_package(pkg_path.name, files)
        out_path = DOCS_DIR / f"{pkg_path.name}.md"
        out_path.write_text(md, encoding="utf-8")
        pkg_count += 1
        # Quick count for the summary line.
        pkg_entries: list[Entry] = []
        for f in files:
            pkg_entries.extend(parse_file(f))
        entry_count += len(pkg_entries)
        all_entries[pkg_path.name] = pkg_entries
        print(f"  ✓ docs/stdlib/{pkg_path.name}.md")

    # Top-level index page.
    index_lines = [
        "# Quirk stdlib — API reference",
        "",
        "> Generated from in-source `---` docstrings by",
        "> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).",
        "> Run `make docs` to refresh.",
        "",
        "## Packages",
        "",
    ]
    for pkg_path in sorted(PACKAGES_DIR.iterdir()):
        if pkg_path.is_dir() and (DOCS_DIR / f"{pkg_path.name}.md").exists():
            index_lines.append(f"- [`{pkg_path.name}`]({pkg_path.name}.md)")
    (DOCS_DIR / "index.md").write_text("\n".join(index_lines) + "\n", encoding="utf-8")

    # Emit the JSON symbol index next to the VSCode extension source.
    # The hover provider falls back to this when executeDefinitionProvider
    # can't navigate to a stdlib symbol (most often: top-level functions
    # accessed via module-alias like `sys.argv()` where the cursor lands
    # on `argv` without an `from sys use { argv }` in scope).
    if VSCODE_DATA.parent.exists():
        VSCODE_DATA.mkdir(exist_ok=True)
        index = build_symbol_index(all_entries)
        idx_path = VSCODE_DATA / "stdlib-index.json"
        idx_path.write_text(json.dumps(index, indent=0), encoding="utf-8")
        print(f"  ✓ {idx_path.relative_to(REPO_ROOT.parent)} ({len(index)} symbols)")

    print(f"\nGenerated {pkg_count} packages, ~{entry_count} documented entries.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
