# Crash / ICE / wrong-output probes

Each `p<NN>_*.quirk` file is a minimal program that *used* to crash,
ICE, or silently produce wrong output before the v2.2.11 bugproofing
pass. They stay here as permanent regression tests.

Run them all with:

```
for p in tests/probes/p*.quirk; do
    quirk --no-aot --no-cache "$p" || echo "REGRESSED: $p"
done
```

A probe is "passing" when it produces one of:
- correct output (e.g. `p13_recursion_deep` prints `stack overflow`);
- a clean Quirk exception (`ValueError`, `TypeError`, `IndexError`,
  `ZeroDivisionError`, `RuntimeError`);
- a clean Sema rejection at compile time.

A probe is **failing** when it produces:
- `SIGSEGV` / `core dumped` / `Segmentation fault`;
- `Internal compiler error: malformed IR`;
- an LLVM assertion (e.g. `ICmpInst::AssertOK`);
- silent wrong-typed output (e.g. `10 / 0` returning `-1736491003`).

If you regress one of these, the failure mode is the documented bug
class it was protecting against — see the comment at the top of each
probe.
