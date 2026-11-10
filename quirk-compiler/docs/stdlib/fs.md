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

#### `extern define remove_tree_raw(path: String) -> Int`

Delete `path` and everything under it, like `rm -rf`. A file is simply
deleted; symlinks are removed themselves, never followed.
@throws IOError if `path` doesn't exist or anything in it can't be removed.
@example
fs.remove_tree("build/tmp")

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

#### `extern define is_link(path: String) -> Bool`

True if `path` is itself a symbolic link (whatever it points to).

#### `define absolute(path: String) -> String`

The absolute, canonical form of `path`: symlinks, `.` and `..` resolved.
@throws FileNotFoundError if `path` doesn't exist.
@example fs.absolute("../notes.txt")   // "/home/me/notes.txt"

#### `define walk(root: String, dirs: Bool = false, files: Bool = true, hidden: Bool = false, follow_links: Bool = false, max_depth: Int = -1) -> List`

Every path under directory `root` (not `root` itself), depth first and
sorted by name within each directory, as `root/...` strings.
@param dirs         also list directories (each before its contents)
@param files        list files (and other non-directories)
@param hidden       include names starting with `.` (and descend into them)
@param follow_links descend into symlinked directories
@param max_depth    how deep to go: 0 = only `root`'s own entries, -1 = all
@example
for path in fs.walk("src") {
    if fs.extname(path) == ".qk" { print(path) }
}

#### `define matches(pattern: String, name: String, hidden: Bool = false) -> Bool`

True if file name `name` matches the shell-style `pattern`: `*` (any
run of characters), `?` (one character), `[abc]` / `[a-z]` / `[!x]`
(one character from a set). A leading `.` must be matched explicitly
unless `hidden` is true. Matches a single name, not a path — see `glob`.
@example fs.matches("*.qk", "main.qk")    // true

#### `define glob(pattern: String, hidden: Bool = false) -> List`

Paths matching `pattern`, sorted. Each `/`-separated part of the pattern
is matched against one level of the tree (see `matches`); a part that is
exactly `**` matches any number of directories, including none. Names
starting with `.` only match when the pattern part starts with `.`, or
with `hidden=true`. Relative patterns give paths relative to the
current directory.
@example fs.glob("*.qk")              // ["main.qk", "util.qk"]
@example fs.glob("src/**/*_test.qk")  // test files at any depth under src

#### `extern define temp_root() -> String`

The directory temporary files go in: `$TMPDIR`, else `/tmp`.

#### `define temp_dir(prefix: String = "quirk-", dir: String = "") -> String`

Create a new, empty, private (owner-only) directory with a unique name
like `/tmp/quirk-a8Xk2p` and return its path. It's yours to delete
(`fs.remove_tree`).
@param prefix start of the directory name
@param dir    where to create it ("" = `fs.temp_root()`)
@throws IOError if it can't be created.
@example
work := fs.temp_dir(prefix="build-")
// ...
fs.remove_tree(work)

#### `define temp_file(prefix: String = "quirk-", suffix: String = "", dir: String = "") -> String`

Create a new, empty, private (owner-only) file with a unique name like
`/tmp/quirk-a8Xk2p.json` and return its path. It's yours to delete
(`fs.remove`).
@param prefix start of the file name
@param suffix end of the file name (e.g. ".json")
@param dir    where to create it ("" = `fs.temp_root()`)
@throws IOError if it can't be created.

#### `define copy(src: String, dst: String) -> String`

Copy file `src` to `dst`, keeping its permissions. If `dst` is an
existing directory, the copy goes inside it under the same name. An
existing file at the destination is replaced. Returns the path written.
@throws FileNotFoundError if `src` doesn't exist; IOError otherwise.
@example fs.copy("config.toml", "backup/")   // "backup/config.toml"

#### `define copy_tree(src: String, dst: String) -> void`

Copy directory `src` and everything in it (hidden files included) to
`dst`, which must not exist yet. Symlinks are copied as the files they
point to.
@throws FileNotFoundError if `src` isn't a directory; IOError if `dst`
        exists or anything can't be copied.
@example fs.copy_tree("assets", "dist/assets")

#### `define read_text(path: String) -> String`

The whole contents of file `path`.
@throws FileNotFoundError if it doesn't exist; IOError if it can't be read.
@example config := fs.read_text("config.toml")

#### `define write_text(path: String, text: String) -> void`

Write `text` to file `path`, replacing what was there (creating it if
needed). @throws IOError on failure.

#### `define append_text(path: String, text: String) -> void`

Append `text` to file `path`, creating it if needed. @throws IOError on failure.
