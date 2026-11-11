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

CLIB="$CONDA_PREFIX/Library"   # not LIB: MSVC's linker reads $LIB for the SDK libraries
WLIB="$(w "$CLIB")"
LLVM_CONFIG="$CLIB/bin/llvm-config"
LLVM_INC="$(w "$("$LLVM_CONFIG" --includedir | tr -d '\r')")"

step "libraries available"
ls "$CLIB/lib" | grep -iE '^(gc|libgc|pthread|libssl|libcrypto|ssl|crypto)' | tee -a "$LOG"
GC_LIB=$(ls "$CLIB/lib" | grep -iE '^(gc|libgc)\.lib$' | head -1)
PTHREAD_LIB=$(ls "$CLIB/lib" | grep -iE '^pthread.*\.lib$' | head -1)
SSL_LIB=$(ls "$CLIB/lib" | grep -iE '^libssl\.lib$' | head -1)
CRYPTO_LIB=$(ls "$CLIB/lib" | grep -iE '^libcrypto\.lib$' | head -1)
echo "gc=$GC_LIB pthread=$PTHREAD_LIB ssl=$SSL_LIB crypto=$CRYPTO_LIB" | tee -a "$LOG"

# ---------- runtime.dll ----------
step "runtime: compile"
# The shared CRT (msvcrt.lib -> ucrtbase.dll), like quirk.exe's /MD:
# one heap and one stdout between the two.
clang -c src/Runtime/runtime.c -O2 -o obj/win/runtime.o -Isrc/Runtime -Isrc/third_party/musl_regex -I"$CLIB/include" \
    -D_CRT_SECURE_NO_WARNINGS -DGC_DLL -D_MT -D_DLL -Xclang --dependent-lib=msvcrt >> "$LOG" 2>&1
# (No GC_WIN32_PTHREADS: conda's gc has no pthread redirects; gc.dll
# registers every thread itself via DllMain, pthreads-win32's included.)
echo "rc=$?" | tee -a "$LOG"

step "runtime: musl regex"
REGEX_OBJS=()
for f in regcomp regexec regerror tre-mem; do
    clang -c "src/third_party/musl_regex/$f.c" -O2 -o "obj/win/rx_$f.o" -Isrc/third_party/musl_regex \
        -D_CRT_SECURE_NO_WARNINGS -D_MT -D_DLL -Xclang --dependent-lib=msvcrt -w >> "$LOG" 2>&1
    echo "$f rc=$?" | tee -a "$LOG"
    REGEX_OBJS+=("obj/win/rx_$f.o")
done

step "runtime: export list"
{
    echo "LIBRARY runtime"
    echo "EXPORTS"
    "$CLIB/bin/llvm-nm" --defined-only --extern-only obj/win/runtime.o 2>>"$LOG" \
        | awk '$2 ~ /^[TDBR]$/ {print "    " $3}' | sort -u
} > obj/win/runtime.def
echo "$(($(wc -l < obj/win/runtime.def) - 2)) symbols" | tee -a "$LOG"

step "runtime: link"
clang -shared obj/win/runtime.o "${REGEX_OBJS[@]}" -o bin/runtime.dll -Wl,/DEF:"$(w obj/win/runtime.def)" -Wl,/NODEFAULTLIB:libcmt.lib \
    -L"$CLIB/lib" ${GC_LIB:+-l${GC_LIB%.lib}} ${PTHREAD_LIB:+-l${PTHREAD_LIB%.lib}} \
    ${SSL_LIB:+-l${SSL_LIB%.lib}} ${CRYPTO_LIB:+-l${CRYPTO_LIB%.lib}} -lws2_32 -ladvapi32 -luser32 >> "$LOG" 2>&1
echo "rc=$?" | tee -a "$LOG"
ls -la bin/runtime.dll bin/runtime.lib 2>&1 | tee -a "$LOG"

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
    export PATH="$PWD/bin:$CLIB/bin:$PATH"
    export QUIRK_HOME="$(w "$PWD")"
    step "quirk --version"
    timeout 60 ./bin/quirk.exe --version 2>&1 | head -5 | tee -a "$LOG"
    smoke() {   # smoke <name> <program>
        step "smoke: $1"
        printf '%s\n' "$2" > "obj/win/$1.qk"
        timeout 120 ./bin/quirk.exe --no-aot --no-cache "obj/win/$1.qk" 2>&1 | head -30 | tee -a "$LOG"
        echo "rc=${PIPESTATUS[0]}" | tee -a "$LOG"
    }
    smoke hello 'print("Hello from Quirk on Windows", 1 + 2)'
    smoke strings 'name := "Quirk"
print("${name} has ${name.length()} letters", name.upper(), [1, 2, 3].map(fn(x) => x * 2))'
    smoke structs 'struct P {
    x: Int
    define __init(self, x: Int) -> void { self.x = x }
    define __str(self) -> String { return "P(" + self.x.str() + ")" }
}
m := {"a": P(1)}
print(m.get("a"), [P(2), P(3)])'
    smoke exceptions 'try {
    throw ValueError("boom")
} catch (e: ValueError) {
    print("caught:", e.message)
}
print("after")'
    smoke files 'use fs
d := fs.temp_dir()
p := fs.join(d, "a.txt")
fs.write_text(p, "hi there")
fs.mkdir(fs.join(d, "sub"))
fs.write_text(fs.join(d, "sub/b.qk"), "x")
print(fs.read_text(p), fs.size(p), fs.walk(d).length(), fs.glob(d + "/**/*.qk").length())
fs.remove_tree(d)
print(fs.exists(d))'
    smoke gc 's := ""
i := 0
while i < 200000 {
    s = "x" + i.str()
    i = i + 1
}
print("gc ok", s)'
    smoke task_one 'async define one() -> Int { return 1 }
print(await one())'
    smoke task_sleep 'use sys
async define nap() -> Int {
    sys.sleep(10)
    return 2
}
t := nap()
print(await t)'
    smoke tasks 'async define sq(n: Int) -> Int { return n * n }
a := sq(3)
b := sq(4)
print(await a + await b)'
    smoke task_throw 'async define bad() -> Int { throw ValueError("in task") }
try {
    print(await bad())
} catch (e: ValueError) {
    print("task error:", e.message)
}'
    smoke matches 'use fs
print(fs.matches("*.qk", "main.qk"), fs.matches("?ain.[a-z]k", "main.qk"), fs.matches("[!a]*", "main.qk"), fs.matches("*", ".env"), fs.matches("*.qk", "main.py"))'
    smoke repl_eval 'print(1, 2, 3, sep="-")'
    step "repl: piped input + history file"
    printf 'x := 40 + 2\nprint(x)\n' | timeout 60 ./bin/quirk.exe repl 2>&1 | tail -2 | tee -a "$LOG"
    tail -2 "$HOME/.quirk/repl_history" 2>&1 | tee -a "$LOG"
    step "quirk eval"
    timeout 60 ./bin/quirk.exe eval 'print("eval works")' 2>&1 | head -5 | tee -a "$LOG"

    # The package manager: a project, a dependency, its tests.
    step "package manager"
    PM=obj/win/pmtest
    rm -rf "$PM"; mkdir -p "$PM"
    ( cd "$PM" && timeout 120 ../../../bin/quirk.exe new demo 2>&1 | tail -3 ) | tee -a "$LOG"
    if [ -d "$PM/demo" ]; then
        ( cd "$PM/demo" && timeout 300 ../../../../bin/quirk.exe install slug 2>&1 | tail -4 ) | tee -a "$LOG"
        printf 'use slug\nprint(slug.slug("Hello Windows World"))\n' > "$PM/demo/use_slug.qk"
        ( cd "$PM/demo" && timeout 120 ../../../../bin/quirk.exe use_slug.qk 2>&1 | tail -8 ) | tee -a "$LOG"
        ( cd "$PM/demo" && timeout 300 ../../../../bin/quirk.exe test 2>&1 | tail -4 ) | tee -a "$LOG"
        ( cd "$PM/demo" && timeout 60 ../../../../bin/quirk.exe deps 2>&1 | tail -3 ) | tee -a "$LOG"
    fi
    timeout 120 ./bin/quirk.exe show slug --no-readme 2>&1 | head -4 | tee -a "$LOG"
    timeout 120 ./bin/quirk.exe search csv 2>&1 | head -3 | tee -a "$LOG"

    # -o: IR -> COFF object in-process, then lld-link / link.exe.
    step "aot: -o"
    cat > obj/win/aot.qk <<'QK'
use fs
struct P { x: Int }
try { throw ValueError("boom") } catch (e: ValueError) { print("caught", e.message) }
print("hi", [1, 2, 3].map(fn(x) => x * 2), P(3).x, fs.exists("."))
QK
    for prog in hello aot; do
        timeout 180 ./bin/quirk.exe -o "obj/win/$prog.exe" "obj/win/$prog.qk" 2>&1 | tail -8 | tee -a "$LOG"
        if [ -f "obj/win/$prog.exe" ]; then
            timeout 60 "./obj/win/$prog.exe" 2>&1 | head -10 | tee -a "$LOG"
            echo "rc=${PIPESTATUS[0]}" | tee -a "$LOG"
        else
            echo "no obj/win/$prog.exe" | tee -a "$LOG"
        fi
    done

    # venvs: create one, activate it from cmd.exe and PowerShell, run in it.
    step "venv"
    rm -rf obj/win/venv
    timeout 120 ./bin/quirk.exe venv new obj/win/venv 2>&1 | tail -3 | tee -a "$LOG"
    ls obj/win/venv/bin 2>&1 | tr '\n' ' ' | tee -a "$LOG"; echo | tee -a "$LOG"
    printf '@echo off\r\ncall obj\\win\\venv\\bin\\activate.bat\r\necho cmd QUIRK_HOME=%%QUIRK_HOME%%\r\nwhere quirk\r\nquirk obj\\win\\hello.qk\r\ncall deactivate\r\necho cmd after=%%QUIRK_HOME%%\r\n' > obj/win/venvtest.bat
    timeout 120 cmd /c "obj\\win\\venvtest.bat" < /dev/null 2>&1 | head -10 | tee -a "$LOG"
    printf '. obj\\win\\venv\\bin\\Activate.ps1\r\n"ps QUIRK_HOME=$env:QUIRK_HOME"\r\n(Get-Command quirk).Source\r\nquirk obj\\win\\hello.qk\r\ndeactivate\r\n"ps after=$env:QUIRK_HOME"\r\n' > obj/win/venvtest.ps1
    timeout 120 powershell -NoProfile -ExecutionPolicy Bypass -File obj/win/venvtest.ps1 2>&1 | head -10 | tee -a "$LOG"

    # The AOT build cache: `--build` once, then `quirk file` runs the
    # cached .exe (argv, exit code passed through).
    step "aot: build cache"
    printf 'use sys\nprint("cached run, args:", sys.argv())\nsys.exit(3)\n' > obj/win/cached.qk
    timeout 180 ./bin/quirk.exe --build obj/win/cached.qk 2>&1 | tail -3 | tee -a "$LOG"
    timeout 60 ./bin/quirk.exe obj/win/cached.qk one two 2>&1 | head -5 | tee -a "$LOG"
    echo "rc=${PIPESTATUS[0]} (expect 3)" | tee -a "$LOG"

    # The real test suite (as CI runs it on Linux), file by file.
    if [ "${QUIRK_WIN_SKIP_TESTS:-0}" = "1" ]; then
        step "test suite skipped (QUIRK_WIN_SKIP_TESTS=1)"
    else
    step "test suite"
    timeout 1500 ./bin/quirk.exe test tests --timeout 60 --exclude server_test --exclude custom_package_test \
        --exclude http_client_test --exclude test_test > "$OUT/tests.txt" 2>&1
    echo "rc=$?" | tee -a "$LOG"
    grep -E '^  (✓|✗) |^Summary|files:|tests:' "$OUT/tests.txt" | tee -a "$LOG"
    fi
fi
echo "== done" | tee -a "$LOG"
exit 0
