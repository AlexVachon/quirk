#!/usr/bin/env bash
# Windows feasibility probe (run by .github/workflows/windows-probe.yml in
# a conda env with llvmdev 14, clang 14, bdw-gc, openssl). Compiles every
# translation unit on its own — compiler sources with clang++ against
# LLVM 14, the runtime unity build with clang — and records the errors
# per file instead of stopping at the first one. Output in windows-probe/.
set -uo pipefail
cd "$(dirname "$0")/.."
OUT=windows-probe
mkdir -p "$OUT"
: > "$OUT/summary.txt"

PREFIX="${CONDA_PREFIX:-}"
# conda on Windows puts tools and headers under Library/.
LIB="$PREFIX/Library"
LLVM_CONFIG="$(command -v llvm-config || echo "$LIB/bin/llvm-config.exe")"
echo "llvm-config: $LLVM_CONFIG ($("$LLVM_CONFIG" --version 2>&1))" | tee -a "$OUT/summary.txt"
CXX="$(command -v clang++ || echo "$LIB/bin/clang++.exe")"
CC="$(command -v clang || echo "$LIB/bin/clang.exe")"
echo "clang++: $CXX ($("$CXX" --version 2>&1 | head -1))" | tee -a "$OUT/summary.txt"
# llvm-config prints MSVC-style flags on Windows (-std:c++14, /EHs-c-),
# which the clang driver rejects: take only the include dir.
LLVM_INC="$("$LLVM_CONFIG" --includedir 2>/dev/null | tr -d '\r')"
LLVM_CXXFLAGS="-I$LLVM_INC -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS -fno-exceptions"
export MSYS2_ARG_CONV_EXCL='*'   # keep Git Bash from rewriting flags into paths
INC="-Iinclude -I$LIB/include"

total_err=0
probe() {   # probe <compiler> <file> <flags...>
    local comp="$1" file="$2"; shift 2
    local log="$OUT/$(echo "$file" | tr '/' '_').log"
    # -fsyntax-only: every compile error, no output file (Windows clang
    # can't write to /dev/null).
    "$comp" "$@" -fsyntax-only "$file" -ferror-limit=0 > "$log" 2>&1
    local rc=$?
    local errs warns
    errs=$(grep -cE '(^| )(fatal )?error: ' "$log" || true)
    warns=$(grep -c ' warning: ' "$log" || true)
    total_err=$((total_err + errs))
    printf '%-44s rc=%-3s errors=%-5s warnings=%s\n' "$file" "$rc" "$errs" "$warns" | tee -a "$OUT/summary.txt"
}

echo "== compiler (C++17, LLVM 14)" | tee -a "$OUT/summary.txt"
for f in src/*.cpp src/Core/*.cpp src/Backend/*.cpp; do
    probe "$CXX" "$f" -std=c++17 -O0 $INC $LLVM_CXXFLAGS -D_CRT_SECURE_NO_WARNINGS
done

echo "== runtime (C, unity build)" | tee -a "$OUT/summary.txt"
probe "$CC" src/Runtime/runtime.c -O0 -Isrc/Runtime -I"$LIB/include" -D_CRT_SECURE_NO_WARNINGS

echo "== third party" | tee -a "$OUT/summary.txt"
[ -f src/third_party/linenoise/linenoise.c ] && probe "$CC" src/third_party/linenoise/linenoise.c -O0

echo "TOTAL errors: $total_err" | tee -a "$OUT/summary.txt"

# The most common error messages across all files.
echo "== most common errors" >> "$OUT/summary.txt"
cat "$OUT"/*.log | grep -E '(^| )(fatal )?error: ' | sed -E 's/.*error: //' | sed -E "s/'[^']*'/'X'/g" \
    | sort | uniq -c | sort -rn | head -40 >> "$OUT/summary.txt"
cat "$OUT/summary.txt"
exit 0
