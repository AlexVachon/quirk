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
#include <sys/stat.h>
#include <chrono>
#include <ctime>
#include <cctype>
#include <functional>
#include <set>
#include <map>
#include <regex>

namespace qpm {

constexpr const char* QUIRK_VERSION = "0.3.0";

namespace fs = std::filesystem;

// ---------------------- Logging ------------------------------------------
//
// One small helper used by every PM command. Three knobs:
//   - color:   ANSI escapes on when stdout is a TTY and $NO_COLOR is unset
//   - verbose: extra detail (URLs, paths, cache locations, timings)
//   - quiet:   only emit `warn`/`err`
//
// Use `log::step("Resolving", "slug 1.0.1")` for cargo-style action lines,
// `log::ok/warn/err` for the trailing per-package marker.
//
namespace log {

inline bool& verbose_flag() { static bool v = false; return v; }
inline bool& quiet_flag()   { static bool q = false; return q; }

inline bool stdout_is_tty() {
    static int cached = -1;
    if (cached < 0) cached = ::isatty(STDOUT_FILENO) ? 1 : 0;
    return cached == 1;
}
inline bool color_enabled() {
    static int cached = -1;
    if (cached < 0) {
        const char* nc = std::getenv("NO_COLOR");
        const char* fc = std::getenv("FORCE_COLOR");
        bool forced = fc && *fc && std::string(fc) != "0";
        if      (forced)        cached = 1;
        else if (nc && *nc)     cached = 0;
        else                    cached = stdout_is_tty() ? 1 : 0;
    }
    return cached == 1;
}

inline const char* RESET()   { return color_enabled() ? "\x1b[0m"  : ""; }
inline const char* BOLD()    { return color_enabled() ? "\x1b[1m"  : ""; }
inline const char* DIM()     { return color_enabled() ? "\x1b[2m"  : ""; }
inline const char* GREEN()   { return color_enabled() ? "\x1b[32m" : ""; }
inline const char* YELLOW()  { return color_enabled() ? "\x1b[33m" : ""; }
inline const char* RED()     { return color_enabled() ? "\x1b[31m" : ""; }
inline const char* CYAN()    { return color_enabled() ? "\x1b[36m" : ""; }
inline const char* BLUE()    { return color_enabled() ? "\x1b[34m" : ""; }

// `step("Resolving", "slug 1.0.1")` →  "    Resolving slug 1.0.1"
// 12-char right-aligned verb pad matches Cargo, so multiple step lines stack
// neatly: the verbs form a tidy left column and the targets line up.
inline void step(const std::string& verb, const std::string& msg) {
    if (quiet_flag()) return;
    int pad = 12 - static_cast<int>(verb.size());
    if (pad < 0) pad = 0;
    std::cout << std::string(pad, ' ')
              << GREEN() << BOLD() << verb << RESET()
              << " " << msg << "\n";
}

inline void ok(const std::string& msg) {
    if (quiet_flag()) return;
    std::cout << "  " << GREEN() << "✓" << RESET() << " " << msg << "\n";
}
inline void warn(const std::string& msg) {
    std::cerr << "  " << YELLOW() << "⚠" << RESET() << " " << msg << "\n";
}
inline void err(const std::string& msg) {
    std::cerr << "  " << RED() << "✗" << RESET() << " " << msg << "\n";
}
inline void note(const std::string& msg) {
    if (quiet_flag()) return;
    std::cout << "  " << DIM() << "·" << RESET() << " " << msg << "\n";
}
inline void download(const std::string& msg) {
    if (quiet_flag()) return;
    std::cout << "  " << CYAN() << "↓" << RESET() << " " << msg << "\n";
}

// Diagnostic detail; only printed when `-v` was passed.
inline void v(const std::string& msg) {
    if (!verbose_flag() || quiet_flag()) return;
    std::cout << "    " << DIM() << msg << RESET() << "\n";
}

inline std::string dim(const std::string& s) {
    return std::string(DIM()) + s + RESET();
}
inline std::string bold(const std::string& s) {
    return std::string(BOLD()) + s + RESET();
}

}  // namespace log

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
    std::string repository;        // git URL where this package lives
    std::string homepage;          // optional documentation/landing URL
    std::string entry;             // optional entry point override (relative path)
    std::string quirk_version;     // compiler version constraint (e.g. ">=0.2.0")
    // deps[name] = "github.com/foo/bar@v0.1.0"
    std::vector<std::pair<std::string, std::string>> deps;
    std::vector<std::pair<std::string, std::string>> dev_deps;
    std::vector<std::pair<std::string, std::string>> scripts;  // name → command
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
            if      (key == "name")          out.name = val;
            else if (key == "version")       out.version = val;
            else if (key == "description")   out.description = val;
            else if (key == "author")        out.author = val;
            else if (key == "license")       out.license = val;
            else if (key == "repository")    out.repository = val;
            else if (key == "homepage")      out.homepage = val;
            else if (key == "entry")         out.entry = val;
            else if (key == "quirk-version") out.quirk_version = val;
        } else if (section == "deps") {
            out.deps.emplace_back(key, val);
        } else if (section == "dev-deps") {
            out.dev_deps.emplace_back(key, val);
        } else if (section == "scripts") {
            out.scripts.emplace_back(key, val);
        }
    }
    return true;
}

static void write_manifest(const std::string& path, const Manifest& m) {
    std::ofstream out(path);
    out << "name          = \"" << m.name << "\"\n";
    out << "version       = \"" << m.version << "\"\n";
    if (!m.description.empty())   out << "description   = \"" << m.description   << "\"\n";
    if (!m.author.empty())        out << "author        = \"" << m.author        << "\"\n";
    if (!m.license.empty())       out << "license       = \"" << m.license       << "\"\n";
    if (!m.repository.empty())    out << "repository    = \"" << m.repository    << "\"\n";
    if (!m.homepage.empty())      out << "homepage      = \"" << m.homepage      << "\"\n";
    if (!m.entry.empty())         out << "entry         = \"" << m.entry         << "\"\n";
    if (!m.quirk_version.empty()) out << "quirk-version = \"" << m.quirk_version << "\"\n";
    out << "\n[deps]\n";
    for (auto& d : m.deps) out << d.first << " = \"" << d.second << "\"\n";
    if (!m.dev_deps.empty()) {
        out << "\n[dev-deps]\n";
        for (auto& d : m.dev_deps) out << d.first << " = \"" << d.second << "\"\n";
    }
    if (!m.scripts.empty()) {
        out << "\n[scripts]\n";
        for (auto& s : m.scripts) out << s.first << " = \"" << s.second << "\"\n";
    }
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
// in slug/src/index.quirk get `Slug_*` linkage names without needing the file
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
inline std::string resolve_self_package(const std::string& moduleName,
                                        const std::string& relativeTo) {
    if (relativeTo.empty() || moduleName.empty()) return "";
    fs::path root = find_project_root(relativeTo);
    if (root.empty()) return "";
    Manifest m;
    if (!read_manifest((root / "quirk.toml").string(), m)) return "";
    if (m.name != moduleName) return "";

    for (const fs::path& candidate : {
            root / "src" / "index.quirk",
            root / "src" / (moduleName + ".quirk"),
            root / (moduleName + ".quirk"),
            root / moduleName / "index.quirk",
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
        "#\n"
        "# Sets QUIRK_HOME + VIRTUAL_ENV, prepends bin/ to PATH, prefixes PS1,\n"
        "# and exposes `deactivate` to undo it all. Re-activating from inside\n"
        "# another Quirk venv silently switches (the inner overrides the outer\n"
        "# without nesting state — mirrors `python -m venv`).\n"
        "\n"
        "_VENV_SCRIPT=\"${BASH_SOURCE[0]:-${(%):-%x}}\"\n"
        "_VENV_DIR=\"$(cd \"$(dirname \"$_VENV_SCRIPT\")/..\" && pwd)\"\n"
        "\n"
        "# If a Quirk venv is already active, switch cleanly instead of stacking.\n"
        "if [ -n \"${_OLD_QUIRK_HOME+x}\" ] && type deactivate >/dev/null 2>&1; then\n"
        "    if [ \"$QUIRK_HOME\" = \"$_VENV_DIR\" ]; then\n"
        "        unset _VENV_SCRIPT _VENV_DIR\n"
        "        return 0 2>/dev/null || exit 0\n"
        "    fi\n"
        "    # deactivate() unsets _VENV_DIR — stash it across the call.\n"
        "    _NEW_VENV_DIR=\"$_VENV_DIR\"\n"
        "    deactivate >/dev/null 2>&1\n"
        "    _VENV_DIR=\"$_NEW_VENV_DIR\"\n"
        "    unset _NEW_VENV_DIR\n"
        "fi\n"
        "\n"
        "export _OLD_QUIRK_HOME=\"${QUIRK_HOME-__UNSET__}\"\n"
        "export _OLD_PATH=\"$PATH\"\n"
        "export _OLD_PS1=\"${PS1-}\"\n"
        "\n"
        "export QUIRK_HOME=\"$_VENV_DIR\"\n"
        "export VIRTUAL_ENV=\"$_VENV_DIR\"     # conventional; tools like Starship pick this up\n"
        "export PATH=\"$_VENV_DIR/bin:$PATH\"\n"
        "if [ -z \"${QUIRK_VENV_DISABLE_PROMPT-}\" ]; then\n"
        "    export PS1=\"(quirk:$(basename \"$_VENV_DIR\")) $PS1\"\n"
        "fi\n"
        "hash -r 2>/dev/null              # drop cached command lookups\n"
        "\n"
        "deactivate() {\n"
        "    if [ \"$_OLD_QUIRK_HOME\" = \"__UNSET__\" ]; then\n"
        "        unset QUIRK_HOME\n"
        "    else\n"
        "        export QUIRK_HOME=\"$_OLD_QUIRK_HOME\"\n"
        "    fi\n"
        "    unset VIRTUAL_ENV\n"
        "    export PATH=\"$_OLD_PATH\"\n"
        "    export PS1=\"$_OLD_PS1\"\n"
        "    hash -r 2>/dev/null\n"
        "    unset _OLD_QUIRK_HOME _OLD_PATH _OLD_PS1 _VENV_DIR _VENV_SCRIPT\n"
        "    unset -f deactivate\n"
        "}\n";
}

// Write the venv's metadata file. Modeled on Python's pyvenv.cfg: a tiny
// k=v file that records who created the venv, when, and what it linked to.
// Read by `quirk env`, `quirk venv info`, and `quirk venv repair`.
static void write_venv_cfg(const fs::path& venvDir,
                           const fs::path& stdlib, const fs::path& binPath) {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", std::gmtime(&t));

    std::ofstream cfg(venvDir / "quirk-env.cfg");
    cfg << "# Quirk virtual environment — created by `quirk venv`.\n"
        << "# Inspect with `quirk env` or `quirk venv info`.\n"
        << "version        = " << QUIRK_VERSION << "\n"
        << "stdlib-version = " << QUIRK_VERSION << "\n"  // coupled today; explicit for forward-compat
        << "created        = " << buf << "Z\n"
        << "stdlib         = " << fs::absolute(stdlib).string() << "\n"
        << "binary         = " << (binPath.empty() ? "(unknown)" : fs::absolute(binPath).string()) << "\n";
}

// Parse the cfg back as a map. Tolerates blank lines, comments, and unknown
// keys (forward-compat).
static std::map<std::string, std::string> read_venv_cfg(const fs::path& venvDir) {
    std::map<std::string, std::string> out;
    std::ifstream in(venvDir / "quirk-env.cfg");
    if (!in) return out;
    std::string line;
    while (std::getline(in, line)) {
        auto pos = line.find('#');
        if (pos != std::string::npos) line.erase(pos);
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        auto trim = [](std::string& s) {
            while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
            while (!s.empty() && std::isspace((unsigned char)s.back()))  s.pop_back();
        };
        trim(k); trim(v);
        if (!k.empty()) out[k] = v;
    }
    return out;
}

// Link/relink stdlib modules into the venv. Skips entries that already exist
// (idempotent — safe to re-run for `quirk venv repair`). Returns the number
// of links newly created (not the total).
static int link_stdlib(const fs::path& venvDir, const fs::path& stdlib) {
    fs::path target = venvDir / "lib" / "quirk" / "stdlib";
    fs::create_directories(target);
    int created = 0;
    for (auto& entry : fs::directory_iterator(stdlib)) {
        if (!entry.is_directory()) continue;
        std::string name = entry.path().filename().string();
        if (name.empty() || name[0] == '.' || name == "packages") continue;
        fs::path linkPath = target / name;
        std::error_code ec;
        if (fs::exists(linkPath) || fs::is_symlink(linkPath)) continue;
        fs::create_symlink(fs::absolute(entry.path()), linkPath, ec);
        if (!ec) created++;
    }
    return created;
}

// Internal: build the venv layout under `venvDir`. Used by `cmd_venv_new` and
// `cmd_venv_repair`. `repair` is true when called to fix an existing venv
// (won't overwrite the activate script unless missing).
static int build_venv(const fs::path& venvDir, bool repair) {
    fs::path stdlib = find_system_stdlib();
    if (stdlib.empty()) {
        log::err("cannot locate the Quirk standard library on this system");
        std::cerr << "    " << log::dim("(tried $QUIRK_HOME, sibling-of-binary, /usr/local/lib/quirk)") << "\n";
        return 1;
    }
    fs::path binPath = find_quirk_binary();

    fs::create_directories(venvDir / "bin");
    fs::create_directories(venvDir / "lib" / "quirk" / "packages");
    fs::create_directories(venvDir / "lib" / "quirk" / "stdlib");

    int linked = link_stdlib(venvDir, stdlib);

    // Symlink the compiler + runtime. On repair, replace stale symlinks so the
    // venv tracks whichever quirk binary is running now.
    auto link_bin = [&](const fs::path& from, const fs::path& to) {
        std::error_code ec;
        if (fs::is_symlink(to) || fs::exists(to)) fs::remove(to, ec);
        fs::create_symlink(fs::absolute(from), to, ec);
    };
    if (!binPath.empty()) {
        link_bin(binPath, venvDir / "bin" / "quirk");
        fs::path rt = binPath.parent_path() / "runtime.so";
        if (fs::exists(rt)) link_bin(rt, venvDir / "bin" / "runtime.so");
    }

    // Always (re)write the activate script — it's small and may have changed
    // between compiler versions.
    {
        std::ofstream activate(venvDir / "bin" / "activate");
        activate << activate_template();
    }

    // .gitignore so the venv is auto-ignored if added to a repo without one.
    if (!fs::exists(venvDir / ".gitignore")) {
        std::ofstream gi(venvDir / ".gitignore");
        gi << "*\n";  // mirror python -m venv: ignore everything inside
    }

    // Cfg always rewritten (records *current* compiler version + paths).
    write_venv_cfg(venvDir, stdlib, binPath);

    log::ok(std::string(repair ? "repaired" : "created") + " " + venvDir.string()
            + log::dim("  (" + std::to_string(linked) + " new stdlib link(s))"));
    return 0;
}

// `quirk venv new <name>` — create a new venv.
static int cmd_venv_new(const std::vector<std::string>& args) {
    if (args.empty()) {
        log::err("need a directory name (e.g. `quirk venv .venv`)");
        return 1;
    }
    fs::path venvDir = args[0];
    if (fs::exists(venvDir)) {
        log::err("'" + venvDir.string() + "' already exists");
        std::cerr << "    " << log::dim("use `quirk venv repair " + venvDir.string()
                                        + "` to fix symlinks in place") << "\n";
        return 1;
    }
    if (build_venv(venvDir, /*repair=*/false) != 0) return 1;
    std::cout << "Activate with:\n"
              << "    " << log::bold("source " + venvDir.string() + "/bin/activate") << "\n";
    return 0;
}

// `quirk venv repair [<path>]` — recreate symlinks + cfg in an existing venv.
// Useful after upgrading the compiler or moving the venv.
static int cmd_venv_repair(const std::vector<std::string>& args) {
    fs::path venvDir = args.empty() ? fs::path(".venv") : fs::path(args[0]);
    if (!fs::is_directory(venvDir)) {
        log::err("'" + venvDir.string() + "' is not a directory");
        return 1;
    }
    // Sanity-check: this should look like a venv (or about to be).
    if (!fs::exists(venvDir / "bin") && !fs::exists(venvDir / "lib" / "quirk")) {
        log::err("'" + venvDir.string() + "' doesn't look like a Quirk venv");
        std::cerr << "    " << log::dim("(no bin/ or lib/quirk/ — use `quirk venv new` to create it)") << "\n";
        return 1;
    }
    return build_venv(venvDir, /*repair=*/true);
}

// `quirk venv info [<path>]` — dump the cfg + a few derived stats.
static int cmd_venv_info(const std::vector<std::string>& args) {
    fs::path venvDir;
    if (!args.empty()) venvDir = args[0];
    else if (const char* h = std::getenv("QUIRK_HOME"); h && fs::exists(fs::path(h) / "bin" / "activate"))
        venvDir = h;
    else if (fs::is_directory(".venv")) venvDir = ".venv";
    else {
        log::err("no venv path given and no active venv");
        std::cerr << "    " << log::dim("usage: quirk venv info <path>") << "\n";
        return 1;
    }
    venvDir = fs::absolute(venvDir);
    if (!fs::exists(venvDir / "bin" / "activate")) {
        log::err("'" + venvDir.string() + "' is not a Quirk venv");
        return 1;
    }
    auto cfg = read_venv_cfg(venvDir);

    auto kv = [](const std::string& k, const std::string& v) {
        std::cout << log::dim(k + std::string(12 - k.size(), ' ')) << v << "\n";
    };
    kv("path:",     venvDir.string());
    kv("created:",  cfg.count("created") ? cfg["created"] : log::dim("(unknown — pre-cfg venv; run `quirk venv repair`)"));
    kv("compiler:", cfg.count("version") ? cfg["version"] : log::dim("(unknown)"));
    kv("stdlib v:", cfg.count("stdlib-version") ? cfg["stdlib-version"]
                  : cfg.count("version")        ? cfg["version"] + log::dim("  (coupled to compiler)")
                  : log::dim("(unknown)"));
    kv("stdlib:",   cfg.count("stdlib")  ? cfg["stdlib"]  : log::dim("(unknown)"));
    kv("binary:",   cfg.count("binary")  ? cfg["binary"]  : log::dim("(unknown)"));

    // Health check: are the symlinks still alive?
    fs::path qkBin = venvDir / "bin" / "quirk";
    std::string binStatus;
    if (!fs::is_symlink(qkBin))             binStatus = std::string(log::YELLOW()) + "missing" + log::RESET();
    else if (!fs::exists(qkBin))            binStatus = std::string(log::RED()) + "broken (target gone)" + log::RESET();
    else                                    binStatus = std::string(log::GREEN()) + "ok" + log::RESET();
    kv("bin/quirk:", binStatus);

    // Count stdlib + package symlinks.
    int stdlibCount = 0, brokenStdlib = 0;
    if (fs::is_directory(venvDir / "lib" / "quirk" / "stdlib")) {
        for (auto& e : fs::directory_iterator(venvDir / "lib" / "quirk" / "stdlib")) {
            stdlibCount++;
            if (fs::is_symlink(e.path()) && !fs::exists(e.path())) brokenStdlib++;
        }
    }
    int pkgCount = 0;
    if (fs::is_directory(venvDir / "lib" / "quirk" / "packages")) {
        for (auto& e : fs::directory_iterator(venvDir / "lib" / "quirk" / "packages")) {
            std::string fn = e.path().filename().string();
            if (fn.empty() || fn[0] == '.') continue;
            if (fn.size() > 10 && fn.substr(fn.size() - 10) == ".dist-info") continue;
            pkgCount++;
        }
    }
    std::string sl = std::to_string(stdlibCount) + " linked";
    if (brokenStdlib > 0) sl += std::string(log::RED()) + " (" + std::to_string(brokenStdlib) + " broken)" + log::RESET();
    kv("stdlib mods:", sl);
    kv("packages:", std::to_string(pkgCount) + " installed");

    if (brokenStdlib > 0 || (fs::is_symlink(qkBin) && !fs::exists(qkBin))) {
        std::cout << "\n" << log::dim("Run `quirk venv repair " + venvDir.string()
                                      + "` to fix broken symlinks.") << "\n";
    }
    return 0;
}

// `quirk venv list` — find venvs under cwd (up to 3 levels deep), one per line.
static int cmd_venv_list() {
    auto looks_like_venv = [](const fs::path& p) {
        return fs::exists(p / "bin" / "activate")
            && (fs::is_directory(p / "lib" / "quirk") || fs::is_symlink(p / "bin" / "quirk"));
    };
    std::vector<fs::path> hits;
    std::function<void(const fs::path&, int)> walk = [&](const fs::path& root, int depth) {
        if (depth > 3) return;
        std::error_code ec;
        for (auto& e : fs::directory_iterator(root, ec)) {
            if (ec) return;
            if (!e.is_directory(ec)) continue;
            std::string fn = e.path().filename().string();
            if (fn.empty() || fn == "node_modules" || fn == "packages") continue;
            // Hidden dirs only descended if they're themselves venv candidates
            // (i.e. .venv/) — saves us from grovelling through .git etc.
            if (fn[0] == '.' && !looks_like_venv(e.path())) continue;
            if (looks_like_venv(e.path())) {
                hits.push_back(fs::relative(e.path(), fs::current_path()));
                continue;          // don't descend into a venv
            }
            walk(e.path(), depth + 1);
        }
    };
    walk(fs::current_path(), 0);

    if (hits.empty()) {
        std::cout << "No venvs found under " << fs::current_path().string() << "\n";
        return 0;
    }
    const char* active = std::getenv("QUIRK_HOME");
    std::string activeAbs;
    if (active && fs::exists(fs::path(active) / "bin" / "activate"))
        activeAbs = fs::absolute(active).string();

    std::cout << log::bold(std::to_string(hits.size()) + " venv(s) under " + fs::current_path().string()) << ":\n";
    for (auto& h : hits) {
        std::string mark = "  ";
        if (!activeAbs.empty() && fs::absolute(h).string() == activeAbs) {
            mark = std::string("  ") + log::GREEN() + "*" + log::RESET() + " ";
        }
        auto cfg = read_venv_cfg(h);
        std::cout << mark << h.string();
        if (cfg.count("version")) std::cout << log::dim("  (compiler " + cfg["version"] + ")");
        std::cout << "\n";
    }
    return 0;
}

// Top-level `quirk venv ...` dispatch — subcommands or back-compat bare name.
static int cmd_venv(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
        std::cout <<
            "Virtual environments:\n"
            "  quirk venv new <path>          create a new venv\n"
            "  quirk venv list                find venvs under cwd          (alias: -l, --list)\n"
            "  quirk venv info [<path>]       show metadata + symlink health\n"
            "  quirk venv repair [<path>]     re-link stdlib/binary, refresh cfg\n"
            "  quirk venv <path>              shorthand for `venv new <path>`\n"
            "\n"
            "Activate with `source <path>/bin/activate`; deactivate with `deactivate`.\n";
        return 0;
    }
    const std::string& sub = args[0];
    std::vector<std::string> tail(args.begin() + 1, args.end());
    if (sub == "list" || sub == "-l" || sub == "--list") return cmd_venv_list();
    if (sub == "info")                                  return cmd_venv_info(tail);
    if (sub == "repair")                                return cmd_venv_repair(tail);
    if (sub == "new")                                   return cmd_venv_new(tail);
    // Back-compat: `quirk venv <path>` ≡ `quirk venv new <path>`.
    return cmd_venv_new(args);
}

// Prompt the user with an optional default. Empty input keeps the default.
// `--yes`/`-y` callers bypass this entirely.
static std::string prompt_default(const std::string& label, const std::string& def) {
    std::cout << label;
    if (!def.empty()) std::cout << " (" << def << ")";
    std::cout << ": ";
    std::cout.flush();
    std::string line;
    if (!std::getline(std::cin, line)) return def;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
        line.pop_back();
    return line.empty() ? def : line;
}

// Best-effort guess at the user's name/email from `git config`. Returns
// "Name <email>" or empty if either is missing.
static std::string guess_author() {
    auto run = [](const std::string& cmd) -> std::string {
        FILE* p = popen(cmd.c_str(), "r");
        if (!p) return "";
        char buf[256]; std::string out;
        while (fgets(buf, sizeof(buf), p)) out += buf;
        pclose(p);
        return trim(out);
    };
    std::string name  = run("git config --get user.name 2>/dev/null");
    std::string email = run("git config --get user.email 2>/dev/null");
    if (name.empty() && email.empty()) return "";
    if (email.empty()) return name;
    if (name.empty())  return email;
    return name + " <" + email + ">";
}

// Best-effort guess at the repository URL from `git remote`.
static std::string guess_repo() {
    FILE* p = popen("git remote get-url origin 2>/dev/null", "r");
    if (!p) return "";
    char buf[512]; std::string out;
    while (fgets(buf, sizeof(buf), p)) out += buf;
    pclose(p);
    return trim(out);
}

static int cmd_init(const std::vector<std::string>& args = {}) {
    fs::path mf = "quirk.toml";
    if (fs::exists(mf)) {
        std::cerr << "quirk.toml already exists.\n";
        return 1;
    }

    bool yes = false;
    for (auto& a : args) if (a == "-y" || a == "--yes") yes = true;

    Manifest m;
    m.name        = fs::current_path().filename().string();
    m.version     = "0.1.0";
    m.license     = "MIT";
    m.author      = guess_author();
    m.repository  = guess_repo();

    if (!yes) {
        std::cout << "This walks you through creating a quirk.toml.\n"
                  << "Press Enter to accept the (default) for each field.\n"
                  << "Use `quirk init -y` to skip the prompts.\n\n";
        m.name        = prompt_default("name",         m.name);
        m.version     = prompt_default("version",      m.version);
        m.description = prompt_default("description",  m.description);
        m.author      = prompt_default("author",       m.author);
        m.license     = prompt_default("license",      m.license);
        m.repository  = prompt_default("repository",   m.repository);
        m.homepage    = prompt_default("homepage",     m.homepage);
        m.entry       = prompt_default("entry point",  m.entry);
    }
    write_manifest(mf.string(), m);
    std::cout << "\nCreated quirk.toml for project '" << m.name << "'.\n";
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

// Split a spec into its package name (no @version, no path/git noise).
// Used by transitive resolution to dedup before doing any network work.
// Empty return = couldn't determine cheaply (caller should fall through
// to a real install and let `install_one` figure out the canonical name).
static std::string preview_name(const std::string& spec) {
    if (spec.empty()) return "";
    // Strip an @<version> suffix — but only when @ isn't part of an
    // ssh URL like `git@github.com:...` (which we don't currently support,
    // but cheap defensive code).
    std::string body = spec;
    auto at = body.find('@');
    if (at != std::string::npos && body.find("://") == std::string::npos
        && body.compare(0, 4, "git@") != 0) {
        body = body.substr(0, at);
    }
    if (is_local_path(body)) {
        // Read the local manifest's `name` if we can — that's authoritative.
        std::error_code ec;
        fs::path abs = fs::absolute(body, ec);
        Manifest m;
        if (!ec && read_manifest((abs / "quirk.toml").string(), m) && !m.name.empty())
            return m.name;
        // Fallback to the directory's basename.
        return fs::path(body).filename().string();
    }
    // Bare name (no slash, no protocol) — that IS the name.
    if (body.find('/') == std::string::npos && body.find("://") == std::string::npos)
        return body;
    // Looks git-like: defer to parse_spec.
    return parse_spec(spec).name;
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

// ----------------------------- Registry / aliases --------------------------
// `quirk pkg install <name>` resolves <name> via two sources, in order:
//   1. ~/.quirk/aliases.toml         — user-local name → URL mappings
//   2. ~/.quirk/registry-cache.toml  — fetched from the central registry
//
// Both files use the same format: one `name = "url[@ref]"` per line.
//
// Local aliases let users define short names without any infrastructure.
// The central registry is optional and is fetched via `quirk pkg registry
// update`. The default registry URL can be overridden in ~/.quirk/config.toml.

static constexpr const char* DEFAULT_REGISTRY_URL =
    "https://raw.githubusercontent.com/quirk-pkg/registry/main/index.toml";

static fs::path quirk_home_dir() {
    const char* h = std::getenv("HOME");
    if (!h) return {};
    fs::path q = fs::path(h) / ".quirk";
    std::error_code ec;
    fs::create_directories(q, ec);
    return q;
}

static fs::path aliases_path()        { return quirk_home_dir() / "aliases.toml"; }
static fs::path registry_cache_path() { return quirk_home_dir() / "registry-cache.toml"; }
static fs::path config_path()         { return quirk_home_dir() / "config.toml"; }

// Tiny TOML kv reader: `key = "value"` lines, sections ignored.
static std::map<std::string, std::string> read_kv_file(const fs::path& path) {
    std::map<std::string, std::string> out;
    std::ifstream in(path);
    if (!in) return out;
    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#' || t.front() == '[') continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        out[trim(t.substr(0, eq))] = unquote(trim(t.substr(eq + 1)));
    }
    return out;
}

static void write_kv_file(const fs::path& path, const std::map<std::string, std::string>& kv,
                          const std::string& header = "") {
    std::ofstream out(path);
    if (!header.empty()) out << "# " << header << "\n\n";
    for (auto& kv2 : kv) out << kv2.first << " = \"" << kv2.second << "\"\n";
}

// Accept short forms for registry URLs and expand to a fetchable raw URL:
//   github.com/owner/repo               → https://raw.githubusercontent.com/owner/repo/main/index.toml
//   github.com/owner/repo@branch        → ...same with @branch on the path
//   github.com/owner/repo/path/file.toml → ...with that path appended
//   AlexVachon/quirk-registry           → github.com/AlexVachon/quirk-registry → expanded
// Fully-qualified URLs (anything with `://`) pass through unchanged.
static std::string expand_registry_url(const std::string& raw) {
    if (raw.empty()) return raw;
    if (raw.find("://") != std::string::npos) return raw;

    // `owner/repo` (no slashes inside) — assume GitHub.
    std::string body = raw;
    if (body.find("github.com/") != 0 && body.find('/') != std::string::npos
        && body.find('/') == body.rfind('/')) {
        body = "github.com/" + body;
    }
    if (body.rfind("github.com/", 0) != 0) {
        // Not a recognized shorthand; pass through.
        return raw;
    }

    std::string rest = body.substr(11);           // strip "github.com/"
    std::string branch = "main";
    auto at = rest.find('@');
    if (at != std::string::npos) {
        branch = rest.substr(at + 1);
        rest   = rest.substr(0, at);
    }

    // owner/repo               → owner/repo/<branch>/index.toml
    // owner/repo/path/to.toml  → owner/repo/<branch>/path/to.toml
    size_t slash1 = rest.find('/');
    size_t slash2 = (slash1 != std::string::npos) ? rest.find('/', slash1 + 1) : std::string::npos;
    std::string path = (slash2 == std::string::npos) ? "/index.toml"
                                                     : "/" + rest.substr(slash2 + 1);
    std::string ownerRepo = (slash2 == std::string::npos) ? rest : rest.substr(0, slash2);
    return "https://raw.githubusercontent.com/" + ownerRepo + "/" + branch + path;
}

// Resolved registry URL — user override in ~/.quirk/config.toml, else default.
static std::string resolve_registry_url() {
    auto cfg = read_kv_file(config_path());
    auto it = cfg.find("registry");
    if (it != cfg.end()) return expand_registry_url(it->second);
    return expand_registry_url(DEFAULT_REGISTRY_URL);
}

// Look up <name> in user aliases first, then the cached registry.
// Returns the URL/spec the user should install, or empty if not found.
static std::string registry_lookup(const std::string& name) {
    auto aliases = read_kv_file(aliases_path());
    auto it = aliases.find(name);
    if (it != aliases.end()) return it->second;
    auto cache = read_kv_file(registry_cache_path());
    it = cache.find(name);
    if (it != cache.end()) return it->second;
    return "";
}

// Forward decl: cache_dir() is defined further down, but tag-discovery
// helpers below need the path.
static fs::path cache_dir();

// ----------------------- Lockfile (quirk.lock) -------------------------
// Records the exact source + commit each package resolved to, so a second
// machine installing from the same manifest gets bit-identical results.

struct LockEntry {
    std::string name;
    std::string version;
    std::string source;   // verbatim spec the user/manifest wrote
    std::string commit;   // git SHA, empty for non-git installs
    bool operator==(const LockEntry& o) const {
        return name == o.name && version == o.version
            && source == o.source && commit == o.commit;
    }
    bool operator!=(const LockEntry& o) const { return !(*this == o); }
};

// Read quirk.lock as a name-indexed map. Returns empty if no lock or unparseable.
static std::map<std::string, LockEntry> read_lockfile(const fs::path& path) {
    std::map<std::string, LockEntry> out;
    std::ifstream in(path);
    if (!in) return out;
    LockEntry cur;
    bool inPkg = false;
    auto flush = [&]() {
        if (inPkg && !cur.name.empty()) out[cur.name] = cur;
        cur = LockEntry{};
        inPkg = false;
    };
    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        if (t == "[[package]]") { flush(); inPkg = true; continue; }
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(t.substr(0, eq));
        std::string v = unquote(trim(t.substr(eq + 1)));
        if (!inPkg) continue;   // ignore top-level keys (e.g. `version = 1`)
        if      (k == "name")    cur.name    = v;
        else if (k == "version") cur.version = v;
        else if (k == "source")  cur.source  = v;
        else if (k == "commit")  cur.commit  = v;
    }
    flush();
    return out;
}

static void write_lockfile(const fs::path& path,
                           const std::map<std::string, LockEntry>& entries) {
    std::ofstream out(path);
    out << "# quirk.lock — generated by `quirk pkg install`. Do not edit.\n"
        << "version = 1\n\n";
    // Stable order: sorted by name. std::map is already sorted.
    for (auto& kv : entries) {
        auto& e = kv.second;
        out << "[[package]]\n"
            << "name    = \"" << e.name    << "\"\n"
            << "version = \"" << e.version << "\"\n"
            << "source  = \"" << e.source  << "\"\n";
        if (!e.commit.empty())
            out << "commit  = \"" << e.commit << "\"\n";
        out << "\n";
    }
}

// ----------------------- Tag discovery & tarball install ---------------
// For GitHub URLs we can:
//   - List versions: `git ls-remote --tags <url>` (or GitHub API)
//   - Download a specific version's source as a tarball — no git clone,
//     no .git directory, no full history. Much faster.
// Non-GitHub URLs fall through to the existing git-clone path.

// Parse "github.com/owner/repo[.git]" out of an arbitrary spec URL.
// Returns "owner/repo" or empty string if it isn't a GitHub URL.
static std::string github_owner_repo(const std::string& url) {
    std::string s = url;
    auto p = s.find("://"); if (p != std::string::npos) s = s.substr(p + 3);
    // SSH form: git@github.com:owner/repo.git → strip prefix + replace ':'
    if (s.rfind("git@github.com:", 0) == 0) {
        s = "github.com/" + s.substr(15);
    }
    if (s.rfind("github.com/", 0) != 0) return "";
    s = s.substr(11);
    if (s.size() > 4 && s.substr(s.size() - 4) == ".git")
        s = s.substr(0, s.size() - 4);
    // Strip trailing slashes / fragments
    auto q = s.find_first_of(" \t?#");
    if (q != std::string::npos) s = s.substr(0, q);
    // Must look like owner/repo
    auto slash = s.find('/');
    if (slash == std::string::npos) return "";
    return s;
}

// Per-URL tag-list cache file. Sanitized owner-repo as filename.
static fs::path tags_cache_path(const std::string& ownerRepo) {
    fs::path c = cache_dir();
    if (c.empty()) return {};
    fs::path dir = c / "_tags";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string fn = ownerRepo;
    for (char& ch : fn) if (ch == '/') ch = '-';
    return dir / (fn + ".tags");
}

// Run `git ls-remote --tags <url>` and return sorted tag names (semver-desc).
// Caches the result for 24h.
static std::vector<std::string> discover_tags(const std::string& gitUrl, bool forceRefresh = false) {
    std::vector<std::string> tags;
    std::string ownerRepo = github_owner_repo(gitUrl);
    if (ownerRepo.empty()) {
        // Non-GitHub: still works, just no separate cache file (use a sanitized URL)
        std::string sane = gitUrl;
        for (char& c : sane) if (c == '/' || c == ':') c = '-';
        ownerRepo = sane;
    }
    fs::path cache = tags_cache_path(ownerRepo);

    // Use cached list if fresh enough (24h). Simple `stat`-based check.
    if (!forceRefresh && !cache.empty() && fs::exists(cache)) {
        struct stat st;
        if (::stat(cache.c_str(), &st) == 0) {
            auto now = ::time(nullptr);
            if (now - st.st_mtime < 24 * 60 * 60) {
                std::ifstream in(cache);
                std::string line;
                while (std::getline(in, line)) {
                    if (!line.empty()) tags.push_back(line);
                }
                return tags;
            }
        }
    }

    // Run `git ls-remote --tags --refs <url>` and parse `<sha>\trefs/tags/<name>`.
    std::string url = gitUrl;
    if (url.find("://") == std::string::npos && url.rfind("git@", 0) != 0) {
        url = "https://" + url;
        if (url.size() < 4 || url.substr(url.size() - 4) != ".git") url += ".git";
    }
    std::string cmd = "git ls-remote --tags --refs \"" + url + "\" 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return tags;
    char buf[1024];
    while (fgets(buf, sizeof(buf), p)) {
        std::string line(buf);
        auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        std::string ref = line.substr(tab + 1);
        const std::string marker = "refs/tags/";
        auto m = ref.find(marker);
        if (m == std::string::npos) continue;
        std::string tag = ref.substr(m + marker.size());
        while (!tag.empty() && (tag.back() == '\n' || tag.back() == '\r')) tag.pop_back();
        if (!tag.empty()) tags.push_back(tag);
    }
    pclose(p);

    // Sort latest-first using version comparison (strip leading `v`).
    std::sort(tags.begin(), tags.end(), [](const std::string& a, const std::string& b) {
        auto strip = [](std::string s) { return (!s.empty() && s[0] == 'v') ? s.substr(1) : s; };
        return compare_versions(strip(a), strip(b)) > 0;
    });

    // Cache the result.
    if (!cache.empty()) {
        std::ofstream out(cache);
        for (auto& t : tags) out << t << "\n";
    }
    return tags;
}

// Try the GitHub codeload tarball for a single exact ref. Returns 0 on
// success. Wrapped by `download_github_tarball` which retries with/without
// the `v` prefix.
static int try_codeload_tarball(const std::string& ownerRepo, const std::string& ref,
                                const fs::path& target, bool quiet) {
    // codeload accepts both refs/tags/X and bare refs (resolves to tag or branch).
    std::string url = "https://codeload.github.com/" + ownerRepo + "/tar.gz/"
                    + (ref.empty() ? "HEAD" : ref);
    fs::path tmpTar = fs::temp_directory_path()
        / ("quirk_dl_" + std::to_string(getpid()) + ".tar.gz");
    fs::path extractDir = fs::temp_directory_path()
        / ("quirk_x_"  + std::to_string(getpid()));
    std::error_code ec;
    fs::remove_all(extractDir, ec);
    fs::create_directories(extractDir, ec);

    // Always silence curl — its 404 is normal when probing tag-name variants
    // (e.g. "1.0.0" vs "v1.0.0"). Caller decides what to do on failure.
    std::string curl = "curl -fsSL \"" + url + "\" -o \"" + tmpTar.string() + "\" 2>/dev/null";
    if (std::system(curl.c_str()) != 0) {
        fs::remove(tmpTar, ec);
        return 1;
    }
    std::string tar = "tar -xzf \"" + tmpTar.string()
                    + "\" -C \"" + extractDir.string() + "\"";
    if (quiet) tar += " 2>/dev/null";
    if (std::system(tar.c_str()) != 0) {
        fs::remove(tmpTar, ec);
        fs::remove_all(extractDir, ec);
        return 1;
    }

    // The tar contains a single `<repo>-<ref>/` directory. Move it to target.
    fs::path inner;
    for (auto& e : fs::directory_iterator(extractDir)) {
        if (e.is_directory()) { inner = e.path(); break; }
    }
    if (inner.empty()) {
        fs::remove(tmpTar, ec);
        fs::remove_all(extractDir, ec);
        return 1;
    }
    fs::remove_all(target, ec);
    fs::create_directories(target.parent_path(), ec);
    fs::rename(inner, target, ec);
    if (ec) {
        // Cross-device — copy + remove.
        fs::copy(inner, target,
                 fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
        fs::remove_all(inner);
    }
    fs::remove(tmpTar, ec);
    fs::remove_all(extractDir, ec);
    return 0;
}

// Download a tarball, being lenient about the `v` prefix. Git-tag convention
// is `v1.0.0`, but users habitually type `1.0.0` in install commands (and
// some communities skip the prefix entirely). We try the user's input first,
// then the other form. On success, `actualRef` is set to whichever form
// worked (useful for messages).
static int download_github_tarball(const std::string& ownerRepo, const std::string& ref,
                                   const fs::path& target, bool quiet,
                                   std::string* actualRef = nullptr) {
    if (try_codeload_tarball(ownerRepo, ref, target, quiet) == 0) {
        if (actualRef) *actualRef = ref;
        return 0;
    }
    // Retry with the opposite v-prefix variant when the ref looks version-like.
    if (!ref.empty()) {
        std::string alt;
        if (std::isdigit((unsigned char)ref[0])) alt = "v" + ref;
        else if (ref[0] == 'v' && ref.size() > 1
                 && std::isdigit((unsigned char)ref[1])) alt = ref.substr(1);
        if (!alt.empty()
            && try_codeload_tarball(ownerRepo, alt, target, quiet) == 0) {
            if (actualRef) *actualRef = alt;
            return 0;
        }
    }
    return 1;
}

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

// Write the metadata sidecar. `commit` is the cloned commit SHA (empty for
// non-git installs).
static void write_dist_info(const fs::path& pkgRoot, const Manifest& m, bool editable,
                            const std::string& commit = "") {
    fs::path d = dist_info_dir(pkgRoot, m.name, m.version);
    fs::create_directories(d);
    std::ofstream out(d / "METADATA");
    out << "Name: "      << m.name    << "\n"
        << "Version: "   << m.version << "\n";
    if (!m.description.empty())   out << "Summary: "        << m.description   << "\n";
    if (!m.author.empty())        out << "Author: "         << m.author        << "\n";
    if (!m.license.empty())       out << "License: "        << m.license       << "\n";
    if (!m.repository.empty())    out << "Repository: "     << m.repository    << "\n";
    if (!m.homepage.empty())      out << "Homepage: "       << m.homepage      << "\n";
    if (!m.quirk_version.empty()) out << "Requires-Quirk: " << m.quirk_version << "\n";
    if (!commit.empty())          out << "Commit: "         << commit          << "\n";
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

// Reject the install if the package declares a `quirk-version` constraint
// the running compiler doesn't satisfy. Empty constraint = no opinion.
static int check_quirk_version(const Manifest& m) {
    if (m.quirk_version.empty()) return 0;
    if (version_satisfies(QUIRK_VERSION, m.quirk_version)) return 0;
    std::cerr << "install: " << m.name << " requires quirk " << m.quirk_version
              << " (running " << QUIRK_VERSION << ")\n";
    return 1;
}

// Dev-only top-level entries we strip from installed packages. The cache
// keeps a faithful mirror; this filter only runs at install-into-packages/
// time. Editable installs aren't filtered (they're symlinks to source).
static bool is_dev_only_entry(const std::string& name) {
    return name == "tests" || name == "test" ||
           name == ".git"  || name == ".github" ||
           name == ".vscode" || name == ".idea";
}

// Copy <src> → <dst> recursively, skipping dev-only entries at the top level.
static std::error_code copy_pruned(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    fs::create_directories(dst, ec);
    if (ec) return ec;
    for (auto& e : fs::directory_iterator(src, ec)) {
        if (ec) return ec;
        std::string n = e.path().filename().string();
        if (is_dev_only_entry(n)) continue;
        fs::copy(e.path(), dst / n,
                 fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
        if (ec) return ec;
    }
    return ec;
}

// Internal: replace packages/<name>/ with content from the cache entry.
//
// Atomicity: a copy that fails partway used to leave the user without their
// old version AND with a half-written new one. We now stage to a sibling
// `.<name>.__staging.<pid>/` and only swap into place once the copy
// completes. The window where `target` doesn't exist is now the single
// `rename(2)` call, not the duration of the copy.
//
// On crash mid-copy, leftover staging dirs are reclaimed by `remove_all` on
// the next install of the same package (we re-prep an empty staging path).
static int materialize_from_cache(const fs::path& cachePath, const fs::path& pkgRoot,
                                  const Manifest& m, bool editable,
                                  const std::string& commit = "") {
    fs::path target  = pkgRoot / m.name;
    fs::path staging = pkgRoot / ("." + m.name + ".__staging." + std::to_string(getpid()));
    std::error_code ec;
    fs::remove_all(staging, ec);   // clear any debris from a prior aborted run

    if (editable) {
        // Editable installs symlink the *source*, not the cache. No copy →
        // nothing to stage; just replace the target directly.
        if (fs::exists(target) || fs::is_symlink(target)) fs::remove_all(target);
        fs::create_directory_symlink(cachePath, target, ec);
        if (ec) {
            log::err("failed to install " + target.string() + ": " + ec.message());
            return 1;
        }
    } else {
        // Copy into staging first; the old target stays intact while we work.
        ec = copy_pruned(cachePath, staging);
        if (ec) {
            log::err("failed to stage " + m.name + ": " + ec.message());
            fs::remove_all(staging);
            return 1;
        }
        // Atomic swap: only at this single instant is `target` gone.
        if (fs::exists(target) || fs::is_symlink(target)) fs::remove_all(target);
        fs::rename(staging, target, ec);
        if (ec) {
            log::err("failed to swap " + m.name + " into place: " + ec.message());
            fs::remove_all(staging);
            return 1;
        }
    }
    clear_dist_info(pkgRoot, m.name);
    write_dist_info(pkgRoot, m, editable, commit);
    return 0;
}

static int install_local(const std::string& path_spec, bool editable,
                         const std::string& pinVersion = "",
                         LockEntry* outLock = nullptr) {
    fs::path src = path_spec;
    if (!path_spec.empty() && path_spec[0] == '~') {
        const char* home = std::getenv("HOME");
        if (home) src = fs::path(home) / path_spec.substr(path_spec.size() > 1 && path_spec[1] == '/' ? 2 : 1);
    }
    std::error_code ec;
    src = fs::absolute(src, ec);
    if (ec || !fs::is_directory(src)) {
        log::err("'" + path_spec + "' is not a directory");
        return 1;
    }
    Manifest m;
    if (!read_manifest((src / "quirk.toml").string(), m) || m.name.empty()) {
        log::err("no quirk.toml (or missing `name =`) in " + src.string());
        return 1;
    }
    if (m.version.empty()) m.version = "0.0.0";
    if (check_quirk_version(m) != 0) return 1;

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
            log::v("cache hit: " + entry.string());
            if (materialize_from_cache(entry, pkgRoot, mCached, false) != 0) return 1;
            log::ok(m.name + " " + cached + log::dim(" (from cache)"));
            if (outLock) *outLock = {m.name, cached, path_spec, ""};
            return 0;
        }
    }

    if (!pinVersion.empty() && !version_satisfies(m.version, pinVersion)) {
        log::err("requested " + m.name + "@" + pinVersion
                 + " but source declares version " + m.version);
        return 1;
    }

    // Populate the cache for this (name, version) if missing. Editable skips
    // the cache (it'd be a symlink to the source — pointless to cache).
    if (!editable) {
        fs::path entry = cache_entry(m.name, m.version);
        if (!entry.empty() && !fs::exists(entry)) {
            fs::create_directories(entry.parent_path());
            log::v("caching " + src.string() + " → " + entry.string());
            fs::copy(src, entry,
                     fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
            if (ec) {
                log::err("failed to cache " + src.string() + " → " + entry.string()
                         + ": " + ec.message());
                fs::remove_all(entry);
                return 1;
            }
            fs::remove_all(entry / ".git");
        } else {
            log::v("cache already has " + m.name + "-" + m.version);
        }
        if (materialize_from_cache(entry, pkgRoot, m, false) != 0) return 1;
        log::ok(m.name + " " + m.version + log::dim(" (snapshot from " + src.string() + ")"));
    } else {
        if (materialize_from_cache(src, pkgRoot, m, true) != 0) return 1;
        log::ok(m.name + " " + m.version + log::dim(" (editable from " + src.string() + ")"));
    }
    if (outLock) *outLock = {m.name, m.version, path_spec, ""};
    return 0;
}

static int install_one(const std::string& spec_str, bool quiet, bool editable = false,
                       LockEntry* outLock = nullptr) {
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
    if (is_local_path(pathPart)) return install_local(pathPart, editable, pinVersion, outLock);

    // Bare-name install: `quirk install <name>[@<range>]`.
    // Two-step resolution:
    //   1. If a pin is given AND we have a matching cached version, switch
    //      (instant; same as before — no network).
    //   2. Otherwise, look the name up via aliases / registry and recurse
    //      with the resolved URL spec.
    if (!isPathLike && !looksLikeGitSpec) {
        if (!pinVersion.empty()) {
            std::string chosen = pick_cached_version(pathPart, pinVersion);
            if (!chosen.empty()) {
                fs::path entry = cache_entry(pathPart, chosen);
                Manifest m;
                if (!read_manifest((entry / "quirk.toml").string(), m)) {
                    m.name = pathPart; m.version = chosen;
                }
                fs::path pkgRoot = package_install_dir();
                fs::create_directories(pkgRoot);
                log::v("cache hit: " + entry.string());
                if (materialize_from_cache(entry, pkgRoot, m, false) != 0) return 1;
                log::ok(pathPart + " " + chosen + log::dim(" (from cache)"));
                if (outLock) *outLock = {pathPart, chosen, spec_str, ""};
                return 0;
            }
        }
        // Try the registry / aliases.
        log::v("registry lookup: " + pathPart);
        std::string resolved = registry_lookup(pathPart);
        if (!resolved.empty()) {
            log::v("resolved → " + resolved);
            // User pin > registry default pin. Strip any `@…` already on the
            // resolved URL and replace it with the user's pin if they gave one.
            std::string full = resolved;
            if (!pinVersion.empty()) {
                auto at = full.find('@');
                if (at != std::string::npos) full = full.substr(0, at);
                full += "@" + pinVersion;
            }
            int rc = install_one(full, quiet, editable, outLock);
            // Preserve the original short-name spec in the lock entry.
            if (rc == 0 && outLock) outLock->source = spec_str;
            return rc;
        }
        if (!pinVersion.empty()) {
            log::err(pathPart + " not in registry or cache");
            std::cerr << "    " << log::dim("hint: `quirk pkg cache list " + pathPart
                                            + "` shows what's cached") << "\n";
            std::cerr << "    " << log::dim("hint: `quirk pkg registry add " + pathPart
                                            + " <url>` registers a name") << "\n";
            return 1;
        }
        log::err(pathPart + " is not a known package");
        std::cerr << "    " << log::dim("hint: `quirk pkg registry add " + pathPart
                                        + " github.com/owner/repo`") << "\n";
        std::cerr << "    " << log::dim("hint: or `quirk pkg install github.com/owner/repo`") << "\n";
        return 1;
    }

    PkgSpec spec = parse_spec(spec_str);
    fs::path pkgRoot = package_install_dir();
    fs::create_directories(pkgRoot);

    // If the user gave a range/version spec but no explicit ref, resolve
    // against the published tags (so `slug@'>=0.1,<1.0'` picks the highest
    // matching tag without us having to clone).
    std::string ownerRepo = github_owner_repo(spec.url);
    if (!ownerRepo.empty()
        && !spec.ref.empty()
        && spec.ref.find_first_of("=<>!,") != std::string::npos) {
        log::step("Resolving", spec.name + log::dim("  " + spec.ref + "  → tag listing"));
        std::vector<std::string> tags = discover_tags(spec.url);
        log::v(std::to_string(tags.size()) + " tag(s) discovered for " + spec.url);
        std::string best;
        for (auto& t : tags) {
            std::string stripped = (!t.empty() && t[0] == 'v') ? t.substr(1) : t;
            if (!version_satisfies(stripped, spec.ref)) continue;
            if (best.empty()
                || compare_versions(stripped[0]=='v'?stripped.substr(1):stripped,
                                    best[0]=='v'?best.substr(1):best) > 0) {
                best = t;
            }
        }
        if (best.empty()) {
            log::err(std::string("no tag of ") + spec.url + " satisfies '" + spec.ref + "'");
            std::cerr << "    " << log::dim("available:");
            for (size_t i = 0; i < tags.size() && i < 8; i++)
                std::cerr << " " << tags[i];
            if (tags.size() > 8) std::cerr << " …";
            std::cerr << "\n";
            return 1;
        }
        log::v("selected " + best + " (highest matching tag)");
        spec.ref = best;
    }

    // Tarball-first download for GitHub URLs (faster, no git required at
    // runtime, no .git dir to strip). Falls back to `git clone` otherwise.
    fs::path tmp = fs::temp_directory_path()
        / ("quirk_dl_" + spec.name + "_" + std::to_string(getpid()));
    std::error_code ec;
    fs::remove_all(tmp, ec);

    log::step("Downloading",
              spec.name + (spec.ref.empty() ? "" : " " + spec.ref)
              + log::dim("  " + spec.url));

    bool usedTarball = false;
    if (!ownerRepo.empty()) {
        std::string actual;
        if (download_github_tarball(ownerRepo, spec.ref, tmp, quiet, &actual) == 0) {
            usedTarball = true;
            spec.ref = actual;     // canonicalize for the success message
            log::v("tarball: codeload.github.com/" + ownerRepo + "/tar.gz/" + actual);
        } else if (!spec.ref.empty()) {
            log::err(std::string("tarball download failed (does tag/branch '")
                     + spec.ref + "' exist?)");
            std::cerr << "    " << log::dim("hint: `quirk pkg versions " + ownerRepo
                                            + "` shows what's published") << "\n";
            return 1;
        }
        // If no ref and tarball failed, fall through to git clone.
    }
    if (!usedTarball) {
        log::v("falling back to `git clone` for " + spec.url);
        auto run_clone = [&](const std::string& ref) -> int {
            std::string cmd = "git -c advice.detachedHead=false clone --quiet --depth 1";
            if (!ref.empty()) cmd += " --branch \"" + ref + "\"";
            cmd += " \"" + spec.url + "\" \"" + tmp.string() + "\"";
            cmd += " 2>/dev/null";
            return std::system(cmd.c_str());
        };
        int rc = run_clone(spec.ref);
        if (rc != 0 && !spec.ref.empty()) {
            std::error_code ec; fs::remove_all(tmp, ec);
            // Retry with the opposite v-prefix variant for version-like refs.
            std::string alt;
            if (std::isdigit((unsigned char)spec.ref[0])) alt = "v" + spec.ref;
            else if (spec.ref[0] == 'v' && spec.ref.size() > 1
                     && std::isdigit((unsigned char)spec.ref[1])) alt = spec.ref.substr(1);
            if (!alt.empty() && run_clone(alt) == 0) {
                spec.ref = alt;
                rc = 0;
            }
        }
        if (rc != 0) {
            log::err("failed to clone " + spec.name);
            fs::remove_all(tmp);
            return rc;
        }
    }
    Manifest m;
    if (!read_manifest((tmp / "quirk.toml").string(), m) || m.name.empty()) {
        log::err(spec.name + ": cloned repo has no quirk.toml (or missing `name =`)");
        fs::remove_all(tmp);
        return 1;
    }
    if (m.version.empty()) m.version = "0.0.0";
    if (check_quirk_version(m) != 0) { fs::remove_all(tmp); return 1; }

    // Capture the commit SHA before stripping `.git/` (tarball downloads
    // don't have a .git, so commit stays empty for those).
    std::string commit = usedTarball ? "" : git_head(tmp);
    fs::remove_all(tmp / ".git");

    // Populate cache for (name, version) if missing; otherwise drop the
    // fresh clone and use what we already cached.
    fs::path entry = cache_entry(m.name, m.version);
    if (entry.empty()) {
        std::cerr << "install: cannot determine cache directory (HOME unset?)\n";
        fs::remove_all(tmp);
        return 1;
    }
    if (!fs::exists(entry)) {
        fs::create_directories(entry.parent_path());
        log::v("caching to " + entry.string());
        fs::rename(tmp, entry, ec);
        if (ec) {
            // rename failed (likely cross-device); fall back to copy.
            fs::copy(tmp, entry,
                     fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
            fs::remove_all(tmp);
            if (ec) {
                log::err("failed to cache " + spec.name + ": " + ec.message());
                return 1;
            }
        }
    } else {
        log::v("cache already has " + m.name + "-" + m.version);
        fs::remove_all(tmp);
    }

    log::v("materializing into " + (pkgRoot / m.name).string());
    if (materialize_from_cache(entry, pkgRoot, m, false, commit) != 0) return 1;
    std::string okMsg = m.name + " " + m.version;
    if (!commit.empty()) okMsg += log::dim(" (" + commit.substr(0, 7) + ")");
    log::ok(okMsg);
    if (outLock) *outLock = {m.name, m.version, spec_str, commit};
    return 0;
}

// Does an installed `version` satisfy the @<pin> appended to a spec?
// Used for transitive conflict checks — we cheap-compare a new constraint
// against an already-resolved version without re-installing.
static bool pin_matches_version(const std::string& pin, const std::string& version) {
    if (pin.empty()) return true;
    if (pin.find_first_of("=<>!,") != std::string::npos)
        return version_satisfies(version, pin);
    // Literal version-or-tag (v1.0.0 vs 1.0.0): strip the `v`.
    std::string p = (!pin.empty() && pin[0] == 'v') ? pin.substr(1) : pin;
    std::string v = (!version.empty() && version[0] == 'v') ? version.substr(1) : version;
    return p == v;
}

// Pull the `@<pin>` part off the *end* of a spec (after the last `@` that
// isn't part of `://` or `git@`). Returns empty if there's no pin.
static std::string pin_of_spec(const std::string& spec) {
    if (spec.empty()) return "";
    // Skip a possible ssh-style `git@host:path` prefix.
    size_t startAt = 0;
    if (spec.compare(0, 4, "git@") == 0) startAt = 4;
    // Skip past `scheme://`.
    auto scheme = spec.find("://", startAt);
    if (scheme != std::string::npos) startAt = scheme + 3;
    auto at = spec.find('@', startAt);
    if (at == std::string::npos) return "";
    return spec.substr(at + 1);
}

static int cmd_install(const std::vector<std::string>& args) {
    // ---- Argument parsing ------------------------------------------------
    // Recognized flags: -r/--read <file>, -e/--editable <path>, --dev,
    // --frozen, --no-lock. Anything else is a positional package spec.
    std::string manifestFile;
    std::vector<std::string> specs;
    std::set<std::string> editableSpecs;
    bool includeDevDeps = false;
    bool frozen = false;
    bool noLock = false;
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "-r" || args[i] == "--read") {
            if (i + 1 >= args.size()) { std::cerr << "install: -r requires a filename\n"; return 1; }
            manifestFile = args[++i];
        } else if (args[i] == "-e" || args[i] == "--editable") {
            if (i + 1 >= args.size()) { std::cerr << "install: -e requires a local path\n"; return 1; }
            const std::string& p = args[++i];
            specs.push_back(p);
            editableSpecs.insert(p);
        } else if (args[i] == "--dev")    includeDevDeps = true;
        else if (args[i] == "--frozen")   frozen = true;
        else if (args[i] == "--no-lock")  noLock = true;
        else                              specs.push_back(args[i]);
    }
    if (specs.empty() && manifestFile.empty()) manifestFile = "quirk.toml";

    fs::path lockPath = fs::path(manifestFile.empty() ? "quirk.toml" : manifestFile)
                            .parent_path() / "quirk.lock";
    auto lock         = read_lockfile(lockPath);
    auto originalLock = lock;
    bool hadLockfile  = !lock.empty() || fs::exists(lockPath);

    // ---- BFS resolver state ----------------------------------------------
    // `processed[name] = (version, source-spec-used)` for every package
    // we've finished installing in this run. Used for (a) dedup — each name
    // is installed exactly once, and (b) conflict detection — if a later
    // queued spec pins a version the already-installed one doesn't match.
    std::map<std::string, std::pair<std::string, std::string>> processed;
    struct Pending { std::string spec; std::string via; bool editable; };
    std::vector<Pending> queue;
    auto enqueue = [&](const std::string& s, const std::string& via, bool ed) {
        queue.push_back({s, via, ed});
    };

    // Materialize one queued spec. Sets processed/lock on success; queues
    // the installed package's manifest [deps] for further processing.
    // Returns 0 on success, 1 on failure, 2 if skipped (already processed).
    auto install_pending = [&](const Pending& p) -> int {
        std::string name = preview_name(p.spec);

        // (a) Dedup + conflict: have we already resolved this name?
        if (!name.empty()) {
            auto it = processed.find(name);
            if (it != processed.end()) {
                std::string pin = pin_of_spec(p.spec);
                if (!pin.empty() && !pin_matches_version(pin, it->second.first)) {
                    log::err("conflicting constraint on " + name
                             + ": " + it->second.first + " (already selected)"
                             + " doesn't satisfy '" + pin + "'"
                             + (p.via.empty() ? "" : " " + log::dim("(via " + p.via + ")")));
                    return 1;
                }
                return 2;  // already done, no work to do
            }
        }

        // (b) --frozen: every package must be present in the lockfile.
        if (frozen && !name.empty() && !lock.count(name)) {
            log::err("--frozen: '" + name + "' not in quirk.lock"
                     + (p.via.empty() ? "" : log::dim(" (via " + p.via + ")")));
            return 1;
        }

        // (c) Lockfile fast-path: if we know `name` and it's locked, install
        // at the pinned version. This is what makes `--frozen` reproducible
        // and gives the rest of the run a stable target.
        if (!name.empty() && lock.count(name)) {
            const LockEntry& le = lock[name];
            std::string src = le.source;
            auto at = src.find('@');
            if (at != std::string::npos) src = src.substr(0, at);
            std::string pin = le.commit.empty() ? le.version : le.commit;
            std::string lockedSpec = src + "@" + pin;
            LockEntry e;
            if (install_one(lockedSpec, false, p.editable, &e) == 0) {
                processed[e.name] = {e.version, lockedSpec};
                lock[e.name] = e;
                // Recurse into the installed package's deps.
                fs::path pkgDir = package_install_dir() / e.name;
                Manifest installed;
                if (read_manifest((pkgDir / "quirk.toml").string(), installed)) {
                    for (auto& dep : installed.deps) enqueue(dep.second, e.name, false);
                }
                return 0;
            }
            if (frozen) {
                log::err("--frozen: failed to install " + name + " at locked pin " + pin);
                return 1;
            }
            log::warn(name + ": locked version no longer installable, re-resolving");
        }

        // (d) Fresh install (no lock, or lock fast-path fell through).
        LockEntry e;
        int rc = install_one(p.spec, false, p.editable, &e);
        if (rc != 0) return 1;
        processed[e.name] = {e.version, p.spec};
        lock[e.name] = e;
        fs::path pkgDir = package_install_dir() / e.name;
        Manifest installed;
        if (read_manifest((pkgDir / "quirk.toml").string(), installed)) {
            for (auto& dep : installed.deps) enqueue(dep.second, e.name, false);
        }
        return 0;
    };

    // ---- Seed the queue --------------------------------------------------
    auto t0 = std::chrono::steady_clock::now();
    size_t directCount = 0;
    if (!manifestFile.empty()) {
        Manifest m;
        if (!read_manifest(manifestFile, m)) {
            log::err("cannot read manifest '" + manifestFile + "'");
            return 1;
        }
        size_t total = m.deps.size() + (includeDevDeps ? m.dev_deps.size() : 0);
        if (total == 0) {
            std::string msg = "no dependencies declared in " + manifestFile;
            if (!includeDevDeps && !m.dev_deps.empty())
                msg += " (use --dev to include " + std::to_string(m.dev_deps.size()) + " dev-deps)";
            log::note(msg);
            return 0;
        }
        if (frozen && !hadLockfile) {
            log::err("--frozen requires an existing quirk.lock");
            return 1;
        }
        log::step("Installing", std::to_string(total) + " package(s) "
                  + log::dim("from " + manifestFile));
        for (auto& d : m.deps)     { enqueue(d.second, "", false); directCount++; }
        if (includeDevDeps) for (auto& d : m.dev_deps) { enqueue(d.second, "", false); directCount++; }
    }
    for (auto& s : specs) { enqueue(s, "", editableSpecs.count(s) > 0); directCount++; }

    // ---- Drain the queue (BFS-ish; insertion order doesn't matter much) --
    size_t okCount = 0, failCount = 0;
    while (!queue.empty()) {
        Pending p = queue.back(); queue.pop_back();
        int rc = install_pending(p);
        if      (rc == 0) okCount++;
        else if (rc == 1) failCount++;
        // rc == 2 = silently dedup'd; don't tally
    }

    // ---- Project manifest: append newly-installed positional specs -------
    Manifest projMan;
    bool haveManifest = fs::exists("quirk.toml") && read_manifest("quirk.toml", projMan);
    if (haveManifest) {
        bool dirty = false;
        for (auto& s : specs) {
            std::string name = preview_name(s);
            if (name.empty() || !processed.count(name)) continue;
            std::string stored;
            if (is_local_path(s)) {
                std::error_code ec;
                fs::path abs = fs::absolute(s, ec);
                stored = ec ? s : abs.string();
            } else {
                stored = s;
            }
            bool found = false;
            for (auto& d : projMan.deps)
                if (d.first == name) { d.second = stored; found = true; break; }
            if (!found) projMan.deps.emplace_back(name, stored);
            dirty = true;
        }
        if (dirty) write_manifest("quirk.toml", projMan);
    }

    // ---- Lockfile --------------------------------------------------------
    if (!noLock && !lock.empty() && lock != originalLock) {
        write_lockfile(lockPath, lock);
        log::v("wrote " + lockPath.string() + " (" + std::to_string(lock.size()) + " entries)");
    }

    // ---- Summary --------------------------------------------------------
    if (!log::quiet_flag() && (okCount + failCount) > 0) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
        std::string elapsed = ms < 1000
            ? std::to_string(ms) + "ms"
            : std::to_string(ms / 1000) + "." + std::to_string((ms / 100) % 10) + "s";
        std::ostringstream tail;
        tail << okCount << " installed";
        size_t transitive = okCount > directCount ? okCount - directCount : 0;
        if (transitive > 0) tail << " (" << transitive << " transitive)";
        if (failCount > 0)  tail << ", " << failCount << " failed";
        tail << " " << log::dim("in " + elapsed);
        log::step("Finished", tail.str());
    }
    return failCount > 0 ? 1 : 0;
}

// `quirk sync` — one-command bootstrap for a fresh `git clone`. Creates a
// `.venv/` if missing, installs every dep declared by `quirk.toml` (frozen
// against `quirk.lock` when one exists), and prints the activation hint.
//
// Re-running is idempotent: existing venv is reused, lock entries that
// already match what's installed are no-ops.
static int cmd_sync(const std::vector<std::string>& args) {
    bool noVenv = false;
    bool includeDevDeps = false;
    for (auto& a : args) {
        if (a == "--no-venv")               noVenv = true;
        else if (a == "--dev")              includeDevDeps = true;
        else if (a == "--help" || a == "-h") {
            std::cout <<
                "quirk sync [--no-venv] [--dev]\n"
                "    Bootstrap a Quirk project for a fresh clone:\n"
                "      1. create ./.venv (unless --no-venv)\n"
                "      2. install every dep from quirk.toml at locked versions\n"
                "      3. print the `source .venv/bin/activate` hint\n"
                "    --no-venv   install into ./packages/ instead of a venv\n"
                "    --dev       also install [dev-deps] (CI / contributors)\n";
            return 0;
        }
        else { log::err("sync: unknown flag '" + a + "'"); return 1; }
    }

    if (!fs::exists("quirk.toml")) {
        log::err("no quirk.toml here — nothing to sync");
        std::cerr << "    " << log::dim("(cd into your project root, or run `quirk new <name>`)") << "\n";
        return 1;
    }

    // Pick the venv. If one is already activated, respect it. Otherwise
    // default to ./.venv (which we create on demand).
    fs::path venvDir;
    bool alreadyActive = false;
    bool createdVenv  = false;
    if (is_active_venv()) {
        venvDir = std::getenv("QUIRK_HOME");
        alreadyActive = true;
    } else if (!noVenv) {
        venvDir = fs::absolute(".venv");
        if (!fs::exists(venvDir)) {
            log::step("Creating", "venv at " + log::dim(".venv/"));
            if (build_venv(venvDir, /*repair=*/false) != 0) return 1;
            createdVenv = true;
        } else {
            log::v("reusing existing .venv");
        }
    }

    // Temporarily point QUIRK_HOME at the venv so `package_install_dir()`
    // routes the install into it. The user hasn't activated yet (and may
    // not — they could just want sync to do the work), but the install
    // should still land in the venv.
    std::string savedHome;
    bool hadSavedHome = false;
    if (!venvDir.empty() && !alreadyActive) {
        if (const char* h = std::getenv("QUIRK_HOME")) { savedHome = h; hadSavedHome = true; }
        setenv("QUIRK_HOME", venvDir.c_str(), 1);
    }

    bool hasLock = fs::exists("quirk.lock");
    std::vector<std::string> installArgs;
    if (hasLock) installArgs.push_back("--frozen");
    if (includeDevDeps) installArgs.push_back("--dev");
    int rc = cmd_install(installArgs);

    // Restore the prior env exactly.
    if (!venvDir.empty() && !alreadyActive) {
        if (hadSavedHome) setenv("QUIRK_HOME", savedHome.c_str(), 1);
        else              unsetenv("QUIRK_HOME");
    }

    if (rc != 0) return rc;

    // Final hint. Skip when the user is already activated (they don't need it).
    if (!alreadyActive && !venvDir.empty()) {
        std::string label = createdVenv ? "Activate the venv with:"
                                        : "To use the venv, run:";
        std::cout << "\n" << label << "\n"
                  << "    " << log::bold("source " + fs::relative(venvDir).string()
                                         + "/bin/activate") << "\n";
    }
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
            log::v("removing " + code.string());
            fs::remove_all(code);
            clear_dist_info(pkgRoot, n);
            if (hadCode || !find_dist_info(pkgRoot, n).empty()) {
                log::ok("removed " + n);
                if (haveManifest) {
                    auto it = projMan.deps.begin();
                    while (it != projMan.deps.end()) {
                        if (it->first == n) it = projMan.deps.erase(it);
                        else                 ++it;
                    }
                }
            } else {
                log::warn(n + " not installed");
            }
            continue;
        }

        // Drop one cached version. Doesn't touch the active install unless
        // that's the version being removed.
        fs::path entry = cache_entry(n, ver);
        if (!fs::exists(entry)) {
            log::warn(n + "@" + ver + " not cached");
        } else {
            log::v("removing " + entry.string());
            fs::remove_all(entry);
            log::ok("removed cache: " + n + "@" + ver);
        }
        // If this was the active version, drop the active install too.
        if (read_current_version(pkgRoot, n) == ver) {
            fs::remove_all(pkgRoot / n);
            clear_dist_info(pkgRoot, n);
            log::note(log::dim("(was active; uninstalled)"));
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
        log::step("Upgrading", d.first);

        // Local install: pick the highest cached version and materialize it.
        bool isLocal = !d.second.empty()
            && (d.second[0] == '/' || d.second[0] == '~' || d.second[0] == '.');
        if (isLocal) {
            std::string best = pick_cached_version(d.first, "");
            std::string active = read_current_version(pkgRoot, d.first);
            if (best.empty()) {
                log::warn(d.first + " not cached");
                continue;
            }
            if (best == active) {
                log::note(d.first + log::dim(" already at " + best));
                continue;
            }
            fs::path entry = cache_entry(d.first, best);
            Manifest mC;
            if (!read_manifest((entry / "quirk.toml").string(), mC)) {
                mC.name = d.first; mC.version = best;
            }
            if (materialize_from_cache(entry, pkgRoot, mC, false) != 0) continue;
            log::ok(d.first + " " + active + log::dim(" → ") + best);
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
    std::cout << log::bold(std::to_string(rows.size()) + " package(s) installed") << ":\n";
    for (auto& r : rows) {
        std::cout << "  " << r.name;
        for (size_t i = r.name.size(); i < pad + 2; i++) std::cout << ' ';
        std::cout << log::GREEN() << r.version << log::RESET();
        if (r.cached.size() > 1) {
            std::ostringstream extras;
            extras << "  (cached: ";
            bool first = true;
            for (auto& v : r.cached) {
                if (v == r.version) continue;
                if (!first) extras << ", ";
                extras << v; first = false;
            }
            extras << ")";
            std::cout << log::dim(extras.str());
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
    auto kv = [](const std::string& k, const std::string& v) {
        std::cout << log::dim(k + std::string(13 - k.size(), ' ')) << v << "\n";
    };
    kv("name:",    log::bold(m.name));
    kv("version:", std::string(log::GREEN()) + m.version + log::RESET());
    if (!m.description.empty()) kv("description:", m.description);
    if (!m.author.empty())      kv("author:",      m.author);
    if (!m.license.empty())     kv("license:",     m.license);
    if (!m.repository.empty())  kv("repository:",  m.repository);
    if (!m.homepage.empty())    kv("homepage:",    m.homepage);

    // Pull commit SHA from the dist-info if available.
    fs::path di = find_dist_info(pkgRoot, args[0]);
    if (!di.empty()) {
        std::ifstream meta(di / "METADATA");
        std::string line;
        while (std::getline(meta, line)) {
            if (line.rfind("Commit: ", 0) == 0) {
                kv("commit:", line.substr(8));
                break;
            }
        }
    }

    if (!m.deps.empty()) {
        std::cout << log::dim("deps:") << "\n";
        for (auto& d : m.deps) std::cout << "  " << d.first << " = " << d.second << "\n";
    }
    auto cached = list_cached_versions(args[0]);
    if (cached.size() > 1) {
        std::cout << log::dim("cached versions: ");
        for (size_t i = 0; i < cached.size(); i++) {
            if (i) std::cout << ", ";
            std::cout << cached[i];
            if (cached[i] == m.version) std::cout << " " << log::GREEN() << "(active)" << log::RESET();
        }
        std::cout << "\n";
    }
    return 0;
}

// Absolute path to the running quirk binary — used when we need to re-invoke
// ourselves (eval, module, check).
static std::string self_binary() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = '\0'; return buf; }
    return "quirk";
}

// ---- Package validation -----------------------------------------------
// Pre-flight checks before registering. Returns the number of errors found
// (warnings don't count). All findings are printed to stdout/stderr with
// a status glyph: ✓ pass, ⚠ warn, ✗ error.

struct CheckFinding {
    enum Level { OK, WARN, ERROR } level;
    std::string message;
    std::string hint;
};

// Run shell command, capture stdout. Returns trimmed output.
static std::string capture(const std::string& cmd) {
    FILE* p = popen((cmd + " 2>/dev/null").c_str(), "r");
    if (!p) return "";
    char buf[1024]; std::string out;
    while (fgets(buf, sizeof(buf), p)) out += buf;
    pclose(p);
    return trim(out);
}

static bool valid_pkg_name(const std::string& n) {
    if (n.empty()) return false;
    for (char c : n) {
        if (!(std::isalnum((unsigned char)c) || c == '-' || c == '_')) return false;
    }
    return true;
}

// Run all package checks on the current directory's package. Reads files
// directly so it works regardless of whether the package is installed.
static std::vector<CheckFinding> validate_package(const Manifest& m) {
    std::vector<CheckFinding> out;
    auto add = [&](CheckFinding::Level lvl, std::string msg, std::string hint = "") {
        out.push_back({lvl, std::move(msg), std::move(hint)});
    };

    // --- Manifest fields ------------------------------------------------
    if (m.name.empty())         add(CheckFinding::ERROR, "manifest is missing `name`");
    else if (!valid_pkg_name(m.name))
        add(CheckFinding::ERROR, "name '" + m.name + "' has invalid characters",
            "use only [a-zA-Z0-9_-]");
    else add(CheckFinding::OK, "name = \"" + m.name + "\"");

    if (m.version.empty())      add(CheckFinding::ERROR, "manifest is missing `version`");
    else                         add(CheckFinding::OK, "version = \"" + m.version + "\"");

    if (m.repository.empty())   add(CheckFinding::ERROR, "manifest is missing `repository`",
                                     "add `repository = \"github.com/<owner>/<repo>\"`");
    else                         add(CheckFinding::OK, "repository = \"" + m.repository + "\"");

    if (m.description.empty())  add(CheckFinding::WARN, "no `description` set",
                                     "users see this in `quirk pkg show`");
    if (m.license.empty())      add(CheckFinding::WARN, "no `license` set",
                                     "common choices: MIT, Apache-2.0, BSD-3-Clause");
    if (m.quirk_version.empty())
        add(CheckFinding::WARN, "no `quirk-version` constraint",
            "add `quirk-version = \">=0.2.0\"` to guard against compiler-version drift");

    // --- Entry point ----------------------------------------------------
    std::string entryRel = m.entry.empty() ? "src/index.quirk" : m.entry;
    fs::path entry = entryRel;
    if (!fs::exists(entry))
        add(CheckFinding::ERROR, "entry point not found: " + entryRel,
            m.entry.empty() ? "create src/index.quirk or set `entry = ...`" : "");
    else add(CheckFinding::OK, "entry point: " + entryRel);

    // --- tests/ directory -----------------------------------------------
    if (!fs::is_directory("tests"))
        add(CheckFinding::WARN, "no tests/ directory",
            "even one tests/sanity.quirk catches regressions");

    // --- .gitignore -----------------------------------------------------
    {
        std::ifstream gi(".gitignore");
        std::string line; bool packagesIgnored = false;
        while (gi && std::getline(gi, line)) {
            std::string t = trim(line);
            if (t == "packages/" || t == "packages" || t == "/packages") {
                packagesIgnored = true; break;
            }
        }
        if (!gi)
            add(CheckFinding::WARN, ".gitignore missing",
                "add one ignoring packages/ and .venv/");
        else if (!packagesIgnored)
            add(CheckFinding::WARN, ".gitignore doesn't exclude `packages/`",
                "consumers' installed deps would otherwise get committed");
    }

    // --- Git state ------------------------------------------------------
    if (fs::is_directory(".git")) {
        std::string status = capture("git status --porcelain");
        if (!status.empty())
            add(CheckFinding::WARN, "working tree is dirty",
                "commit or stash before tagging a release");

        std::string remote = capture("git remote get-url origin");
        if (!remote.empty() && !m.repository.empty()) {
            // Normalize both sides for comparison
            auto norm = [](std::string s) {
                auto p = s.find("://"); if (p != std::string::npos) s = s.substr(p + 3);
                if (s.rfind("git@", 0) == 0) {
                    auto colon = s.find(':');
                    if (colon != std::string::npos) s = s.substr(4) /* drop git@ */, s.replace(s.find(':'), 1, "/");
                }
                if (s.size() > 4 && s.substr(s.size() - 4) == ".git") s.resize(s.size() - 4);
                return s;
            };
            if (norm(remote) != norm(m.repository))
                add(CheckFinding::WARN, "git remote 'origin' (" + remote
                    + ") differs from manifest `repository`",
                    "either fix the manifest or update the remote");
        }

        std::string tags = capture("git tag --sort=-v:refname");
        if (tags.empty())
            add(CheckFinding::WARN, "no git tags",
                "tag a release: `git tag v" + m.version + " && git push --tags`");
        else {
            std::string latest = tags.substr(0, tags.find('\n'));
            std::string stripped = latest;
            if (!stripped.empty() && stripped[0] == 'v') stripped = stripped.substr(1);
            if (stripped != m.version)
                add(CheckFinding::WARN, "latest git tag (" + latest
                    + ") doesn't match manifest version (" + m.version + ")",
                    "tag a release: `git tag v" + m.version + " && git push --tags`");
        }
    } else {
        add(CheckFinding::WARN, "not a git repo",
            "run `git init` so users can install via URL");
    }

    // --- Compile check on the entry --------------------------------------
    if (fs::exists(entry)) {
        std::string cmd = "\"" + self_binary() + "\" --check \"" + entry.string() + "\" 2>&1";
        FILE* p = popen(cmd.c_str(), "r");
        std::string output;
        char buf[2048];
        while (p && fgets(buf, sizeof(buf), p)) output += buf;
        int rc = p ? pclose(p) : -1;
        if (rc == 0) add(CheckFinding::OK, "type-check passed (" + entryRel + ")");
        else {
            std::string firstLine = output.substr(0, output.find('\n'));
            add(CheckFinding::ERROR, "type-check failed",
                firstLine.empty() ? "" : "first error: " + firstLine);
        }
    }
    return out;
}

// Print findings in a uniform way, return error count.
static int print_findings(const std::vector<CheckFinding>& fs) {
    int errors = 0, warns = 0, oks = 0;
    for (auto& f : fs) {
        const char* glyph = f.level == CheckFinding::ERROR ? "✗"
                          : f.level == CheckFinding::WARN  ? "⚠" : "✓";
        std::ostream& out = (f.level == CheckFinding::ERROR) ? std::cerr : std::cout;
        out << "  " << glyph << " " << f.message << "\n";
        if (!f.hint.empty()) out << "      → " << f.hint << "\n";
        if (f.level == CheckFinding::ERROR) errors++;
        else if (f.level == CheckFinding::WARN) warns++;
        else oks++;
    }
    std::cout << "\n  " << oks << " ok, " << warns << " warning(s), "
              << errors << " error(s)\n";
    return errors;
}

// ─────────────────────────────────────────────────────────────────────────
// `quirk fmt` — code formatter. Mirrors the VS Code FormatterProvider so the
// CLI and the editor produce identical output. Operates line-by-line with
// awareness of:
//   • brace depth → indentation (4 spaces / level)
//   • string literals → preserved verbatim
//   • `//` comments → preserved verbatim
//   • `---` docstring blocks → contents untouched, only re-indented
// And applies operator/separator spacing rules to non-string, non-comment
// regions. The formatter is safe by design — it doesn't reorder, wrap, or
// re-bracket; it only fixes whitespace.
// ─────────────────────────────────────────────────────────────────────────

// Apply the per-token spacing rules to a chunk of code (already stripped
// of string literals and trailing comments). Used by fmt_one_line.
static std::string fmt_apply_spacing(std::string text) {
    static const std::regex reComma     (R"(,\s*)");
    // Multi-char operators must come BEFORE the single-char ones so they
    // win the alternation: compound assigns (`+=`, `-=`, `*=`, `/=`, `%=`)
    // before `+`/`-`/`*`/`/`/`%`; `->` before `-`; `==`/`!=`/`<=`/`>=`
    // before single-char comparison operators. Without these, `i += 1`
    // becomes `i + = 1` — a syntax error.
    static const std::regex reOps       (R"(\s*(:=|\+=|-=|\*=|/=|%=|->|==|!=|<=|>=|\+|-|\*|/)\s*)");
    static const std::regex reColon     (R"(:(?!=)\s*)");
    // Don't insert space inside an empty `{}` (Map literal). The lookahead
    // `(?!\s*\})` only matches `{` when something other than `}` follows.
    static const std::regex reBrace     (R"(\s*\{(?!\s*\})\s*)");
    static const std::regex reDoubleSpc (R"( +)");
    // Unary-minus fixup: when ` - <digit>` follows an operator, `(`, `[`,
    // or `,`, the `-` is the sign of a numeric literal — drop the space
    // between `-` and the digit. So `x = - 5` → `x = -5`, `foo(-1, -2)`
    // stays unmangled. Binary uses (`a - b`) aren't touched because their
    // left context is a word character, not an operator.
    static const std::regex reUnaryMinus(
        R"(([\(\[,=+\-*/%<>!]|:=|->|==|!=|<=|>=) - (\d))");
    // After unary fixup, `( -5` becomes `( -5`. Collapse the leftover space
    // after `(`/`[` so we get `(-5)` / `[-1, -2]` — canonical form.
    static const std::regex reOpenUnary(R"(([\(\[]) -(\d))");
    text = std::regex_replace(text, reComma,  ", ");
    text = std::regex_replace(text, reOps,    " $1 ");
    text = std::regex_replace(text, reColon,  ": ");
    text = std::regex_replace(text, reBrace,  " { ");
    text = std::regex_replace(text, reDoubleSpc, " ");
    text = std::regex_replace(text, reUnaryMinus, "$1 -$2");
    text = std::regex_replace(text, reOpenUnary,  "$1-$2");
    // Deliberately *don't* trim trailing whitespace here. This function is
    // called whenever we flush a code segment, including right before a
    // string literal — e.g. `: ` flushed before `"Alice"` must keep its
    // trailing space, otherwise `{ "name": "Alice" }` becomes `..":"Alice".
    // Whole-line trimming happens in fmt_source (top of the per-line loop).
    return text;
}

// Format the spacing of a single line. Walks character-by-character so we
// can identify string literals and comments without touching them; spacing
// rules run only on the "code" segments between strings/comments.
static std::string fmt_one_line(const std::string& line) {
    std::string out;
    std::string buffer;
    bool inString = false;
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        // Toggle string state on unescaped `"`.
        if (c == '"' && (i == 0 || line[i - 1] != '\\')) {
            if (!inString) {
                out += fmt_apply_spacing(buffer);
                buffer.clear();
                inString = true;
                out += '"';
                continue;
            } else {
                out += buffer + '"';
                buffer.clear();
                inString = false;
                continue;
            }
        }
        if (inString) {
            buffer += c;
        } else {
            // Stop formatting at a line comment — emit it verbatim.
            if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') {
                out += fmt_apply_spacing(buffer);
                if (!out.empty() && out.back() != ' ') out += ' ';
                out += line.substr(i);
                buffer.clear();
                return out;
            }
            buffer += c;
        }
    }
    if (!buffer.empty()) out += inString ? buffer : fmt_apply_spacing(buffer);
    // Final whole-line trim: the ` { ` spacing rule appends a trailing space
    // when `{` ends a line (block opening). Strip leading/trailing whitespace
    // on the assembled formatted line; indentation is reapplied by the caller.
    size_t s = 0, e = out.size();
    while (s < e && (out[s] == ' ' || out[s] == '\t')) s++;
    while (e > s && (out[e - 1] == ' ' || out[e - 1] == '\t')) e--;
    return out.substr(s, e - s);
}

// Net brace-depth change for this line — `+1` per `{`/`[` outside a string
// or comment, `-1` per `}`/`]`. Used to track indentation for the NEXT line.
static int fmt_indent_delta(const std::string& line) {
    int change = 0;
    bool inString = false;
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (!inString && c == '/' && i + 1 < line.size() && line[i + 1] == '/') break;
        if (c == '"' && (i == 0 || line[i - 1] != '\\')) { inString = !inString; continue; }
        if (inString) continue;
        if (c == '{' || c == '[') change++;
        else if (c == '}' || c == ']') change--;
    }
    return change;
}

// Reformat an entire source string. Returns the canonical form.
static std::string fmt_source(const std::string& src) {
    std::vector<std::string> lines;
    {
        std::string cur;
        for (char c : src) {
            if (c == '\n') { lines.push_back(std::move(cur)); cur.clear(); }
            else if (c != '\r') cur += c;
        }
        if (!cur.empty()) lines.push_back(std::move(cur));
    }

    int indent = 0;
    bool inDocBlock = false;
    std::string out;
    out.reserve(src.size());

    for (auto& raw : lines) {
        // Trim trailing whitespace before any analysis.
        std::string line = raw;
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();

        // Trimmed view for keyword detection.
        size_t s = 0;
        while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) s++;
        std::string trimmed = line.substr(s);

        if (trimmed.empty()) { out += '\n'; continue; }

        // Toggle docstring block state on bare `---`.
        if (trimmed == "---") {
            inDocBlock = !inDocBlock;
            for (int k = 0; k < indent * 4; k++) out += ' ';
            out += "---\n";
            continue;
        }

        // Inline single-line docstring `--- text ---` — preserve the whole
        // line verbatim modulo indentation. Otherwise the `-` operator-
        // spacing rule would mangle the leading/trailing `---` into ` - - -`.
        // Requires `length >= 7` so we don't match `------` lines (still a
        // bare separator) and the start/end markers can't overlap.
        if (trimmed.size() >= 7
            && trimmed.compare(0, 3, "---") == 0
            && trimmed.compare(trimmed.size() - 3, 3, "---") == 0) {
            for (int k = 0; k < indent * 4; k++) out += ' ';
            out += trimmed;
            out += '\n';
            continue;
        }

        // Inside a docstring: preserve the line verbatim modulo trailing
        // whitespace (already trimmed). Don't re-indent docstring content.
        if (inDocBlock) { out += line; out += '\n'; continue; }

        // Lines opening with `}`/`]`/`else`/`elif` dedent themselves first.
        int currentIndent = indent;
        if (!trimmed.empty()
            && (trimmed[0] == '}' || trimmed[0] == ']'
                || trimmed.rfind("else", 0) == 0
                || trimmed.rfind("elif", 0) == 0))
            currentIndent = std::max(0, currentIndent - 1);

        std::string formatted = fmt_one_line(trimmed);

        for (int k = 0; k < currentIndent * 4; k++) out += ' ';
        out += formatted;
        out += '\n';

        indent += fmt_indent_delta(formatted);
        if (indent < 0) indent = 0;
    }

    // Ensure exactly one trailing newline — but leave a fully-empty file
    // alone (don't inject a newline into a 0-byte file).
    while (out.size() >= 2 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n')
        out.pop_back();
    if (!out.empty() && out.back() != '\n') out += '\n';
    return out;
}

// `quirk fmt [--check|--stdout] <file>...`
//   no flag   → format each file in place (no-op if already formatted)
//   --check   → exit 1 if any file would change (prints names); useful in CI
//   --stdout  → print formatted output to stdout, don't write back
static int cmd_fmt(const std::vector<std::string>& args) {
    bool checkOnly = false;
    bool toStdout  = false;
    std::vector<std::string> files;
    for (auto& a : args) {
        if (a == "--check")       checkOnly = true;
        else if (a == "--stdout") toStdout  = true;
        else if (a == "--help" || a == "-h") {
            std::cout <<
                "quirk fmt [--check|--stdout] <file>...\n"
                "    Reformat Quirk source files to a canonical style.\n"
                "    --check    exit 1 if any file would change, list them\n"
                "    --stdout   print formatted output, don't modify files\n"
                "    With no files, formats every .quirk under the current directory.\n";
            return 0;
        }
        else if (a[0] == '-') {
            log::err("fmt: unknown flag '" + a + "'");
            return 1;
        }
        else files.push_back(a);
    }

    // Walk a directory tree for .quirk files, skipping the obvious noise dirs.
    // Used both for `quirk fmt` with no args (root = ".") and for directory
    // arguments like `quirk fmt libs/`.
    auto collectQkFiles = [](const fs::path& root, std::vector<std::string>& out) {
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(root, ec);
             it != fs::recursive_directory_iterator() && !ec;
             it.increment(ec)) {
            const auto& e = *it;
            std::string fn = e.path().filename().string();
            if (e.is_directory(ec) && (fn == "packages" || fn == ".venv" ||
                                       fn == ".git"     || fn == "node_modules")) {
                it.disable_recursion_pending();
                continue;
            }
            if (e.is_regular_file(ec) && e.path().extension() == ".quirk")
                out.push_back(e.path().string());
        }
    };

    // Expand any directory arguments into their contained .quirk files.
    // `quirk fmt libs/` now does the obvious thing instead of trying to
    // write to a directory.
    std::vector<std::string> expanded;
    for (auto& f : files) {
        std::error_code ec;
        if (fs::is_directory(f, ec)) collectQkFiles(f, expanded);
        else                          expanded.push_back(f);
    }
    files = std::move(expanded);

    // No files (and no args) → walk cwd.
    if (files.empty()) {
        if (args.empty() || (args.size() == 1 && (args[0] == "--check" || args[0] == "--stdout")))
            collectQkFiles(".", files);
    }

    if (files.empty()) {
        log::note("no files to format");
        return 0;
    }

    int wouldChange = 0;
    int changed     = 0;
    for (auto& f : files) {
        std::ifstream in(f);
        if (!in) { log::err("cannot read " + f); return 1; }
        std::stringstream buf; buf << in.rdbuf();
        std::string orig = buf.str();
        std::string out  = fmt_source(orig);

        if (toStdout) { std::cout << out; continue; }
        if (out == orig) continue;

        if (checkOnly) {
            std::cout << "would format: " << f << "\n";
            wouldChange++;
            continue;
        }
        std::ofstream wf(f);
        if (!wf) { log::err("cannot write " + f); return 1; }
        wf << out;
        changed++;
        log::ok(f);
    }

    if (checkOnly) {
        if (wouldChange > 0) {
            std::cerr << wouldChange << " file(s) would be reformatted.\n";
            return 1;
        }
        log::note("all files already formatted");
        return 0;
    }
    if (!toStdout) {
        if (changed == 0) log::note("all files already formatted");
        else              log::step("Formatted", std::to_string(changed) + " file(s)");
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────
// `quirk repl` — interactive shell.
//
// Maintains an in-memory session:
//   • `preamble`   — top-level declarations (use, define, struct, enum, type).
//                    Persists across every input.
//   • `state`      — variable bindings from `x := ...` lines. Re-evaluated
//                    on every input so later lines can reference them.
//   • current line — the expression or statement just typed. Run once.
//
// Each turn writes `<preamble>\n<state>\n<wrapper(current)>` to a temp file
// and execs the compiler on it via popen(). Slow per command (~100ms) but
// correct, and crucially insulates the REPL from JIT/codegen state leakage.
//
// Special commands:
//   :help / :h          show shortcuts
//   :quit / :q / Ctrl-D leave
//   :reset              wipe preamble + state
//   :state              show current session
// ─────────────────────────────────────────────────────────────────────────

// Net brace+paren balance for a line, ignoring strings and `//` comments.
// Used to detect "the user's input is incomplete, prompt for continuation."
static int repl_brace_balance(const std::string& line) {
    int depth = 0;
    bool inStr = false;
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (!inStr && c == '/' && i + 1 < line.size() && line[i + 1] == '/') break;
        if (c == '"' && (i == 0 || line[i - 1] != '\\')) { inStr = !inStr; continue; }
        if (inStr) continue;
        if (c == '{' || c == '(' || c == '[') depth++;
        else if (c == '}' || c == ')' || c == ']') depth--;
    }
    return depth;
}

// Classify a top-of-statement line by its first non-whitespace token. The
// REPL routes definitions to the preamble, `x := ...` bindings to the
// session state, statement-shaped lines (`print(x)`, `for ... { }`) get
// executed as-is, and value-bearing expressions are wrapped in `print(...)`
// so they display.
enum class ReplKind { Definition, Binding, Statement, Expression, Empty };
static ReplKind repl_classify(const std::string& src) {
    // Skip leading whitespace.
    size_t i = 0;
    while (i < src.size() && (src[i] == ' ' || src[i] == '\t' || src[i] == '\n')) i++;
    if (i >= src.size()) return ReplKind::Empty;

    // Top-level declarations.
    static const std::vector<std::string> defWords = {
        "define ", "struct ", "enum ",   "use ",
        "from ",   "type ",   "extern ", "interface ",
        "extend ",
    };
    for (auto& w : defWords)
        if (src.compare(i, w.size(), w) == 0) return ReplKind::Definition;

    // Statement-shaped keywords: re-running their first invocation is fine.
    static const std::vector<std::string> stmtWords = {
        "if ", "while ", "for ", "match ", "try ", "throw ",
        "return ", "break", "continue", "del ", "nonlocal ", "global ",
        "print(", "printf(", "write(", "writeln(",
    };
    for (auto& w : stmtWords)
        if (src.compare(i, w.size(), w) == 0) return ReplKind::Statement;

    // Assignment of any kind — declaration (`:=`) or reassignment (`=`,
    // `+=`, `-=`, …). All of them must persist in the session state so
    // later lines see the updated value.
    size_t eol = src.find('\n', i);
    if (eol == std::string::npos) eol = src.size();
    std::string firstLine = src.substr(i, eol - i);
    if (firstLine.find(":=") != std::string::npos) return ReplKind::Binding;
    if (firstLine.find("+=") != std::string::npos
     || firstLine.find("-=") != std::string::npos
     || firstLine.find("*=") != std::string::npos
     || firstLine.find("/=") != std::string::npos
     || firstLine.find("%=") != std::string::npos) return ReplKind::Binding;
    // Plain `x = expr` (single `=`, not part of `==`, `=>`, `<=`, `>=`, `!=`, `:=`).
    for (size_t j = 0; j + 1 < firstLine.size(); j++) {
        if (firstLine[j] != '=') continue;
        char nxt = firstLine[j + 1];
        char prv = j > 0 ? firstLine[j - 1] : '\0';
        if (nxt == '=' || nxt == '>') continue;
        if (prv == '!' || prv == '<' || prv == '>' || prv == ':' || prv == '=') continue;
        // Inside a string literal? cheap check — count quotes before this `=`.
        size_t quotes = 0;
        for (size_t k = 0; k < j; k++) if (firstLine[k] == '"') quotes++;
        if (quotes % 2 == 1) continue;
        return ReplKind::Binding;
    }

    return ReplKind::Expression;
}

// Drive one REPL turn: compose the program, write it, exec the compiler,
// stream output back. Returns 0 on success (we keep going either way —
// the REPL only exits on :quit or EOF).
static int repl_run_program(const std::string& fullProgram, const fs::path& tmp) {
    {
        std::ofstream out(tmp);
        out << fullProgram;
    }
    // Use the running quirk binary so we don't depend on PATH.
    std::string cmd = self_binary() + " " + tmp.string() + " 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) { log::err("repl: failed to spawn compiler"); return 1; }
    char buf[4096];
    while (size_t n = fread(buf, 1, sizeof(buf), p)) std::cout.write(buf, n);
    int rc = pclose(p);
    std::cout.flush();
    return rc == 0 ? 0 : 1;
}

static int cmd_repl(const std::vector<std::string>& args) {
    for (auto& a : args) {
        if (a == "--help" || a == "-h") {
            std::cout <<
                "quirk repl\n"
                "    Interactive Quirk shell. Type expressions to evaluate them;\n"
                "    type definitions to add them to the session.\n"
                "      :help / :h     show this message\n"
                "      :quit / :q     exit (or Ctrl-D)\n"
                "      :reset         clear the session\n"
                "      :state         show the current session program\n";
            return 0;
        }
    }

    std::cout << log::bold("Quirk REPL") << " " << QUIRK_VERSION
              << log::dim("  — :help for shortcuts, :quit to exit") << "\n";

    fs::path tmp = fs::temp_directory_path() / ("quirk_repl_" + std::to_string(getpid()) + ".quirk");
    std::vector<std::string> preamble;     // top-level decls (define, struct, ...)
    std::vector<std::string> state;        // `x := ...` lines (re-evaluated each turn)

    std::string carry;                     // unfinished multi-line input
    bool active = true;
    while (active) {
        // Prompt: `>>> ` fresh, `... ` continuation.
        std::cout << (carry.empty() ? log::CYAN() : log::DIM()) << (carry.empty() ? ">>> " : "... ") << log::RESET();
        std::cout.flush();
        std::string line;
        if (!std::getline(std::cin, line)) { std::cout << "\n"; break; }

        // ── Meta commands (only at top level, not mid-multi-line) ──────
        if (carry.empty() && !line.empty() && line[0] == ':') {
            std::string cmd = line.substr(1);
            // Strip trailing whitespace from the command.
            while (!cmd.empty() && (cmd.back() == ' ' || cmd.back() == '\t' || cmd.back() == '\r'))
                cmd.pop_back();
            if (cmd == "quit" || cmd == "q") break;
            if (cmd == "help" || cmd == "h") {
                std::cout <<
                    "  :quit / :q     exit the REPL\n"
                    "  :reset         clear session preamble + state\n"
                    "  :state         print the assembled program\n"
                    "  :help / :h     this list\n";
                continue;
            }
            if (cmd == "reset") {
                preamble.clear(); state.clear();
                log::note("session cleared");
                continue;
            }
            if (cmd == "state") {
                std::cout << log::dim("// preamble:\n");
                for (auto& p : preamble) std::cout << p << "\n";
                std::cout << log::dim("// state:\n");
                for (auto& s : state) std::cout << s << "\n";
                continue;
            }
            log::warn("unknown command ':" + cmd + "' — try :help");
            continue;
        }

        // Accumulate multi-line input until braces balance.
        carry += (carry.empty() ? "" : "\n") + line;
        int depth = repl_brace_balance(carry);
        if (depth > 0) continue;          // still inside an open block
        if (depth < 0) { log::warn("unbalanced bracket — discarded"); carry.clear(); continue; }

        // Empty line — drop and reprompt.
        std::string toEval = std::move(carry);
        carry.clear();
        bool blank = true;
        for (char c : toEval) if (c != ' ' && c != '\t' && c != '\n') { blank = false; break; }
        if (blank) continue;

        ReplKind kind = repl_classify(toEval);

        // ── Compose the program for this turn ──────────────────────────
        std::ostringstream program;
        for (auto& p : preamble) program << p << "\n";
        if (kind == ReplKind::Definition) {
            program << toEval << "\n";
            // Minimal main so the compiler has an entry point.
            program << "define main() -> void {\n";
            for (auto& s : state) program << "    " << s << "\n";
            program << "}\n";
        } else {
            program << "define main() -> void {\n";
            for (auto& s : state) program << "    " << s << "\n";
            if (kind == ReplKind::Binding || kind == ReplKind::Statement) {
                // Bindings + statement-shaped lines (print/if/for/...) run as-is.
                program << "    " << toEval << "\n";
            } else { // Expression — wrap in print() so the value shows up.
                program << "    print(" << toEval << ")\n";
            }
            program << "}\n";
        }

        int rc = repl_run_program(program.str(), tmp);

        // Only commit to session state if the run succeeded — keeps the
        // session from accumulating broken lines.
        if (rc == 0) {
            if (kind == ReplKind::Definition) preamble.push_back(toEval);
            else if (kind == ReplKind::Binding) state.push_back(toEval);
        }
    }

    std::error_code ec; fs::remove(tmp, ec);
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────
// `quirk test` — discover and run *_test.quirk files.
//
// Walks the given directory (or `./tests` by default) for files matching
// `*_test.quirk`, invokes `<self> run <file>` on each, and parses the test
// framework's "N passed" / "N failed" summary out of stdout. A file is a
// failure if it exited non-zero, or its summary reports any failures, or
// it produced no summary at all (likely a crash or test file with no
// `test.run_all(...)` call).
//
// Exit code: 0 if every file's summary is fully green, 1 otherwise.
// ─────────────────────────────────────────────────────────────────────────

struct TestFileResult {
    std::string file;
    int passed = 0;
    int failed = 0;
    int exitCode = 0;
    bool sawSummary = false;
    std::string tail;     // last lines of output for diagnostic display
};

// Run one test file: spawn `<self> run <file>` and parse the framework's
// summary. The framework prints either "N passed" (success) or
// "M failed, N passed (of T)" (failure). We scan stdout for both shapes
// to support repeated runs (some test files invoke run_all more than once).
static TestFileResult run_one_test_file(const fs::path& file) {
    TestFileResult r;
    r.file = file.string();
    std::string cmd = "\"" + self_binary() + "\" run \"" + file.string() + "\" 2>&1";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) { r.failed = 1; r.exitCode = -1; return r; }
    std::string out;
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) out += buf;
    int rc = pclose(pipe);
    r.exitCode = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;

    // Parse every "N passed" and "M failed, N passed" line. Multiple
    // summaries in one file are summed.
    std::istringstream iss(out);
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(iss, line)) {
        lines.push_back(line);

        // Strip ANSI escapes so "\x1b[32m12 passed\x1b[0m" matches.
        std::string stripped;
        for (size_t i = 0; i < line.size(); i++) {
            if (line[i] == '\x1b' && i + 1 < line.size() && line[i + 1] == '[') {
                while (i < line.size() && line[i] != 'm') i++;
                continue;
            }
            stripped += line[i];
        }

        // "M failed, N passed (of T)"
        size_t fpos = stripped.find(" failed, ");
        if (fpos != std::string::npos) {
            try {
                size_t mstart = fpos;
                while (mstart > 0 && std::isdigit(static_cast<unsigned char>(stripped[mstart - 1]))) mstart--;
                int f = std::stoi(stripped.substr(mstart, fpos - mstart));
                size_t ppos = stripped.find(" passed", fpos);
                if (ppos != std::string::npos) {
                    size_t pstart = ppos;
                    while (pstart > 0 && std::isdigit(static_cast<unsigned char>(stripped[pstart - 1]))) pstart--;
                    int p = std::stoi(stripped.substr(pstart, ppos - pstart));
                    r.failed += f;
                    r.passed += p;
                    r.sawSummary = true;
                    continue;
                }
            } catch (...) {}
        }

        // "N passed" — bare success line, only if no leading "failed" word.
        size_t ppos = stripped.find(" passed");
        if (ppos != std::string::npos && stripped.find("failed") == std::string::npos) {
            try {
                size_t pstart = ppos;
                while (pstart > 0 && std::isdigit(static_cast<unsigned char>(stripped[pstart - 1]))) pstart--;
                if (pstart < ppos) {
                    int p = std::stoi(stripped.substr(pstart, ppos - pstart));
                    r.passed += p;
                    r.sawSummary = true;
                }
            } catch (...) {}
        }
    }

    // Keep the last ~15 lines for the tail diagnostic on failure.
    size_t start = lines.size() > 15 ? lines.size() - 15 : 0;
    for (size_t i = start; i < lines.size(); i++) r.tail += lines[i] + "\n";
    return r;
}

static int cmd_test(const std::vector<std::string>& args) {
    std::vector<std::string> roots;
    bool verbose = false;
    for (auto& a : args) {
        if (a == "--help" || a == "-h") {
            std::cout <<
                "quirk test [-v] [<path>...]\n"
                "    Discover *_test.quirk files and run them with the test framework.\n"
                "    Default path is ./tests if it exists, else the current directory.\n"
                "    -v / --verbose   print each file's full output\n";
            return 0;
        }
        if (a == "-v" || a == "--verbose") { verbose = true; continue; }
        if (!a.empty() && a[0] == '-') {
            log::err("test: unknown flag '" + a + "'");
            return 1;
        }
        roots.push_back(a);
    }

    if (roots.empty()) {
        std::error_code ec;
        if (fs::is_directory("tests", ec)) roots.push_back("tests");
        else                                roots.push_back(".");
    }

    // Collect *_test.quirk files from each root (file paths pass through verbatim).
    std::vector<fs::path> files;
    for (auto& r : roots) {
        std::error_code ec;
        if (fs::is_regular_file(r, ec)) { files.emplace_back(r); continue; }
        if (!fs::is_directory(r, ec)) {
            log::err("test: not a file or directory: '" + r + "'");
            return 1;
        }
        for (auto it = fs::recursive_directory_iterator(r, ec);
             it != fs::recursive_directory_iterator() && !ec;
             it.increment(ec)) {
            const auto& e = *it;
            std::string fn = e.path().filename().string();
            if (e.is_directory(ec) && (fn == "packages" || fn == ".venv" ||
                                       fn == ".git"     || fn == "node_modules")) {
                it.disable_recursion_pending();
                continue;
            }
            if (e.is_regular_file(ec)
                && e.path().extension() == ".quirk"
                && fn.size() >= 11
                && fn.compare(fn.size() - 11, 11, "_test.quirk") == 0) {
                files.push_back(e.path());
            }
        }
    }
    std::sort(files.begin(), files.end());

    if (files.empty()) {
        log::note("no *_test.quirk files found");
        return 0;
    }

    std::cout << log::bold("Running ") << files.size() << " test file(s)\n\n";

    int totalPassed = 0, totalFailed = 0, filesOk = 0, filesFail = 0;
    std::vector<TestFileResult> failures;
    for (auto& f : files) {
        // Print the in-progress marker only on a TTY; piped output would
        // show both the "…" and final lines, which is noisy.
        bool tty = log::stdout_is_tty();
        if (tty) std::cout << log::dim("  …") << " " << f.string() << std::flush;
        auto res = run_one_test_file(f);
        totalPassed += res.passed;
        totalFailed += res.failed;
        // A file is a failure only if the process itself didn't exit 0.
        // Cases counted out of "M failed, N passed" lines are surfaced in
        // the summary but don't decide pass/fail on their own — files like
        // tests/test_test.quirk deliberately include failing cases inside a
        // meta-check and exit 0 when the framework correctly counts them.
        bool fileGreen = res.exitCode == 0;

        // Erase the in-progress line so the final status glyph stands alone.
        if (tty) std::cout << "\r\x1b[2K";
        if (fileGreen) {
            filesOk++;
            std::cout << "  " << log::GREEN() << "✓" << log::RESET() << " "
                      << f.string();
            if (res.sawSummary)
                std::cout << "  " << log::dim("(" + std::to_string(res.passed) + " passed)");
            std::cout << "\n";
        } else {
            filesFail++;
            failures.push_back(res);
            std::cout << "  " << log::RED() << "✗" << log::RESET() << " "
                      << f.string();
            if (res.sawSummary)
                std::cout << "  " << log::dim("(" + std::to_string(res.failed)
                                              + " failed, "
                                              + std::to_string(res.passed)
                                              + " passed)");
            else
                std::cout << "  " << log::dim("(exit " + std::to_string(res.exitCode) + ")");
            std::cout << "\n";
        }

        if (verbose) {
            std::cout << log::dim("─── output ───") << "\n" << res.tail
                      << log::dim("──────────────") << "\n";
        }
    }

    std::cout << "\n";
    if (filesFail > 0) {
        std::cout << log::bold("Failures:") << "\n";
        for (auto& res : failures) {
            std::cout << log::RED() << "  " << res.file << log::RESET() << "\n";
            // Indent the captured tail.
            std::istringstream iss(res.tail);
            std::string line;
            while (std::getline(iss, line)) std::cout << "      " << line << "\n";
            std::cout << "\n";
        }
    }

    std::cout << log::bold("Summary: ")
              << log::GREEN() << filesOk << " ok" << log::RESET()
              << ", "
              << (filesFail > 0 ? log::RED() : log::DIM())
              << filesFail << " failed" << log::RESET()
              << log::dim("  · ")
              << totalPassed << " passed / " << totalFailed << " failed (cases)\n";

    return filesFail == 0 ? 0 : 1;
}

// `quirk pkg check` — validate ./quirk.toml and the package layout without
// registering. Pure read-only diagnostic, safe to run any time.
static int cmd_check(const std::vector<std::string>&) {
    Manifest m;
    if (!read_manifest("quirk.toml", m)) {
        std::cerr << "check: no ./quirk.toml here\n";
        return 1;
    }
    std::cout << "Checking " << (m.name.empty() ? std::string("(unnamed)") : m.name)
              << " " << m.version << "\n\n";
    auto findings = validate_package(m);
    int errs = print_findings(findings);
    return errs > 0 ? 1 : 0;
}

// Bump one component of a semver-ish version string. Drops any pre-release
// or build suffix. Unknown component falls back to patch.
static std::string bump_version(const std::string& version, const std::string& part) {
    auto v = parse_version(version);                  // [major, minor, patch]
    if      (part == "major") { v[0]++; v[1] = 0; v[2] = 0; }
    else if (part == "minor") {          v[1]++; v[2] = 0; }
    else                       {                  v[2]++;     }   // default: patch
    return std::to_string(v[0]) + "." + std::to_string(v[1]) + "." + std::to_string(v[2]);
}

// Rewrite `version = "..."` in a manifest file in place. Doesn't touch
// anything else (preserves comments / formatting around it). If no version
// line exists, appends one near the top.
static int rewrite_manifest_version(const fs::path& path, const std::string& newVersion) {
    std::ifstream in(path);
    if (!in) return 1;
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    in.close();

    // Replace the first line that starts with `version` (optional whitespace).
    std::regex versionLine(R"((^|\n)([ \t]*version[ \t]*=[ \t]*)\"[^\"]*\")");
    if (std::regex_search(content, versionLine)) {
        content = std::regex_replace(content, versionLine,
                                     "$1$2\"" + newVersion + "\"",
                                     std::regex_constants::format_first_only);
    } else {
        // No version line — prepend one.
        content = "version = \"" + newVersion + "\"\n" + content;
    }
    std::ofstream out(path);
    out << content;
    return 0;
}

// `quirk pkg release [--bump patch|minor|major] [--no-push]` — validate,
// tag the current `version` from quirk.toml as `vX.Y.Z`, and push.
static int cmd_release(const std::vector<std::string>& args) {
    std::string bumpPart;
    bool push = true;
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "--bump") {
            if (i + 1 >= args.size()) {
                std::cerr << "release: --bump requires patch|minor|major\n";
                return 1;
            }
            bumpPart = args[++i];
            if (bumpPart != "patch" && bumpPart != "minor" && bumpPart != "major") {
                std::cerr << "release: --bump must be one of: patch, minor, major\n";
                return 1;
            }
        } else if (args[i] == "--no-push") {
            push = false;
        } else {
            std::cerr << "release: unknown flag '" << args[i] << "'\n";
            return 1;
        }
    }

    if (!fs::exists("quirk.toml")) {
        std::cerr << "release: no ./quirk.toml here\n";
        return 1;
    }
    if (!fs::is_directory(".git")) {
        std::cerr << "release: not a git repo (run `git init` first)\n";
        return 1;
    }

    // Bump manifest version + commit, if --bump given.
    if (!bumpPart.empty()) {
        Manifest cur;
        if (!read_manifest("quirk.toml", cur) || cur.version.empty()) {
            std::cerr << "release: can't read current `version` from quirk.toml\n";
            return 1;
        }
        std::string next = bump_version(cur.version, bumpPart);
        std::cout << "Bumping " << cur.version << " → " << next << "\n";
        if (rewrite_manifest_version("quirk.toml", next) != 0) return 1;
        std::string cmd = "git add quirk.toml && git commit -m \"release v"
                       + next + "\" 2>&1";
        if (std::system(cmd.c_str()) != 0) {
            std::cerr << "release: failed to commit version bump\n";
            return 1;
        }
    }

    // Read (possibly just-bumped) manifest.
    Manifest m;
    if (!read_manifest("quirk.toml", m) || m.version.empty()) {
        std::cerr << "release: invalid quirk.toml\n";
        return 1;
    }

    // Run the validation pipeline first.
    std::cout << "\nValidating " << m.name << " " << m.version << "\n\n";
    auto findings = validate_package(m);
    int errors = print_findings(findings);
    if (errors > 0) {
        std::cerr << "\nRelease aborted: fix the error(s) above first.\n";
        return 1;
    }
    std::cout << "\n";

    // Refuse if working tree is dirty (unless we just bumped, in which case
    // we committed cleanly).
    std::string status = capture("git status --porcelain");
    if (!status.empty()) {
        std::cerr << "release: working tree is dirty — commit or stash first\n";
        std::cerr << status << "\n";
        return 1;
    }

    std::string tag = "v" + m.version;

    // Refuse if the tag already exists.
    std::string existing = capture("git tag -l " + tag);
    if (!existing.empty()) {
        std::cerr << "release: tag '" << tag << "' already exists\n";
        std::cerr << "  Bump the version with --bump or delete the old tag:\n";
        std::cerr << "    git tag -d " << tag << " && git push origin :refs/tags/" << tag << "\n";
        return 1;
    }

    // Tag the current HEAD.
    std::string cmd = "git tag " + tag + " 2>&1";
    if (std::system(cmd.c_str()) != 0) {
        std::cerr << "release: failed to create tag\n";
        return 1;
    }
    std::cout << "  ✓ tagged " << tag << "\n";

    if (push) {
        std::cout << "  ↑ pushing...\n";
        // Push the commit (if any) and the new tag. Bail early if the
        // branch push fails so the tag push doesn't quietly succeed on a
        // partial publish.
        std::string p1 = "git push 2>&1";
        if (std::system(p1.c_str()) != 0) {
            std::cerr << "release: `git push` failed; tag not pushed\n";
            return 1;
        }
        std::string p2 = "git push origin " + tag + " 2>&1";
        if (std::system(p2.c_str()) != 0) {
            std::cerr << "release: pushed branch but `git push origin "
                      << tag << "` failed\n";
            return 1;
        }
        std::cout << "  ✓ pushed " << tag << " to origin\n";
    } else {
        std::cout << "  (skipped push — run `git push origin " << tag << "` when ready)\n";
    }

    std::cout << "\nReleased " << m.name << " " << tag << ".\n";
    if (!m.repository.empty()) {
        std::cout << "  Install with: quirk pkg install " << m.name << "@" << tag << "\n";
    }
    return 0;
}

// `quirk pkg versions <name>` — list every published version of a package
// by querying its git tags (via `git ls-remote`, cached 24h).
static int cmd_versions(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "versions: need a package name or URL\n";
        return 1;
    }
    std::string spec = args[0];
    bool refresh = false;
    for (size_t i = 1; i < args.size(); i++) {
        if (args[i] == "--refresh") refresh = true;
    }

    // Resolve a bare name through the registry/aliases first.
    std::string url;
    if (spec.find('/') == std::string::npos
        && spec.find("://") == std::string::npos) {
        std::string resolved = registry_lookup(spec);
        if (resolved.empty()) {
            std::cerr << "versions: '" << spec << "' not in registry / aliases\n";
            return 1;
        }
        // Strip any @<default-ref> from the registry entry.
        auto at = resolved.find('@');
        url = (at == std::string::npos) ? resolved : resolved.substr(0, at);
    } else {
        url = spec;
        auto at = url.find('@');
        if (at != std::string::npos) url = url.substr(0, at);
    }

    auto tags = discover_tags(url, refresh);
    if (tags.empty()) {
        std::cout << "No tagged versions for " << url << ".\n";
        std::cout << "  (HEAD of default branch is the only installable target.)\n";
        return 0;
    }
    // Mark cached/active versions.
    std::string pkgName = spec;
    if (pkgName.find('/') != std::string::npos)
        pkgName = pkgName.substr(pkgName.find_last_of('/') + 1);
    auto cached = list_cached_versions(pkgName);
    std::set<std::string> cachedSet(cached.begin(), cached.end());
    std::string active = read_current_version(package_install_dir(), pkgName);

    std::cout << tags.size() << " published version(s) of " << url << ":\n";
    for (auto& t : tags) {
        std::string v = (!t.empty() && t[0] == 'v') ? t.substr(1) : t;
        std::cout << "  " << t;
        if (cachedSet.count(v))    std::cout << "  [cached]";
        if (v == active)           std::cout << "  [active]";
        std::cout << "\n";
    }
    return 0;
}

// `quirk pkg register [<alias>]` — register THIS project under a name,
// reading the manifest's `name` and `repository` fields. Lets a package
// author do one command from inside their repo and have it findable by
// short name everywhere else.
static int cmd_register(const std::vector<std::string>& args) {
    Manifest m;
    if (!read_manifest("quirk.toml", m) || m.name.empty()) {
        std::cerr << "register: no ./quirk.toml here (or missing `name =`)\n";
        std::cerr << "  Run `quirk init` first.\n";
        return 1;
    }

    // Allow `--skip-checks` for power users / CI that already validated.
    bool skipChecks = false;
    std::vector<std::string> aliasArgs;
    for (auto& a : args) {
        if (a == "--skip-checks") skipChecks = true;
        else aliasArgs.push_back(a);
    }

    if (!skipChecks) {
        std::cout << "Validating " << m.name << " " << m.version << "\n\n";
        auto findings = validate_package(m);
        int errs = print_findings(findings);
        if (errs > 0) {
            std::cerr << "\nRegister aborted: fix the error(s) above first.\n";
            std::cerr << "(Override with --skip-checks if you really know what you're doing.)\n";
            return 1;
        }
        std::cout << "\n";
    }

    if (m.repository.empty()) {
        std::cerr << "register: ./quirk.toml has no `repository = ...` field\n";
        std::cerr << "  Add e.g.   repository = \"github.com/<you>/" << m.name << "\"   to your manifest first.\n";
        return 1;
    }

    // Normalize repo: strip leading https:// and trailing .git for the alias
    // value — `install_one`'s git path accepts both forms anyway.
    std::string repo = m.repository;
    auto colon = repo.find("://");
    if (colon != std::string::npos) repo = repo.substr(colon + 3);
    if (repo.size() > 4 && repo.substr(repo.size() - 4) == ".git")
        repo = repo.substr(0, repo.size() - 4);

    std::string alias = aliasArgs.empty() ? m.name : aliasArgs[0];
    auto aliases = read_kv_file(aliases_path());
    auto existing = aliases.find(alias);
    if (existing != aliases.end() && existing->second != repo) {
        std::cerr << "register: '" << alias << "' is already registered to "
                  << existing->second << "\n";
        std::cerr << "  Override:  quirk pkg registry add " << alias << " " << repo << "\n";
        return 1;
    }
    aliases[alias] = repo;
    write_kv_file(aliases_path(), aliases,
                  "Quirk package aliases. `name = \"github.com/owner/repo\"`");
    std::cout << "Registered: " << alias << " → " << repo << "\n";
    std::cout << "  Others can now run:  quirk pkg install " << alias << "\n";
    return 0;
}

// ----------------------- Advisories (security audit) ------------------
// Registry hosts a flat `advisories.toml` of known-bad versions. Local cache
// at ~/.quirk/advisories-cache.toml with a 24h TTL. `quirk pkg audit` reads
// quirk.lock and reports any matches.

struct Advisory {
    std::string id;            // QSA-####
    std::string package;       // affected package name
    std::string versions;      // semver range (e.g. "<0.2.0,>=0.1.0")
    std::string severity;      // "low" | "moderate" | "high" | "critical"
    std::string title;
    std::string description;
    std::string fix;
    std::string url;
};

static fs::path advisories_cache_path() {
    fs::path q = quirk_home_dir();
    return q.empty() ? fs::path{} : q / "advisories-cache.toml";
}

// Derive an advisory-index URL from the registry URL. Convention: same repo,
// `advisories.toml` instead of `index.toml`.
static std::string resolve_advisories_url() {
    std::string idx = resolve_registry_url();
    auto pos = idx.rfind("index.toml");
    if (pos != std::string::npos) return idx.substr(0, pos) + "advisories.toml";
    return idx + "/advisories.toml";
}

static std::vector<Advisory> read_advisories(const fs::path& path) {
    std::vector<Advisory> out;
    std::ifstream in(path);
    if (!in) return out;
    Advisory cur;
    bool inAdv = false;
    auto flush = [&]() {
        if (inAdv && !cur.package.empty()) out.push_back(cur);
        cur = Advisory{};
        inAdv = false;
    };
    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        if (t == "[[advisory]]") { flush(); inAdv = true; continue; }
        if (!inAdv) continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(t.substr(0, eq));
        std::string v = unquote(trim(t.substr(eq + 1)));
        if      (k == "id")          cur.id          = v;
        else if (k == "package")     cur.package     = v;
        else if (k == "versions")    cur.versions    = v;
        else if (k == "severity")    cur.severity    = v;
        else if (k == "title")       cur.title       = v;
        else if (k == "description") cur.description = v;
        else if (k == "fix")         cur.fix         = v;
        else if (k == "url")         cur.url         = v;
    }
    flush();
    return out;
}

// Pull the latest advisory list. Uses the local cache if fresh (24h).
static int fetch_advisories(bool forceRefresh, bool quiet = false) {
    fs::path cache = advisories_cache_path();
    if (cache.empty()) return 1;
    if (!forceRefresh && fs::exists(cache)) {
        struct stat st;
        if (::stat(cache.c_str(), &st) == 0
            && ::time(nullptr) - st.st_mtime < 24 * 60 * 60) {
            return 0;  // cache is fresh
        }
    }
    std::string url = resolve_advisories_url();
    if (!quiet) std::cout << "Fetching advisories from " << url << "...\n";
    fs::path tmp = cache.string() + ".tmp";
    std::string cmd = "curl -fsSL \"" + url + "\" -o \"" + tmp.string() + "\" 2>/dev/null";
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        if (!quiet) std::cerr << "audit: couldn't fetch advisories (network down?)\n"
                              << "  Continuing with the cached list (may be stale).\n";
        std::error_code ec; fs::remove(tmp, ec);
        return 0;   // soft-fail: keep old cache
    }
    std::error_code ec;
    fs::rename(tmp, cache, ec);
    return 0;
}

// Severity → numeric for sorting / exit codes.
static int severity_rank(const std::string& s) {
    if (s == "critical") return 4;
    if (s == "high")     return 3;
    if (s == "moderate") return 2;
    if (s == "low")      return 1;
    return 0;
}

// `quirk pkg audit` — check installed deps against the advisory list.
static int cmd_audit(const std::vector<std::string>& args) {
    bool refresh = false;
    std::string minSeverity;     // empty = report all
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "--refresh") refresh = true;
        else if (args[i] == "--severity") {
            if (i + 1 >= args.size()) {
                std::cerr << "audit: --severity requires a level (low|moderate|high|critical)\n";
                return 1;
            }
            minSeverity = args[++i];
        }
    }

    fetch_advisories(refresh);
    auto advisories = read_advisories(advisories_cache_path());
    if (advisories.empty()) {
        std::cout << "audit: no advisories available "
                  << "(empty cache or registry has no advisories.toml)\n";
        return 0;
    }

    // Build a name → version map of what's installed in this project.
    // Prefer quirk.lock (more precise); fall back to scanning packages/.
    std::map<std::string, std::string> installed;
    auto lock = read_lockfile("quirk.lock");
    if (!lock.empty()) {
        for (auto& [name, e] : lock) installed[name] = e.version;
    } else {
        fs::path pkgRoot = package_install_dir();
        if (fs::is_directory(pkgRoot)) {
            for (auto& entry : fs::directory_iterator(pkgRoot)) {
                std::string fn = entry.path().filename().string();
                if (fn.empty() || fn[0] == '.') continue;
                if (fn.size() > 10 && fn.substr(fn.size() - 10) == ".dist-info") continue;
                Manifest m;
                if (read_manifest((entry.path() / "quirk.toml").string(), m) && !m.version.empty())
                    installed[fn] = m.version;
            }
        }
    }

    if (installed.empty()) {
        std::cout << "audit: no installed packages to check.\n";
        return 0;
    }

    // Match.
    std::vector<Advisory> hits;
    int minRank = minSeverity.empty() ? 0 : severity_rank(minSeverity);
    for (auto& adv : advisories) {
        auto it = installed.find(adv.package);
        if (it == installed.end()) continue;
        if (!version_satisfies(it->second, adv.versions)) continue;
        if (severity_rank(adv.severity) < minRank) continue;
        hits.push_back(adv);
    }

    std::cout << "Audited " << installed.size() << " package(s) against "
              << advisories.size() << " advisor" << (advisories.size() == 1 ? "y" : "ies") << ".\n";
    if (hits.empty()) {
        std::cout << "\nNo known issues.\n";
        return 0;
    }

    // Sort hits by severity desc, then by name.
    std::sort(hits.begin(), hits.end(), [](const Advisory& a, const Advisory& b) {
        int ra = severity_rank(a.severity), rb = severity_rank(b.severity);
        if (ra != rb) return ra > rb;
        return a.package < b.package;
    });

    std::cout << "\n" << hits.size() << " issue(s) found:\n\n";
    for (auto& a : hits) {
        const char* glyph = severity_rank(a.severity) >= 3 ? "✗" : "⚠";
        std::cout << "  " << glyph << " [" << (a.severity.empty() ? "?" : a.severity)
                  << "] " << a.package << " " << installed[a.package]
                  << "  (" << a.id << ")\n";
        if (!a.title.empty())    std::cout << "      " << a.title << "\n";
        if (!a.versions.empty()) std::cout << "      vulnerable: " << a.versions << "\n";
        if (!a.fix.empty())      std::cout << "      fix:        " << a.fix << "\n";
        if (!a.url.empty())      std::cout << "      ref:        " << a.url << "\n";
        std::cout << "\n";
    }
    return 1;
}

// `quirk pkg registry <subcommand>` — manage name → URL mappings so users
// can `quirk pkg install <name>` instead of typing full git URLs.
static int cmd_registry(const std::vector<std::string>& args) {
    std::string sub = args.empty() ? "list" : args[0];
    if (sub == "-l" || sub == "--list") sub = "list";

    if (sub == "list") {
        auto aliases  = read_kv_file(aliases_path());
        auto registry = read_kv_file(registry_cache_path());
        if (aliases.empty() && registry.empty()) {
            std::cout << "No registered names.\n"
                      << "  add one:    quirk pkg registry add <name> <url>\n"
                      << "  fetch idx:  quirk pkg registry update\n";
            return 0;
        }
        auto printRows = [](const std::string& title, const std::map<std::string, std::string>& m) {
            if (m.empty()) return;
            std::cout << title << ":\n";
            size_t pad = 0;
            for (auto& kv : m) if (kv.first.size() > pad) pad = kv.first.size();
            for (auto& kv : m) {
                std::cout << "  " << kv.first;
                for (size_t i = kv.first.size(); i < pad + 2; i++) std::cout << ' ';
                std::cout << kv.second << "\n";
            }
        };
        printRows("Local aliases (~/.quirk/aliases.toml)", aliases);
        if (!aliases.empty() && !registry.empty()) std::cout << "\n";
        printRows("Registry (~/.quirk/registry-cache.toml)", registry);
        return 0;
    }

    if (sub == "search") {
        if (args.size() < 2) {
            std::cerr << "registry search: need a query string\n";
            return 1;
        }
        const std::string& q = args[1];
        auto check = [&](const std::map<std::string, std::string>& m) {
            for (auto& kv : m) {
                if (kv.first.find(q)  != std::string::npos
                 || kv.second.find(q) != std::string::npos) {
                    std::cout << "  " << kv.first << "  " << kv.second << "\n";
                }
            }
        };
        check(read_kv_file(aliases_path()));
        check(read_kv_file(registry_cache_path()));
        return 0;
    }

    if (sub == "add") {
        if (args.size() < 3) {
            std::cerr << "registry add: usage `quirk pkg registry add <name> <url>`\n";
            return 1;
        }
        auto aliases = read_kv_file(aliases_path());
        aliases[args[1]] = args[2];
        write_kv_file(aliases_path(), aliases,
                      "Quirk package aliases. `name = \"github.com/owner/repo\"`");
        std::cout << "Added: " << args[1] << " → " << args[2] << "\n";
        return 0;
    }

    if (sub == "remove") {
        if (args.size() < 2) {
            std::cerr << "registry remove: need a name\n";
            return 1;
        }
        auto aliases = read_kv_file(aliases_path());
        auto it = aliases.find(args[1]);
        if (it == aliases.end()) {
            std::cerr << "registry remove: '" << args[1] << "' not found in local aliases\n";
            return 1;
        }
        aliases.erase(it);
        write_kv_file(aliases_path(), aliases,
                      "Quirk package aliases. `name = \"github.com/owner/repo\"`");
        std::cout << "Removed: " << args[1] << "\n";
        return 0;
    }

    if (sub == "update") {
        std::string url = resolve_registry_url();
        std::cout << "Fetching registry index from " << url << "...\n";
        fs::path tmp = registry_cache_path().string() + ".tmp";
        std::string cmd = "curl -fsSL \"" + url + "\" -o \"" + tmp.string() + "\"";
        int rc = std::system(cmd.c_str());
        if (rc != 0) {
            std::cerr << "registry update: fetch failed (curl rc=" << rc << ")\n";
            std::cerr << "  Local aliases (`registry add`) still work.\n";
            fs::remove(tmp);
            return 1;
        }
        std::error_code ec;
        fs::rename(tmp, registry_cache_path(), ec);
        auto kv = read_kv_file(registry_cache_path());
        std::cout << "  ✓ registry cached (" << kv.size() << " packages)\n";
        return 0;
    }

    if (sub == "url") {
        // Print or set the configured registry URL. Accepts short forms
        // like `github.com/owner/repo` or just `owner/repo`; we store the
        // user's input verbatim and expand at fetch time so the config file
        // stays readable.
        if (args.size() == 1) {
            auto cfg = read_kv_file(config_path());
            auto it = cfg.find("registry");
            std::string raw = (it != cfg.end()) ? it->second : std::string(DEFAULT_REGISTRY_URL);
            std::string full = expand_registry_url(raw);
            std::cout << raw << "\n";
            if (full != raw) std::cout << "  → " << full << "\n";
            return 0;
        }
        auto cfg = read_kv_file(config_path());
        cfg["registry"] = args[1];
        write_kv_file(config_path(), cfg, "Quirk config");
        std::string full = expand_registry_url(args[1]);
        std::cout << "Registry: " << args[1];
        if (full != args[1]) std::cout << "\n      → " << full;
        std::cout << "\n";
        return 0;
    }

    std::cerr << "registry: unknown subcommand '" << sub << "'\n";
    std::cerr << "  usage: quirk pkg registry list\n";
    std::cerr << "         quirk pkg registry search <query>\n";
    std::cerr << "         quirk pkg registry add <name> <url>\n";
    std::cerr << "         quirk pkg registry remove <name>\n";
    std::cerr << "         quirk pkg registry update\n";
    std::cerr << "         quirk pkg registry url [<url>]\n";
    return 1;
}

// `quirk cache <subcommand>` — manage the cross-project version cache.
static int cmd_cache(const std::vector<std::string>& args) {
    fs::path cdir = cache_dir();
    std::string sub = args.empty() ? "list" : args[0];
    if (sub == "-l" || sub == "--list") sub = "list";

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
        / ("quirk_eval_" + std::to_string(getpid()) + ".quirk");
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

// Find the entry .quirk file of a named module by mirroring the compiler's
// resolveImportPath: search install dirs + stdlib + project-local for the
// usual layouts (X.quirk, X/index.quirk, X/src/index.quirk, X/current/src/index.quirk).
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
                root / (name + ".quirk"),
                root / name / "index.quirk",
                root / name / "src" / "index.quirk",
                root / name / "src" / (name + ".quirk"),
                root / name / "current" / "src" / "index.quirk",
                root / name / "current" / "src" / (name + ".quirk"),
             }) {
            if (fs::exists(c)) return c;
        }
    }
    return {};
}

// `quirk script <name>` — run a named script from `./quirk.toml [scripts]`.
// Each script is a shell command; we hand it to the shell verbatim so users
// can chain commands. Extra args appended after the command.
static int cmd_script(const std::vector<std::string>& args) {
    if (args.empty()) {
        // List available scripts when called with no args.
        Manifest pm;
        if (!read_manifest("quirk.toml", pm) || pm.scripts.empty()) {
            std::cerr << "script: no scripts defined in ./quirk.toml\n";
            std::cerr << "  add a [scripts] block: e.g.   test = \"quirk run tests/all.quirk\"\n";
            return 1;
        }
        std::cout << "Scripts in ./quirk.toml:\n";
        size_t pad = 0;
        for (auto& s : pm.scripts) if (s.first.size() > pad) pad = s.first.size();
        for (auto& s : pm.scripts) {
            std::cout << "  " << s.first;
            for (size_t i = s.first.size(); i < pad + 2; i++) std::cout << ' ';
            std::cout << s.second << "\n";
        }
        return 0;
    }
    Manifest pm;
    if (!read_manifest("quirk.toml", pm)) {
        std::cerr << "script: no quirk.toml here\n";
        return 1;
    }
    const std::string& name = args[0];
    for (auto& s : pm.scripts) {
        if (s.first != name) continue;
        std::string cmd = s.second;
        for (size_t i = 1; i < args.size(); i++) cmd += " \"" + args[i] + "\"";
        int rc = std::system(cmd.c_str());
        return WIFEXITED(rc) ? WEXITSTATUS(rc) : 1;
    }
    std::cerr << "script: '" << name << "' not found in ./quirk.toml [scripts]\n";
    if (!pm.scripts.empty()) {
        std::cerr << "  available:";
        for (auto& s : pm.scripts) std::cerr << " " << s.first;
        std::cerr << "\n";
    }
    return 1;
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
    auto kv = [](const std::string& k, const std::string& v) {
        std::cout << log::dim(k + std::string(16 - k.size(), ' ')) << v << "\n";
    };
    kv("quirk:",      self_binary());
    kv("version:",    QUIRK_VERSION);
    kv("QUIRK_HOME:", envHome ? envHome : log::dim("(unset)"));
    kv("in venv:",    isVenv ? (std::string(log::GREEN()) + "yes" + log::RESET())
                              : (std::string(log::dim("no"))));

    // If activated, surface the venv's metadata inline — saves a separate
    // `quirk venv info` call for the common case.
    if (isVenv) {
        auto cfg = read_venv_cfg(envHome);
        if (cfg.count("created"))  kv("  created:",  cfg["created"]);
        if (cfg.count("version"))  kv("  built by:", cfg["version"]);
        bool versionMismatch = cfg.count("version") && cfg["version"] != QUIRK_VERSION;
        if (versionMismatch) {
            std::cout << "    " << log::YELLOW() << "⚠ this venv was created by compiler "
                      << cfg["version"] << ", but you're running " << QUIRK_VERSION << log::RESET() << "\n"
                      << "    " << log::dim("→ run `quirk venv repair` to refresh it") << "\n";
        }
    }

    fs::path proj = find_project_root(fs::current_path());
    kv("project root:", proj.empty() ? log::dim("(none)") : proj.string());
    kv("install dir:",  package_install_dir().string());
    fs::path stdlib = find_system_stdlib();
    kv("stdlib:",       stdlib.empty() ? log::dim("(not found)") : stdlib.string());
    kv("stdlib v:",     std::string(QUIRK_VERSION)
                      + log::dim("  (bundled with compiler — `quirk stdlib` to inspect)"));
    kv("user-global:",  std::getenv("HOME")
        ? std::string(std::getenv("HOME")) + "/.quirk/packages"
        : log::dim("(no $HOME)"));
    return 0;
}

// `quirk stdlib` introspection. Stdlib version is coupled to the compiler
// today (one number for both), but we expose it through a dedicated verb so
// users can ask "what stdlib am I using and where is it" without grepping
// `quirk env`. Future-proofs the surface if we ever split the two.
//
// Subcommands:
//   quirk stdlib                  → list (default)
//   quirk stdlib version          → print version
//   quirk stdlib path             → print root path
//   quirk stdlib list             → list modules
//   quirk stdlib show <mod>       → list files inside a module
static int cmd_stdlib(const std::vector<std::string>& args) {
    fs::path stdlib = find_system_stdlib();

    std::string sub = args.empty() ? "list" : args[0];
    if (sub == "-l" || sub == "--list") sub = "list";

    if (sub == "version") {
        std::cout << QUIRK_VERSION << "\n";
        return 0;
    }
    if (sub == "path") {
        if (stdlib.empty()) { log::err("stdlib not found on this system"); return 1; }
        std::cout << stdlib.string() << "\n";
        return 0;
    }
    if (sub == "list") {
        if (stdlib.empty()) { log::err("stdlib not found on this system"); return 1; }
        std::vector<std::string> mods;
        for (auto& e : fs::directory_iterator(stdlib)) {
            if (!e.is_directory()) continue;
            std::string n = e.path().filename().string();
            if (n.empty() || n[0] == '.' || n == "packages") continue;
            mods.push_back(n);
        }
        std::sort(mods.begin(), mods.end());
        std::cout << log::bold(std::to_string(mods.size()) + " stdlib module(s)")
                  << log::dim(" — " + stdlib.string() + "  (bundled with quirk " + QUIRK_VERSION + ")") << "\n";
        size_t pad = 0;
        for (auto& m : mods) if (m.size() > pad) pad = m.size();
        for (auto& m : mods) {
            // Count .quirk files for a quick "size" hint.
            int files = 0;
            for (auto& e : fs::recursive_directory_iterator(stdlib / m)) {
                if (e.is_regular_file() && e.path().extension() == ".quirk") files++;
            }
            std::cout << "  " << m;
            for (size_t i = m.size(); i < pad + 2; i++) std::cout << ' ';
            std::cout << log::dim(std::to_string(files) + " file" + (files == 1 ? "" : "s")) << "\n";
        }
        return 0;
    }
    if (sub == "show") {
        if (args.size() < 2) { log::err("stdlib show: need a module name"); return 1; }
        if (stdlib.empty()) { log::err("stdlib not found on this system"); return 1; }
        const std::string& mod = args[1];
        fs::path modPath = stdlib / mod;
        if (!fs::is_directory(modPath)) {
            log::err("stdlib has no module '" + mod + "'");
            std::cerr << "    " << log::dim("(`quirk stdlib list` shows what's available)") << "\n";
            return 1;
        }
        std::cout << log::bold(mod) << log::dim("  " + modPath.string()) << "\n";
        std::vector<fs::path> files;
        for (auto& e : fs::recursive_directory_iterator(modPath)) {
            if (e.is_regular_file() && e.path().extension() == ".quirk")
                files.push_back(fs::relative(e.path(), modPath));
        }
        std::sort(files.begin(), files.end());
        for (auto& f : files) std::cout << "  " << f.string() << "\n";
        return 0;
    }

    std::cerr << "stdlib: unknown subcommand '" << sub << "'\n"
              << "    usage: quirk stdlib [version|path|list|show <mod>]\n";
    return 1;
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
        std::ofstream out(dir / "src" / "index.quirk");
        out << "// " << name << " — Quirk package entry point\n\n"
            << "define hello(who: String) -> String {\n"
            << "    return \"Hello, \" + who + \"!\"\n"
            << "}\n\n"
            << "define main() -> void {\n"
            << "    print(hello(\"world\"))\n"
            << "}\n";
    }
    {
        std::ofstream out(dir / "tests" / (name + "_test.quirk"));
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
              << "  ├── src/index.quirk\n"
              << "  ├── tests/" << name << "_test.quirk\n"
              << "  └── .gitignore\n";
    return 0;
}

// Forward declaration so cmd_help can describe the full set of commands.
static void print_pm_help();
// Defined below in the dispatch section; used by help_for() to normalize
// short verb aliases (i/add/rm/un/up/ls) before matching.
static std::string canonicalize_verb(const std::string& v);

// Per-command help text. Returns empty string if `cmd` isn't recognized,
// in which case cmd_help falls back to the top-level summary.
static std::string help_for(const std::string& cmdIn) {
    // Accept short aliases (`quirk help i`, `quirk help rm`, …) by mapping
    // to the canonical verb name.
    const std::string cmd = canonicalize_verb(cmdIn);
    if (cmd == "run")
        return "quirk run <file.quirk> [args...]\n"
               "    Run a Quirk script. Equivalent to `quirk <file.quirk>`.\n";
    if (cmd == "eval" || cmd == "-c")
        return "quirk eval \"<code>\"\n"
               "    Wrap a one-liner in `define main() { ... }` and run it.\n"
               "    Short form: quirk -c \"<code>\"\n";
    if (cmd == "module" || cmd == "-m")
        return "quirk module <name> [args...]\n"
               "    Import the named module and call its `main()`.\n"
               "    Short form: quirk -m <name>\n";
    if (cmd == "install")
        return "quirk install [-r <file>] [-e <path>] [--dev] [--frozen] [--no-lock] [pkg ...]\n"
               "    Short aliases: quirk i, quirk add\n"
               "    Install dependencies. Specs can be:\n"
               "      <name>[@<range>]                resolved via registry\n"
               "      github.com/owner/repo[@ref]    direct git URL\n"
               "      <path>[@<version>]              local directory\n"
               "    -e <path>   editable (symlink) install\n"
               "    -r <file>   read deps from a manifest\n"
               "    --dev       also install [dev-deps]\n"
               "    --frozen    fail if quirk.lock is missing or out of date (CI)\n"
               "    --no-lock   don't read or write quirk.lock\n"
               "\n"
               "  quirk.lock is generated on first install and used on subsequent\n"
               "  runs to install the exact same versions across machines.\n";
    if (cmd == "upgrade")
        return "quirk upgrade [pkg ...]\n"
               "    Short alias: quirk up\n"
               "    Bump packages to the latest installed version (local) or\n"
               "    re-clone HEAD (git).\n";
    if (cmd == "remove" || cmd == "uninstall")
        return "quirk remove <pkg>[@<version>] ...\n"
               "    Short aliases: quirk rm, quirk un, quirk uninstall\n"
               "    Remove a package, or a specific version of it.\n";
    if (cmd == "list" || cmd == "packages")
        return "quirk list\n"
               "    Short aliases: quirk ls, quirk -p, quirk packages\n"
               "    Print installed packages with versions.\n";
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
    if (cmd == "sync")
        return "quirk sync [--no-venv] [--dev]\n"
               "    Bootstrap a clone: create ./.venv if missing, install every\n"
               "    dep from quirk.toml at the versions pinned in quirk.lock,\n"
               "    then print how to activate.\n"
               "    --no-venv  install into ./packages/ instead of a venv\n"
               "    --dev      also install [dev-deps]\n";
    if (cmd == "stdlib")
        return "quirk stdlib [version | path | list | show <mod>]\n"
               "    Introspect the bundled standard library.\n"
               "      version    print the stdlib version (= compiler version)\n"
               "      path       print the stdlib root directory\n"
               "      list       list modules with file counts (default)\n"
               "      show <m>   list .quirk files inside module <m>\n"
               "    The stdlib ships with the compiler; to upgrade it, upgrade\n"
               "    the compiler. See `quirk env` for stdlib path resolution.\n";
    if (cmd == "repl")
        return "quirk repl\n"
               "    Start an interactive Quirk shell. Expressions are evaluated\n"
               "    and printed; `x := ...` bindings persist across lines;\n"
               "    `define`/`struct`/`use` declarations accumulate in the session.\n"
               "    Multi-line input is detected via brace balance — keep typing\n"
               "    until braces close. Meta commands:\n"
               "      :quit / :q     exit\n"
               "      :reset         wipe the session\n"
               "      :state         print the assembled program\n"
               "      :help / :h     this list\n";
    if (cmd == "fmt")
        return "quirk fmt [--check|--stdout] [file ...]\n"
               "    Reformat Quirk source to a canonical style: 4-space indents\n"
               "    tracked from brace depth, single space around binary operators,\n"
               "    `, ` after commas, one trailing newline, no trailing whitespace.\n"
               "    String literals and `//` comments are preserved verbatim.\n"
               "    With no files, formats every .quirk under the current directory.\n"
               "      --check    exit 1 if any file would change, list them (CI)\n"
               "      --stdout   print formatted output, don't modify files\n"
               "    Same rules as the VS Code formatter — CLI and editor agree.\n";
    if (cmd == "cache")
        return "quirk pkg cache list [<pkg>]\n"
               "    List versions cached at ~/.quirk/cache/ (cross-project).\n"
               "quirk pkg cache clean [<pkg>[@<ver>]]\n"
               "    Drop the whole cache, all versions of one package, or one version.\n"
               "quirk pkg cache dir\n"
               "    Print the cache root path.\n";
    if (cmd == "registry")
        return "quirk pkg registry list\n"
               "    Show known names (local aliases + cached registry).\n"
               "quirk pkg registry search <query>\n"
               "    Find a package by name or URL substring.\n"
               "quirk pkg registry add <name> <url>\n"
               "    Define a local alias so `quirk install <name>` works.\n"
               "quirk pkg registry remove <name>\n"
               "    Drop a local alias.\n"
               "quirk pkg registry update\n"
               "    Fetch the central registry index (cached at\n"
               "    ~/.quirk/registry-cache.toml).\n"
               "quirk pkg registry url [<url>]\n"
               "    Show or set the registry URL (~/.quirk/config.toml).\n";
    if (cmd == "init")
        return "quirk init [-y]\n"
               "    Scaffold a quirk.toml in the current directory by prompting\n"
               "    for each field. Use -y / --yes to skip the prompts and accept\n"
               "    sensible defaults (dir name, MIT, git config for author/repo).\n";
    if (cmd == "register")
        return "quirk pkg register [<alias>] [--skip-checks]\n"
               "    Register THIS project under a short name so others can\n"
               "    `quirk pkg install <alias>`. Reads `name` and `repository`\n"
               "    from ./quirk.toml. With no <alias>, uses the manifest name.\n"
               "    Runs `quirk pkg check` first; --skip-checks bypasses it.\n";
    if (cmd == "check")
        return "quirk pkg check\n"
               "    Validate ./quirk.toml + package layout. Reports manifest\n"
               "    issues, missing files, dirty git state, and runs a\n"
               "    type-check on the entry point.\n"
               "    Same checks run automatically before `quirk pkg register`.\n";
    if (cmd == "versions")
        return "quirk pkg versions <name|url> [--refresh]\n"
               "    List every published version (git tag) of a package.\n"
               "    Cached locally for 24h; --refresh re-queries the remote.\n"
               "    Tags marked [cached] are already in ~/.quirk/cache;\n"
               "    [active] is the one currently installed in this venv.\n";
    if (cmd == "release")
        return "quirk pkg release [--bump patch|minor|major] [--no-push]\n"
               "    Tag the current `version` in ./quirk.toml as `vX.Y.Z` and push.\n"
               "    Runs `quirk pkg check` first; refuses to tag if the working\n"
               "    tree is dirty or the tag already exists.\n"
               "    --bump <part>  bumps the version (and commits the bump) before tagging.\n"
               "    --no-push      tag locally, don't push.\n";
    if (cmd == "audit")
        return "quirk pkg audit [--refresh] [--severity low|moderate|high|critical]\n"
               "    Check installed packages against the registry's advisory list.\n"
               "    Reads quirk.lock if present (precise versions), else scans\n"
               "    packages/. Exit code 1 if any matching advisory is found.\n"
               "    --refresh   re-fetch the advisory list (default: 24h cache)\n"
               "    --severity  only report at or above this severity\n";
    if (cmd == "new")
        return "quirk new <name>\n"
               "    Scaffold a new package: <name>/quirk.toml, src/index.quirk,\n"
               "    tests/, .gitignore.\n";
    if (cmd == "venv")
        return "quirk venv <path>                            create a venv (= `venv new`)\n"
               "quirk venv new <path>                        create a venv at <path>\n"
               "quirk venv list                              find venvs under cwd (alias: -l, --list)\n"
               "quirk venv info [<path>]                     print cfg + symlink health\n"
               "quirk venv repair [<path>]                   relink stdlib/binary, refresh cfg\n"
               "    Activate with `source <path>/bin/activate`, deactivate with `deactivate`.\n"
               "    Each venv records the compiler version that built it; `quirk env` warns\n"
               "    when an activated venv was built by a different compiler — run `repair`\n"
               "    after upgrading.\n";
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
    if (cmd == "script")
        return "quirk script [<name>] [args...]\n"
               "    Run a named script from ./quirk.toml [scripts].\n"
               "    With no args: list available scripts.\n"
               "    `quirk run <name>` also works as long as <name> isn't a path.\n";
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
        "  quirk run <file.quirk> [args...]              run a Quirk script\n"
        "  quirk run <script>                         run a script from [scripts] (alias: quirk script <name>)\n"
        "  quirk eval \"<code>\"                        run a one-liner (alias: -c)\n"
        "  quirk module <name>                        invoke a module's main() (alias: -m)\n"
        "\n"
        "Project / environment:\n"
        "  quirk new <name>                           scaffold a new package\n"
        "  quirk init                                 write a quirk.toml here\n"
        "  quirk sync                                 bootstrap a clone: venv + frozen install\n"
        "  quirk venv <path>                          create a venv (subcmds: new|list|info|repair)\n"
        "  quirk env                                  print resolution context (warns on stale venv)\n"
        "  quirk stdlib                               introspect the bundled standard library\n"
        "  quirk fmt [--check] [file ...]             reformat source to canonical style\n"
        "  quirk repl                                 interactive shell — try snippets, inspect values\n"
        "\n"
        "Packages (short forms shown; long forms work too — see `quirk pkg --help`):\n"
        "  quirk i, add <spec>                        install (alias of `pkg install`)\n"
        "  quirk up [pkg ...]                         upgrade installed versions\n"
        "  quirk rm  <pkg>[@<ver>] ...                remove a package or version\n"
        "  quirk ls                                   list installed packages\n"
        "  quirk show <pkg>                           detailed package info\n"
        "  quirk deps                                 print deps in installable form\n"
        "  quirk cache    list|clean|dir              manage the cross-project cache\n"
        "  quirk registry list|search|add|...         name → URL mappings\n"
        "\n"
        "Misc:\n"
        "  quirk help [command]                       show help (per-command)\n"
        "  quirk version (or --version)               print quirk version\n"
        "\n"
        "Specs:  github.com/owner/repo[@ref]   (git: ref is a tag/branch/SHA)\n"
        "        ./path/to/lib[@<version>]    (local; @<version> can be a range)\n";
}

// Map a short verb to its canonical name. npm/cargo-style aliases so users
// can type `quirk i slug` instead of `quirk install slug`. Identity for
// anything that isn't an alias. Always run on the verb string *before*
// is_subcommand/is_pkg_subcommand and the dispatch chain.
static std::string canonicalize_verb(const std::string& v) {
    static const std::map<std::string, std::string> aliases = {
        {"i",         "install"},   // npm i
        {"add",       "install"},   // cargo add / uv add — friendlier verb
        {"rm",        "remove"},    // Unix-y
        {"un",        "remove"},    // npm un
        {"uninstall", "remove"},
        {"up",        "upgrade"},   // npm up
        {"ls",        "list"},      // Unix / npm ls
    };
    auto it = aliases.find(v);
    return it != aliases.end() ? it->second : v;
}

// Subcommands the package-manager group accepts (`quirk pkg <sub>`).
static bool is_pkg_subcommand(const std::string& arg) {
    std::string c = canonicalize_verb(arg);
    return c == "install" || c == "upgrade" || c == "remove" ||
           c == "list" || c == "packages" ||
           c == "show" || c == "deps" || c == "cache" ||
           c == "registry" || c == "register" || c == "check" ||
           c == "versions" || c == "release" || c == "audit";
}

// Top-level subcommand verbs (and not a path).
static bool is_subcommand(const std::string& arg) {
    return is_pkg_subcommand(arg) ||
           arg == "pkg" ||
           arg == "init" || arg == "version" || arg == "venv" ||
           arg == "run" || arg == "eval" || arg == "module" ||
           arg == "env" || arg == "new" || arg == "help" ||
           arg == "script" || arg == "sync" || arg == "stdlib" || arg == "fmt" ||
           arg == "repl" || arg == "test";
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
inline bool dispatch(int& argc, char** argv, int& outRc) {
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
    // If <file> isn't a path that exists, fall through to a script lookup
    // in ./quirk.toml so `quirk run test` works for a named script.
    if (first == "run") {
        if (argc < 3) {
            std::cerr << "run: need a filename or script name\n";
            outRc = 1; return true;
        }
        std::string target = argv[2];
        // Shortcut: `quirk run -l` / `quirk run --list` lists named scripts.
        if (target == "-l" || target == "--list") {
            outRc = cmd_script({});
            return true;
        }
        if (!fs::exists(target) && target.find('/') == std::string::npos
                                && target.find('.') == std::string::npos) {
            std::vector<std::string> sa;
            for (int i = 2; i < argc; i++) sa.emplace_back(argv[i]);
            outRc = cmd_script(sa);
            return true;
        }
        shift_argv(argc, argv);
        return false;
    }

    std::vector<std::string> rest;
    for (int i = 2; i < argc; i++) rest.emplace_back(argv[i]);

    // Pull `--verbose` / `--quiet` / `-q` out of the args before subcommands
    // see them — they're global to every PM verb. (`-v` is taken: it means
    // `--version` when alone, and the compiler's own verbose otherwise.)
    {
        std::vector<std::string> filtered;
        for (auto& a : rest) {
            if (a == "--verbose")            log::verbose_flag() = true;
            else if (a == "--quiet" || a == "-q") log::quiet_flag()   = true;
            else                             filtered.push_back(a);
        }
        rest.swap(filtered);
    }

    // `quirk pkg <subcommand>` — re-target the dispatch so the rest works
    // the same as the flat form (kept for back-compat: `quirk install ...`).
    // Canonicalize short aliases (i/add/rm/un/up/ls) up-front so the rest
    // of the dispatch only needs to know about canonical verbs.
    std::string verb = canonicalize_verb(first);
    std::vector<std::string> verbArgs = rest;
    if (verb == "pkg") {
        if (rest.empty() || rest[0] == "help" || rest[0] == "--help" || rest[0] == "-h") {
            std::cout <<
                "Package management — short forms in the left column, long forms on the right:\n"
                "  i, add        install [spec ...]            (aliases: install)\n"
                "  up            upgrade [pkg ...]             (aliases: upgrade)\n"
                "  rm, un        remove <pkg>[@<ver>] ...      (aliases: remove, uninstall)\n"
                "  ls            list                          (aliases: list, -l, --list, -p)\n"
                "  show <pkg>    detailed package info\n"
                "  versions <name>   list every published tag  (alias: -l on a name)\n"
                "  deps          print deps in installable form\n"
                "  audit         check installed pkgs against advisories\n"
                "  cache         list|clean|dir                cross-project cache\n"
                "  registry      list|search|add|remove|update|url\n"
                "  register      register this project for short-name install\n"
                "  release       [--bump patch|minor|major]    tag + push\n"
                "\n"
                "Specs can be:\n"
                "  <name>[@<ver>]                  resolved via registry/aliases\n"
                "  github.com/owner/repo[@<ref>]   direct git URL\n"
                "  <path>[@<ver>]                  local directory (snapshot)\n"
                "\n"
                "Global flags (any pkg command):\n"
                "  --verbose                 extra detail (URLs, paths, cache hits)\n"
                "  --quiet, -q               suppress per-step output; errors only\n"
                "  NO_COLOR / FORCE_COLOR    env vars override color auto-detection\n"
                "\n"
                "Every verb also works flat (no `pkg` prefix), e.g.\n"
                "`quirk i slug` ≡ `quirk install slug` ≡ `quirk pkg install slug`.\n";
            outRc = 0; return true;
        }
        // Shortcut: `quirk pkg -l` / `quirk pkg --list` == `quirk pkg list`.
        if (rest[0] == "-l" || rest[0] == "--list") {
            verb = "list";
            verbArgs.clear();
        } else if (!is_pkg_subcommand(rest[0])) {
            std::cerr << "pkg: unknown subcommand '" << rest[0] << "'\n";
            outRc = 1; return true;
        } else {
            verb = canonicalize_verb(rest[0]);
            verbArgs.assign(rest.begin() + 1, rest.end());
        }
    }

    if (verb == "install")           outRc = cmd_install(verbArgs);
    else if (verb == "upgrade")      outRc = cmd_upgrade(verbArgs);
    else if (verb == "remove" ||
             verb == "uninstall")    outRc = cmd_remove(verbArgs);
    else if (verb == "list" ||
             verb == "packages")     outRc = cmd_list();
    else if (verb == "show")         outRc = cmd_show(verbArgs);
    else if (verb == "init")         outRc = cmd_init(verbArgs);
    else if (verb == "new")          outRc = cmd_new(verbArgs);
    else if (verb == "venv")         outRc = cmd_venv(verbArgs);
    else if (verb == "version")      outRc = cmd_version();
    else if (verb == "eval")         outRc = cmd_eval(verbArgs);
    else if (verb == "module")       outRc = cmd_module(verbArgs);
    else if (verb == "deps")         outRc = cmd_deps();
    else if (verb == "env")          outRc = cmd_env();
    else if (verb == "cache")        outRc = cmd_cache(verbArgs);
    else if (verb == "registry")     outRc = cmd_registry(verbArgs);
    else if (verb == "register")     outRc = cmd_register(verbArgs);
    else if (verb == "check")        outRc = cmd_check(verbArgs);
    else if (verb == "versions")     outRc = cmd_versions(verbArgs);
    else if (verb == "release")      outRc = cmd_release(verbArgs);
    else if (verb == "audit")        outRc = cmd_audit(verbArgs);
    else if (verb == "script")       outRc = cmd_script(verbArgs);
    else if (verb == "sync")         outRc = cmd_sync(verbArgs);
    else if (verb == "fmt")          outRc = cmd_fmt(verbArgs);
    else if (verb == "repl")         outRc = cmd_repl(verbArgs);
    else if (verb == "test")         outRc = cmd_test(verbArgs);
    else if (verb == "stdlib")       outRc = cmd_stdlib(verbArgs);
    else if (verb == "help")         outRc = cmd_help(verbArgs);
    else { print_pm_help(); outRc = 1; }
    return true;
}

}  // namespace qpm
