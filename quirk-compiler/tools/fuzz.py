#!/usr/bin/env python3
"""
Mutation-based fuzzer for the Quirk compiler.

Strategy: take a random `.quirk` file from the seed corpus
(tests/probes + tests/*.quirk), apply 1-3 small mutations
(swap an operator, change a literal, drop a line, etc.), then
compile + run it. We're looking for *catastrophic* failures:

  - SIGSEGV / SIGABRT / SIGFPE        (rc=139/134/136)
  - "Internal compiler error"          (LLVM verifier reject)
  - "malformed IR"
  - ICmpInst::AssertOK / similar LLVM asserts

Sema rejections (clean error → exit 1) are FINE. The bar is "no
mutation, however nonsensical, should crash the compiler".

Each unique crash is saved to `tools/fuzz_findings/c<N>_<hint>.quirk`
with the seed file + mutation log embedded as a comment, so the
user can replay it manually and decide whether to promote it to
`tests/probes/`.

Run:
    python3 tools/fuzz.py --iters 500
    python3 tools/fuzz.py --iters 100 --seed 12345     # reproducible
    python3 tools/fuzz.py --iters 200 --keep-passing   # also save non-crashing
"""

from __future__ import annotations

import argparse
import hashlib
import os
import random
import re
import shutil
import signal
import subprocess
import sys
from pathlib import Path
from dataclasses import dataclass
from typing import Optional

REPO_ROOT = Path(__file__).resolve().parents[1]
QUIRK = REPO_ROOT / "bin" / "quirk"
SEED_DIRS = [REPO_ROOT / "tests" / "probes", REPO_ROOT / "tests"]
FINDINGS_DIR = REPO_ROOT / "tools" / "fuzz_findings"

# Markers that flag a true compiler crash vs a clean Sema rejection.
CRASH_MARKERS = [
    "Internal compiler error",
    "malformed IR",
    "Segmentation fault",
    "core dumped",
    "ICmpInst::AssertOK",
    "Assertion `getOperand",
    "Stack dump:",
    "PHI node operands",
]
CRASH_SIGNALS = {139, 134, 136}   # SIGSEGV, SIGABRT, SIGFPE
TIMEOUT_S = 8


# ────────────────────────── Mutations ──────────────────────────

OP_PAIRS = [
    # Arithmetic / comparison swap — same arity, often valid syntax.
    ("+", "-"), ("+", "*"), ("-", "*"), ("*", "/"),
    ("==", "!="), ("==", "<"), ("<", ">"), ("<=", ">="),
    ("and", "or"), ("not ", ""),
]
TYPE_PAIRS = [
    ("Int", "String"), ("Int", "Bool"), ("Int", "Double"),
    ("String", "Int"), ("Bool", "Int"), ("Double", "Int"),
    ("List", "Map"), ("Map", "Set"),
]


def mutate_operator_swap(src: str, rng: random.Random) -> tuple[str, str]:
    a, b = rng.choice(OP_PAIRS)
    if a not in src:
        return src, ""
    # One occurrence per mutation; pick a random index in the matches.
    idxs = [m.start() for m in re.finditer(re.escape(a), src)]
    if not idxs:
        return src, ""
    i = rng.choice(idxs)
    return src[:i] + b + src[i + len(a):], f"op {a!r}→{b!r} @{i}"


def mutate_type_swap(src: str, rng: random.Random) -> tuple[str, str]:
    a, b = rng.choice(TYPE_PAIRS)
    pat = re.compile(rf"\b{re.escape(a)}\b")
    if not pat.search(src):
        return src, ""
    # Pick one occurrence.
    occurrences = list(pat.finditer(src))
    occ = rng.choice(occurrences)
    return src[:occ.start()] + b + src[occ.end():], f"type {a}→{b} @{occ.start()}"


def mutate_int_literal(src: str, rng: random.Random) -> tuple[str, str]:
    pat = re.compile(r"\b(\d+)\b")
    matches = list(pat.finditer(src))
    if not matches:
        return src, ""
    m = rng.choice(matches)
    new = rng.choice(["0", "1", "-1", "2147483647", "-2147483648", "42"])
    return src[:m.start()] + new + src[m.end():], f"int {m.group(1)}→{new} @{m.start()}"


def mutate_string_literal(src: str, rng: random.Random) -> tuple[str, str]:
    pat = re.compile(r'"[^"\n]*"')
    matches = list(pat.finditer(src))
    if not matches:
        return src, ""
    m = rng.choice(matches)
    new = rng.choice(['""', '"x"', '"a${b}c"', '"' + ("x" * 100) + '"'])
    return src[:m.start()] + new + src[m.end():], f"str {m.group(0)[:10]}…→{new[:10]}… @{m.start()}"


def mutate_bool_flip(src: str, rng: random.Random) -> tuple[str, str]:
    pat = re.compile(r"\b(true|false)\b")
    matches = list(pat.finditer(src))
    if not matches:
        return src, ""
    m = rng.choice(matches)
    new = "false" if m.group(1) == "true" else "true"
    return src[:m.start()] + new + src[m.end():], f"bool {m.group(1)}→{new} @{m.start()}"


def mutate_drop_line(src: str, rng: random.Random) -> tuple[str, str]:
    lines = src.split("\n")
    if len(lines) < 5:
        return src, ""
    i = rng.randrange(1, len(lines) - 1)
    # Avoid dropping lines that obviously change brace balance.
    if lines[i].count("{") != lines[i].count("}"):
        return src, ""
    return "\n".join(lines[:i] + lines[i+1:]), f"drop line {i}"


def mutate_dup_line(src: str, rng: random.Random) -> tuple[str, str]:
    lines = src.split("\n")
    if len(lines) < 5:
        return src, ""
    i = rng.randrange(1, len(lines) - 1)
    if lines[i].count("{") != lines[i].count("}"):
        return src, ""
    return "\n".join(lines[:i] + [lines[i]] + lines[i:]), f"dup line {i}"


def mutate_swap_lines(src: str, rng: random.Random) -> tuple[str, str]:
    lines = src.split("\n")
    if len(lines) < 6:
        return src, ""
    i = rng.randrange(1, len(lines) - 2)
    if any(lines[k].count("{") != lines[k].count("}") for k in (i, i+1)):
        return src, ""
    new_lines = lines[:i] + [lines[i+1], lines[i]] + lines[i+2:]
    return "\n".join(new_lines), f"swap lines {i},{i+1}"


MUTATIONS = [
    mutate_operator_swap,
    mutate_type_swap,
    mutate_int_literal,
    mutate_string_literal,
    mutate_bool_flip,
    mutate_drop_line,
    mutate_dup_line,
    mutate_swap_lines,
]


# ────────────────────────── Runner ──────────────────────────

@dataclass
class Outcome:
    rc: int
    output: str
    crashed: bool
    marker: Optional[str]


def run(quirk_path: Path) -> Outcome:
    try:
        # `errors="replace"` because some fuzzed programs print garbage
        # bytes (e.g. unboxed pointer interpreted as a String*); we'd
        # rather see the rest of the output than choke on decoding.
        proc = subprocess.run(
            [str(QUIRK), "--no-aot", "--no-cache", str(quirk_path)],
            capture_output=True, text=True, timeout=TIMEOUT_S,
            errors="replace",
        )
    except subprocess.TimeoutExpired:
        return Outcome(rc=-1, output="<timeout>", crashed=False, marker=None)
    out = (proc.stdout or "") + (proc.stderr or "")
    rc = proc.returncode
    marker = None
    for m in CRASH_MARKERS:
        if m in out:
            marker = m
            break
    crashed = (rc in CRASH_SIGNALS) or marker is not None
    return Outcome(rc=rc, output=out, crashed=crashed, marker=marker)


def collect_seeds() -> list[Path]:
    # Sort so the `--seed N` reproducibility actually works across
    # machines. `rglob` returns entries in filesystem-iteration order,
    # which varies between local dev (ext4) and CI (overlay), so the
    # same `--seed 1` could pick a different seed file in step 86
    # depending on where we ran. Stable ordering removes that source
    # of flake.
    seeds: list[Path] = []
    for d in SEED_DIRS:
        for p in d.rglob("*.quirk"):
            if p.is_file():
                seeds.append(p)
    seeds.sort()
    return seeds


def crash_signature(out: Outcome) -> str:
    """First line of the crash output, with paths stripped — used to
    deduplicate findings so we don't save 50 copies of the same bug."""
    if out.marker:
        # Find the line containing the marker; keep ~80 chars of it.
        for line in out.output.splitlines():
            if out.marker in line:
                line = re.sub(r"/[^\s]+\.quirk", "<path>", line)
                return line.strip()[:120]
    if out.rc in CRASH_SIGNALS:
        return f"signal:{out.rc}"
    return "unknown"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--iters", type=int, default=200)
    parser.add_argument("--seed", type=int, default=None)
    parser.add_argument("--max-mutations", type=int, default=3)
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    if not QUIRK.exists():
        print(f"ERR: {QUIRK} not found — run `make` first", file=sys.stderr)
        return 1

    seeds = collect_seeds()
    if not seeds:
        print("ERR: no seed .quirk files found", file=sys.stderr)
        return 1

    rng = random.Random(args.seed)
    FINDINGS_DIR.mkdir(parents=True, exist_ok=True)

    seen_signatures: dict[str, Path] = {}
    crashes = 0
    timeouts = 0
    work = REPO_ROOT / "tools" / "fuzz_work"
    work.mkdir(parents=True, exist_ok=True)
    work_quirk = work / "input.quirk"

    for it in range(args.iters):
        seed = rng.choice(seeds)
        src = seed.read_text(encoding="utf-8", errors="replace")
        applied: list[str] = []
        for _ in range(rng.randint(1, args.max_mutations)):
            mut = rng.choice(MUTATIONS)
            new_src, note = mut(src, rng)
            if note:
                src = new_src
                applied.append(note)
        if not applied:
            continue
        work_quirk.write_text(src, encoding="utf-8")
        out = run(work_quirk)
        if out.rc == -1:
            timeouts += 1
            if not args.quiet:
                print(f"  [{it:4d}] timeout — seed={seed.name}")
            continue
        if not out.crashed:
            if not args.quiet and it % 50 == 0:
                print(f"  [{it:4d}] ok  rc={out.rc:3d}  seed={seed.name}")
            continue
        # Crash. Dedup.
        sig = crash_signature(out)
        if sig in seen_signatures:
            continue
        crashes += 1
        path_hint = re.sub(r"[^a-z0-9]+", "_", sig.lower())[:40]
        n = len(seen_signatures) + 1
        save = FINDINGS_DIR / f"c{n:03d}_{path_hint}.quirk"
        header = (
            f"// Fuzz finding #{n}\n"
            f"//   seed: {seed.relative_to(REPO_ROOT)}\n"
            f"//   mutations: {applied}\n"
            f"//   rc={out.rc} marker={out.marker!r}\n"
            f"//   signature: {sig}\n"
            f"//\n"
        )
        save.write_text(header + src, encoding="utf-8")
        seen_signatures[sig] = save
        print(f"  [{it:4d}] CRASH #{n}: {sig}")
        print(f"          saved → {save.relative_to(REPO_ROOT)}")

    print()
    print(f"Iterations: {args.iters}")
    print(f"Timeouts:   {timeouts}")
    print(f"Crashes:    {crashes} unique  ({len(seen_signatures)} dedup'd)")
    print(f"Findings:   {FINDINGS_DIR.relative_to(REPO_ROOT)}/")
    return 0 if crashes == 0 else 2


if __name__ == "__main__":
    sys.exit(main())
