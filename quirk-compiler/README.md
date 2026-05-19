```
quirk [options] <file.quirk>
```

| Command | Description |
|---|---|
| `quirk file.quirk` | Compile and run (default) |
| `quirk --compile-only file.quirk` | Compile only, no execution |
| `quirk -v file.quirk` | Compile and run with verbose debug output |
| `quirk --emit-ir file.quirk` | Write LLVM IR to `file.ll` next to source |
| `quirk --emit-ast file.quirk` | Write AST dump to `file.ast.log` next to source |

**Combinations:**
```bash
quirk -v file.quirk                        # run with debug output
quirk --compile-only --emit-ir file.quirk  # compile and keep the .ll, no run
quirk --emit-ir --emit-ast file.quirk      # dump both, then run
quirk -v --emit-ir file.quirk              # run with debug and keep IR
```