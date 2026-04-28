```
quirk [options] <file.qk>
```

| Command | Description |
|---|---|
| `quirk file.qk` | Compile and run (default) |
| `quirk --compile-only file.qk` | Compile only, no execution |
| `quirk -v file.qk` | Compile and run with verbose debug output |
| `quirk --emit-ir file.qk` | Write LLVM IR to `file.ll` next to source |
| `quirk --emit-ast file.qk` | Write AST dump to `file.ast.log` next to source |

**Combinations:**
```bash
quirk -v file.qk                        # run with debug output
quirk --compile-only --emit-ir file.qk  # compile and keep the .ll, no run
quirk --emit-ir --emit-ast file.qk      # dump both, then run
quirk -v --emit-ir file.qk              # run with debug and keep IR
```