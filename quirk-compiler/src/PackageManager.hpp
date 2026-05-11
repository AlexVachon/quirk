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

    // Build the venv layout.
    fs::create_directories(venvDir / "bin");
    fs::create_directories(venvDir / "lib" / "quirk" / "packages");

    // Symlink each top-level stdlib module (typing, console, math, ...)
    // into <venv>/lib/quirk/. The compiler's resolver looks there because
    // QUIRK_HOME points at <venv>.
    int linked = 0;
    for (auto& entry : fs::directory_iterator(stdlib)) {
        if (!entry.is_directory()) continue;
        std::string name = entry.path().filename().string();
        if (name.empty() || name[0] == '.' || name == "packages") continue;
        fs::path linkPath = venvDir / "lib" / "quirk" / name;
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

// Where new packages get installed:
//   inside a venv (QUIRK_HOME set) → $QUIRK_HOME/lib/quirk/packages/
//   otherwise                       → ./packages/
static fs::path package_install_dir() {
    if (const char* env = std::getenv("QUIRK_HOME")) {
        return fs::path(env) / "lib" / "quirk" / "packages";
    }
    return fs::path("packages");
}

static int install_one(const std::string& spec_str, bool quiet) {
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
    // Parse flags: -r/--read <file> or positional pkg specs.
    std::string manifestFile;
    std::vector<std::string> specs;
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "-r" || args[i] == "--read") {
            if (i + 1 >= args.size()) {
                std::cerr << "install: -r requires a filename\n";
                return 1;
            }
            manifestFile = args[++i];
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
        if (install_one(s, false) != 0) continue;
        if (haveManifest) {
            PkgSpec ps = parse_spec(s);
            // Replace existing entry or append.
            bool found = false;
            for (auto& d : projMan.deps) if (d.first == ps.name) { d.second = s; found = true; break; }
            if (!found) projMan.deps.emplace_back(ps.name, s);
            dirty = true;
        }
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
    for (auto& n : names) {
        fs::path p = pkgRoot / n;
        if (fs::exists(p)) {
            fs::remove_all(p);
            std::cout << "  ✓ removed " << n << "\n";
        } else {
            std::cerr << "  ! " << n << " not installed\n";
        }
        if (haveManifest) {
            auto it = projMan.deps.begin();
            while (it != projMan.deps.end()) {
                if (it->first == n) it = projMan.deps.erase(it);
                else                 ++it;
            }
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
        fs::path p = pkgRoot / d.first;
        if (fs::exists(p)) fs::remove_all(p);
        std::cout << "Upgrading " << d.first << "...\n";
        // Drop any pinned @ref so we get the default branch's HEAD.
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
    std::vector<std::pair<std::string, std::string>> rows;  // (name, version)
    for (auto& entry : fs::directory_iterator(pkgDir)) {
        if (!entry.is_directory()) continue;
        std::string name = entry.path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        Manifest m;
        std::string ver = "?";
        if (read_manifest((entry.path() / "quirk.toml").string(), m) && !m.version.empty())
            ver = m.version;
        rows.emplace_back(name, ver);
    }
    if (rows.empty()) {
        std::cout << "No packages installed.\n";
        return 0;
    }
    size_t pad = 0;
    for (auto& r : rows) if (r.first.size() > pad) pad = r.first.size();
    std::cout << rows.size() << " package(s) installed:\n";
    for (auto& r : rows) {
        std::cout << "  " << r.first;
        for (size_t i = r.first.size(); i < pad + 2; i++) std::cout << ' ';
        std::cout << r.second << "\n";
    }
    return 0;
}

static int cmd_show(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "show: need a package name\n";
        return 1;
    }
    fs::path p = package_install_dir() / args[0];
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
    return 0;
}

static int cmd_version() {
    std::cout << "quirk " << QUIRK_VERSION << "\n";
    return 0;
}

static void print_pm_help() {
    std::cout <<
        "Quirk — language toolchain & package manager\n"
        "\n"
        "Run a script:\n"
        "  quirk <file.qk> [args...]              run a Quirk script\n"
        "  quirk -v <file.qk>                     run with verbose codegen\n"
        "  quirk --check <file.qk>                type-check only\n"
        "  quirk -o <out> <file.qk>               compile to native binary\n"
        "\n"
        "Package management:\n"
        "  quirk install [-r <file>] [pkg ...]    install dependencies\n"
        "  quirk upgrade [pkg ...]                bump to latest versions\n"
        "  quirk remove <pkg> [pkg ...]           uninstall packages\n"
        "  quirk list (or `packages`)             show installed packages\n"
        "  quirk show <pkg>                       package details\n"
        "  quirk init                             scaffold quirk.toml\n"
        "  quirk venv <name>                      create an isolated environment\n"
        "  quirk version (or --version)           print quirk version\n"
        "\n"
        "A package spec is `github.com/owner/repo[@ref]`. `ref` may be a tag\n"
        "(`v1.2.0`), branch (`main`), or commit SHA. Installed under\n"
        "./packages/<name>/.\n";
}

// True if `arg` is a recognized subcommand verb (and not a path).
static bool is_subcommand(const std::string& arg) {
    return arg == "install" || arg == "upgrade" || arg == "remove" ||
           arg == "uninstall" || arg == "list" || arg == "packages" ||
           arg == "show" || arg == "init" || arg == "version" ||
           arg == "venv";
}

// Top-level dispatch. Returns true if we handled the command and main()
// should exit; returns the exit code via `outRc`. Returns false to let
// the rest of main() proceed (i.e. it's a script run, not a PM command).
static bool dispatch(int argc, char** argv, int& outRc) {
    // No args → printUsage handled elsewhere.
    if (argc < 2) return false;
    std::string first = argv[1];

    // Top-level version flag.
    if (first == "--version" || (argc == 2 && first == "-v")) {
        outRc = cmd_version();
        return true;
    }
    // Top-level packages flag.
    if (first == "-p" || first == "--packages") {
        outRc = cmd_list();
        return true;
    }

    if (!is_subcommand(first)) return false;

    std::vector<std::string> rest;
    for (int i = 2; i < argc; i++) rest.emplace_back(argv[i]);

    if (first == "install")          outRc = cmd_install(rest);
    else if (first == "upgrade")     outRc = cmd_upgrade(rest);
    else if (first == "remove" ||
             first == "uninstall")   outRc = cmd_remove(rest);
    else if (first == "list" ||
             first == "packages")    outRc = cmd_list();
    else if (first == "show")        outRc = cmd_show(rest);
    else if (first == "init")        outRc = cmd_init();
    else if (first == "venv")        outRc = cmd_venv(rest);
    else if (first == "version")     outRc = cmd_version();
    else { print_pm_help(); outRc = 1; }
    return true;
}

}  // namespace qpm
