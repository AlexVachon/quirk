# musl regex (TRE)

POSIX `regcomp` / `regexec` / `regerror` / `regfree` for the **native
Windows build**, which has no `<regex.h>` (docs/WINDOWS-PORT.md). Linux and
macOS use the system's regex; on Windows `src/Runtime/libs/regex.c`
compiles these files, so `regex` patterns behave the same everywhere
(POSIX ERE).

From musl libc **v1.2.5** (https://git.musl-libc.org/cgit/musl, tag
`v1.2.5`), unmodified: `regcomp.c`, `regexec.c`, `regerror.c`,
`tre-mem.c`, `tre.h` (`SHA256SUMS`). Added here:

- `regex.h` — musl's `include/regex.h` with its types declared inline
  instead of via musl's internal `<bits/alltypes.h>`; defines musl's
  `hidden` visibility marker as nothing.
- `locale_impl.h` — `LCTRANS_CUR(s)` as `s` (untranslated messages).

Licenses: the TRE code is Copyright © 2001-2009 Ville Laurikari, 2-clause
BSD (`LICENSE-TRE.txt`, also at the top of each file), modified by Rich
Felker for musl; musl is MIT (`LICENSE-musl.txt`).
