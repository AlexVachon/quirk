#!/bin/sh
# bin/quirk — v5.0.0 selfhost-driven compiler driver.
#
# Compile + JIT-execute a Quirk source file via the selfhost
# compiler (bin/quirk-selfhost emits LLVM IR; llc-14 lowers to
# x86_64 assembly; clang-14 -no-pie links against the shared
# runtime; exec runs the result). Behaves enough like the old
# bin/quirk (now bin/quirk-cpp) to drive day-to-day compile +
# execute workflows.
#
# Falls back to the C++ binary for package manager subcommands
# (`quirk install`, `quirk new`, `quirk pkg …`) and any flag
# combination this wrapper doesn't recognise — selfhost has no
# native package manager yet.
#
# Flag handling:
#   --no-aot, --no-cache         no-ops (selfhost has no AOT cache)
#   -o <file>, <src> <out.ll>    emit IR to file (skip execute)
#   --ir, -S                     emit IR to stdout (skip execute)
#   --cpp                        delegate everything to bin/quirk-cpp
#   anything else                forwarded to the program's argv

set -e

DIR=$(cd "$(dirname "$0")" && pwd)
SELFHOST="$DIR/quirk-selfhost"
RUNTIME="$DIR/runtime.so"
CPP="$DIR/quirk-cpp"

# Subcommand routing: anything that doesn't look like a path to a
# .quirk source falls through to the C++ binary so `quirk install`,
# `quirk new`, etc. keep working.
case "${1:-}" in
    pkg|install|uninstall|new|list|packages|run|init|update|publish|fmt|repl)
        exec "$CPP" "$@"
        ;;
    --cpp)
        shift
        exec "$CPP" "$@"
        ;;
esac

# Parse flags. Track the source file separately from forwarded
# program args.
src=""
out=""
emit_ir=0
prog_args=""
saw_double_dash=0

while [ $# -gt 0 ]; do
    if [ $saw_double_dash -eq 1 ]; then
        prog_args="$prog_args \"$1\""
        shift
        continue
    fi
    case "$1" in
        --no-aot|--no-cache|-v|--verbose|--quiet)
            shift ;;
        --ir|-S)
            emit_ir=1
            shift ;;
        -o)
            out="$2"
            shift 2 ;;
        --)
            saw_double_dash=1
            shift ;;
        -*)
            # Unknown flag — pass through to bin/quirk-cpp.
            exec "$CPP" "$@" ;;
        *)
            if [ -z "$src" ]; then
                src="$1"
                shift
            elif [ -z "$out" ] && [ "${1##*.}" = "ll" ]; then
                # Legacy `quirk source.quirk out.ll` shape — selfhost
                # binary already understands it.
                out="$1"
                shift
            else
                prog_args="$prog_args \"$1\""
                shift
            fi ;;
    esac
done

if [ -z "$src" ]; then
    echo "usage: quirk <source.quirk> [-o <out.ll> | --ir] [-- prog-args...]" >&2
    echo "       quirk --cpp <c++-compiler args>" >&2
    exit 1
fi

# IR-emit modes — skip llc/clang/exec.
if [ $emit_ir -eq 1 ]; then
    exec "$SELFHOST" "$src"
fi
if [ -n "$out" ]; then
    exec "$SELFHOST" "$src" "$out"
fi

# Compile + execute: IR → assembly → ELF → exec.
TMP=$(mktemp -d -t quirk-XXXXXX)
LL=$TMP/p.ll
S=$TMP/p.s
BIN=$TMP/p

"$SELFHOST" "$src" "$LL"
if [ ! -s "$LL" ]; then
    echo "quirk: $src — selfhost emitted no IR" >&2
    rm -rf "$TMP"
    exit 1
fi
llc-14 "$LL" -o "$S"
clang-14 -no-pie "$S" "$RUNTIME" -lm -o "$BIN"

# eval forwards parsed-out program args while preserving quoting.
eval exec "\"$BIN\"" $prog_args
