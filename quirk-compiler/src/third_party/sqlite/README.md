# SQLite 3.54.1 (amalgamation)

`sqlite3.c` and `sqlite3.h` from `sqlite-amalgamation-3540100.zip`,
https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip
(2990966 bytes, SHA3-256 `92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30`,
as listed on https://www.sqlite.org/download.html). Unmodified.

SQLite is in the public domain: https://www.sqlite.org/copyright.html

Built as its own object and linked into the runtime (Makefile `SQLITE_OBJ`,
`tools/windows_build.sh`), not included in the unity build: it's 9 MB of C.
Used by `src/Runtime/libs/sqlite.c` (the `sqlite` package).

To update: download the new amalgamation, check its SHA3-256 against the
download page, replace the two files, and update this file.
