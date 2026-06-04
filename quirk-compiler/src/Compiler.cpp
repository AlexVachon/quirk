#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetSelect.h"
// TargetRegistry was relocated from Support/ to MC/ mid-LLVM-14. Some
// distros still ship the old layout (Debian's WSL builds, older Ubuntu);
// GitHub's ubuntu-22.04 runner has only the new path. __has_include lets
// the same source compile against both, and forward-portable to LLVM 15+.
#if __has_include(<llvm/MC/TargetRegistry.h>)
#  include "llvm/MC/TargetRegistry.h"
#else
#  include "llvm/Support/TargetRegistry.h"
#endif
#include "llvm/Support/Host.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/SourceMgr.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <vector>
#include <unistd.h>

#include "PackageManager.hpp"

#include "lexer.hpp"
#include "parser.hpp"
#include "sema.hpp"

#include "Backend/Codegen.cpp"

namespace fs = std::filesystem;

// ==========================================================
//  COMPILER OPTIONS
// ==========================================================

// Declared in ast.hpp; defined here so it's a single linker-visible
// symbol. Flipped by `--diagnostics-json`; consulted by every error
// printer (Parser::flushErrors, Sema::printSemaError, etc).
bool g_diagnostics_json = false;

struct CompilerOptions {
    std::string inputFile;
    std::string outputFile;   // non-empty → produce native binary, skip JIT
    bool runImmediate = true;
    bool verbose      = false;
    bool emitIR       = false;
    bool emitAST      = false;
    bool checkOnly    = false; // --check: lex + parse + sema only, no codegen
    // LLVM optimization level. Default 1 — runs the minimal correctness
    // pass set (mem2reg + EarlyCSE + instcombine + simplifycfg + DCE) which
    // the JIT needs to execute our codegen's "loose" IR safely. O2 adds
    // inlining + loop opts + a longer pipeline; meaningful only for compute-
    // heavy code (`--release`). O0 is unsafe today and segfaults a chunk of
    // the suite; reserved for future codegen rewrites.
    int  optLevel     = 1;
    // --debug: pause execution at every Quirk statement and drop into the
    // interactive qdb prompt. Forces optLevel = 0 so line boundaries survive.
    bool debugMode    = false;
    // --no-cache: skip the per-invocation bitcode cache. The cache lives
    // at $HOME/.quirk/cache/<sha256>.bc and stores the post-optimisation
    // LLVM IR for a given (input file, compiler version, opt level)
    // triple. A cache hit skips parse + Sema + Codegen + LLVM passes,
    // which on small scripts is most of the per-run latency.
    bool useCache     = true;
};

// ==========================================================
//  LOGGER
//  In verbose mode: writes [DEBUG] lines to stderr directly.
//  In normal mode:  silent unless there's an error.
// ==========================================================

struct Logger {
    bool verbose;
    explicit Logger(bool v) : verbose(v) {}

    void debug(const std::string& msg) const {
        if (verbose)
            std::cerr << "[DEBUG] " << msg << std::endl;
    }

    void warn(const std::string& msg) const {
        std::cerr << "[WARNING] " << msg << std::endl;
    }

    void error(const std::string& msg) const {
        std::cerr << "Error: " << msg << std::endl;
    }
};

// ==========================================================
//  AST PRINTER
// ==========================================================

class ASTPrinter {
    std::ostream& out;

    void printIndent(int indent) {
        for (int i = 0; i < indent; ++i)
            out << "  ";
    }

public:
    ASTPrinter(std::ostream& output) : out(output) {}

    void print(Node* node, int indent = 0) {
        if (!node) return;

        printIndent(indent);

        if (auto f = dynamic_cast<FunctionNode*>(node)) {
            out << "[Function] " << f->name << (f->isExtern ? " (extern)" : "")
                << " -> " << (f->returnType.empty() ? "void" : f->returnType) << "\n";
            for (auto& p : f->parameters) {
                printIndent(indent + 1);
                out << "Param: " << p.name << " : " << p.type << "\n";
            }
            for (auto& stmt : f->body)
                print(stmt.get(), indent + 1);

        } else if (auto s = dynamic_cast<StructNode*>(node)) {
            out << "[Struct] " << s->name << "\n";
            for (auto& field : s->fields) {
                printIndent(indent + 1);
                out << "Field: " << field.name << " : " << field.type << "\n";
            }

        } else if (auto v = dynamic_cast<VarDeclNode*>(node)) {
            out << "[VarDecl] " << v->op << " Type: " << v->typeAnnotation << "\n";
            print(v->lhs.get(), indent + 1);
            print(v->expression.get(), indent + 1);

        } else if (auto i = dynamic_cast<IfNode*>(node)) {
            out << "[If]\n";
            printIndent(indent + 1);
            out << "Condition:\n";
            print(i->condition.get(), indent + 2);
            printIndent(indent + 1);
            out << "Then:\n";
            for (auto& s : i->thenBranch)
                print(s.get(), indent + 2);
            if (!i->elseBranch.empty()) {
                printIndent(indent + 1);
                out << "Else:\n";
                for (auto& s : i->elseBranch)
                    print(s.get(), indent + 2);
            }

        } else if (auto w = dynamic_cast<WhileNode*>(node)) {
            out << "[While]\n";
            print(w->condition.get(), indent + 1);
            for (auto& s : w->body)
                print(s.get(), indent + 1);

        } else if (auto ret = dynamic_cast<ReturnNode*>(node)) {
            out << "[Return]\n";
            if (ret->expression)
                print(ret->expression.get(), indent + 1);

        } else if (auto call = dynamic_cast<CallNode*>(node)) {
            out << "[Call]\n";
            print(call->callee.get(), indent + 1);
            for (auto& arg : call->args) {
                printIndent(indent + 1);
                out << "Arg: " << arg.name << "\n";
                print(arg.value.get(), indent + 2);
            }

        } else if (auto bin = dynamic_cast<BinaryOpNode*>(node)) {
            out << "[BinaryOp] " << bin->op << "\n";
            print(bin->left.get(), indent + 1);
            print(bin->right.get(), indent + 1);

        } else if (auto lit = dynamic_cast<LiteralNode*>(node)) {
            out << "[Literal] " << lit->value << "\n";

        } else if (auto mem = dynamic_cast<MemberAccessNode*>(node)) {
            out << "[MemberAccess] ." << mem->memberName << "\n";
            print(mem->object.get(), indent + 1);

        } else if (auto cons = dynamic_cast<ConstructorNode*>(node)) {
            out << "[Constructor] " << cons->structName << "\n";
            for (auto& arg : cons->args) {
                printIndent(indent + 1);
                out << "Field: " << arg.fieldName << "\n";
                print(arg.value.get(), indent + 2);
            }

        } else if (auto use = dynamic_cast<UseNode*>(node)) {
            out << "[Use] " << use->moduleName << "\n";

        } else {
            out << "[Unknown Node]\n";
        }
    }

    void printAll(const std::vector<std::unique_ptr<Node>>& nodes) {
        for (const auto& node : nodes) {
            print(node.get());
            out << "\n";
        }
    }
};

// ==========================================================
//  IMPORT RESOLUTION
// ==========================================================

// Per-process cache. The search paths depend only on QUIRK_HOME, HOME, and
// whether QUIRK_HOME contains bin/activate — all stable for the lifetime of
// one compiler invocation. `resolveImportPath()` is hit once per import,
// stdlib loading hits this hundreds of times, so caching saves a meaningful
// amount of allocation + string concat + fs::exists checks.
static std::vector<std::string>& getSearchPaths() {
    static std::vector<std::string> paths = []() {
        std::vector<std::string> p;
        p.reserve(8);

        // Project-local installs win — matches pip's project-takes-precedence
        // behavior and means `quirk install -e <path>` works even when
        // QUIRK_HOME points at a venv or dev tree elsewhere.
        p.push_back("./packages/");

        const char* envHome = std::getenv("QUIRK_HOME");
        bool isVenv = false;
        if (envHome) {
            std::string venvBase = envHome;
            p.push_back(venvBase + "/lib/quirk/packages/");
            p.push_back(venvBase + "/lib/quirk/stdlib/");  // new layout
            p.push_back(venvBase + "/lib/quirk/");         // legacy / dev install
            p.push_back(venvBase + "/packages/");
            // Distinguish a real venv from a dev-tree QUIRK_HOME: only the former
            // has bin/activate. Venvs *isolate* — they don't see user-global.
            isVenv = fs::exists(fs::path(venvBase) / "bin" / "activate");
        }

        // User-global installs — `pip install --user` / `cargo install` equivalent.
        // Skipped when a venv is active so the venv stays a closed world.
        if (!isVenv) {
            if (const char* home = std::getenv("HOME")) {
                p.push_back(std::string(home) + "/.quirk/packages/");
            }
        }

        // Legacy: pre-1.0.8 installs put the stdlib in `libs/`. Keep
        // checking both old and new locations during the migration so
        // an old `~/.quirk/libs/` still works while users upgrade.
        p.push_back("./libs/");
        if (const char* home2 = std::getenv("HOME")) {
            p.push_back(std::string(home2) + "/.quirk/libs/");
        }

        if (!envHome) {
            p.push_back("/usr/local/lib/quirk/packages/");
            p.push_back("/usr/local/lib/quirk/");
        }
        return p;
    }();
    return paths;
}

std::string resolveImportPath(const std::string& moduleName, const std::string& relativeTo = "") {

    // `effectiveName` lets the relative-import miss fall through into the
    // absolute search below (`moduleName` itself is a const reference).
    std::string effectiveName = moduleName;

    // Relative imports (start with '.')
    if (!effectiveName.empty() && effectiveName[0] == '.') {
        if (relativeTo.empty()) {
            std::cerr << "Error: Relative import '" << effectiveName << "' used without context." << std::endl;
            exit(1);
        }

        size_t dotCount = 0;
        while (dotCount < effectiveName.size() && effectiveName[dotCount] == '.')
            dotCount++;

        fs::path baseDir = fs::path(relativeTo).parent_path();
        for (size_t i = 1; i < dotCount; i++)
            baseDir = baseDir.parent_path();

        std::string subPath = effectiveName.substr(dotCount);

        fs::path candidateFile = baseDir / (subPath + ".quirk");
        if (fs::exists(candidateFile)) return candidateFile.string();

        fs::path candidateInit = baseDir / subPath / "index.quirk";
        if (fs::exists(candidateInit)) return candidateInit.string();

        // Fall through to the absolute search below: relative walks that
        // miss (e.g. `from ...sys` inside an installed typing package
        // where `sys` is a sibling in the bundled stdlib but absent in
        // a project-local install) shouldn't hard-fail. Strip the leading
        // dots and let the absolute resolver have a go with each search
        // path root. Without this, splitting the stdlib into independent
        // repos broke every cross-package relative import that used to
        // rely on the flat bundled layout.
        effectiveName = subPath;
    }

    // Self-resolution: if the importing file lives inside a project whose
    // quirk.toml declares `name = <moduleName>`, resolve to that project's
    // own entry point. Wins over the standard search paths so that local
    // edits aren't shadowed by an installed copy of the same package.
    if (!relativeTo.empty()) {
        std::string selfPath = qpm::resolve_self_package(effectiveName, relativeTo);
        if (!selfPath.empty()) return selfPath;
    }

    // Absolute imports
    std::string relPath = effectiveName;
    std::replace(relPath.begin(), relPath.end(), '.', '/');

    std::vector<std::string> variants = {
        relPath + ".quirk",
        relPath + "/index.quirk",
        relPath + "/src/index.quirk",                       // package layout: pkg/src/index.quirk
        relPath + "/src/" + relPath + ".quirk",
    };

    // Sub-module access through a `src/`-layout package: when the import is
    // `typing.primitives.string`, the installed copy lives at
    // `<root>/typing/src/primitives/string.quirk` — note the `src/` is
    // *between* the package name and the submodule path, not at either
    // end. Without this, `quirk pkg install typing` would install the
    // package correctly but every `from typing.primitives.string` would
    // miss the project-local copy and fall through to the bundled stdlib.
    //
    // Try every possible split point — for a 3-segment path
    // (a/b/c) that's `a/src/b/c.quirk` and `a/b/src/c.quirk`. The
    // generated list is short (≤ N-1 entries for N segments) and we
    // only build it once per resolve call.
    {
        std::vector<std::string> parts;
        std::string cur;
        for (char c : relPath) {
            if (c == '/') { if (!cur.empty()) parts.push_back(cur); cur.clear(); }
            else cur += c;
        }
        if (!cur.empty()) parts.push_back(cur);
        for (size_t k = 1; k < parts.size(); k++) {
            std::string prefix, suffix;
            for (size_t i = 0; i < k; i++) {
                if (i) prefix += '/';
                prefix += parts[i];
            }
            for (size_t i = k; i < parts.size(); i++) {
                if (i > k) suffix += '/';
                suffix += parts[i];
            }
            variants.push_back(prefix + "/src/" + suffix + ".quirk");
            variants.push_back(prefix + "/src/" + suffix + "/index.quirk");
        }
    }

    for (const auto& root : getSearchPaths()) {
        for (const auto& variant : variants) {
            fs::path fullPath = fs::path(root) / variant;
            if (fs::exists(fullPath)) return fullPath.string();
        }
        // Fall back to the package's manifest `entry` field if present.
        // Lets a package override the default entry-point conventions.
        fs::path pkgDir = fs::path(root) / relPath;
        if (fs::exists(pkgDir / "quirk.toml")) {
            qpm::Manifest m;
            if (qpm::read_manifest((pkgDir / "quirk.toml").string(), m)
                && !m.entry.empty()) {
                fs::path entryPath = pkgDir / m.entry;
                if (fs::exists(entryPath)) return entryPath.string();
            }
        }
    }

    return "";
}

// ==========================================================
//  MODULE LOADING
// ==========================================================

std::set<std::string> loadedModules;

std::string getModuleName(const std::string& path) {
    // Fall back to the manifest's `name` ONLY if the path has no standard
    // layout marker. Otherwise the path-based stripping below handles it.
    // This prevents stdlib files reached through `<venv>/lib/quirk/` from
    // picking up the venv's own quirk.toml as their module name.
    bool hasLayoutMarker =
        path.find("/libs/") != std::string::npos ||
        path.find("/lib/quirk/") != std::string::npos ||
        path.find("/packages/") != std::string::npos;
    if (!hasLayoutMarker) {
        std::string pkg = qpm::project_name_for_file(path);
        if (!pkg.empty()) return pkg;
    }

    std::string mod = path;

    if (mod.find("./") == 0) mod = mod.substr(2);

    if (mod.find("packages/") == 0) mod = mod.substr(9);
    else if (mod.find("libs/") == 0) mod = mod.substr(5);    // legacy pre-1.0.8 layout
    else if (mod.find("src/") == 0) mod = mod.substr(4);

    const char* envHome = std::getenv("QUIRK_HOME");
    if (envHome) {
        std::string base       = std::string(envHome);
        std::string venvPkg    = base + "/lib/quirk/packages/";
        std::string venvStdlib = base + "/lib/quirk/stdlib/";   // new venv layout
        std::string venvCore   = base + "/lib/quirk/";          // legacy + dev install
        std::string venvLibs   = base + "/packages/";           // 1.0.8+ dev layout

        size_t pos;
        if ((pos = mod.find(venvPkg)) != std::string::npos)
            mod = mod.substr(pos + venvPkg.length());
        else if ((pos = mod.find(venvStdlib)) != std::string::npos)
            mod = mod.substr(pos + venvStdlib.length());
        else if ((pos = mod.find(venvCore)) != std::string::npos)
            mod = mod.substr(pos + venvCore.length());
        else if ((pos = mod.find(venvLibs)) != std::string::npos)
            mod = mod.substr(pos + venvLibs.length());
    }

    size_t lastDot = mod.find_last_of('.');
    if (lastDot != std::string::npos) mod = mod.substr(0, lastDot);
    std::replace(mod.begin(), mod.end(), '/', '.');
    std::replace(mod.begin(), mod.end(), '\\', '.');

    return mod;
}

std::vector<std::unique_ptr<Node>> processFile(const std::string& filePath,
                                               const Logger& log,
                                               std::map<std::string, std::string>& sourceMap,
                                               bool isMainFile = false) {
    log.debug("Processing file: " + filePath);

    std::string absPath = fs::absolute(filePath).string();
    if (loadedModules.count(absPath))
        return {};
    loadedModules.insert(absPath);

    std::ifstream file(filePath);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open module '" << filePath << "'" << std::endl;
        exit(1);
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string source = buffer.str();
    sourceMap[absPath] = source;

    Lexer lexer(source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens, source, filePath);
    auto nodes = parser.parse();
    if (parser.hasErrors()) {
        parser.flushErrors();
        if (!g_diagnostics_json) std::cerr << "Compilation failed." << std::endl;
        exit(1);
    }

    std::string currentModule = isMainFile ? "main" : getModuleName(filePath);
    std::vector<std::unique_ptr<Node>> allNodes;

    for (auto& node : nodes) {
        node->moduleName = currentModule;
        node->filePath   = absPath;

        if (auto use = dynamic_cast<UseNode*>(node.get())) {
            std::string importPath = resolveImportPath(use->moduleName, filePath);

            if (importPath.empty()) {
                std::cerr << "Error: Could not resolve module '"
                          << use->moduleName << "' imported from " << filePath << std::endl;
                std::cerr << "Checked paths (ensure QUIRK_HOME is set if using venv):" << std::endl;
                for (auto& p : getSearchPaths())
                    std::cerr << "  - " << p << std::endl;
                exit(1);
            }

            auto importedNodes = processFile(importPath, log, sourceMap, false);
            for (auto& importedNode : importedNodes)
                allNodes.push_back(std::move(importedNode));
            allNodes.push_back(std::move(node));
        } else {
            allNodes.push_back(std::move(node));
        }
    }
    return allNodes;
}

// ==========================================================
//  USAGE
// ==========================================================

void printUsage() {
    std::cout <<
        "Usage:  quirk <command> [options] [args...]\n"
        "\n"
        "RUN CODE\n"
        "  run <file>                Compile and run a .quirk script\n"
        "  eval \"<code>\"             Run a one-liner (alias: -c)\n"
        "  module <name>             Run an installed module's main() (alias: -m)\n"
        "  test [<file>...]          Run *_test.quirk files\n"
        "  repl                      Interactive shell\n"
        "\n"
        "PROJECT\n"
        "  new <name>                Scaffold a new package directory\n"
        "  init                      Add quirk.toml in the current dir\n"
        "  venv <path>               Create / repair / inspect an isolated env\n"
        "  env                       Show the active resolution context\n"
        "  fmt [<files>...]          Reformat .quirk source to canonical style\n"
        "\n"
        "PACKAGES\n"
        "  install [<spec>...]       Install dependencies (alias: i, add)\n"
        "  upgrade [<pkg>...]        Bump to latest versions\n"
        "  remove <pkg> [<pkg>...]   Uninstall a package (alias: rm)\n"
        "  list                      Show installed packages (alias: ls)\n"
        "  show <pkg>                Detailed package info\n"
        "  deps                      Print deps in installable form\n"
        "  pkg <subcommand>          Same verbs, scoped — see `quirk pkg help`\n"
        "\n"
        "PUBLISHING\n"
        "  auth login                Authenticate via GitHub device flow\n"
        "  release [--bump X]        Validate + tag + push the current package\n"
        "  audit                     Scan installed packages against advisories\n"
        "\n"
        "COMPILER (self-management)\n"
        "  compiler version          Print the running compiler version\n"
        "  compiler check            Check GitHub for a newer release\n"
        "  compiler update           Replace this compiler with the latest\n"
        "  compiler install <ver>    Install a specific version\n"
        "  compiler list             List available releases\n"
        "  compiler bump <part>      Bump QUIRK_VERSION (compiler maintainers)\n"
        "  compiler stdlib           Show where the bundled stdlib lives\n"
        "\n"
        "MISC\n"
        "  help [<command>]          Per-command help\n"
        "  cache <subcommand>        Manage the cross-project version cache\n"
        "  registry <subcommand>     Manage name → URL mappings\n"
        "  completion <shell>        Emit shell tab-completion script\n"
        "  version, --version        Print the running compiler version\n"
        "\n"
        "RUN FLAGS  (apply to bare `quirk <file>` and `quirk run`)\n"
        "  --check                   Type-check only (no codegen, no run)\n"
        "  --compile-only            Compile + emit IR, don't run\n"
        "  -o <file>                 Compile to a native binary at <file>\n"
        "  -O0|-O1|-O2|-O3           LLVM optimization level (default -O1)\n"
        "  --release                 Alias for -O2\n"
        "  --debug                   Run under the interactive (qdb) line stepper\n"
        "  --emit-ir                 Write LLVM IR to <basename>.ll\n"
        "  --emit-ast                Write AST dump to <basename>.ast.log\n"
        "  --no-cache                Skip the ~/.quirk/cache/ bitcode cache\n"
        "  -v                        Verbose compiler output\n"
        "\n"
        "ENVIRONMENT\n"
        "  QUIRK_HOME                Location of the stdlib / venv (auto-detected)\n"
        "  QUIRK_NO_UPDATE_CHECK=1   Suppress the once-per-day update notice\n"
        "  NO_COLOR=1                Disable ANSI colors in output\n"
        "\n"
        "More detail: `quirk help <command>` or https://github.com/AlexVachon/quirk\n"
        << std::endl;
}

// ==========================================================
//  BITCODE CACHE  (~/.quirk/cache/<sha256>.bc)
// ==========================================================
//
// On a cache hit we skip parse + Sema + Codegen + LLVM passes entirely;
// the JIT just consumes the cached bitcode and runs. On a cache miss
// we run the full pipeline and save the post-pass IR for next time.
//
// Cache key derivation hashes input content + compiler version + opt
// level + debug flag. Transitive imports are NOT yet part of the key —
// changing a `use`d file won't invalidate the cache. Until that lands,
// users with mutable imports should disable the cache (`--no-cache`)
// or clear `~/.quirk/cache`. Documented as a known limitation in 1.0.10.

// Mini-scanner that mimics the parser's import resolution. Walks the
// transitive `use` graph from `entry`, folding each file's bytes into
// the running hasher. We deliberately use a line-based scan (not the
// full lexer) — it's tiny, has no AST/Sema cost, and the worst-case
// failure mode is benign: any divergence from the real parser just
// downgrades the run to a cache miss, never produces a wrong result.
//
// Notable simplifications vs the parser:
//   - `---` block-comment tracking is line-anchored. A `use` directive
//     hidden mid-line inside a block comment would be picked up. That
//     would just cause a spurious dependency in the hash (safe).
//   - String literals containing `use ...` would also be picked up.
//     Same safety story.
static void hashImportGraph(const std::string& entry,
                            llvm::SHA256& hasher,
                            std::set<std::string>& visited) {
    std::string abs;
    try { abs = fs::absolute(entry).string(); }
    catch (...) { return; }
    if (visited.count(abs)) return;
    visited.insert(abs);

    std::ifstream f(entry, std::ios::binary);
    if (!f) return;
    std::stringstream ss; ss << f.rdbuf();
    std::string content = ss.str();

    // Mix the file content + a delimiter that pins the absolute path so
    // two files with identical bytes at different paths produce
    // different hashes (mostly belt-and-suspenders — collisions here
    // would already require an attacker-shaped scenario).
    hasher.update(llvm::ArrayRef<uint8_t>(
        reinterpret_cast<const uint8_t*>(content.data()), content.size()));
    std::string delim = "\n--quirk-file:" + abs + "--\n";
    hasher.update(llvm::ArrayRef<uint8_t>(
        reinterpret_cast<const uint8_t*>(delim.data()), delim.size()));

    std::istringstream lines(content);
    std::string line;
    bool inBlock = false;
    while (std::getline(lines, line)) {
        // Find first non-whitespace; bail early on blanks.
        size_t s = line.find_first_not_of(" \t");
        if (s == std::string::npos) continue;
        std::string t = line.substr(s);

        if (inBlock) {
            if (t.find("---") != std::string::npos) inBlock = false;
            continue;
        }
        if (t.rfind("---", 0) == 0) {
            // open `---` at start of line; close is anywhere in the rest
            inBlock = (t.length() == 3) || (t.find("---", 3) == std::string::npos);
            continue;
        }
        if (t.rfind("//", 0) == 0) continue;

        // Extract module name from `use NAME` or `from NAME use { ... }`.
        // `use .foo`, `use .foo.bar`, `use foo`, `from foo use {...}` all work.
        size_t pos;
        if (t.rfind("use ", 0) == 0)        pos = 4;
        else if (t.rfind("from ", 0) == 0)  pos = 5;
        else continue;
        while (pos < t.size() && (t[pos] == ' ' || t[pos] == '\t')) pos++;

        std::string moduleName;
        while (pos < t.size()) {
            char c = t[pos];
            if (isalnum((unsigned char)c) || c == '_' || c == '.') {
                moduleName += c;
                pos++;
            } else break;
        }
        if (moduleName.empty()) continue;

        std::string resolved = resolveImportPath(moduleName, entry);
        if (!resolved.empty()) hashImportGraph(resolved, hasher, visited);
    }
}

// Cache key = sha256 of the import-closure (entry + everything `use`d
// transitively) + compiler version + opt level. Returns hex.
//
// On a stale `use`d file the new key won't match the old cache file,
// so we get a clean miss. Recompile + new cache entry. The old entry
// just lingers until cache cleanup (TODO: LRU eviction).
static std::string computeCacheKey(const std::string& inputPath, int optLevel) {
    if (!fs::exists(inputPath)) return "";

    llvm::SHA256 hasher;
    std::set<std::string> visited;

    // Seed `typing` — it's auto-imported by every program (see step 1
    // of main()), so its bytes need to influence the key even though
    // the entry file doesn't write `use typing` explicitly.
    std::string typingPath = resolveImportPath("typing", "");
    if (!typingPath.empty()) hashImportGraph(typingPath, hasher, visited);

    hashImportGraph(inputPath, hasher, visited);

    std::string suffix = std::string("\n--quirk-") + qpm::QUIRK_VERSION
                       + "-O" + std::to_string(optLevel) + "--";
    hasher.update(llvm::ArrayRef<uint8_t>(
        reinterpret_cast<const uint8_t*>(suffix.data()), suffix.size()));

    auto digest = hasher.final();
    static const char hexDigits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(digest.size() * 2);
    for (uint8_t b : digest) {
        hex += hexDigits[(b >> 4) & 0xF];
        hex += hexDigits[b & 0xF];
    }
    return hex;
}

static fs::path cachePathFor(const std::string& key) {
    const char* home = std::getenv("HOME");
    fs::path root = home ? fs::path(home) / ".quirk" / "cache"
                         : fs::temp_directory_path() / "quirk-cache";
    return root / (key + ".bc");
}

static std::unique_ptr<llvm::Module>
loadCachedBitcode(const fs::path& path, llvm::LLVMContext& ctx) {
    auto bufOrErr = llvm::MemoryBuffer::getFile(path.string());
    if (!bufOrErr) return nullptr;
    auto modOrErr = llvm::parseBitcodeFile((*bufOrErr)->getMemBufferRef(), ctx);
    if (!modOrErr) return nullptr;
    return std::move(*modOrErr);
}

static bool saveCachedBitcode(const fs::path& path, llvm::Module& module) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;
    llvm::raw_fd_ostream out(path.string(), ec, llvm::sys::fs::OF_None);
    if (ec) return false;
    llvm::WriteBitcodeToFile(module, out);
    out.flush();
    return !out.has_error();
}

// Resolve runtime.so the same way the JIT path does (binary-relative
// first, then $QUIRK_HOME/bin, then CWD fallback). Pulled into its own
// helper so the cache-hit fast path can call it without duplicating
// the search logic.
static std::string resolveRuntimeSoPath() {
    std::string runtimePath;
    std::string exe = qpm::self_binary();
    if (!exe.empty() && exe != "quirk") {
        fs::path next = fs::path(exe).parent_path() / "runtime.so";
        if (fs::exists(next)) runtimePath = next.string();
    }
    if (const char* envHome = std::getenv("QUIRK_HOME")) {
        std::string venvRuntime = std::string(envHome) + "/bin/runtime.so";
        if (fs::exists(venvRuntime)) runtimePath = venvRuntime;
    }
    if (runtimePath.empty()) runtimePath = "./bin/runtime.so";
    return runtimePath;
}

// JIT-compile and run a Module. Used by both the normal pipeline (after
// Codegen) and the cache-hit fast path (after loading bitcode). Returns
// the script's exit code, or a non-zero failure code from the JIT setup.
static int runJITModule(std::unique_ptr<llvm::Module> module,
                        std::unique_ptr<llvm::LLVMContext> ctx,
                        const std::string& runtimePath,
                        const std::string& inputFile,
                        const std::vector<std::string>& scriptArgs) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    for (const char* gcLib : {"libgc.so.1", "libgc.so"}) {
        std::string errMsg;
        if (!llvm::sys::DynamicLibrary::LoadLibraryPermanently(gcLib, &errMsg)) break;
    }

    {
        std::string verifyErrors;
        llvm::raw_string_ostream verifyStream(verifyErrors);
        if (llvm::verifyModule(*module, &verifyStream)) {
            std::cerr << "Internal compiler error: malformed IR\n"
                      << verifyStream.str() << std::endl;
            return 1;
        }
    }

    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    if (!jitOrErr) {
        std::cerr << "Error: failed to create JIT: "
                  << llvm::toString(jitOrErr.takeError()) << std::endl;
        return 1;
    }
    auto& JIT = *jitOrErr;
    auto& JD  = JIT->getMainJITDylib();

    auto genOrErr = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
        JIT->getDataLayout().getGlobalPrefix());
    if (!genOrErr) {
        std::cerr << "Error: process symbol generator failed: "
                  << llvm::toString(genOrErr.takeError()) << std::endl;
        return 1;
    }
    JD.addGenerator(std::move(*genOrErr));

    auto rtGenOrErr = llvm::orc::DynamicLibrarySearchGenerator::Load(
        runtimePath.c_str(), JIT->getDataLayout().getGlobalPrefix());
    if (!rtGenOrErr) {
        std::cerr << "Error: could not load runtime.so (" << runtimePath << "): "
                  << llvm::toString(rtGenOrErr.takeError()) << std::endl;
        return 1;
    }
    JD.addGenerator(std::move(*rtGenOrErr));

    if (auto err = JIT->addIRModule(
            llvm::orc::ThreadSafeModule(std::move(module), std::move(ctx)))) {
        std::cerr << "Error: JIT addIRModule failed: "
                  << llvm::toString(std::move(err)) << std::endl;
        return 1;
    }

    auto mainSym = JIT->lookup("main");
    if (!mainSym) {
        std::cerr << "Error: JIT could not find 'main': "
                  << llvm::toString(mainSym.takeError()) << std::endl;
        return 1;
    }
    auto mainFn = (int(*)(int, char**))mainSym->getAddress();

    std::vector<char*> scriptArgv;
    scriptArgv.push_back(const_cast<char*>(inputFile.c_str()));
    for (auto& a : scriptArgs) scriptArgv.push_back(const_cast<char*>(a.c_str()));
    int ret = mainFn((int)scriptArgv.size(), scriptArgv.data());
    if (ret != 0) {
        std::cerr << "Error: Program exited with code " << ret << std::endl;
        return ret;
    }
    return 0;
}

// ==========================================================
//  MAIN
// ==========================================================

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage();
        return 1;
    }

    // Package-manager subcommand dispatch: `quirk install / upgrade / remove
    // / list / show / init / version`, plus `--version` and `-p|--packages`
    // shortcuts. Returns true when it handled the invocation; otherwise we
    // fall through to the normal script-run path below.
    int pmRc = 0;
    if (qpm::dispatch(argc, argv, pmRc)) {
        // Fire the once-per-24h update notice on the subcommand path —
        // those are the invocations the user is paying attention to.
        // Skipped automatically on pipes/CI, dev builds, and when
        // QUIRK_NO_UPDATE_CHECK is set.
        qpm::maybe_announce_update();
        return pmRc;
    }

    // Parse CLI flags. Anything after the input file (the first non-`-` arg)
    // is forwarded to the user script via sys.argv() — same convention as
    // python/node: `quirk [compiler-flags] script.quirk [script-args...]`.
    CompilerOptions opts;
    std::vector<std::string> scriptArgs;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (!opts.inputFile.empty()) {
            // Already seen script — everything else is the script's own argv.
            scriptArgs.push_back(arg);
            continue;
        }
        if      (arg == "-h" || arg == "--help") { printUsage(); return 0; }
        else if (arg == "--compile-only") opts.runImmediate = false;
        else if (arg == "--check")        { opts.checkOnly = true; opts.runImmediate = false; }
        else if (arg == "-v")             opts.verbose      = true;
        else if (arg == "--emit-ir")      opts.emitIR       = true;
        else if (arg == "--emit-ast")     opts.emitAST      = true;
        else if (arg == "--release")      opts.optLevel     = 2;
        else if (arg == "--debug") {
            opts.debugMode = true;
            opts.optLevel  = 0;  // keep statement boundaries intact for the stepper
        }
        else if (arg == "-O0")            opts.optLevel     = 0;
        else if (arg == "-O1")            opts.optLevel     = 1;
        else if (arg == "-O2")            opts.optLevel     = 2;
        else if (arg == "-O3")            opts.optLevel     = 3;
        else if (arg == "--no-cache")     opts.useCache     = false;
        else if (arg == "--diagnostics-json") {
            // Structured error mode for tooling (LSP). Switches every
            // error printer to NDJSON-on-stdout; the bitcode cache is
            // disabled so each invocation emits fresh diagnostics
            // instead of silently short-circuiting on a stale hit.
            g_diagnostics_json = true;
            opts.useCache = false;
        }
        else if (arg == "-o") {
            if (++i >= argc) { std::cerr << "Error: -o requires a filename\n"; return 1; }
            opts.outputFile    = argv[i];
            opts.runImmediate  = false;
        }
        else if (arg[0] != '-')           opts.inputFile    = arg;
        else {
            std::cerr << "Unknown option: " << arg << std::endl;
            printUsage();
            return 1;
        }
    }

    if (opts.inputFile.empty()) {
        std::cerr << "Error: No input file specified." << std::endl;
        printUsage();
        return 1;
    }

    // Derive base name for output files (e.g. "tests/strings.quirk" -> "strings")
    fs::path inputPath(opts.inputFile);
    std::string baseName = inputPath.stem().string();    // "strings"
    std::string baseDir  = inputPath.parent_path().string();
    if (baseDir.empty()) baseDir = ".";

    Logger log(opts.verbose);

    // =======================================================
    // 0. BITCODE CACHE FAST PATH
    // =======================================================
    // Only attempted for the JIT path (`quirk file.quirk` with no `-o`,
    // no `--check`, no `--debug`). On hit we skip ALL Quirk frontend
    // work and hand the cached IR straight to the JIT.
    if (opts.useCache && opts.runImmediate && !opts.debugMode
        && !opts.checkOnly && !opts.emitIR && !opts.emitAST
        && opts.outputFile.empty()) {
        std::string key = computeCacheKey(opts.inputFile, opts.optLevel);
        if (!key.empty()) {
            fs::path cp = cachePathFor(key);
            if (fs::exists(cp)) {
                log.debug("Cache HIT: " + cp.string());
                auto ctx = std::make_unique<llvm::LLVMContext>();
                if (auto cached = loadCachedBitcode(cp, *ctx)) {
                    std::string runtimePath = resolveRuntimeSoPath();
                    return runJITModule(std::move(cached), std::move(ctx),
                                        runtimePath, opts.inputFile, scriptArgs);
                }
                log.warn("Failed to load cached bitcode, falling through to full compile");
            } else {
                log.debug("Cache MISS: " + cp.string());
            }
        }
    }

    // =======================================================
    // 1. LOAD STANDARD LIBRARY (PRELUDE)
    // =======================================================
    log.debug("Loading Standard Library (Prelude)...");

    std::vector<std::unique_ptr<Node>> ast;
    std::map<std::string, std::string> sourceMap;

    std::string corePath = resolveImportPath("typing", "");
    if (!corePath.empty()) {
        auto coreNodes = processFile(corePath, log, sourceMap);
        log.debug("Loaded " + std::to_string(coreNodes.size()) + " nodes from typing.");
        for (auto& node : coreNodes)
            ast.push_back(std::move(node));
    } else {
        log.warn("'typing' library not found! Standard types (String, List) will fail.");
        const char* env = std::getenv("QUIRK_HOME");
        log.warn(std::string("QUIRK_HOME: ") + (env ? env : "(unset)"));
        for (const auto& p : getSearchPaths())
            log.warn("  Search path: " + p);
    }

    // =======================================================
    // 2. LOAD USER FILE
    // =======================================================
    log.debug("Loading user file: " + opts.inputFile);

    auto userNodes = processFile(opts.inputFile, log, sourceMap, true);
    for (auto& node : userNodes)
        ast.push_back(std::move(node));

    // =======================================================
    // 3. AST DUMP (opt-in only)
    // =======================================================
    if (opts.emitAST) {
        std::string astPath = baseDir + "/" + baseName + ".ast.log";
        std::ofstream astLog(astPath);
        if (astLog.is_open()) {
            ASTPrinter printer(astLog);
            printer.printAll(ast);
            astLog.close();
            log.debug("AST written to " + astPath);
        } else {
            log.warn("Could not write AST log to " + astPath);
        }
    }

    // =======================================================
    // 4. SEMANTIC ANALYSIS
    // =======================================================
    log.debug("Running Semantic Analysis...");

    Sema sema;
    sema.setSourceMap(sourceMap);
    sema.setUserFile(fs::absolute(opts.inputFile).string());
    if (!sema.analyze(ast)) {
        if (!g_diagnostics_json) std::cerr << "Compilation failed." << std::endl;
        return 1;
    }

    if (opts.checkOnly) {
        // In JSON mode the LSP infers "no errors" from an empty stdout —
        // skip the human-readable confirmation. Exit code 0 still means
        // success; the LSP looks at that, not at any banner.
        if (!g_diagnostics_json) std::cout << opts.inputFile << ": OK\n";
        return 0;
    }

    // =======================================================
    // 5. CODE GENERATION
    // =======================================================
    log.debug("Starting Code Generation...");

    // Resolve runtime.so path via the shared helper — keeps the JIT path
    // and the cache-hit fast path in sync, and routes the binary-path
    // lookup through `qpm::self_binary()` which has the macOS shim.
    std::string runtimePath = resolveRuntimeSoPath();

    // =======================================================
    // 6a. EMIT IR (--emit-ir or --compile-only)
    // =======================================================
    if (opts.emitIR || !opts.runImmediate) {
        std::string irPath = opts.emitIR
            ? (baseDir + "/" + baseName + ".ll")
            : ("/tmp/quirk_" + baseName + ".ll");

        std::error_code EC;
        raw_fd_ostream dest(irPath, EC, sys::fs::OF_None);
        if (EC) {
            std::cerr << "Error: Could not open IR output file '" << irPath
                      << "': " << EC.message() << std::endl;
            return 1;
        }
        LLVMCodegen codegen;
        codegen.setVerbose(opts.verbose);
        codegen.setOptLevel(opts.optLevel);
        codegen.setDebugMode(opts.debugMode);
        codegen.setSourceMap(sourceMap);
        codegen.compile(ast, dest);
        dest.flush();

        if (opts.emitIR) {
            log.debug("LLVM IR written to " + irPath);
            return 0;
        }
    }

    // =======================================================
    // 6b. NATIVE BINARY OUTPUT (-o <file>)
    // =======================================================
    if (!opts.outputFile.empty()) {
        // Emit optimised IR to a temp file, then compile + link via llc-14 + gcc.
        // This is the same pipeline used by --emit-ir; it's reliable and avoids
        // having to manage TargetMachine lifetime alongside the running compiler.
        std::string irPath  = "/tmp/quirk_" + baseName + ".ll";
        std::string objPath = "/tmp/quirk_" + baseName + ".o";

        {
            std::error_code EC;
            llvm::raw_fd_ostream irDest(irPath, EC, llvm::sys::fs::OF_None);
            if (EC) {
                std::cerr << "Error: cannot open temp IR file '" << irPath
                          << "': " << EC.message() << std::endl;
                return 1;
            }
            LLVMCodegen codegen;
            codegen.setVerbose(opts.verbose);
            codegen.setOptLevel(opts.optLevel);
            codegen.setDebugMode(opts.debugMode);
            codegen.setSourceMap(sourceMap);
            codegen.compile(ast, irDest);
        }

        // Compile IR → object file
        std::string llcCmd = "llc-14 -filetype=obj -relocation-model=pic -O2 "
                           + irPath + " -o " + objPath;
        log.debug("Compiling: " + llcCmd);
        if (int r = std::system(llcCmd.c_str())) {
            std::cerr << "Error: llc failed (exit " << r << ")" << std::endl;
            return 1;
        }

        // Link: object + runtime.so → output binary
        std::string runtimeDir = fs::path(runtimePath).parent_path().string();
        std::string linkCmd = "gcc " + objPath + " " + runtimePath
                            + " -lgc -lm -o " + opts.outputFile
                            + " -Wl,-rpath," + runtimeDir;
        log.debug("Linking: " + linkCmd);
        if (int r = std::system(linkCmd.c_str())) {
            std::cerr << "Error: linker failed (exit " << r << ")" << std::endl;
            return 1;
        }

        log.debug("Binary written to " + opts.outputFile);
        return 0;
    }

    // =======================================================
    // 6c. JIT COMPILE + RUN (runImmediate, no subprocess)
    // =======================================================
    if (opts.runImmediate && !opts.emitIR) {
        // Build IR + run passes, get ownership of the module.
        LLVMCodegen codegen;
        codegen.setVerbose(opts.verbose);
        codegen.setOptLevel(opts.optLevel);
        codegen.setDebugMode(opts.debugMode);
        codegen.setSourceMap(sourceMap);
        auto [module, ctx] = codegen.compileAndRelease(ast);

        // Save bitcode for next run BEFORE the JIT consumes the module —
        // the JIT takes ownership of `module` and we can't serialize a
        // moved-from unique_ptr. The fast-path early exit at the top of
        // main checks for this file on subsequent invocations.
        if (opts.useCache && !opts.debugMode) {
            std::string key = computeCacheKey(opts.inputFile, opts.optLevel);
            if (!key.empty()) {
                fs::path cp = cachePathFor(key);
                if (saveCachedBitcode(cp, *module)) {
                    log.debug("Cache SAVE: " + cp.string());
                } else {
                    log.debug("Cache SAVE failed: " + cp.string());
                }
            }
        }

        return runJITModule(std::move(module), std::move(ctx),
                            runtimePath, opts.inputFile, scriptArgs);
    }

    log.debug("Done.");
    return 0;
}