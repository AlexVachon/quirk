# `fs` — API reference

> Generated from in-source `---` docstrings by
> [`tools/gen_stdlib_docs.py`](../../tools/gen_stdlib_docs.py).
> Do not hand-edit — re-run `make docs` to refresh.


## `fs/index.quirk`


### Module-level functions

#### `extern define exists(path: String) -> Bool`

True if anything (file, directory, symlink) exists at `path`.

#### `extern define is_file(path: String) -> Bool`

True only if `path` is a regular file.

#### `extern define is_dir(path: String) -> Bool`

True only if `path` is a directory.

#### `extern define size(path: String) -> Int`

File size in bytes. Returns -1 if `path` doesn't exist or stat fails.

#### `extern define mtime(path: String) -> Int`

Last-modified time as Unix epoch seconds. -1 on failure.

#### `extern define mkdir_raw(path: String, parents: Int) -> Int`

Create the directory at `path`. Returns 0 on success, -1 on failure.
@param parents If true, also creates any missing intermediate directories
              (like `mkdir -p`) and treats "already exists" as success.

#### `define mkdir(path: String, parents: Bool = false) -> void`

Create a directory.
@param parents If true, intermediate directories are also created and an
              existing directory is not an error (`mkdir -p` semantics).
@throws IOError on failure.

#### `extern define rmdir_raw(path: String) -> Int`

Remove an empty directory. @throws IOError on failure (incl. non-empty dir).

#### `extern define remove_raw(path: String) -> Int`

Delete a file. @throws IOError on failure.

#### `extern define rename_raw(src: String, dst: String) -> Int`

Rename / move from one path to another. Atomic on the same filesystem.
@throws IOError on failure.

#### `extern define list_dir(path: String) -> List`

List the immediate entries of directory `path` as a List<String>.
Excludes `.` and `..`. Returns an empty List if the path doesn't exist
or isn't a directory.
@example
for name in fs.list_dir(".") { print(name) }

#### `extern define cwd() -> String`

Current working directory as a String.

#### `extern define chdir_raw(path: String) -> Int`

Change current working directory. @throws IOError on failure.

#### `define join(a: String, b: String) -> String`

Join two path components with `/`. Avoids double-slashes when `a` already
ends with one. (Forward slashes work on Windows in modern paths.)
@example fs.join("/etc", "hosts")  // "/etc/hosts"

#### `define basename(path: String) -> String`

Final component of `path` — strips any trailing slash and returns the
last segment. `"a/b/c.txt"` → `"c.txt"`; `"/etc/"` → `"etc"`.

#### `define dirname(path: String) -> String`

Everything before the final path component. `"a/b/c.txt"` → `"a/b"`.
Returns "" if the path has no separators.

#### `define extname(path: String) -> String`

File extension including the leading dot (`".txt"`), or "" when there
is none. A leading dot in the basename (e.g. `.gitignore`) is NOT an
extension — it's the filename.

#### `define split_ext(path: String) -> Tuple`

Splits `path` into `(stem, ext)`.
`"foo.tar.gz"` → `("foo.tar", ".gz")`. `"README"` → `("README", "")`.
