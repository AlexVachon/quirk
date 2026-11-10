#!/usr/bin/env bash
# Native Windows build (Stage 3 of docs/WINDOWS-PORT.md). Run by
# .github/workflows/windows-probe.yml in a conda env (llvmdev 14, clang 14,
# bdw-gc, pthreads-win32, openssl) with MSVC on PATH (msvc-dev-cmd).
#
#   bin/runtime.dll  — the runtime (clang), every external symbol exported
#                      (the JIT finds them by name)
#   bin/quirk.exe    — the compiler (MSVC cl), linked against LLVM 14
#
# Then a smoke test: --version, and a one-line program through the JIT.
# Prints each step's outcome; exits 0 so the job's other steps still run.
set -uo pipefail
cd "$(dirname "$0")/.."
export MSYS2_ARG_CONV_EXCL='*'
OUT=windows-probe
mkdir -p "$OUT" bin obj/win
LOG="$OUT/build.txt"
: > "$LOG"
step() { echo "== $*" | tee -a "$LOG"; }
w() { cygpath -w "$1" 2>/dev/null || echo "$1"; }

LIB="$CONDA_PREFIX/Library"
WLIB="$(w "$LIB")"
LLVM_CONFIG="$LIB/bin/llvm-config"
LLVM_INC="$(w "$("$LLVM_CONFIG" --includedir | tr -d '\r')")"

step "libraries available"
ls "$LIB/lib" | grep -iE '^(gc|libgc|pthread|libssl|libcrypto|ssl|crypto)' | tee -a "$LOG"
GC_LIB=$(ls "$LIB/lib" | grep -iE '^(gc|libgc)\.lib$' | head -1)
PTHREAD_LIB=$(ls "$LIB/lib" | grep -iE '^pthread.*\.lib$' | head -1)
SSL_LIB=$(ls "$LIB/lib" | grep -iE '^libssl\.lib$' | head -1)
CRYPTO_LIB=$(ls "$LIB/lib" | grep -iE '^libcrypto\.lib$' | head -1)
echo "gc=$GC_LIB pthread=$PTHREAD_LIB ssl=$SSL_LIB crypto=$CRYPTO_LIB" | tee -a "$LOG"

# ---------- runtime.dll ----------
step "runtime: compile"
clang -c src/Runtime/runtime.c -O2 -o obj/win/runtime.o -Isrc/Runtime -I"$LIB/include" \
    -D_CRT_SECURE_NO_WARNINGS -DGC_DLL >> "$LOG" 2>&1
# (No GC_WIN32_PTHREADS: conda's gc has no pthread redirects; gc.dll
# registers every thread itself via DllMain, pthreads-win32's included.)
echo "rc=$?" | tee -a "$LOG"

step "runtime: export list"
{
    echo "LIBRARY runtime"
    echo "EXPORTS"
    "$LIB/bin/llvm-nm" --defined-only --extern-only obj/win/runtime.o 2>>"$LOG" \
        | awk '$2 ~ /^[TDBR]$/ {print "    " $3}' | sort -u
} > obj/win/runtime.def
echo "$(($(wc -l < obj/win/runtime.def) - 2)) symbols" | tee -a "$LOG"

step "runtime: link"
clang -shared obj/win/runtime.o -o bin/runtime.dll -Wl,/DEF:"$(w obj/win/runtime.def)" \
    -L"$LIB/lib" ${GC_LIB:+-l${GC_LIB%.lib}} ${PTHREAD_LIB:+-l${PTHREAD_LIB%.lib}} \
    ${SSL_LIB:+-l${SSL_LIB%.lib}} ${CRYPTO_LIB:+-l${CRYPTO_LIB%.lib}} -lws2_32 -ladvapi32 -luser32 >> "$LOG" 2>&1
echo "rc=$?" | tee -a "$LOG"
ls -la bin/runtime.dll 2>&1 | tee -a "$LOG"

# ---------- quirk.exe ----------
CLFLAGS=(/nologo /c /O2 /std:c++17 /EHsc /permissive- /utf-8 /MD /Iinclude /Isrc "/I$LLVM_INC" "/I$WLIB\\include"
         /D__STDC_CONSTANT_MACROS /D__STDC_FORMAT_MACROS /D__STDC_LIMIT_MACROS /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX)
SRCS="$(sed -n 's/^SRCS *= *//p' Makefile) src/platform_win32.cpp"
OBJS=()
ok=1
for f in $SRCS; do
    o="obj/win/$(basename "${f%.cpp}").obj"
    step "compile $f"
    cl.exe "${CLFLAGS[@]}" "$f" "/Fo$(w "$o")" > "$OUT/cl_$(basename "$f").log" 2>&1
    rc=$?
    grep -E ': (fatal )?error' "$OUT/cl_$(basename "$f").log" | head -20 | tee -a "$LOG"
    echo "rc=$rc" | tee -a "$LOG"
    [ $rc -eq 0 ] || ok=0
    OBJS+=("$(w "$o")")
done
step "compile linenoise_win32.c"
cl.exe /nologo /c /O2 /MD /TC src/third_party/linenoise/linenoise_win32.c "/Fo$(w obj/win/linenoise.obj)" >> "$LOG" 2>&1
echo "rc=$?" | tee -a "$LOG"
OBJS+=("$(w obj/win/linenoise.obj)")

if [ $ok -eq 1 ]; then
    step "link quirk.exe"
    LLVM_LIBS=$("$LLVM_CONFIG" --libnames all 2>/dev/null | tr -d '\r')
    SYS_LIBS=$("$LLVM_CONFIG" --system-libs 2>/dev/null | tr -d '\r')
    # Git Bash's /usr/bin/link (coreutils) shadows MSVC's link.exe.
    MSVC_LINK="$(cygpath -u "${VCToolsInstallDir:-}")/bin/Hostx64/x64/link.exe"
    [ -x "$MSVC_LINK" ] || MSVC_LINK="$(where.exe link.exe 2>/dev/null | tr -d '\r' | grep -i 'Microsoft Visual Studio' | head -1)"
    echo "linker: $MSVC_LINK" | tee -a "$LOG"
    "$MSVC_LINK" /nologo /OUT:"$(w bin/quirk.exe)" "${OBJS[@]}" /LIBPATH:"$WLIB\\lib" $LLVM_LIBS $SYS_LIBS \
        ws2_32.lib advapi32.lib shell32.lib ole32.lib uuid.lib psapi.lib ntdll.lib > "$OUT/link.log" 2>&1
    rc=$?
    grep -iE 'error|unresolved' "$OUT/link.log" | head -40 | tee -a "$LOG"
    echo "rc=$rc" | tee -a "$LOG"
    ls -la bin/quirk.exe 2>&1 | tee -a "$LOG"
fi

# ---------- smoke test ----------
if [ -f bin/quirk.exe ] && [ -f bin/runtime.dll ]; then
    export PATH="$PWD/bin:$LIB/bin:$PATH"
    export QUIRK_HOME="$(w "$PWD")"
    step "quirk --version"
    timeout 60 ./bin/quirk.exe --version 2>&1 | head -5 | tee -a "$LOG"
    step "hello world (JIT)"
    printf 'print("Hello from Quirk on Windows", 1 + 2)\n' > obj/win/hello.qk
    timeout 120 ./bin/quirk.exe --no-aot --no-cache obj/win/hello.qk 2>&1 | head -30 | tee -a "$LOG"
    echo "rc=${PIPESTATUS[0]}" | tee -a "$LOG"
fi
echo "== done" | tee -a "$LOG"
exit 0
