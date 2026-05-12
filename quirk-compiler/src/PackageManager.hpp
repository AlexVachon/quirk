#pragma once
// ---------------------------------------------------------------------------
//  Quirk's built-in package manager — surface area of the `quirk` binary
//  beyond running scripts.
//
//  Subcommands:
//    quirk install [-r <file>] [pkg ...]    install one+ packages (or all from manifest)
//    quirk upgrade [pkg ...]                bump to latest matching versions
//    quirk remove <pkg>                     delete a package
//    quirk list / packages                  list installed packages with versions
//    quirk show <pkg>                       show details for one installed package
//    quirk init                             scaffold a quirk.toml in cwd
//    quirk version / --version              print compiler version
//
//  Storage:
//    ./quirk.toml      project manifest (declared deps)
//    ./quirk.lock      lockfile (pinned commits)
//    ./packages/<name>/  cloned repo per dependency
//
//  Resolution backend: plain `git clone`. A package spec is
//    github.com/user/repo[@ref]
//  where `ref` is a tag, branch, or commit SHA. The package's name is
//  the repo basename unless overridden in its manifest.
//
//  This is intentionally bare-bones — no central registry, no semver
//  solver. Designed to be replaceable with a registry-backed resolver
//  later without changing the CLI shape.
// ---------------------------------------------------------------------------

#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <cstdlib>
#include <system_error>
#include <unistd.h>
#include <sys/wait.h>

namespace qpm {

constexpr const char* QUIRK_VERSION = "0.2.0";

namespace fs = std::filesystem;

// ---------------------- Tiny TOML-ish manifest parser ----------------------
// Supports:
//   name = "pretty"
//   version = "0.1.0"
//   [deps]
//   pretty = "github.com/foo/pretty@v0.1.0"
// Comments start with #. No nested tables beyond [deps]. Plenty for v1.

struct Manifest {
    std::string name;
    std::string version = "0.0.0";
    std::string description;
    std::string author;
    std::string license;
    // deps[name] = "github.com/foo/bar@v0.1.0"
    std::vector<std::pair<std::string, std::string>> deps;
};

static std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, b - a + 1);
}

static std::string unquote(std::string s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return s.substr(1, s.size() - 2);
    return s;
}

static bool read_manifest(const std::string& path, Manifest& out) {
    std::ifstream in(path);
    if (!in.is_open()) return false;
    std::string line;
    std::string section;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        if (t.front() == '[' && t.back() == ']') {
            section = t.substr(1, t.size() - 2);
            continue;
        }
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(t.substr(0, eq));
        std::string val = unquote(trim(t.substr(eq + 1)));
        if (section.empty()) {
            if      (key == "name")        out.name = val;
            else if (key == "version")     out.version = val;
            else if (key == "description") out.description = val;
            else if (key == "author")      out.author = val;
            else if (key == "license")     out.license = val;
        } else if (section == "deps") {
            out.deps.emplace_back(key, val);
        }
    }
    return true;
}

static void write_manifest(const std::string& path, const Manifest& m) {
    std::ofstream out(path);
    out << "name        = \"" << m.name << "\"\n";
    out << "version     = \"" << m.version << "\"\n";
    if (!m.description.empty()) out << "description = \"" << m.description << "\"\n";
    if (!m.author.empty())      out << "author      = \"" << m.author << "\"\n";
    if (!m.license.empty())     out << "license     = \"" << m.license << "\"\n";
    out << "\n[deps]\n";
    for (auto& d : m.deps) out << d.first << " = \"" << d.second << "\"\n";
}

// Parse a package spec like "github.com/foo/bar@v0.1.0" into (url, ref, name).
struct PkgSpec {
    std::string url;       // "https://github.com/foo/bar.git"
    std::string ref;       // "v0.1.0" — may be empty
    std::string name;      // "bar" — derived from the URL's basename
    std::string original;  // raw spec
};

static PkgSpec parse_spec(const std::string& raw) {
    PkgSpec s; s.original = raw;
    std::string body = raw;
    size_t at = body.find('@');
    if (at != std::string::npos) {
        s.ref = body.substr(at + 1);
        body = body.substr(0, at);
    }
    // Accept short github.com/... form OR a full URL.
    if (body.find("://") == std::string::npos) {
        s.url = "https://" + body + ".git";
    } else {
        s.url = body;
        if (s.url.size() < 4 || s.url.substr(s.url.size() - 4) != ".git") s.url += ".git";
    }
    // Name is the last segment of the URL minus .git.
    std::string tmp = body;
    auto slash = tmp.find_last_of('/');
    s.name = slash == std::string::npos ? tmp : tmp.substr(slash + 1);
    if (s.name.size() > 4 && s.name.substr(s.name.size() - 4) == ".git")
        s.name = s.name.substr(0, s.name.size() - 4);
    return s;
}

// Run a shell command, return exit status. Echoes the command at -v level.
static int sh(const std::string& cmd, bool quiet = false) {
    if (!quiet) std::cerr << "$ " << cmd << "\n";
    return std::system(cmd.c_str());
}

// ---------------------- Semver-lite parsing + matching --------------------
// Parse "1.2.3" / "1.2" / "1" into a 3-element vector, missing components 0.
// Anything after a `-` or `+` is treated as pre-release/build metadata and
// dropped — comparisons consider only MAJOR.MINOR.PATCH.
static std::vector<int> parse_version(const std::string& s) {
    std::vector<int> out{0, 0, 0};
    std::string core = s;
    for (char trim : {'-', '+'}) {
        auto p = core.find(trim);
        if (p != std::string::npos) core = core.substr(0, p);
    }
    size_t i = 0;
    for (int slot = 0; slot < 3 && i < core.size(); slot++) {
        int v = 0;
        while (i < core.size() && std::isdigit((unsigned char)core[i])) {
            v = v * 10 + (core[i] - '0');
            i++;
        }
        out[slot] = v;
        if (i < core.size() && core[i] == '.') i++;
    }
    return out;
}

// -1/0/+1 in the spirit of strcmp.
static int compare_versions(const std::string& a, const std::string& b) {
    auto va = parse_version(a), vb = parse_version(b);
    for (int i = 0; i < 3; i++) {
        if (va[i] != vb[i]) return va[i] < vb[i] ? -1 : 1;
    }
    return 0;
}

// Match a version against a single clause like ">=1.0", "<2.0", "=1.5", "1.5".
// A bare version (no operator) is treated as `==`.
static bool match_clause(const std::string& version, const std::string& clauseRaw) {
    std::string clause = clauseRaw;
    // strip whitespace
    auto trimSp = [](std::string& s) {
        while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
        while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    };
    trimSp(clause);
    if (clause.empty()) return true;
    std::string op = "=";
    std::string val = clause;
    auto consume = [&](const std::string& tok) -> bool {
        if (clause.rfind(tok, 0) == 0) { op = tok; val = clause.substr(tok.size()); return true; }
        return false;
    };
    consume(">=") || consume("<=") || consume("==") || consume("!=")
        || consume(">") || consume("<") || consume("=");
    trimSp(val);
    int cmp = compare_versions(version, val);
    if (op == "=" || op == "==") return cmp == 0;
    if (op == "!=")              return cmp != 0;
    if (op == ">=")              return cmp >= 0;
    if (op == "<=")              return cmp <= 0;
    if (op == ">")               return cmp >  0;
    if (op == "<")               return cmp <  0;
    return false;
}

// Match against a comma-separated AND of clauses, e.g. ">=1.0,<2.0".
static bool version_satisfies(const std::string& version, const std::string& range) {
    if (range.empty()) return true;
    size_t i = 0;
    while (i < range.size()) {
        auto comma = range.find(',', i);
        std::string clause = (comma == std::string::npos)
            ? range.substr(i)
            : range.substr(i, comma - i);
        if (!match_clause(version, clause)) return false;
        if (comma == std::string::npos) break;
        i = comma + 1;
    }
    return true;
}

// Walk up from `start` looking for the nearest `quirk.toml`. Returns the
// directory containing it, or empty if none is found before the filesystem
// root. `start` can be a file or directory path.
static fs::path find_project_root(fs::path start) {
    std::error_code ec;
    fs::path p = fs::absolute(start, ec);
    if (ec) return {};
    if (fs::is_regular_file(p)) p = p.parent_path();
    while (!p.empty()) {
        if (fs::exists(p / "quirk.toml")) return p;
        fs::path parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
    return {};
}

// For a given source file, return the package name declared in the nearest
// ancestor `quirk.toml`, or empty if none. Used so that `use slug` from
// inside the slug project resolves to its own src/, and so functions defined
// in slug/src/index.qk get `Slug_*` linkage names without needing the file
// to live under libs/ or packages/.
static std::string project_name_for_file(const std::string& filePath) {
    fs::path root = find_project_root(filePath);
    if (root.empty()) return "";
    Manifest m;
    if (!read_manifest((root / "quirk.toml").string(), m)) return "";
    return m.name;
}

// Resolve a `use <name>` to a file inside a project rooted at a `quirk.toml`
// whose `name = <name>`. Walks up from `relativeTo` to find that manifest,
// then probes the conventional entry-point layouts. Returns empty if no
// match. Lets a library import itself from anywhere inside its own repo
// — the in-tree equivalent of `pip install -e .`.
static std::string resolve_self_package(const std::string& moduleName,
                                        const std::string& relativeTo) {
    if (relativeTo.empty() || moduleName.empty()) return "";
    fs::path root = find_project_root(relativeTo);
    if (root.empty()) return "";
    Manifest m;
    if (!read_manifest((root / "quirk.toml").string(), m)) return "";
    if (m.name != moduleName) return "";

    for (const fs::path& candidate : {
            root / "src" / "index.qk",
            root / "src" / (moduleName + ".qk"),
            root / (moduleName + ".qk"),
            root / moduleName / "index.qk",
         }) {
        if (fs::exists(candidate)) return candidate.string();
    }
    return "";
}

// Get the current resolved revision of a checked-out repo.
static std::string git_head(const fs::path& dir) {
    std::string cmd = "git -C \"" + dir.string() + "\" rev-parse HEAD 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    char buf[64]; std::string out;
    while (fgets(buf, sizeof(buf), pipe)) out += buf;
    pclose(pipe);
    return trim(out);
}

// ---------------------------- Operations ----------------------------------

// Resolve where the system-wide Quirk standard library lives. Tries, in order:
//   $QUIRK_HOME/lib/quirk
//   $QUIRK_HOME/libs
//   <bindir>/../libs           (dev tree: quirk-compiler/libs)
//   <bindir>/../lib/quirk      (installed tree: /usr/local/lib/quirk)
//   /usr/local/lib/quirk
//   /usr/lib/quirk
// Returns an empty path if none found.
static fs::path find_system_stdlib() {
    auto try_dir = [](const fs::path& p) -> bool {
        return fs::exists(p / "typing") || fs::exists(p / "console");
    };

    if (const char* env = std::getenv("QUIRK_HOME")) {
        fs::path h(env);
        if (try_dir(h / "lib" / "quirk")) return h / "lib" / "quirk";
        if (try_dir(h / "libs"))          return h / "libs";
    }

    // Walk up from /proc/self/exe to find sibling libs/ directories.
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        fs::path bin(buf);
        fs::path bindir = bin.parent_path();
        if (try_dir(bindir.parent_path() / "libs"))          return bindir.parent_path() / "libs";
        if (try_dir(bindir.parent_path() / "lib" / "quirk")) return bindir.parent_path() / "lib" / "quirk";
    }

    for (const char* p : {"/usr/local/lib/quirk", "/usr/lib/quirk"}) {
        if (try_dir(p)) return fs::path(p);
    }
    return {};
}

// Best-effort path to the running `quirk` binary itself — used so the venv's
// `bin/quirk` can symlink back at the system install.
static fs::path find_quirk_binary() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    return fs::path(buf);
}

// Generated activate script. Sourced with `source <venv>/bin/activate` from
// bash/zsh; sets QUIRK_HOME, prepends bin/ to PATH, exposes a `deactivate`
// function that restores the previous environment.
static const char* activate_template() {
    return
        "# Source this from bash/zsh: `source <venv>/bin/activate`\n"
        "if [ -n \"${_OLD_QUIRK_HOME+x}\" ]; then\n"
        "    echo 'A Quirk env is already active — run `deactivate` first.' >&2\n"
        "    return 1 2>/dev/null || exit 1\n"
        "fi\n"
        "\n"
        "_VENV_SCRIPT=\"${BASH_SOURCE[0]:-${(%):-%x}}\"\n"
        "_VENV_DIR=\"$(cd \"$(dirname \"$_VENV_SCRIPT\")/..\" && pwd)\"\n"
        "\n"
        "export _OLD_QUIRK_HOME=\"${QUIRK_HOME-__UNSET__}\"\n"
        "export _OLD_PATH=\"$PATH\"\n"
        "export _OLD_PS1=\"${PS1-}\"\n"
        "\n"
        "export QUIRK_HOME=\"$_VENV_DIR\"\n"
        "export PATH=\"$_VENV_DIR/bin:$PATH\"\n"
        "export PS1=\"(quirk:$(basename \"$_VENV_DIR\")) $PS1\"\n"
        "\n"
        "deactivate() {\n"
        "    if [ \"$_OLD_QUIRK_HOME\" = \"__UNSET__\" ]; then\n"
        "        unset QUIRK_HOME\n"
        "    else\n"
        "        export QUIRK_HOME=\"$_OLD_QUIRK_HOME\"\n"
        "    fi\n"
        "    export PATH=\"$_OLD_PATH\"\n"
        "    export PS1=\"$_OLD_PS1\"\n"
        "    unset _OLD_QUIRK_HOME _OLD_PATH _OLD_PS1 _VENV_DIR _VENV_SCRIPT\n"
        "    unset -f deactivate\n"
        "}\n";
}

static int cmd_venv(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "venv: need a directory name (e.g. `quirk venv myenv`)\n";
        return 1;
    }
    fs::path venvDir = args[0];
    if (fs::exists(venvDir)) {
        std::cerr << "venv: '" << venvDir.string() << "' already exists\n";
        return 1;
    }

    fs::path stdlib = find_system_stdlib();
    if (stdlib.empty()) {
        std::cerr << "venv: cannot locate the Quirk standard library on this system\n";
        std::cerr << "      (tried $QUIRK_HOME, sibling-of-binary, /usr/local/lib/quirk)\n";
        return 1;
    }
    fs::path binPath = find_quirk_binary();

    // Build the venv layout:
    //   <venv>/lib/quirk/stdlib/<mod>   → symlinks to system stdlib modules
    //   <venv>/lib/quirk/packages/<pkg> → user-installed packages
    // The two-bucket split keeps stdlib (frozen at venv-creation time) visually
    // separate from installed packages (mutated by `quirk install`).
    fs::create_directories(venvDir / "bin");
    fs::create_directories(venvDir / "lib" / "quirk" / "packages");
    fs::create_directories(venvDir / "lib" / "quirk" / "stdlib");

    int linked = 0;
    for (auto& entry : fs::directory_iterator(stdlib)) {
        if (!entry.is_directory()) continue;
        std::string name = entry.path().filename().string();
        if (name.empty() || name[0] == '.' || name == "packages") continue;
        fs::path linkPath = venvDir / "lib" / "quirk" / "stdlib" / name;
        std::error_code ec;
        fs::create_symlink(fs::absolute(entry.path()), linkPath, ec);
        if (!ec) linked++;
    }

    // Symlink the running compiler so `<venv>/bin/quirk` resolves; also
    // symlink the runtime.so that lives next to it so the JIT can find it.
    if (!binPath.empty()) {
        std::error_code ec;
        fs::create_symlink(fs::absolute(binPath), venvDir / "bin" / "quirk", ec);
        fs::path rt = binPath.parent_path() / "runtime.so";
        if (fs::exists(rt)) {
            fs::create_symlink(fs::absolute(rt), venvDir / "bin" / "runtime.so", ec);
        }
    }

    // activate script (bash-compatible; zsh-compatible via the BASH_SOURCE
    // fallback in the template).
    std::ofstream activate(venvDir / "bin" / "activate");
    activate << activate_template();
    activate.close();

    // Starter manifest.
    Manifest m;
    m.name = fs::path(args[0]).filename().string();
    m.version = "0.1.0";
    write_manifest((venvDir / "quirk.toml").string(), m);

    std::cout << "Created Quirk venv '" << venvDir.string() << "' with "
              << linked << " stdlib module(s) linked.\n"
              << "Activate it with:\n"
              << "    source " << venvDir.string() << "/bin/activate\n";
    return 0;
}

static int cmd_init() {
    fs::path mf = "quirk.toml";
    if (fs::exists(mf)) {
        std::cerr << "quirk.toml already exists.\n";
        return 1;
    }
    Manifest m;
    m.name = fs::current_path().filename().string();
    m.version = "0.1.0";
    m.license = "MIT";
    write_manifest(mf.string(), m);
    std::cout << "Created quirk.toml for project '" << m.name << "'.\n";
    return 0;
}

// True when QUIRK_HOME points at an *activated venv* (as opposed to a dev
// tree or a stray export). Distinguished by the presence of bin/activate,
// which `quirk venv` writes but dev trees never have.
static bool is_active_venv() {
    const char* env = std::getenv("QUIRK_HOME");
    if (!env) return false;
    return fs::exists(fs::path(env) / "bin" / "activate");
}

// Resolve the user-global package dir (~/.quirk/packages/), creating the
// parent ~/.quirk/ if missing. Mirrors `pip install --user` / `cargo install`
// — user-scoped, no root needed, persists across projects.
static fs::path user_packages_dir() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) return {};
    fs::path base = fs::path(home) / ".quirk" / "packages";
    std::error_code ec;
    fs::create_directories(base, ec);
    return base;
}

// Where new packages get installed:
//   1. Active venv (has bin/activate)  → $QUIRK_HOME/lib/quirk/packages/   (pip: venv wins)
//   2. Inside a project (quirk.toml at or above cwd)  → <project>/packages/  (npm: project-local)
//   3. Otherwise                        → ~/.quirk/packages/               (pip --user / cargo)
// System-wide /usr/local/... is reserved for stdlib & vendor; never for user installs.
static fs::path package_install_dir() {
    if (is_active_venv()) return fs::path(std::getenv("QUIRK_HOME")) / "lib" / "quirk" / "packages";
    fs::path proj = find_project_root(fs::current_path());
    if (!proj.empty()) return proj / "packages";
    fs::path userDir = user_packages_dir();
    if (!userDir.empty()) return userDir;
    return fs::path("packages");   // last-ditch fallback if $HOME is unset
}

// True when `spec` looks like a local filesystem path rather than a git spec.
// Accepts: absolute paths, ./relative, ../relative, ~user paths, the literal
// ".", and anything that already exists as a directory on disk.
static bool is_local_path(const std::string& spec) {
    if (spec.empty()) return false;
    if (spec[0] == '/' || spec[0] == '~' || spec[0] == '.') return true;
    // Bare directory name that exists relative to cwd
    std::error_code ec;
    return fs::is_directory(spec, ec);
}

// ----------------------------- Layout (Option B) ---------------------------
// Active installs are flat in `packages/`:
//   packages/<name>/                 ← active code (one version per package)
//   packages/<name>-<ver>.dist-info/ ← metadata sidecar (Name/Version/Installer)
//
// All versions ever fetched live in a cross-project cache:
//   ~/.quirk/cache/<name>-<ver>/
//
// `quirk install <name>@<ver>` checks the cache first; missing versions are
// downloaded (git) or copied (local path) into the cache once, then linked
// into packages/.

static fs::path cache_dir() {
    const char* h = std::getenv("HOME");
    if (!h) return {};
    fs::path c = fs::path(h) / ".quirk" / "cache";
    std::error_code ec;
    fs::create_directories(c, ec);
    return c;
}

static fs::path cache_entry(const std::string& name, const std::string& version) {
    fs::path c = cache_dir();
    if (c.empty()) return {};
    return c / (name + "-" + version);
}

// dist-info dir for an active install. Lives next to `<name>/` in packages/.
static fs::path dist_info_dir(const fs::path& pkgRoot, const std::string& name,
                              const std::string& version) {
    return pkgRoot / (name + "-" + version + ".dist-info");
}

// Locate the .dist-info for a name (matches `<name>-*.dist-info`). Returns
// the first match or an empty path. The active version is the unique sidecar.
static fs::path find_dist_info(const fs::path& pkgRoot, const std::string& name) {
    if (!fs::is_directory(pkgRoot)) return {};
    std::string prefix = name + "-";
    for (auto& e : fs::directory_iterator(pkgRoot)) {
        std::string fn = e.path().filename().string();
        if (fn.size() > prefix.size() + 10
            && fn.rfind(prefix, 0) == 0
            && fn.size() > 10
            && fn.substr(fn.size() - 10) == ".dist-info") {
            return e.path();
        }
    }
    return {};
}

// Read the active version of a package by inspecting its dist-info name.
// Returns "" if the package is not installed in `pkgRoot`.
static std::string read_current_version(const fs::path& pkgRoot, const std::string& name) {
    fs::path d = find_dist_info(pkgRoot, name);
    if (d.empty()) return "";
    std::string fn = d.filename().string();
    std::string prefix = name + "-";
    return fn.substr(prefix.size(), fn.size() - prefix.size() - 10); // strip prefix + .dist-info
}

// Convenience overload kept for the older callsites: caller passes pkgPath
// = <pkgRoot>/<name>, we infer name from its filename.
static std::string read_current_version(const fs::path& pkgPath) {
    return read_current_version(pkgPath.parent_path(), pkgPath.filename().string());
}

// All cached versions of <name> as a sorted vector.
static std::vector<std::string> list_cached_versions(const std::string& name) {
    std::vector<std::string> out;
    fs::path c = cache_dir();
    if (c.empty() || !fs::is_directory(c)) return out;
    std::string prefix = name + "-";
    for (auto& e : fs::directory_iterator(c)) {
        std::string fn = e.path().filename().string();
        if (fn.rfind(prefix, 0) == 0 && (fs::is_directory(e.path()) || fs::is_symlink(e.path()))) {
            out.push_back(fn.substr(prefix.size()));
        }
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b){
        return compare_versions(a, b) < 0;
    });
    return out;
}

// Legacy helper still used elsewhere — list "installed versions" now means
// "cached versions" since active installs are single-version.
static std::vector<std::string> list_installed_versions(const fs::path& pkgPath) {
    return list_cached_versions(pkgPath.filename().string());
}

// Pick the highest cached version of <name> matching `range`. Empty string
// if no cached version matches.
static std::string pick_cached_version(const std::string& name, const std::string& range) {
    std::string best;
    for (auto& v : list_cached_versions(name)) {
        if (!version_satisfies(v, range)) continue;
        if (best.empty() || compare_versions(v, best) > 0) best = v;
    }
    return best;
}

// Write the metadata sidecar.
static void write_dist_info(const fs::path& pkgRoot, const Manifest& m, bool editable) {
    fs::path d = dist_info_dir(pkgRoot, m.name, m.version);
    fs::create_directories(d);
    std::ofstream out(d / "METADATA");
    out << "Name: "      << m.name    << "\n"
        << "Version: "   << m.version << "\n";
    if (!m.description.empty()) out << "Summary: " << m.description << "\n";
    if (!m.author.empty())      out << "Author: "  << m.author      << "\n";
    if (!m.license.empty())     out << "License: " << m.license     << "\n";
    out << "Installer: quirk\n";
    if (editable) out << "Editable: true\n";
    for (auto& dep : m.deps) out << "Requires: " << dep.first << " (" << dep.second << ")\n";
}

// Remove any existing dist-info sidecar(s) for <name> in pkgRoot. Used
// before writing a new one so we never end up with two sidecars.
static void clear_dist_info(const fs::path& pkgRoot, const std::string& name) {
    if (!fs::is_directory(pkgRoot)) return;
    std::string prefix = name + "-";
    std::vector<fs::path> toRemove;
    for (auto& e : fs::directory_iterator(pkgRoot)) {
        std::string fn = e.path().filename().string();
        if (fn.size() > prefix.size() + 10
            && fn.rfind(prefix, 0) == 0
            && fn.size() > 10
            && fn.substr(fn.size() - 10) == ".dist-info") {
            toRemove.push_back(e.path());
        }
    }
    for (auto& p : toRemove) fs::remove_all(p);
}

// Repoint <pkg>/current → <version>/. Used both by fresh installs and by
// version-switch operations.
static int set_current_version(const fs::path& pkgDir, const std::string& version) {
    fs::path link = pkgDir / "current";
    std::error_code ec;
    if (fs::exists(link) || fs::is_symlink(link)) fs::remove(link);
    fs::create_directory_symlink(version, link, ec);
    if (ec) {
        std::cerr << "install: failed to update 'current' symlink: " << ec.message() << "\n";
        return 1;
    }
    return 0;
}

// Of the already-installed versions of `pkgDir`, return the highest one that
// satisfies `range`. Empty string if none.
static std::string pick_installed_version(const fs::path& pkgDir, const std::string& range) {
    std::string best;
    if (!fs::is_directory(pkgDir)) return best;
    for (auto& v : fs::directory_iterator(pkgDir)) {
        std::string name = v.path().filename().string();
        if (name.empty() || name[0] == '.' || name == "current") continue;
        if (!fs::is_directory(v.path()) && !fs::is_symlink(v.path())) continue;
        if (!version_satisfies(name, range)) continue;
        if (best.empty() || compare_versions(name, best) > 0) best = name;
    }
    return best;
}

// Local install of a package. Versioned layout:
//   packages/<name>/<version>/   ← actual install
//   packages/<name>/current      ← symlink to active version
//
// Two modes:
//   editable=true   → <version>/ is a symlink to the source (live edits)
//   editable=false  → <version>/ is a recursive copy (snapshot)
//
// If `pinVersion` is non-empty, the source's manifest version must match;
// otherwise we install the source's declared version.
// Internal: replace packages/<name>/ with content from the cache entry.
// Strategy: copy (snapshot) so re-installs don't suffer from cache mutation.
// Returns 0 on success, non-zero on failure.
static int materialize_from_cache(const fs::path& cachePath, const fs::path& pkgRoot,
                                  const Manifest& m, bool editable) {
    fs::path target = pkgRoot / m.name;
    std::error_code ec;
    if (fs::exists(target) || fs::is_symlink(target)) fs::remove_all(target);

    if (editable) {
        // Editable installs symlink the *source*, not the cache.
        // (Caller passes cachePath = source path in that case.)
        fs::create_directory_symlink(cachePath, target, ec);
    } else {
        fs::copy(cachePath, target,
                 fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
        if (!ec) fs::remove_all(target / ".git");
    }
    if (ec) {
        std::cerr << "install: failed to install " << target.string()
                  << ": " << ec.message() << "\n";
        fs::remove_all(target);
        return 1;
    }
    clear_dist_info(pkgRoot, m.name);
    write_dist_info(pkgRoot, m, editable);
    return 0;
}

static int install_local(const std::string& path_spec, bool editable,
                         const std::string& pinVersion = "") {
    fs::path src = path_spec;
    if (!path_spec.empty() && path_spec[0] == '~') {
        const char* home = std::getenv("HOME");
        if (home) src = fs::path(home) / path_spec.substr(path_spec.size() > 1 && path_spec[1] == '/' ? 2 : 1);
    }
    std::error_code ec;
    src = fs::absolute(src, ec);
    if (ec || !fs::is_directory(src)) {
        std::cerr << "install: '" << path_spec << "' is not a directory\n";
        return 1;
    }
    Manifest m;
    if (!read_manifest((src / "quirk.toml").string(), m) || m.name.empty()) {
        std::cerr << "install: no quirk.toml (or missing `name =`) in " << src.string() << "\n";
        return 1;
    }
    if (m.version.empty()) m.version = "0.0.0";

    fs::path pkgRoot = package_install_dir();
    fs::create_directories(pkgRoot);

    // Fast-path: a pin matches something we already have cached. No re-fetch,
    // just materialize from the cache. Editable always re-symlinks from source.
    if (!pinVersion.empty() && !editable) {
        std::string cached = pick_cached_version(m.name, pinVersion);
        if (!cached.empty()) {
            Manifest mCached;
            fs::path entry = cache_entry(m.name, cached);
            if (entry.empty() || !read_manifest((entry / "quirk.toml").string(), mCached)) {
                mCached.name = m.name; mCached.version = cached;
            }
            if (materialize_from_cache(entry, pkgRoot, mCached, false) != 0) return 1;
            std::cout << "  ✓ " << m.name << " " << cached << " (from cache)\n";
            return 0;
        }
    }

    if (!pinVersion.empty() && !version_satisfies(m.version, pinVersion)) {
        std::cerr << "install: requested " << m.name << "@" << pinVersion
                  << " but source declares version " << m.version << "\n";
        return 1;
    }

    // Populate the cache for this (name, version) if missing. Editable skips
    // the cache (it'd be a symlink to the source — pointless to cache).
    if (!editable) {
        fs::path entry = cache_entry(m.name, m.version);
        if (!entry.empty() && !fs::exists(entry)) {
            fs::create_directories(entry.parent_path());
            fs::copy(src, entry,
                     fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
            if (ec) {
                std::cerr << "install: failed to cache " << src.string() << " → "
                          << entry.string() << ": " << ec.message() << "\n";
                fs::remove_all(entry);
                return 1;
            }
            fs::remove_all(entry / ".git");
        }
        if (materialize_from_cache(entry, pkgRoot, m, false) != 0) return 1;
        std::cout << "  ✓ " << m.name << " " << m.version
                  << " (snapshot from " << src.string() << ")\n";
    } else {
        if (materialize_from_cache(src, pkgRoot, m, true) != 0) return 1;
        std::cout << "  ✓ " << m.name << " " << m.version
                  << " (editable from " << src.string() << ")\n";
    }
    return 0;
}

static int install_one(const std::string& spec_str, bool quiet, bool editable = false) {
    // Split off `@<version>` from a local path or bare-name spec (git URLs
    // are handled by parse_spec below). A leading `~/`, `./`, or `/` marks a
    // path; otherwise we'll see if it's a bare package name below.
    std::string pathPart = spec_str;
    std::string pinVersion;
    bool isPathLike = !spec_str.empty()
        && (spec_str[0] == '/' || spec_str[0] == '~' || spec_str[0] == '.');
    bool looksLikeGitSpec = spec_str.find("://") != std::string::npos
                         || spec_str.find('/') != std::string::npos;
    if (isPathLike || !looksLikeGitSpec) {
        auto at = spec_str.find('@');
        if (at != std::string::npos) {
            pathPart = spec_str.substr(0, at);
            pinVersion = spec_str.substr(at + 1);
        }
    }
    if (is_local_path(pathPart)) return install_local(pathPart, editable, pinVersion);

    // Bare-name switch: `quirk install <name>@<range>` with no path/URL.
    // Picks the highest cached version satisfying the range and swaps the
    // active install in `packages/<name>/`.
    if (!isPathLike && !looksLikeGitSpec && !pinVersion.empty()) {
        std::string chosen = pick_cached_version(pathPart, pinVersion);
        if (chosen.empty()) {
            std::cerr << "install: no cached version of '" << pathPart
                      << "' satisfies '" << pinVersion << "'\n";
            std::cerr << "  (`quirk cache list " << pathPart << "` to see what's cached)\n";
            return 1;
        }
        fs::path entry = cache_entry(pathPart, chosen);
        Manifest m;
        if (!read_manifest((entry / "quirk.toml").string(), m)) {
            m.name = pathPart; m.version = chosen;
        }
        fs::path pkgRoot = package_install_dir();
        fs::create_directories(pkgRoot);
        if (materialize_from_cache(entry, pkgRoot, m, false) != 0) return 1;
        std::cout << "  ✓ " << pathPart << " " << chosen << " (from cache)\n";
        return 0;
    }

    PkgSpec spec = parse_spec(spec_str);
    fs::path pkgRoot = package_install_dir();
    fs::create_directories(pkgRoot);
    fs::path target = pkgRoot / spec.name;

    if (fs::exists(target)) {
        std::cout << "  · " << spec.name << " already installed (skipping)\n";
        return 0;
    }
    std::string cmd = "git clone --depth 1";
    if (!spec.ref.empty()) cmd += " --branch \"" + spec.ref + "\"";
    cmd += " \"" + spec.url + "\" \"" + target.string() + "\"";
    if (quiet) cmd += " 2>/dev/null";
    int rc = sh(cmd, quiet);
    if (rc != 0) {
        std::cerr << "  ✗ failed to install " << spec.name << "\n";
        fs::remove_all(target);
        return rc;
    }
    fs::remove_all(target / ".git");  // strip vcs metadata for cleanliness
    std::cout << "  ✓ " << spec.name << (spec.ref.empty() ? "" : " (" + spec.ref + ")") << "\n";
    return 0;
}

static int cmd_install(const std::vector<std::string>& args) {
    // Parse flags: -r/--read <file>, -e/--editable <path>, or positional pkg specs.
    std::string manifestFile;
    std::vector<std::string> specs;
    std::set<std::string> editableSpecs;     // paths marked with -e
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "-r" || args[i] == "--read") {
            if (i + 1 >= args.size()) {
                std::cerr << "install: -r requires a filename\n";
                return 1;
            }
            manifestFile = args[++i];
        } else if (args[i] == "-e" || args[i] == "--editable") {
            if (i + 1 >= args.size()) {
                std::cerr << "install: -e requires a local path\n";
                return 1;
            }
            const std::string& p = args[++i];
            specs.push_back(p);
            editableSpecs.insert(p);
        } else {
            specs.push_back(args[i]);
        }
    }

    // No args → install everything in ./quirk.toml's [deps].
    if (specs.empty() && manifestFile.empty()) manifestFile = "quirk.toml";

    if (!manifestFile.empty()) {
        Manifest m;
        if (!read_manifest(manifestFile, m)) {
            std::cerr << "install: cannot read manifest '" << manifestFile << "'\n";
            return 1;
        }
        if (m.deps.empty()) {
            std::cout << "No dependencies declared in " << manifestFile << ".\n";
            return 0;
        }
        std::cout << "Installing " << m.deps.size() << " package(s) from " << manifestFile << ":\n";
        for (auto& d : m.deps) {
            install_one(d.second, false);
        }
    }

    // Positional specs: install and (when in a project) add to manifest.
    Manifest projMan;
    bool haveManifest = fs::exists("quirk.toml") && read_manifest("quirk.toml", projMan);
    bool dirty = false;
    for (auto& s : specs) {
        bool editable = editableSpecs.count(s) > 0;
        if (install_one(s, false, editable) != 0) continue;
        if (!haveManifest) continue;

        // Local path: use the manifest name and store the absolute path so
        // future `quirk install` rebuilds the install from the right location.
        // Otherwise: parse the git spec for its inferred name.
        std::string name, stored;
        if (is_local_path(s)) {
            Manifest lm;
            std::error_code ec;
            fs::path abs = fs::absolute(s, ec);
            if (ec || !read_manifest((abs / "quirk.toml").string(), lm) || lm.name.empty()) continue;
            name = lm.name;
            stored = abs.string();
        } else {
            name = parse_spec(s).name;
            stored = s;
        }
        bool found = false;
        for (auto& d : projMan.deps) if (d.first == name) { d.second = stored; found = true; break; }
        if (!found) projMan.deps.emplace_back(name, stored);
        dirty = true;
    }
    if (dirty) write_manifest("quirk.toml", projMan);
    return 0;
}

static int cmd_remove(const std::vector<std::string>& names) {
    if (names.empty()) {
        std::cerr << "remove: need at least one package name\n";
        return 1;
    }
    Manifest projMan;
    bool haveManifest = fs::exists("quirk.toml") && read_manifest("quirk.toml", projMan);
    fs::path pkgRoot = package_install_dir();
    for (auto& nv : names) {
        // <name>@<version> drops only that cached version; bare name uninstalls
        // the active code + dist-info (cache untouched — use `quirk cache clean`).
        std::string n = nv, ver;
        auto at = nv.find('@');
        if (at != std::string::npos) { n = nv.substr(0, at); ver = nv.substr(at + 1); }

        if (ver.empty()) {
            fs::path code = pkgRoot / n;
            bool hadCode = fs::exists(code) || fs::is_symlink(code);
            fs::remove_all(code);
            clear_dist_info(pkgRoot, n);
            if (hadCode || !find_dist_info(pkgRoot, n).empty()) {
                std::cout << "  ✓ removed " << n << "\n";
                if (haveManifest) {
                    auto it = projMan.deps.begin();
                    while (it != projMan.deps.end()) {
                        if (it->first == n) it = projMan.deps.erase(it);
                        else                 ++it;
                    }
                }
            } else {
                std::cerr << "  ! " << n << " not installed\n";
            }
            continue;
        }

        // Drop one cached version. Doesn't touch the active install unless
        // that's the version being removed.
        fs::path entry = cache_entry(n, ver);
        if (!fs::exists(entry)) {
            std::cerr << "  ! " << n << "@" << ver << " not cached\n";
        } else {
            fs::remove_all(entry);
            std::cout << "  ✓ removed cache: " << n << "@" << ver << "\n";
        }
        // If this was the active version, drop the active install too.
        if (read_current_version(pkgRoot, n) == ver) {
            fs::remove_all(pkgRoot / n);
            clear_dist_info(pkgRoot, n);
            std::cout << "    (was active; uninstalled)\n";
        }
    }
    if (haveManifest) write_manifest("quirk.toml", projMan);
    return 0;
}

static int cmd_upgrade(const std::vector<std::string>& names) {
    Manifest projMan;
    if (!read_manifest("quirk.toml", projMan)) {
        std::cerr << "upgrade: no quirk.toml in this directory\n";
        return 1;
    }
    auto picked = [&](const std::string& n) {
        if (names.empty()) return true;
        for (auto& x : names) if (x == n) return true;
        return false;
    };
    fs::path pkgRoot = package_install_dir();
    for (auto& d : projMan.deps) {
        if (!picked(d.first)) continue;
        std::cout << "Upgrading " << d.first << "...\n";

        // Local install: pick the highest cached version and materialize it.
        bool isLocal = !d.second.empty()
            && (d.second[0] == '/' || d.second[0] == '~' || d.second[0] == '.');
        if (isLocal) {
            std::string best = pick_cached_version(d.first, "");
            std::string active = read_current_version(pkgRoot, d.first);
            if (best.empty()) {
                std::cerr << "  ! " << d.first << " not cached\n";
                continue;
            }
            if (best == active) {
                std::cout << "  · " << d.first << " already at " << best << "\n";
                continue;
            }
            fs::path entry = cache_entry(d.first, best);
            Manifest mC;
            if (!read_manifest((entry / "quirk.toml").string(), mC)) {
                mC.name = d.first; mC.version = best;
            }
            if (materialize_from_cache(entry, pkgRoot, mC, false) != 0) continue;
            std::cout << "  ✓ " << d.first << " " << active << " → " << best << "\n";
            continue;
        }

        // Git URL: re-clone at the default branch's HEAD (existing behavior).
        fs::path p = pkgRoot / d.first;
        if (fs::exists(p)) fs::remove_all(p);
        std::string spec = d.second;
        auto at = spec.find('@');
        if (at != std::string::npos) spec = spec.substr(0, at);
        install_one(spec, false);
    }
    return 0;
}

static int cmd_list() {
    fs::path pkgDir = package_install_dir();
    if (!fs::exists(pkgDir) || !fs::is_directory(pkgDir)) {
        std::cout << "No packages installed (no " << pkgDir.string() << "/ directory).\n";
        return 0;
    }
    struct Row { std::string name, version; std::vector<std::string> cached; };
    std::vector<Row> rows;
    for (auto& entry : fs::directory_iterator(pkgDir)) {
        std::string fn = entry.path().filename().string();
        if (fn.empty() || fn[0] == '.') continue;
        // Skip dist-info dirs; we use them as the index instead.
        if (fn.size() > 10 && fn.substr(fn.size() - 10) == ".dist-info") continue;
        if (!fs::is_directory(entry.path()) && !fs::is_symlink(entry.path())) continue;
        Row r;
        r.name    = fn;
        r.version = read_current_version(pkgDir, fn);
        if (r.version.empty()) {
            // Fallback: read manifest directly (handles installs without dist-info)
            Manifest m;
            if (read_manifest((entry.path() / "quirk.toml").string(), m) && !m.version.empty())
                r.version = m.version;
            else r.version = "?";
        }
        r.cached = list_cached_versions(fn);
        rows.push_back(std::move(r));
    }
    if (rows.empty()) {
        std::cout << "No packages installed.\n";
        return 0;
    }
    size_t pad = 0;
    for (auto& r : rows) if (r.name.size() > pad) pad = r.name.size();
    std::cout << rows.size() << " package(s) installed:\n";
    for (auto& r : rows) {
        std::cout << "  " << r.name;
        for (size_t i = r.name.size(); i < pad + 2; i++) std::cout << ' ';
        std::cout << r.version;
        if (r.cached.size() > 1) {
            std::cout << "  (cached: ";
            bool first = true;
            for (auto& v : r.cached) {
                if (v == r.version) continue;
                if (!first) std::cout << ", ";
                std::cout << v; first = false;
            }
            std::cout << ")";
        }
        std::cout << "\n";
    }
    return 0;
}

static int cmd_show(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "show: need a package name\n";
        return 1;
    }
    fs::path pkgRoot = package_install_dir();
    fs::path p = pkgRoot / args[0];
    if (!fs::exists(p)) {
        std::cerr << args[0] << ": not installed\n";
        return 1;
    }
    Manifest m;
    if (!read_manifest((p / "quirk.toml").string(), m)) {
        std::cout << args[0] << " (installed; no manifest)\n";
        return 0;
    }
    std::cout << "name:        " << m.name << "\n";
    std::cout << "version:     " << m.version << "\n";
    if (!m.description.empty()) std::cout << "description: " << m.description << "\n";
    if (!m.author.empty())      std::cout << "author:      " << m.author << "\n";
    if (!m.license.empty())     std::cout << "license:     " << m.license << "\n";
    if (!m.deps.empty()) {
        std::cout << "deps:\n";
        for (auto& d : m.deps) std::cout << "  " << d.first << " = " << d.second << "\n";
    }
    auto cached = list_cached_versions(args[0]);
    if (cached.size() > 1) {
        std::cout << "cached versions: ";
        for (size_t i = 0; i < cached.size(); i++) {
            if (i) std::cout << ", ";
            std::cout << cached[i];
            if (cached[i] == m.version) std::cout << " (active)";
        }
        std::cout << "\n";
    }
    return 0;
}

// `quirk cache <subcommand>` — manage the cross-project version cache.
static int cmd_cache(const std::vector<std::string>& args) {
    fs::path cdir = cache_dir();
    std::string sub = args.empty() ? "list" : args[0];

    if (sub == "list") {
        // `quirk cache list [pkg]` — show cached versions overall or filter to one pkg
        std::string filter = args.size() > 1 ? args[1] : "";
        if (!fs::is_directory(cdir)) {
            std::cout << "Cache empty.\n";
            return 0;
        }
        std::map<std::string, std::vector<std::string>> byPkg;
        for (auto& e : fs::directory_iterator(cdir)) {
            std::string fn = e.path().filename().string();
            auto dash = fn.find_last_of('-');
            if (dash == std::string::npos) continue;
            std::string n = fn.substr(0, dash);
            std::string v = fn.substr(dash + 1);
            if (!filter.empty() && n != filter) continue;
            byPkg[n].push_back(v);
        }
        if (byPkg.empty()) {
            std::cout << (filter.empty() ? "Cache empty." : ("No cached versions for '" + filter + "'.")) << "\n";
            return 0;
        }
        for (auto& kv : byPkg) {
            std::sort(kv.second.begin(), kv.second.end(), [](const std::string& a, const std::string& b){
                return compare_versions(a, b) < 0;
            });
            std::cout << kv.first << ":";
            for (auto& v : kv.second) std::cout << " " << v;
            std::cout << "\n";
        }
        return 0;
    }
    if (sub == "clean") {
        // `quirk cache clean [pkg[@ver]]` — wipe cache (filtered).
        if (!fs::is_directory(cdir)) { std::cout << "Cache empty.\n"; return 0; }
        if (args.size() < 2) {
            // Wipe everything
            for (auto& e : fs::directory_iterator(cdir)) fs::remove_all(e.path());
            std::cout << "Cache cleared.\n";
            return 0;
        }
        std::string target = args[1];
        std::string n = target, v;
        auto at = target.find('@');
        if (at != std::string::npos) { n = target.substr(0, at); v = target.substr(at + 1); }
        int removed = 0;
        for (auto& e : fs::directory_iterator(cdir)) {
            std::string fn = e.path().filename().string();
            auto dash = fn.find_last_of('-');
            if (dash == std::string::npos) continue;
            std::string en = fn.substr(0, dash);
            std::string ev = fn.substr(dash + 1);
            if (en != n) continue;
            if (!v.empty() && ev != v) continue;
            fs::remove_all(e.path());
            removed++;
        }
        std::cout << "Removed " << removed << " cache entries.\n";
        return 0;
    }
    if (sub == "dir") {
        std::cout << cdir.string() << "\n";
        return 0;
    }
    std::cerr << "cache: unknown subcommand '" << sub << "'\n";
    std::cerr << "  usage: quirk cache list [<pkg>]\n";
    std::cerr << "         quirk cache clean [<pkg>[@<ver>]]\n";
    std::cerr << "         quirk cache dir\n";
    return 1;
}

static int cmd_version() {
    std::cout << "quirk " << QUIRK_VERSION << "\n";
    return 0;
}

// Absolute path to the running quirk binary — used when we need to re-invoke
// ourselves (eval, module).
static std::string self_binary() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = '\0'; return buf; }
    return "quirk";
}

// `quirk eval "<code>"` — wrap the code in `define main() { ... }` and run it.
// We write a temp file and re-exec ourselves so the normal compile path runs.
static int cmd_eval(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "eval: need a code string\n";
        std::cerr << "  e.g. quirk eval 'print(\"hi\")'\n";
        return 1;
    }
    std::string code;
    for (size_t i = 0; i < args.size(); i++) {
        if (i) code += " ";
        code += args[i];
    }
    fs::path tmp = fs::temp_directory_path()
        / ("quirk_eval_" + std::to_string(getpid()) + ".qk");
    {
        std::ofstream out(tmp);
        out << "define main() -> void {\n    " << code << "\n}\n";
    }
    std::string cmd = "\"" + self_binary() + "\" \"" + tmp.string() + "\"";
    int rc = std::system(cmd.c_str());
    std::error_code ec;
    fs::remove(tmp, ec);
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : 1;
}

// Find the entry .qk file of a named module by mirroring the compiler's
// resolveImportPath: search install dirs + stdlib + project-local for the
// usual layouts (X.qk, X/index.qk, X/src/index.qk, X/current/src/index.qk).
static fs::path locate_module_file(const std::string& name) {
    std::vector<fs::path> roots;
    roots.push_back(fs::current_path() / "packages");
    if (const char* h = std::getenv("QUIRK_HOME")) {
        fs::path home(h);
        roots.push_back(home / "lib" / "quirk" / "packages");
        roots.push_back(home / "lib" / "quirk" / "stdlib");
        roots.push_back(home / "lib" / "quirk");
        roots.push_back(home / "libs");
    }
    if (const char* hm = std::getenv("HOME"))
        roots.push_back(fs::path(hm) / ".quirk" / "packages");
    fs::path stdlib = find_system_stdlib();
    if (!stdlib.empty()) roots.push_back(stdlib);

    for (const auto& root : roots) {
        for (const fs::path& c : {
                root / (name + ".qk"),
                root / name / "index.qk",
                root / name / "src" / "index.qk",
                root / name / "src" / (name + ".qk"),
                root / name / "current" / "src" / "index.qk",
                root / name / "current" / "src" / (name + ".qk"),
             }) {
            if (fs::exists(c)) return c;
        }
    }
    return {};
}

// `quirk module <name> [args...]` — locate the module's entry file and run
// it. The module is expected to define `main()`; if it doesn't, the compiler
// errors out cleanly.
static int cmd_module(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "module: need a module name\n";
        std::cerr << "  e.g. quirk module test\n";
        return 1;
    }
    const std::string& name = args[0];
    fs::path file = locate_module_file(name);
    if (file.empty()) {
        std::cerr << "module: '" << name << "' not found in any search path\n";
        std::cerr << "  (run `quirk env` to see search paths)\n";
        return 1;
    }
    std::string cmd = "\"" + self_binary() + "\" \"" + file.string() + "\"";
    for (size_t i = 1; i < args.size(); i++) cmd += " \"" + args[i] + "\"";
    int rc = std::system(cmd.c_str());
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : 1;
}

// `quirk deps` — print installed packages in `name = "version"` form.
// Output is suitable for redirecting into a `quirk.toml` [deps] block.
static int cmd_deps() {
    fs::path pkgDir = package_install_dir();
    if (!fs::is_directory(pkgDir)) {
        std::cout << "# (no packages installed)\n";
        return 0;
    }
    bool any = false;
    for (auto& entry : fs::directory_iterator(pkgDir)) {
        std::string fn = entry.path().filename().string();
        if (fn.empty() || fn[0] == '.') continue;
        if (fn.size() > 10 && fn.substr(fn.size() - 10) == ".dist-info") continue;
        if (!fs::is_directory(entry.path()) && !fs::is_symlink(entry.path())) continue;
        std::string ver = read_current_version(pkgDir, fn);
        if (ver.empty()) {
            Manifest m;
            if (read_manifest((entry.path() / "quirk.toml").string(), m))
                ver = m.version.empty() ? "0.0.0" : m.version;
            else
                ver = "0.0.0";
        }
        std::cout << fn << " = \"" << ver << "\"\n";
        any = true;
    }
    if (!any) std::cout << "# (no packages installed)\n";
    return 0;
}

// `quirk env` — print the resolution context for debugging.
static int cmd_env() {
    const char* envHome = std::getenv("QUIRK_HOME");
    bool isVenv = envHome && fs::exists(fs::path(envHome) / "bin" / "activate");
    std::cout << "quirk:           " << self_binary() << "\n";
    std::cout << "version:         " << QUIRK_VERSION << "\n";
    std::cout << "QUIRK_HOME:      " << (envHome ? envHome : "(unset)") << "\n";
    std::cout << "in venv:         " << (isVenv ? "yes" : "no") << "\n";
    fs::path proj = find_project_root(fs::current_path());
    std::cout << "project root:    " << (proj.empty() ? "(none)" : proj.string()) << "\n";
    std::cout << "install dir:     " << package_install_dir().string() << "\n";
    fs::path stdlib = find_system_stdlib();
    std::cout << "stdlib:          " << (stdlib.empty() ? "(not found)" : stdlib.string()) << "\n";
    std::cout << "user-global:     " << (std::getenv("HOME")
        ? std::string(std::getenv("HOME")) + "/.quirk/packages" : "(no $HOME)") << "\n";
    return 0;
}

// `quirk new <name>` — scaffold a new package: directory, quirk.toml, src/,
// tests/, .gitignore. Like `cargo new` or `npm init -y`.
static int cmd_new(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "new: need a project name\n";
        std::cerr << "  e.g. quirk new my-lib\n";
        return 1;
    }
    fs::path dir = args[0];
    if (fs::exists(dir)) {
        std::cerr << "new: '" << dir.string() << "' already exists\n";
        return 1;
    }
    std::string name = dir.filename().string();
    fs::create_directories(dir / "src");
    fs::create_directories(dir / "tests");

    Manifest m;
    m.name = name;
    m.version = "0.1.0";
    m.license = "MIT";
    m.description = "";
    write_manifest((dir / "quirk.toml").string(), m);

    {
        std::ofstream out(dir / "src" / "index.qk");
        out << "// " << name << " — Quirk package entry point\n\n"
            << "define hello(who: String) -> String {\n"
            << "    return \"Hello, \" + who + \"!\"\n"
            << "}\n\n"
            << "define main() -> void {\n"
            << "    print(hello(\"world\"))\n"
            << "}\n";
    }
    {
        std::ofstream out(dir / "tests" / (name + "_test.qk"));
        out << "use " << name << "\n"
            << "from " << name << " use { hello }\n\n"
            << "define main() -> void {\n"
            << "    print(hello(\"test\"))\n"
            << "}\n";
    }
    {
        std::ofstream out(dir / ".gitignore");
        out << "packages/\n"
            << ".venv/\n"
            << "*.ll\n"
            << "*.ast.log\n";
    }
    std::cout << "Created Quirk package '" << name << "' in " << dir.string() << "/\n"
              << "  ├── quirk.toml\n"
              << "  ├── src/index.qk\n"
              << "  ├── tests/" << name << "_test.qk\n"
              << "  └── .gitignore\n";
    return 0;
}

// Forward declaration so cmd_help can describe the full set of commands.
static void print_pm_help();

// Per-command help text. Returns empty string if `cmd` isn't recognized,
// in which case cmd_help falls back to the top-level summary.
static std::string help_for(const std::string& cmd) {
    if (cmd == "run")
        return "quirk run <file.qk> [args...]\n"
               "    Run a Quirk script. Equivalent to `quirk <file.qk>`.\n";
    if (cmd == "eval" || cmd == "-c")
        return "quirk eval \"<code>\"\n"
               "    Wrap a one-liner in `define main() { ... }` and run it.\n"
               "    Short form: quirk -c \"<code>\"\n";
    if (cmd == "module" || cmd == "-m")
        return "quirk module <name> [args...]\n"
               "    Import the named module and call its `main()`.\n"
               "    Short form: quirk -m <name>\n";
    if (cmd == "install")
        return "quirk install [-r <file>] [-e <path>] [pkg ...]\n"
               "    Install dependencies. Specs can be:\n"
               "      github.com/owner/repo[@ref]    git package\n"
               "      <path>                          local snapshot\n"
               "      <path>@<version>                pinned local\n"
               "      <name>@<range>                  switch installed version\n"
               "    -e <path>  editable (symlink) install\n"
               "    -r <file>  read deps from a manifest\n";
    if (cmd == "upgrade")
        return "quirk upgrade [pkg ...]\n"
               "    Bump packages to the latest installed version (local) or\n"
               "    re-clone HEAD (git).\n";
    if (cmd == "remove" || cmd == "uninstall")
        return "quirk remove <pkg>[@<version>] ...\n"
               "    Remove a package, or a specific version of it.\n";
    if (cmd == "list" || cmd == "packages")
        return "quirk list\n"
               "    Print installed packages with versions. Also: quirk -p\n";
    if (cmd == "show")
        return "quirk show <pkg>\n"
               "    Print the manifest of an installed package.\n";
    if (cmd == "deps")
        return "quirk deps\n"
               "    Print installed packages as `name = \"version\"` lines.\n"
               "    Redirect into a manifest's [deps] block for reproducible installs.\n";
    if (cmd == "env")
        return "quirk env\n"
               "    Dump resolution context: QUIRK_HOME, install dir, stdlib,\n"
               "    user-global dir. Use to debug \"why isn't this resolving?\"\n";
    if (cmd == "cache")
        return "quirk cache list [<pkg>]\n"
               "    List versions cached at ~/.quirk/cache/ (cross-project).\n"
               "quirk cache clean [<pkg>[@<ver>]]\n"
               "    Drop the whole cache, all versions of one package, or one version.\n"
               "quirk cache dir\n"
               "    Print the cache root path.\n";
    if (cmd == "init")
        return "quirk init\n"
               "    Scaffold a quirk.toml in the current directory.\n";
    if (cmd == "new")
        return "quirk new <name>\n"
               "    Scaffold a new package: <name>/quirk.toml, src/index.qk,\n"
               "    tests/, .gitignore.\n";
    if (cmd == "venv")
        return "quirk venv <name>\n"
               "    Create an isolated environment at ./<name>/. Activate with\n"
               "    `source <name>/bin/activate`.\n";
    if (cmd == "version" || cmd == "--version")
        return "quirk version\n"
               "    Print the Quirk compiler version.\n";
    if (cmd == "help")
        return "quirk help [command]\n"
               "    Show overall help, or the help text for one command.\n";
    if (cmd == "pkg")
        return "quirk pkg <subcommand> [args...]\n"
               "    Group prefix for package operations:\n"
               "      install, upgrade, remove, list, show, deps, cache.\n"
               "    All also accepted without the `pkg` prefix; the `pkg`\n"
               "    form is the explicit, documented one.\n";
    return "";
}

static int cmd_help(const std::vector<std::string>& args) {
    if (args.empty()) { print_pm_help(); return 0; }
    std::string detail = help_for(args[0]);
    if (detail.empty()) {
        std::cerr << "help: unknown command '" << args[0] << "'\n";
        print_pm_help();
        return 1;
    }
    std::cout << detail;
    return 0;
}

static void print_pm_help() {
    std::cout <<
        "Quirk — language toolchain & package manager\n"
        "\n"
        "Run code:\n"
        "  quirk run <file.qk> [args...]              run a Quirk script\n"
        "  quirk eval \"<code>\"                        run a one-liner (alias: -c)\n"
        "  quirk module <name>                        invoke a module's main() (alias: -m)\n"
        "\n"
        "Project / environment:\n"
        "  quirk new <name>                           scaffold a new package\n"
        "  quirk init                                 write a quirk.toml here\n"
        "  quirk venv <name>                          create an isolated environment\n"
        "  quirk env                                  print resolution context\n"
        "\n"
        "Packages (`quirk pkg <subcommand>` or flat — both work):\n"
        "  quirk pkg install [-e] [-r <file>] [spec ...]   install dependencies\n"
        "  quirk pkg upgrade [pkg ...]                bump installed versions\n"
        "  quirk pkg remove  <pkg>[@<ver>] ...        uninstall a package or version\n"
        "  quirk pkg list                             list installed packages (alias: -p)\n"
        "  quirk pkg show    <pkg>                    detailed package info\n"
        "  quirk pkg deps                             print deps in installable form\n"
        "  quirk pkg cache   list|clean|dir           manage the cross-project cache\n"
        "\n"
        "Misc:\n"
        "  quirk help [command]                       show help (per-command)\n"
        "  quirk version (or --version)               print quirk version\n"
        "\n"
        "Specs:  github.com/owner/repo[@ref]   (git: ref is a tag/branch/SHA)\n"
        "        ./path/to/lib[@<version>]    (local; @<version> can be a range)\n";
}

// Subcommands the package-manager group accepts (`quirk pkg <sub>`).
static bool is_pkg_subcommand(const std::string& arg) {
    return arg == "install" || arg == "upgrade" || arg == "remove" ||
           arg == "uninstall" || arg == "list" || arg == "packages" ||
           arg == "show" || arg == "deps" || arg == "cache";
}

// Top-level subcommand verbs (and not a path).
static bool is_subcommand(const std::string& arg) {
    return is_pkg_subcommand(arg) ||
           arg == "pkg" ||
           arg == "init" || arg == "version" || arg == "venv" ||
           arg == "run" || arg == "eval" || arg == "module" ||
           arg == "env" || arg == "new" || arg == "help";
}

// Drop argv[1] (a verb) and shift everything left. argc decreases by 1.
// Used by `run` and the `-m`/`-c` aliases: after the shift, main() sees a
// plain `quirk <something>` invocation and runs its normal compile path.
static void shift_argv(int& argc, char** argv) {
    for (int i = 1; i < argc - 1; i++) argv[i] = argv[i + 1];
    argv[argc - 1] = nullptr;
    argc--;
}

// Top-level dispatch. Returns true if we handled the command and main()
// should exit; returns the exit code via `outRc`. Returns false to let
// the rest of main() proceed (i.e. it's a script run, not a PM command).
// `argc` is taken by reference so commands like `run` and the `-m`/`-c`
// aliases can rewrite argv before letting main() carry on.
static bool dispatch(int& argc, char** argv, int& outRc) {
    if (argc < 2) return false;
    std::string first = argv[1];

    // Standalone top-level flags.
    if (first == "--version" || (argc == 2 && first == "-v")) {
        outRc = cmd_version();
        return true;
    }
    if (first == "-p" || first == "--packages") {
        outRc = cmd_list();
        return true;
    }
    if (first == "--help" || first == "-h") {
        print_pm_help();
        outRc = 0;
        return true;
    }

    // `quirk -c "<code>"` is an alias for `quirk eval`.
    // `quirk -m <name>`    is an alias for `quirk module`.
    if (first == "-c" || first == "-m") {
        std::vector<std::string> rest;
        for (int i = 2; i < argc; i++) rest.emplace_back(argv[i]);
        outRc = (first == "-c") ? cmd_eval(rest) : cmd_module(rest);
        return true;
    }

    if (!is_subcommand(first)) return false;

    // `quirk run <file>` — strip the verb and let main() take over.
    if (first == "run") {
        if (argc < 3) {
            std::cerr << "run: need a filename\n";
            outRc = 1; return true;
        }
        shift_argv(argc, argv);
        return false;
    }

    std::vector<std::string> rest;
    for (int i = 2; i < argc; i++) rest.emplace_back(argv[i]);

    // `quirk pkg <subcommand>` — re-target the dispatch so the rest works
    // the same as the flat form (kept for back-compat: `quirk install ...`).
    std::string verb = first;
    std::vector<std::string> verbArgs = rest;
    if (verb == "pkg") {
        if (rest.empty() || rest[0] == "help" || rest[0] == "--help" || rest[0] == "-h") {
            std::cout <<
                "Package management:\n"
                "  quirk pkg install [-e] [-r <file>] [spec ...]\n"
                "  quirk pkg upgrade [pkg ...]\n"
                "  quirk pkg remove <pkg>[@<ver>] ...\n"
                "  quirk pkg list                       (alias: -p)\n"
                "  quirk pkg show <pkg>\n"
                "  quirk pkg deps\n"
                "  quirk pkg cache list|clean|dir\n"
                "\n"
                "All commands also work without the `pkg` prefix, e.g.\n"
                "`quirk install slug` is the same as `quirk pkg install slug`.\n";
            outRc = 0; return true;
        }
        if (!is_pkg_subcommand(rest[0])) {
            std::cerr << "pkg: unknown subcommand '" << rest[0] << "'\n";
            outRc = 1; return true;
        }
        verb = rest[0];
        verbArgs.assign(rest.begin() + 1, rest.end());
    }

    if (verb == "install")           outRc = cmd_install(verbArgs);
    else if (verb == "upgrade")      outRc = cmd_upgrade(verbArgs);
    else if (verb == "remove" ||
             verb == "uninstall")    outRc = cmd_remove(verbArgs);
    else if (verb == "list" ||
             verb == "packages")     outRc = cmd_list();
    else if (verb == "show")         outRc = cmd_show(verbArgs);
    else if (verb == "init")         outRc = cmd_init();
    else if (verb == "new")          outRc = cmd_new(verbArgs);
    else if (verb == "venv")         outRc = cmd_venv(verbArgs);
    else if (verb == "version")      outRc = cmd_version();
    else if (verb == "eval")         outRc = cmd_eval(verbArgs);
    else if (verb == "module")       outRc = cmd_module(verbArgs);
    else if (verb == "deps")         outRc = cmd_deps();
    else if (verb == "env")          outRc = cmd_env();
    else if (verb == "cache")        outRc = cmd_cache(verbArgs);
    else if (verb == "help")         outRc = cmd_help(verbArgs);
    else { print_pm_help(); outRc = 1; }
    return true;
}

}  // namespace qpm
