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
#include <cstring>
#include <cctype>
#include <ctime>
#include <functional>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <vector>
#include "platform.hpp"

#include "PackageManager.hpp"
#include "Watch.hpp"

#include "lexer.hpp"
#include "typenames.hpp"
#include "parser.hpp"
#include "sema.hpp"

#include "Backend/Codegen.cpp"

#ifdef _WIN32
// ---- `-o` on Windows: IR -> COFF object in-process, then lld-link / link.exe.
// The C library names generated code calls are renamed to the runtime's
// quirk_crt_* wrappers, the same binding the JIT makes (see win_compat.h):
// the UCRT exports no `printf`, and one CRT means one stdout buffer.
static bool windowsEmitObject(const std::string& irPath, const std::string& objPath, std::string& err) {
    llvm::LLVMContext ctx;
    llvm::SMDiagnostic diag;
    auto mod = llvm::parseIRFile(irPath, diag, ctx);
    if (!mod) { err = "can't read the generated IR: " + diag.getMessage().str(); return false; }
    for (const char* n : {"printf", "sprintf", "fopen", "fclose", "fwrite", "fread", "fseek",
                          "ftell", "strlen", "malloc", "free", "exit", "write", "GC_malloc"}) {
        if (llvm::Function* f = mod->getFunction(n))
            if (f->isDeclaration()) f->setName(std::string("quirk_crt_") + n);
    }
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    std::string triple = llvm::sys::getDefaultTargetTriple();
    std::string lookupErr;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, lookupErr);
    if (!target) { err = "no LLVM target for " + triple + ": " + lookupErr; return false; }
    llvm::TargetOptions to;
    std::unique_ptr<llvm::TargetMachine> tm(target->createTargetMachine(
        triple, llvm::sys::getHostCPUName(), "", to, llvm::Reloc::PIC_, llvm::None,
        llvm::CodeGenOpt::Default));
    mod->setTargetTriple(triple);
    mod->setDataLayout(tm->createDataLayout());
    std::error_code ec;
    llvm::raw_fd_ostream out(objPath, ec, llvm::sys::fs::OF_None);
    if (ec) { err = "can't write " + objPath + ": " + ec.message(); return false; }
    llvm::legacy::PassManager pm;
    if (tm->addPassesToEmitFile(pm, out, nullptr, llvm::CGFT_ObjectFile)) {
        err = "LLVM can't emit an object file for " + triple; return false;
    }
    pm.run(*mod);
    out.flush();
    return true;
}

// Links against runtime.lib (the import library next to runtime.dll) and
// the shared CRT. Needs a linker plus the Windows SDK / MSVC libraries —
// a Visual Studio (Build Tools) developer prompt has both.
static bool windowsLinkExe(const std::string& objPath, const std::string& outPath,
                           const std::string& runtimePath, std::string& err,
                           bool windowed = false, const std::string& extraInputs = "") {
    std::filesystem::path rtLib = std::filesystem::path(runtimePath).replace_extension(".lib");
    if (!std::filesystem::exists(rtLib)) { err = "missing " + rtLib.string() + " (the runtime's import library)"; return false; }
    std::string linker = qplat::have_command("lld-link") ? "lld-link"
                       : qplat::have_command("link.exe") ? "link.exe" : "";
    if (linker.empty()) {
        err = "-o needs a linker: run from a Visual Studio developer prompt (link.exe) "
              "or put LLVM's lld-link on PATH";
        return false;
    }
    std::string out = outPath;
    if (std::filesystem::path(out).extension().empty()) out += ".exe";
    // A window app: the GUI subsystem (no console opens with it), still
    // starting at main() through the CRT.
    std::string subsystem = windowed ? " /subsystem:windows /entry:mainCRTStartup" : " /subsystem:console";
    std::string cmd = linker + " /nologo" + subsystem + " /out:\"" + out + "\" \"" + objPath
                    + "\" \"" + rtLib.string() + "\" msvcrt.lib ucrt.lib vcruntime.lib kernel32.lib"
                      " /nodefaultlib:libcmt.lib";
    if (!extraInputs.empty()) cmd += " " + extraInputs;
    if (int r = qplat::run_shell(cmd)) { err = "linker failed (exit " + std::to_string(r) + "): " + cmd; return false; }
    return true;
}
#endif

namespace fs = std::filesystem;

// ==========================================================
//  COMPILER OPTIONS
// ==========================================================

// Declared in ast.hpp; defined here so it's a single linker-visible
// symbol. Flipped by `--diagnostics-json`; consulted by every error
// printer (Parser::flushErrors, Sema::printSemaError, etc).
bool g_diagnostics_json = false;
// `--stdin-source`: the entry file's text comes from stdin (an editor's
// unsaved buffer) while its path still anchors imports and positions.
static bool g_stdin_source = false;
static std::string g_stdin_source_path, g_stdin_source_text;
// `--test-harness` (set by `quirk test`): an entry file with no `main()`
// gets one that runs each `define test_*()` — see testHarnessSource.
// `--test-k=<substr>` keeps only the tests whose name contains it.
static bool g_test_harness = false;
static std::string g_test_k;

// The `main()` `quirk test` runs for a file of `test_*` functions. Each
// test runs on its own (setup → test → teardown, failures caught and
// printed) between marker lines the runner reads:
//   ##quirk-test:begin <every test name>, then per test:
//   ##quirk-test:start <name>   …the test's output…   ##quirk-test:pass|fail <name>
// The process exits 1 when any test failed.
static std::string testHarnessSource(const std::vector<std::string>& tests,
                                     bool hasSetup, bool hasTeardown) {
    std::string planned;
    for (const auto& t : tests) planned += " " + t;
    std::string g = "\n\n// ---- generated by `quirk test` ----\n"
                    "define main() -> void {\n"
                    "    print(\"##quirk-test:begin" + planned + "\")\n"
                    "    __qt_failed := 0\n"
                    "    __qt_ok := true\n";
    for (const auto& t : tests) {
        g += "    print(\"##quirk-test:start " + t + "\")\n"
             "    __qt_ok = true\n"
             "    try {\n";
        if (hasSetup) g += "        setup()\n";
        g += "        " + t + "()\n"
             "    } catch (__qt_e: Exception) {\n"
             "        __qt_e.print_traceback()\n"
             "        __qt_ok = false\n"
             "    }\n";
        if (hasTeardown)
            g += "    try {\n"
                 "        teardown()\n"
                 "    } catch (__qt_e: Exception) {\n"
                 "        print(\"teardown failed:\")\n"
                 "        __qt_e.print_traceback()\n"
                 "        __qt_ok = false\n"
                 "    }\n";
        g += "    if __qt_ok {\n"
             "        print(\"##quirk-test:pass " + t + "\")\n"
             "    } else {\n"
             "        print(\"##quirk-test:fail " + t + "\")\n"
             "        __qt_failed = __qt_failed + 1\n"
             "    }\n";
    }
    g += "    if __qt_failed > 0 { exit(1) }\n"
         "}\n";
    return g;
}

struct CompilerOptions {
    std::string inputFile;
    std::string outputFile;   // non-empty → produce native binary, skip JIT
    bool runImmediate = true;
    bool verbose      = false;
    bool emitIR       = false;
    bool emitAST      = false;
    bool checkOnly    = false; // --check: lex + parse + sema only, no codegen
    bool watch        = false; // --watch: run it again on every change (Watch.hpp)
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
    // --symbols-json: dump the AST's declaration set as NDJSON to
    // stdout instead of running. One record per top-level definition,
    // method, field, parameter, variable, or enum variant. Designed
    // for the LSP's scope-aware features (completion, future semantic
    // rename). Implies --check (compile through Sema, no codegen / run).
    bool symbolsJson  = false;
    // v2.0: AOT build cache. `--build` (or `quirk build <file>`) runs
    // the full Codegen + llc + gcc pipeline and stashes the resulting
    // native binary in `~/.quirk/cache/build/<sha>-<arch>/quirk-bin`.
    // `quirk <file>` (with no flag) checks the cache before any other
    // work and `execv`s the binary on a hit; misses fall through to
    // the existing JIT path. `--no-aot` disables the implicit lookup
    // for users who prefer JIT-always.
    bool buildOnly    = false;
    bool useAotCache  = true;
    // --no-cache: skip the per-invocation bitcode cache. The cache lives
    // at $HOME/.quirk/cache/<sha256>.bc and stores the post-optimisation
    // LLVM IR for a given (input file, compiler version, opt level)
    // triple. A cache hit skips parse + Sema + Codegen + LLVM passes,
    // which on small scripts is most of the per-run latency.
    bool useCache     = true;
    // `quirk bundle <file> [-o dist] [--docker] [--include path]`: a folder
    // to copy to a server — the binary, the runtime next to it (found
    // through $ORIGIN / @executable_path), and the app's files.
    bool bundle       = false;
    bool bundleDocker = false;
    std::vector<std::string> bundleIncludes;
    // `--name "Quirk IDE"`: the program's name (the .exe's, the .app's, the
    // menu entry's); `--icon logo.png`: its icon (a PNG, 256x256 is best);
    // `--windowed`: a window app — on Windows, no console window with it.
    std::string bundleName;
    std::string bundleIcon;
    bool bundleWindowed = false;
    // `--embed` / `embed = true`: the --include files built into the
    // program (read from there by fs, images, fonts, icons), not copied.
    bool bundleEmbed = false;
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
        // Project-local third-party installs. site-packages/ is the new
        // (2.2.11+) Python-conventional location; packages/ stays
        // accepted as a fallback so older project trees keep resolving
        // until a `quirk pkg install` migrates them on next run.
        p.push_back("./site-packages/");
        p.push_back("./packages/");

        const char* envHome = std::getenv("QUIRK_HOME");
        bool isVenv = false;
        if (envHome) {
            std::string venvBase = envHome;
            // Venv layout, Python-strict: site-packages/ holds third-
            // party installs and *wins* over stdlib/, exactly mirroring
            // Python's resolver order. stdlib/ is symlinks to the global
            // bundled stdlib that track `quirk compiler update`. The
            // legacy packages/ entry stays for backwards-compat with
            // venvs created before 2.2.11 (`venv repair` migrates them).
            p.push_back(venvBase + "/lib/quirk/site-packages/");
            p.push_back(venvBase + "/lib/quirk/packages/");  // legacy
            p.push_back(venvBase + "/lib/quirk/stdlib/");
            p.push_back(venvBase + "/lib/quirk/");           // dev install
            p.push_back(venvBase + "/site-packages/");
            p.push_back(venvBase + "/packages/");            // legacy
            // Distinguish a real venv from a dev-tree QUIRK_HOME: only the former
            // has bin/activate. Venvs *isolate* — they don't see user-global.
            isVenv = fs::exists(fs::path(venvBase) / "bin" / "activate");
        }

        // The install the running binary came from: <root>/bin/quirk next to
        // <root>/packages/ (install.sh / install.ps1 with a custom --dir /
        // -Dir, an unpacked release tarball) — the stdlib made for this
        // compiler. Before the user-global dir: that's ~/.quirk/packages/,
        // the stdlib of a default install — another, older one when this
        // binary lives elsewhere (its `ui` 1.0 hid this one's 1.11). Same
        // directory as ~/.quirk/packages/ for a default install.
        if (!envHome) {
            std::string exe = qpm::self_binary();
            std::error_code ec;
            fs::path rootPkgs = fs::path(exe).parent_path().parent_path() / "packages";
            if (!exe.empty() && exe != "quirk" && fs::is_directory(rootPkgs, ec))
                p.push_back(rootPkgs.string() + "/");
        }

        // User-global installs — `pip install --user` / `cargo install` equivalent.
        // Skipped when a venv is active so the venv stays a closed world.
        if (!isVenv) {
            if (const char* home = qplat::home_env()) {
                p.push_back(std::string(home) + "/.quirk/packages/");
            }
        }

        // Legacy: pre-1.0.8 installs put the stdlib in `libs/`. Keep
        // checking both old and new locations during the migration so
        // an old `~/.quirk/libs/` still works while users upgrade.
        p.push_back("./libs/");
        if (const char* home2 = qplat::home_env()) {
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

// The folder of the file being compiled (`quirk run main.qk`): the app's
// root, where `from src.pages use {...}` looks last.
static std::string g_entry_dir;

// The file being compiled/run: its folder is the app root for local imports.
static std::string g_entry_file;

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

        for (const std::string& stem : {(baseDir / subPath).string(),
                                        (baseDir / subPath / "index").string()})
            for (const std::string& candidate : quirk_src::variants(stem))
                if (fs::exists(candidate)) return candidate;

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

    // Extension-less stems, in priority order; each is tried as `.qk`
    // then `.quirk` below.
    std::vector<std::string> stems = {
        relPath,
        relPath + "/index",
        relPath + "/src/index",                       // package layout: pkg/src/index.qk
        relPath + "/src/" + relPath,
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
            stems.push_back(prefix + "/src/" + suffix);
            stems.push_back(prefix + "/src/" + suffix + "/index");
        }
    }
    std::vector<std::string> variants;
    for (const auto& stem : stems)
        for (const auto& v : quirk_src::variants(stem)) variants.push_back(v);

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

    // Last: the app's own files, without a leading dot — `from src.pages
    // use { Home }` in a project's router.qk. Looked up next to the
    // importing file, then from the project root (the nearest folder with
    // a quirk.toml). Packages and the stdlib win over a local file.
    if (!relativeTo.empty()) {
        // Same path shape as a relative import (`from .src.pages`), so the
        // module gets the same name either way.
        std::vector<fs::path> bases;
        fs::path dir = fs::path(relativeTo).parent_path();
        bases.push_back(dir);
        if (!g_entry_file.empty()) {
            fs::path entryDir = fs::path(g_entry_file).parent_path();
            if (entryDir != dir) bases.push_back(entryDir);
        }
        if (!g_entry_dir.empty() || relativeTo.find('/') != std::string::npos) {
            fs::path entry = g_entry_dir;
            if (entry != dir) bases.push_back(entry);
        }
        // Then each folder above the importing file, nearest first, up to
        // the project root (quirk.toml): `src.components.x` from
        // app/src/pages.qk finds app/src/components/x.qk wherever the
        // program is run from (a test in another folder, say).
        fs::path up = dir;
        for (fs::path d = fs::absolute(relativeTo).parent_path(); ; d = d.parent_path()) {
            if (up != dir) bases.push_back(up);
            if (fs::exists(d / "quirk.toml") || d == d.parent_path()) break;
            up = up / "..";
        }
        for (const auto& base : bases)
            for (const std::string& stem : {relPath, relPath + "/index"})
                for (const std::string& candidate : quirk_src::variants((base / stem).string()))
                    if (fs::exists(candidate)) return candidate;
    }

    return "";
}

// ==========================================================
//  MODULE LOADING
// ==========================================================

std::set<std::string> loadedModules;

std::string getModuleName(const std::string& path) {
    // A package installed into a project (`quirk install`: site-packages/
    // <name>/...): its module is its path inside site-packages. It was the
    // whole path ("site-packages.syntax.index"), so `syntax.dark()` didn't
    // match syntax's module and found another package's `dark()` (ui's).
    {
        std::string p = path;
        std::replace(p.begin(), p.end(), '\\', '/');
        size_t sp = p.rfind("site-packages/");
        if (sp != std::string::npos) {
            std::string mod = p.substr(sp + 14);
            size_t dot = mod.find_last_of('.');
            if (dot != std::string::npos && mod.find('/', dot) == std::string::npos) mod = mod.substr(0, dot);
            std::replace(mod.begin(), mod.end(), '/', '.');
            return mod;
        }
    }
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


// ==========================================================
//  PER-MODULE TYPE NAMES
// ==========================================================
//
// Struct / enum / interface / alias names are one namespace in Sema and
// Codegen, so a user `struct Button` used to replace `ui.Button` (its
// methods, fields and constructor). When a name is defined in more than one
// file, every definition outside the main file gets an internal name,
// `<Name>_in_<module>`, and the references that mean it are rewritten at the
// token level before parsing (so generic templates, re-parsed from tokens,
// see it too):
//   - `alias.Name` where `alias` is a `use` of the defining module (or a
//     module that re-exports it) → one identifier `Name_in_module`;
//   - a bare `Name` in a file that doesn't define it but reaches exactly one
//     definition through its imports (`from m use { Name }`, `use m`).
// The main file's own definition keeps its name. Groups involving `typing`
// (the prelude: String, List, ...) are left alone. typenames.hpp maps the
// internal names back to `module.Name` for messages and repr.
struct FileRewrite {
    std::map<std::string, std::string> bare;                               // Name → internal
    std::map<std::pair<std::string, std::string>, std::string> qualified;  // (alias, Name) → internal
};
static std::map<std::string, FileRewrite> g_typeRewrites;   // abs path → rewrite

static void applyTypeRewrites(const std::string& absPath, std::vector<Token>& tokens) {
    auto it = g_typeRewrites.find(absPath);
    if (it == g_typeRewrites.end()) return;
    const FileRewrite& rw = it->second;
    std::vector<Token> out;
    out.reserve(tokens.size());
    for (size_t i = 0; i < tokens.size(); i++) {
        const Token& t = tokens[i];
        bool afterDot = !out.empty() && (out.back().type == TokenType::DOT ||
                                         out.back().type == TokenType::QUESTION_DOT);
        if (t.type == TokenType::IDENTIFIER && !afterDot) {
            if (i + 2 < tokens.size() && tokens[i + 1].type == TokenType::DOT &&
                tokens[i + 2].type == TokenType::IDENTIFIER) {
                auto q = rw.qualified.find({t.value, tokens[i + 2].value});
                if (q != rw.qualified.end()) {
                    out.push_back({TokenType::IDENTIFIER, q->second, t.line, t.col});
                    i += 2;
                    continue;
                }
            }
            auto b = rw.bare.find(t.value);
            if (b != rw.bare.end()) {
                out.push_back({TokenType::IDENTIFIER, b->second, t.line, t.col});
                continue;
            }
        }
        out.push_back(t);
    }
    tokens.swap(out);
}

std::vector<std::unique_ptr<Node>> processFile(const std::string& filePath,
                                               const Logger& log,
                                               std::map<std::string, std::string>& sourceMap,
                                               bool isMainFile = false) {
    log.debug("Processing file: " + filePath);

    // lexically_normal: `a/b/../index.qk` and `a/index.qk` are one module
    // (loaded twice, its types clashed with themselves).
    std::string absPath = fs::absolute(filePath).lexically_normal().string();
    if (loadedModules.count(absPath))
        return {};
    loadedModules.insert(absPath);

    std::string source;
    if (g_stdin_source && absPath == g_stdin_source_path) {
        source = g_stdin_source_text;
    } else {
        std::ifstream file(filePath);
        if (!file.is_open()) {
            std::cerr << "Error: Could not open module '" << filePath << "'" << std::endl;
            exit(1);
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        source = buffer.str();
    }
    sourceMap[absPath] = source;

    Lexer lexer(source);
    auto tokens = lexer.tokenize();
    applyTypeRewrites(absPath, tokens);
    Parser parser(tokens, source, filePath);
    auto nodes = parser.parse();
    if (parser.hasErrors()) {
        parser.flushErrors();
        if (!g_diagnostics_json) std::cerr << "Compilation failed." << std::endl;
        exit(1);
    }

    // `quirk test`: a test file without main() gets the generated runner,
    // appended after the user's code so their line numbers don't move.
    if (isMainFile && g_test_harness) {
        bool hasMain = false, hasSetup = false, hasTeardown = false;
        std::vector<std::string> tests;
        for (const auto& n : nodes) {
            auto* fn = dynamic_cast<FunctionNode*>(n.get());
            if (!fn || !fn->cls.empty()) continue;
            if (fn->name == "main") hasMain = true;
            else if (fn->name == "setup" && fn->parameters.empty()) hasSetup = true;
            else if (fn->name == "teardown" && fn->parameters.empty()) hasTeardown = true;
            else if (fn->name.rfind("test_", 0) == 0 && fn->parameters.empty() &&
                     (g_test_k.empty() || fn->name.find(g_test_k) != std::string::npos))
                tests.push_back(fn->name);
        }
        if (!hasMain) {
            source += testHarnessSource(tests, hasSetup, hasTeardown);
            sourceMap[absPath] = source;
            Lexer lexer2(source);
            auto tokens2 = lexer2.tokenize();
            applyTypeRewrites(absPath, tokens2);
            Parser parser2(tokens2, source, filePath);
            nodes = parser2.parse();
            if (parser2.hasErrors()) {
                parser2.flushErrors();
                if (!g_diagnostics_json) std::cerr << "Compilation failed." << std::endl;
                exit(1);
            }
        }
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



// `ui` for <root>/packages/ui/index.quirk, `ui.widgets` for .../ui/widgets.qk,
// `models` for a project file next to the main one — the name a type from
// that file is shown with (`ui.Button`).
static std::string moduleDisplayName(const std::string& absFile, const std::string& mainAbs) {
    std::string p = absFile;
    std::replace(p.begin(), p.end(), '\\', '/');
    size_t cut = std::string::npos;
    for (const char* marker : {"/site-packages/", "/packages/", "/stdlib/", "/lib/quirk/", "/libs/"}) {
        size_t at = p.rfind(marker);
        if (at != std::string::npos && (cut == std::string::npos || at + std::strlen(marker) > cut))
            cut = at + std::strlen(marker);
    }
    if (cut != std::string::npos) p = p.substr(cut);
    else {
        std::string base = fs::path(mainAbs).parent_path().string() + "/";
        if (p.rfind(base, 0) == 0) p = p.substr(base.size());
        else p = fs::path(p).filename().string();
    }
    size_t dot = p.find_last_of('.');
    if (dot != std::string::npos && p.find('/', dot) == std::string::npos) p = p.substr(0, dot);
    const std::string idx = "/index";
    if (p.size() > idx.size() && p.compare(p.size() - idx.size(), idx.size(), idx) == 0)
        p = p.substr(0, p.size() - idx.size());
    // A package's src/ layout (<pkg>/src/...) isn't part of its name.
    if (size_t src = p.find("/src/"); src != std::string::npos) p = p.substr(0, src) + p.substr(src + 4);
    else if (p.size() > 4 && p.compare(p.size() - 4, 4, "/src") == 0) p = p.substr(0, p.size() - 4);
    std::replace(p.begin(), p.end(), '/', '.');
    return p;
}

// Fills g_typeRewrites from a first parse of the program (see
// PER-MODULE TYPE NAMES above). Returns true when some file needs one —
// the caller then loads the program again.
static bool planTypeRewrites(const std::vector<std::unique_ptr<Node>>& ast,
                             const std::string& mainAbs, const std::string& typingDir) {
    struct UseInfo { std::string alias, target; std::vector<std::string> filters; };
    std::map<std::string, std::set<std::string>> defs;        // file → names
    std::map<std::string, std::vector<UseInfo>> uses;         // file → imports
    std::map<std::string, std::vector<std::string>> definers; // name → files (load order)
    auto addDef = [&](const std::string& file, const std::string& name) {
        if (name.empty() || file.empty()) return;
        if (defs[file].insert(name).second) definers[name].push_back(file);
    };
    for (const auto& n : ast) {
        const std::string& file = n->filePath;
        if (auto st = dynamic_cast<StructNode*>(n.get())) addDef(file, st->name);
        else if (auto en = dynamic_cast<EnumNode*>(n.get())) addDef(file, en->name);
        else if (auto in = dynamic_cast<InterfaceNode*>(n.get())) addDef(file, in->name);
        else if (auto ta = dynamic_cast<TypeAliasNode*>(n.get())) addDef(file, ta->name);
        else if (auto u = dynamic_cast<UseNode*>(n.get())) {
            std::string target = resolveImportPath(u->moduleName, file);
            if (target.empty()) continue;
            UseInfo info;
            info.target = fs::absolute(target).lexically_normal().string();
            info.filters = u->filterList;
            if (!u->alias.empty()) info.alias = u->alias;
            else if (u->filterList.empty()) {
                info.alias = u->moduleName;
                size_t cut = info.alias.rfind('.');
                if (cut == std::string::npos) cut = info.alias.rfind('/');
                if (cut != std::string::npos) info.alias = info.alias.substr(cut + 1);
            }
            uses[file].push_back(info);
        }
    }

    auto inTyping = [&](const std::string& f) {
        return !typingDir.empty() && fs::path(f).generic_string().rfind(typingDir, 0) == 0;
    };
    // name → (defining file → internal name)
    std::map<std::string, std::map<std::string, std::string>> internal;
    std::set<std::string> taken;
    for (const auto& [name, files] : definers) taken.insert(name);
    for (const auto& [name, files] : definers) {
        if (files.size() < 2) continue;
        bool typing = false;
        for (const auto& f : files) typing = typing || inTyping(f);
        if (typing) continue;
        for (const auto& f : files) {
            if (f == mainAbs) continue;
            std::string mod = moduleDisplayName(f, mainAbs);
            std::string slug;
            for (char c : mod) slug += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
            std::string iname = name + "_in_" + slug;
            for (int k = 2; taken.count(iname); k++) iname = name + "_in_" + slug + std::to_string(k);
            taken.insert(iname);
            internal[name][f] = iname;
            quirk::typeDisplayNames()[iname] = mod + "." + name;
        }
    }
    if (internal.empty()) return false;

    // The file whose definition `name` means inside `file`: its own, the one
    // a `from m use { name }` brings in, or the single one reachable through
    // plain `use`s. "" when none / ambiguous.
    std::map<std::pair<std::string, std::string>, std::string> memo;
    std::function<std::string(const std::string&, const std::string&, std::set<std::string>&)> resolve =
        [&](const std::string& file, const std::string& name, std::set<std::string>& seen) -> std::string {
        if (defs[file].count(name)) return file;
        if (!seen.insert(file).second) return "";
        auto key = std::make_pair(file, name);
        if (auto m = memo.find(key); m != memo.end()) return m->second;
        std::string found;
        for (const auto& u : uses[file])
            if (std::find(u.filters.begin(), u.filters.end(), name) != u.filters.end()) {
                found = resolve(u.target, name, seen);
                if (!found.empty()) break;
            }
        if (found.empty()) {
            std::set<std::string> hits;
            for (const auto& u : uses[file])
                if (u.filters.empty()) {
                    std::string r = resolve(u.target, name, seen);
                    if (!r.empty()) hits.insert(r);
                }
            if (hits.size() == 1) found = *hits.begin();
        }
        memo[key] = found;
        return found;
    };
    auto internalFor = [&](const std::string& name, const std::string& defFile) -> std::string {
        auto g = internal.find(name);
        if (g == internal.end()) return "";
        auto f = g->second.find(defFile);
        return f == g->second.end() ? "" : f->second;
    };

    std::set<std::string> files;
    for (const auto& n : ast) if (!n->filePath.empty()) files.insert(n->filePath);
    for (const auto& file : files) {
        FileRewrite rw;
        for (const auto& [name, perFile] : internal) {
            std::set<std::string> seen;
            std::string def = resolve(file, name, seen);
            std::string iname = internalFor(name, def);
            if (!iname.empty()) rw.bare[name] = iname;
            for (const auto& u : uses[file]) {
                if (u.alias.empty()) continue;
                std::set<std::string> seen2;
                std::string qdef = resolve(u.target, name, seen2);
                std::string qname = internalFor(name, qdef);
                if (!qname.empty()) rw.qualified[{u.alias, name}] = qname;
            }
        }
        if (std::getenv("QUIRK_DEBUG_TYPENAMES")) {
            for (auto& [k, v] : rw.bare) std::cerr << "[typenames] " << file << ": " << k << " -> " << v << "\n";
            for (auto& [k, v] : rw.qualified) std::cerr << "[typenames] " << file << ": " << k.first << "." << k.second << " -> " << v << "\n";
        }
        if (!rw.bare.empty() || !rw.qualified.empty()) g_typeRewrites[file] = std::move(rw);
    }
    return !g_typeRewrites.empty();
}

// ==========================================================
//  USAGE
// ==========================================================

// One help text for every entry point: `quirk`, `quirk -h` and a bad run
// flag print the same page as `quirk help` (it used to be a second,
// drifting copy here).
void printUsage() { qpm::print_pm_help(); }

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
    try { abs = fs::absolute(entry).lexically_normal().string(); }
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

static std::string resolveRuntimeSoPath();

// LLVM's `llc`, to make a native binary (-o, the build cache, bundle):
// `llc-14` on PATH (Debian / Ubuntu name it so), else the one beside the LLVM
// this compiler was built with (Homebrew's llvm@14 keeps it in its own bin,
// not on PATH: -o and the build cache never worked on macOS), else `llc`.
static std::string llcCommand() {
    if (qplat::have_command("llc-14")) return "llc-14";
#ifdef QUIRK_LLVM_BINDIR
    std::error_code ec;
    fs::path beside = fs::path(QUIRK_LLVM_BINDIR) / "llc";
    if (fs::exists(beside, ec)) return "'" + beside.string() + "'";
#endif
    return "llc";
}

// The Boehm GC, to link a native binary with: `-lgc` — on macOS, the very
// library runtime.so was linked against (`otool -L`): Homebrew's isn't on
// the linker's path ("library 'gc' not found").
static std::string gcLinkArg(const std::string& runtimePath) {
#ifdef __APPLE__
    FILE* p = qplat::popen_cmd("otool -L '" + runtimePath + "' 2>/dev/null", "r");
    char buf[1024];
    std::string found;
    while (p && fgets(buf, sizeof(buf), p)) {
        std::string l(buf);
        size_t start = l.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        std::string path = l.substr(start, l.find(" (") - start);
        if (path.find("libgc") != std::string::npos && path.find("cord") == std::string::npos) { found = path; break; }
    }
    if (p) pclose(p);
    if (!found.empty() && found.front() == '/') return "'" + found + "'";
#else
    (void)runtimePath;
#endif
    return "-lgc";
}

// ---- quirk bundle ---------------------------------------------------------

// Copy `from` (a file or folder) to `to`; hidden entries left out.
static bool bundleCopy(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    if (fs::is_directory(from, ec)) {
        fs::create_directories(to, ec);
        for (auto it = fs::recursive_directory_iterator(from, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            std::string name = it->path().filename().string();
            if (!name.empty() && name[0] == '.') {
                if (it->is_directory(ec)) it.disable_recursion_pending();
                continue;
            }
            fs::path dest = to / fs::relative(it->path(), from, ec);
            if (it->is_directory(ec)) fs::create_directories(dest, ec);
            else if (it->is_regular_file(ec)) fs::copy_file(it->path(), dest, fs::copy_options::overwrite_existing, ec);
        }
        return true;
    }
    if (!fs::is_regular_file(from, ec)) return false;
    fs::create_directories(to.parent_path(), ec);
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    return !ec;
}

// The program's name as a file name: "Quirk IDE" -> "quirk-ide".
static std::string bundleSlug(const std::string& name) {
    std::string out;
    for (char c : name) {
        unsigned char u = (unsigned char)c;
        if (std::isalnum(u)) out += (char)std::tolower(u);
        else if (!out.empty() && out.back() != '-') out += '-';
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "app" : out;
}

// A PNG's size from its header; false if it isn't a PNG.
static bool bundlePngSize(const std::string& path, int& w, int& h) {
    std::ifstream f(path, std::ios::binary);
    unsigned char b[24];
    if (!f.read((char*)b, 24)) return false;
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (memcmp(b, sig, 8) != 0 || memcmp(b + 12, "IHDR", 4) != 0) return false;
    w = (b[16] << 24) | (b[17] << 16) | (b[18] << 8) | b[19];
    h = (b[20] << 24) | (b[21] << 16) | (b[22] << 8) | b[23];
    return true;
}

static std::string bundleReadAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// A Windows .ico holding the PNG as is (Windows Vista and later read PNG icons).
static bool bundleWriteIco(const std::string& png, const std::string& ico) {
    int w = 0, h = 0;
    if (!bundlePngSize(png, w, h)) return false;
    std::string data = bundleReadAll(png);
    std::ofstream o(ico, std::ios::binary);
    auto u16 = [&](unsigned v) { o.put((char)(v & 0xFF)); o.put((char)((v >> 8) & 0xFF)); };
    auto u32 = [&](unsigned v) { u16(v & 0xFFFF); u16(v >> 16); };
    u16(0); u16(1); u16(1);                                  // reserved, an icon, one image
    o.put((char)(w >= 256 ? 0 : w)); o.put((char)(h >= 256 ? 0 : h));
    o.put(0); o.put(0);                                      // no palette, reserved
    u16(1); u16(32);                                         // planes, bits per pixel
    u32((unsigned)data.size()); u32(6 + 16);
    o.write(data.data(), (std::streamsize)data.size());
    return (bool)o;
}

// A macOS .icns holding the PNG (its type by its size).
static bool bundleWriteIcns(const std::string& png, const std::string& icns) {
    int w = 0, h = 0;
    if (!bundlePngSize(png, w, h)) return false;
    std::string data = bundleReadAll(png);
    const char* type = w <= 16 ? "icp4" : w <= 32 ? "icp5" : w <= 64 ? "icp6" : w <= 128 ? "ic07"
                     : w <= 256 ? "ic08" : w <= 512 ? "ic09" : "ic10";
    std::ofstream o(icns, std::ios::binary);
    auto be32 = [&](unsigned v) { o.put((char)(v >> 24)); o.put((char)(v >> 16)); o.put((char)(v >> 8)); o.put((char)v); };
    o.write("icns", 4); be32((unsigned)(8 + 8 + data.size()));
    o.write(type, 4); be32((unsigned)(8 + data.size()));
    o.write(data.data(), (std::streamsize)data.size());
    return (bool)o;
}

// After the binary and its files: what makes it an app of the desktop's —
// on Linux a .desktop launcher (with the icon next to it), on macOS the .app's
// Info.plist and icon. (On Windows the icon is in the .exe already.)
static void bundleDesktopFiles(const CompilerOptions& opts, const fs::path& dir, const std::string& binPath,
                               std::vector<std::string>& made) {
    if (opts.bundleName.empty() && opts.bundleIcon.empty()) return;
    std::string name = !opts.bundleName.empty() ? opts.bundleName : fs::path(opts.inputFile).stem().string();
    std::string slug = bundleSlug(name);
    std::error_code ec;
#if defined(__APPLE__)
    fs::path contents = dir.parent_path();                   // Name.app/Contents (dir is its MacOS/)
    fs::create_directories(contents / "Resources", ec);
    std::string iconFile;
    if (!opts.bundleIcon.empty() && bundleWriteIcns(opts.bundleIcon, (contents / "Resources" / (slug + ".icns")).string()))
        iconFile = slug + ".icns";
    std::ofstream pl((contents / "Info.plist").string());
    pl << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
       << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
       << "<plist version=\"1.0\">\n<dict>\n"
       << "  <key>CFBundleName</key><string>" << name << "</string>\n"
       << "  <key>CFBundleDisplayName</key><string>" << name << "</string>\n"
       << "  <key>CFBundleExecutable</key><string>" << fs::path(binPath).filename().string() << "</string>\n"
       << "  <key>CFBundleIdentifier</key><string>dev.quirk." << slug << "</string>\n"
       << "  <key>CFBundlePackageType</key><string>APPL</string>\n"
       << "  <key>CFBundleVersion</key><string>1.0</string>\n"
       << "  <key>NSHighResolutionCapable</key><true/>\n";
    if (!iconFile.empty()) pl << "  <key>CFBundleIconFile</key><string>" << iconFile << "</string>\n";
    pl << "</dict>\n</plist>\n";
    made.push_back(contents.parent_path().filename().string() + "  (open it, or drag it to Applications)");
#elif !defined(_WIN32)
    fs::path absDir = fs::absolute(dir);
    std::string icon;
    if (!opts.bundleIcon.empty()) {
        fs::copy_file(opts.bundleIcon, dir / (slug + ".png"), fs::copy_options::overwrite_existing, ec);
        if (!ec) icon = (absDir / (slug + ".png")).string();
    }
    std::ofstream de((dir / (slug + ".desktop")).string());
    de << "[Desktop Entry]\n"
       << "Type=Application\n"
       << "Name=" << name << "\n"
       << "Exec=\"" << (absDir / fs::path(binPath).filename()).string() << "\" %F\n"
       << "Path=" << absDir.string() << "\n";
    if (!icon.empty()) de << "Icon=" << icon << "\n";
    de << "Terminal=false\n"
       << "Categories=Development;\n";
    de.close();
    fs::permissions(dir / (slug + ".desktop"), fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add, ec);
    made.push_back(slug + ".desktop  (in your menu: cp it to ~/.local/share/applications/)");
    if (!icon.empty()) made.push_back(slug + ".png");
#else
    (void)dir; (void)binPath; (void)made; (void)slug; (void)ec;
#endif
}

// One value of a [build] line: "text" / 'text' / true / false / ["a", "b"].
static std::vector<std::string> buildValues(const std::string& raw) {
    std::vector<std::string> out;
    std::string v = raw;
    size_t hash = std::string::npos;
    bool inq = false;
    char q = 0;
    for (size_t i = 0; i < v.size(); i++) {
        char c = v[i];
        if (inq) { if (c == q) inq = false; continue; }
        if (c == '"' || c == '\'') { inq = true; q = c; continue; }
        if (c == '#') { hash = i; break; }
    }
    if (hash != std::string::npos) v = v.substr(0, hash);
    auto trimS = [](std::string t) {
        size_t a = t.find_first_not_of(" \t\r\n"), b = t.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
    };
    v = trimS(v);
    if (!v.empty() && v.front() == '[') {
        v = v.substr(1, v.size() >= 2 ? v.size() - 2 : 0);
        std::string cur;
        bool q2 = false;
        char qc = 0;
        for (char c : v) {
            if (q2) { if (c == qc) { q2 = false; out.push_back(cur); cur.clear(); } else cur += c; continue; }
            if (c == '"' || c == '\'') { q2 = true; qc = c; }
        }
        return out;
    }
    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
    out.push_back(v);
    return out;
}

// `quirk build`: the nearest quirk.toml's [build] section into the bundle
// options (what's on the command line wins). Paths are the project's.
static bool loadBuildConfig(CompilerOptions& opts, std::string& err) {
    std::error_code ec;
    fs::path dir = fs::current_path(ec);
    fs::path toml;
    for (fs::path d = dir; ; d = d.parent_path()) {
        if (fs::exists(d / "quirk.toml", ec)) { toml = d / "quirk.toml"; break; }
        if (d == d.parent_path()) break;
    }
    if (toml.empty()) { err = "no quirk.toml here (or give the entry file: quirk build main.qk)"; return false; }
    fs::path root = toml.parent_path();
    std::ifstream in(toml.string());
    std::string line, section, entry, topEntry, name, icon, output;
    std::vector<std::string> includes;
    bool windowed = false, embed = false, any = false;
    while (std::getline(in, line)) {
        size_t a = line.find_first_not_of(" \t");
        if (a == std::string::npos || line[a] == '#') continue;
        std::string t = line.substr(a);
        while (!t.empty() && (t.back() == '\r' || t.back() == ' ')) t.pop_back();
        if (t.front() == '[' && t.back() == ']') { section = t.substr(1, t.size() - 2); continue; }
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = t.substr(0, eq);
        while (!key.empty() && key.back() == ' ') key.pop_back();
        std::vector<std::string> vals = buildValues(t.substr(eq + 1));
        std::string v = vals.empty() ? "" : vals[0];
        if (section.empty() && key == "entry") topEntry = v;
        if (section != "build") continue;
        any = true;
        if (key == "entry") entry = v;
        else if (key == "name") name = v;
        else if (key == "icon") icon = v;
        else if (key == "output") output = v;
        else if (key == "windowed") windowed = v == "true";
        else if (key == "embed") embed = v == "true";
        else if (key == "include") includes = vals;
        else { err = toml.string() + ": [build] has no '" + key + "' (entry, name, icon, windowed, include, embed, output)"; return false; }
    }
    if (entry.empty()) entry = !topEntry.empty() ? topEntry : "main.qk";
    if (!fs::exists(root / entry, ec)) { err = "the entry " + (root / entry).string() + " isn't there (set entry = \"...\" in [build])"; return false; }
    opts.inputFile = (root / entry).string();
    if (opts.outputFile.empty()) opts.outputFile = (root / (output.empty() ? "dist" : output)).string();
    if (opts.bundleName.empty()) opts.bundleName = name;
    if (opts.bundleIcon.empty() && !icon.empty()) opts.bundleIcon = (root / icon).string();
    if (windowed) opts.bundleWindowed = true;
    if (embed) opts.bundleEmbed = true;
    if (opts.bundleIncludes.empty())
        for (auto& inc : includes) opts.bundleIncludes.push_back((root / inc).string());
    if (!any) std::cerr << "build: no [build] in " << toml.string() << " — the defaults (" << entry << ", dist/)\n";
    return true;
}

// The --include files to build into the program: [path relative to the
// entry's folder (with /), file].
static std::vector<std::pair<std::string, fs::path>> embedFiles(const CompilerOptions& opts) {
    std::vector<std::pair<std::string, fs::path>> out;
    std::error_code ec;
    fs::path root = fs::absolute(opts.inputFile).parent_path();
    auto add = [&](const fs::path& f) {
        std::error_code rec;                          // (its own: the walk's ec ends the loop)
        fs::path abs = fs::absolute(f, rec);
        fs::path rel = abs.lexically_relative(fs::absolute(root, rec));
        if (rel.empty() || rel.native()[0] == '.') rel = f.lexically_relative(root);
        if (rel.empty()) rel = f.filename();
        out.push_back({rel.generic_string(), abs});
    };
    for (auto& inc : opts.bundleIncludes) {
        fs::path src = root / inc;
        if (!fs::exists(src, ec)) src = inc;
        if (fs::is_directory(src, ec)) {
            for (auto it = fs::recursive_directory_iterator(src, fs::directory_options::skip_permission_denied, ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                std::string n = it->path().filename().string();
                if (!n.empty() && n[0] == '.') { if (it->is_directory(ec)) it.disable_recursion_pending(); continue; }
                if (it->is_regular_file(ec)) add(it->path());
            }
        } else if (fs::is_regular_file(src, ec)) {
            add(src);
        }
    }
    return out;
}

// LLVM IR (text) holding the files and a constructor that hands their table
// to the runtime (quirk_embed_register) before main runs.
static bool writeEmbedIR(const std::vector<std::pair<std::string, fs::path>>& files, const std::string& llPath) {
    std::ofstream o(llPath);
    auto bytes = [&](const std::string& d) {
        std::string s;
        s.reserve(d.size() * 3);
        static const char* hex = "0123456789ABCDEF";
        for (unsigned char c : d) {
            if (c >= 32 && c < 127 && c != '"' && c != '\\') s += (char)c;
            else { s += '\\'; s += hex[c >> 4]; s += hex[c & 15]; }
        }
        return s;
    };
    o << "%QEmb = type { i8*, i8*, i64 }\n";
    size_t i = 0;
    for (auto& [rel, path] : files) {
        std::string data = bundleReadAll(path.string());
        o << "@q_emb_p" << i << " = private constant [" << rel.size() + 1 << " x i8] c\"" << bytes(rel) << "\\00\"\n";
        o << "@q_emb_d" << i << " = private constant [" << data.size() + 1 << " x i8] c\"" << bytes(data) << "\\00\"\n";
        i++;
    }
    o << "@q_emb_table = private constant [" << files.size() << " x %QEmb] [";
    i = 0;
    for (auto& [rel, path] : files) {
        std::error_code ec;
        uintmax_t n = fs::file_size(path, ec);
        if (i) o << ", ";
        o << "%QEmb { i8* getelementptr ([" << rel.size() + 1 << " x i8], [" << rel.size() + 1 << " x i8]* @q_emb_p" << i << ", i64 0, i64 0), "
          << "i8* getelementptr ([" << n + 1 << " x i8], [" << n + 1 << " x i8]* @q_emb_d" << i << ", i64 0, i64 0), i64 " << n << " }";
        i++;
    }
    o << "]\n";
    o << "declare void @quirk_embed_register(%QEmb*, i64)\n"
      << "define internal void @q_emb_init() {\n"
      << "  call void @quirk_embed_register(%QEmb* getelementptr ([" << files.size() << " x %QEmb], [" << files.size()
      << " x %QEmb]* @q_emb_table, i64 0, i64 0), i64 " << files.size() << ")\n"
      << "  ret void\n}\n"
      << "@llvm.global_ctors = appending global [1 x { i32, void ()*, i8* }] [{ i32, void ()*, i8* } { i32 65535, void ()* @q_emb_init, i8* null }]\n";
    return (bool)o;
}

// The rest of `quirk bundle`, once the binary is linked into `dir`: the
// runtime (and on Windows the DLLs it needs) next to it, the app's files,
// and with --docker a Dockerfile.
static int finishBundle(const CompilerOptions& opts, const std::string& dir,
                        const std::string& runtimePath, const std::string& binPath) {
    std::error_code ec;
    fs::path out(dir);
    fs::path rtDir = fs::path(runtimePath).parent_path();
#ifdef _WIN32
    for (auto& e : fs::directory_iterator(rtDir, ec))
        if (e.path().extension() == ".dll") fs::copy_file(e.path(), out / e.path().filename(), fs::copy_options::overwrite_existing, ec);
#else
    fs::copy_file(runtimePath, out / "runtime.so", fs::copy_options::overwrite_existing, ec);
    if (ec) { std::cerr << "bundle: can't copy the runtime: " << ec.message() << "\n"; return 1; }
#endif
    // The app's files: what a web / live app reads from its folder at run
    // time — the base page, static files, templates (a component's .html
    // next to its .qk) — plus --include. Never .db files, tests, hidden or
    // installed folders: data and secrets stay where they are.
    fs::path root = fs::absolute(opts.inputFile).parent_path();
    fs::path outAbs = fs::absolute(out);
    std::vector<std::string> copied;
    for (const char* name : {"index.html", "public", "static", "views", "templates"}) {
        if (fs::exists(root / name, ec) && bundleCopy(root / name, out / name)) copied.push_back(name);
    }
    int templates = 0;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const fs::path& p = it->path();
        std::string name = p.filename().string();
        if (it->is_directory(ec)) {
            fs::path abs = fs::absolute(p);
            if ((!name.empty() && name[0] == '.') || name == "packages" || name == "node_modules" || name == "tests" ||
                name == "public" || name == "static" || name == "views" || name == "templates" ||
                abs == outAbs)
                it.disable_recursion_pending();
            continue;
        }
        if (p.extension() != ".html" || p.parent_path() == root) continue;
        fs::path qk = p; qk.replace_extension(".qk");
        bool beside = fs::exists(qk, ec);
        if (!beside) {                                       // a component's template: any .qk in its folder
            for (auto& sib : fs::directory_iterator(p.parent_path(), ec))
                if (sib.path().extension() == ".qk" || sib.path().extension() == ".quirk") { beside = true; break; }
        }
        if (!beside) continue;
        if (bundleCopy(p, out / fs::relative(p, root, ec))) templates++;
    }
    for (auto& inc : opts.bundleIncludes) {
        fs::path src = root / inc;
        if (!fs::exists(src, ec)) src = inc;
        if (!fs::exists(src, ec)) { std::cerr << "bundle: --include " << inc << ": not found\n"; return 1; }
        std::string shown = fs::relative(fs::absolute(src), root, ec).generic_string();
        if (opts.bundleEmbed) { copied.push_back(shown + "  (built into the program)"); continue; }
        bundleCopy(src, out / fs::relative(fs::absolute(src), root, ec));
        copied.push_back(shown);
    }
    if (opts.bundleDocker) {
#ifdef _WIN32
        std::cerr << "bundle: --docker makes a Linux image: bundle on Linux\n";
#else
        // The image: the build machine's distribution (the binary is linked
        // against its libraries), with the packages that hold them.
        std::string distro = "ubuntu", version = "20.04";
        {
            std::ifstream os("/etc/os-release");
            std::string line;
            while (std::getline(os, line)) {
                auto val = [&](const std::string& k) {
                    std::string v = line.substr(k.size());
                    if (v.size() >= 2 && v.front() == '"') v = v.substr(1, v.size() - 2);
                    return v;
                };
                if (line.rfind("ID=", 0) == 0) distro = val("ID=");
                else if (line.rfind("VERSION_ID=", 0) == 0) version = val("VERSION_ID=");
            }
        }
        std::set<std::string> pkgs;
        bool dpkg = qplat::have_command("dpkg");
        if (dpkg) {
            FILE* p = qplat::popen_cmd("ldd '" + binPath + "' 2>/dev/null", "r");
            char buf[1024];
            while (p && fgets(buf, sizeof(buf), p)) {
                std::string l(buf);
                size_t arrow = l.find("=> ");
                if (arrow == std::string::npos) continue;
                std::string lib = l.substr(arrow + 3);
                lib = lib.substr(0, lib.find(" ("));
                std::string base = fs::path(lib).filename().string();
                if (base.rfind("libc.so", 0) == 0 || base.rfind("libm.so", 0) == 0 || base.rfind("libpthread", 0) == 0 ||
                    base.rfind("libdl.so", 0) == 0 || base.rfind("librt.so", 0) == 0 || base == "runtime.so" ||
                    lib.find(out.string()) != std::string::npos)
                    continue;
                std::string real = fs::canonical(lib, ec).string();
                FILE* q = qplat::popen_cmd("dpkg -S '" + (ec ? lib : real) + "' 2>/dev/null || dpkg -S '" + lib + "' 2>/dev/null", "r");
                char pb[512];
                if (q && fgets(pb, sizeof(pb), q)) {
                    std::string pkg(pb);
                    pkg = pkg.substr(0, pkg.find(':'));
                    if (!pkg.empty()) pkgs.insert(pkg);
                }
                if (q) pclose(q);
            }
            if (p) pclose(p);
        }
        std::string list = "ca-certificates";
        for (auto& k : pkgs) list += " " + k;
        std::string bin = fs::path(binPath).filename().string();
        std::ofstream df((out / "Dockerfile").string());
        df << "# Made by `quirk bundle --docker`: the app as built on this machine (" << distro << " " << version << ").\n"
           << "#   docker build -t myapp .\n"
           << "#   docker run -p 3000:3000 -e QUIRK_SESSION_SECRET=... myapp\n"
           << "# Files the app writes (a SQLite database) are lost when the container is\n"
           << "# replaced: keep them on a volume — open the database at data/app.db, say,\n"
           << "# and add  -v myapp-data:/app/data  to docker run.\n"
           << "FROM " << distro << ":" << version << "\n";
        if (dpkg)
            df << "RUN apt-get update && apt-get install -y --no-install-recommends " << list
               << " && rm -rf /var/lib/apt/lists/*\n";
        else
            df << "# Install the libraries `ldd " << bin << "` lists (the Boehm GC, OpenSSL) here.\n";
        df << "WORKDIR /app\n"
           << "COPY . /app\n"
           << "# Listen on every address in the container (web's default is 127.0.0.1 only).\n"
           << "ENV QUIRK_HOST=0.0.0.0\n"
           << "# The port your app listens on.\n"
           << "EXPOSE 3000\n"
           << "CMD [\"./" << bin << "\"]\n";
        std::ofstream di((out / ".dockerignore").string());
        di << "*.db\n*.db-journal\n*.db-wal\n";
#endif
    }
    std::vector<std::string> made;
    bundleDesktopFiles(opts, out, binPath, made);
    std::cout << "Bundled " << opts.inputFile << " into " << dir << "/\n"
              << "  " << fs::path(binPath).filename().string() << "   (the app; the runtime next to it)\n";
    for (auto& m : made) std::cout << "  " << m << "\n";
    for (auto& c : copied) std::cout << "  " << c << "\n";
    if (templates) std::cout << "  " << templates << " template file" << (templates == 1 ? "" : "s") << " (next to their .qk)\n";
    if (opts.bundleDocker) std::cout << "  Dockerfile\n";
    if (opts.bundleEmbed && copied.size() == opts.bundleIncludes.size() && !templates)
        std::cout << "Run it from anywhere (its files are inside it):  " << (fs::path(dir) / fs::path(binPath).filename()).string() << "\n";
    else
        std::cout << "Run it from that folder (it reads its files from there):  cd " << dir << " && ./"
                  << fs::path(binPath).filename().string() << "\n";
    return 0;
}

// Size + mtime of a build artifact, for the cache key. A dev rebuild
// keeps QUIRK_VERSION but changes these, so its stale entries miss.
static std::string fileStamp(const std::string& path) {
    std::error_code ec;
    auto size = fs::file_size(path, ec);
    if (ec) return path + ":?";
    // libc++ (macOS) counts file_time_type in __int128 — no to_string overload.
    auto mtime = static_cast<long long>(fs::last_write_time(path, ec).time_since_epoch().count());
    return path + ":" + std::to_string(size) + ":" + std::to_string(mtime);
}

// Cache key = sha256 of the import-closure (entry + everything `use`d
// transitively) + compiler version + opt level + the compiler binary
// and runtime.so it was built with. Returns hex.
//
// On a stale `use`d file the new key won't match the old cache file,
// so we get a clean miss. Recompile + new cache entry. The old entry
// lingers until `pruneCaches` evicts it (LRU, size-budgeted).
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
                       + "-O" + std::to_string(optLevel)
                       + "-" + fileStamp(qpm::self_binary())
                       + "-" + fileStamp(resolveRuntimeSoPath())
                       + (g_test_harness ? "-test[" + g_test_k + "]" : std::string()) + "--";
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
    const char* home = qplat::home_env();
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
        fs::path next = fs::path(exe).parent_path() / qplat::RUNTIME_LIB;
        if (fs::exists(next)) runtimePath = next.string();
    }
    if (const char* envHome = std::getenv("QUIRK_HOME")) {
        std::string venvRuntime = std::string(envHome) + "/bin/" + qplat::RUNTIME_LIB;
        if (fs::exists(venvRuntime)) runtimePath = venvRuntime;
    }
    if (runtimePath.empty()) runtimePath = std::string("./bin/") + qplat::RUNTIME_LIB;
    return runtimePath;
}

// ==========================================================
//  AOT BUILD CACHE  (~/.quirk/cache/build/<key>-<arch>/quirk-bin)
// ==========================================================
//
// Sibling of the bitcode cache. v2.0 introduces an AOT build cache:
//   - `quirk build <file>` produces a native binary and stashes it at
//     ~/.quirk/cache/build/<sha256>-<arch>/quirk-bin.
//   - `quirk <file>` checks for a cached binary first; on a hit, the
//     compiler `execv`s it (replacing this process) and skips all of
//     parse → Sema → Codegen → JIT.
// Cache miss falls through to the existing JIT path — no behaviour
// change for never-built scripts.
//
// The key derivation reuses `computeCacheKey` (input file content +
// transitive imports + compiler version + opt level) and appends the
// host arch, since a Linux x86_64 binary is unusable on a macOS arm64
// runtime and vice versa.

static std::string detectArchTag() {
#if defined(__APPLE__) && defined(__aarch64__)
    return "darwin-arm64";
#elif defined(__APPLE__) && defined(__x86_64__)
    return "darwin-x86_64";
#elif defined(__linux__) && defined(__aarch64__)
    return "linux-arm64";
#elif defined(__linux__) && defined(__x86_64__)
    return "linux-x86_64";
#elif defined(_WIN32) && defined(_M_ARM64)
    return "windows-arm64";
#elif defined(_WIN32) && defined(_M_X64)
    return "windows-x86_64";
#else
    return "unknown-arch";
#endif
}

static fs::path buildCachePathFor(const std::string& key) {
    const char* home = qplat::home_env();
    fs::path root = home ? fs::path(home) / ".quirk" / "cache" / "build"
                         : fs::temp_directory_path() / "quirk-cache-build";
#ifdef _WIN32
    return root / (key + "-" + detectArchTag()) / "quirk-bin.exe";
#else
    return root / (key + "-" + detectArchTag()) / "quirk-bin";
#endif
}

// ==========================================================
//  CACHE EVICTION  (LRU by mtime, size-budgeted)
// ==========================================================
//
// Every edit to a program mints a new cache key, so without eviction
// both caches grow forever. Hits bump the entry's mtime (`touchCacheEntry`)
// so mtime == last use; after each save, `pruneCaches` deletes the
// least-recently-used entries until the bitcode + AOT caches together fit
// in the budget: $QUIRK_CACHE_MAX_MB, default 512. The entry just written
// is never evicted.

static void touchCacheEntry(const fs::path& p) {
    std::error_code ec;
    fs::last_write_time(p, fs::file_time_type::clock::now(), ec);
}

static uintmax_t cacheBudgetBytes() {
    uintmax_t mb = 512;
    if (const char* env = std::getenv("QUIRK_CACHE_MAX_MB")) {
        char* end = nullptr;
        unsigned long long v = std::strtoull(env, &end, 10);
        if (end && end != env && *end == '\0') mb = v;
    }
    return mb * 1024 * 1024;
}

static void pruneCaches(const fs::path& justSaved) {
    struct Entry { fs::path path; uintmax_t size; fs::file_time_type used; };
    std::vector<Entry> entries;
    uintmax_t total = 0;
    std::error_code ec;

    // Bitcode cache: one `<key>.bc` file per entry.
    fs::path bcRoot = cachePathFor("x").parent_path();
    for (auto& f : fs::directory_iterator(bcRoot, ec)) {
        if (!f.is_regular_file(ec) || f.path().extension() != ".bc") continue;
        uintmax_t sz = f.file_size(ec);
        entries.push_back({f.path(), sz, f.last_write_time(ec)});
        total += sz;
    }
    // AOT cache: one `<key>-<arch>/` directory per entry; its `quirk-bin`
    // carries the last-use mtime.
    fs::path aotRoot = buildCachePathFor("x").parent_path().parent_path();
    for (auto& d : fs::directory_iterator(aotRoot, ec)) {
        if (!d.is_directory(ec)) continue;
        uintmax_t sz = 0;
        for (auto& f : fs::recursive_directory_iterator(d.path(), ec))
            if (f.is_regular_file(ec)) sz += f.file_size(ec);
        fs::path bin = d.path() / "quirk-bin";
        auto used = fs::exists(bin, ec) ? fs::last_write_time(bin, ec) : d.last_write_time(ec);
        entries.push_back({d.path(), sz, used});
        total += sz;
    }

    uintmax_t budget = cacheBudgetBytes();
    if (total <= budget) return;
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.used < b.used; });
    for (const auto& e : entries) {
        if (total <= budget) break;
        if (e.path == justSaved || justSaved.parent_path() == e.path) continue;
        fs::remove_all(e.path, ec);
        if (!ec) total -= e.size;
    }
}

// Compile the source at `inputPath` into a native binary at `outPath`.
// Uses the same llc + gcc pipeline as `quirk -o <file>` — the new
// wrapper only handles the cache plumbing and a sane `outPath` default.
// Returns 0 on success, non-zero on failure (caller falls back to JIT).
static int runAotPipeline(const std::string& irPath,
                          const std::string& outPath,
                          const std::string& runtimePath,
                          bool verbose) {
    fs::path outDir = fs::path(outPath).parent_path();
    if (!outDir.empty()) {
        std::error_code ec;
        fs::create_directories(outDir, ec);
        if (ec) {
            if (verbose) std::cerr << "build: can't create cache dir: " << ec.message() << "\n";
            return 1;
        }
    }
    // Stage into a sibling tempfile so concurrent builds (two `quirk`
    // invocations racing on the same key) don't half-clobber each
    // other. Final move is atomic on POSIX.
    std::string tmpObj = outPath + ".o.tmp." + std::to_string(getpid());
    std::string tmpBin = outPath + ".tmp." + std::to_string(getpid());
#ifdef _WIN32
    {
        tmpObj = outPath + ".obj.tmp." + std::to_string(getpid());
        tmpBin = outPath + ".tmp." + std::to_string(getpid()) + ".exe";
        std::string err;
        bool ok = windowsEmitObject(irPath, tmpObj, err) &&
                  windowsLinkExe(tmpObj, tmpBin, runtimePath, err);
        std::error_code ec;
        fs::remove(tmpObj, ec);
        if (!ok) {
            if (verbose) std::cerr << "build: " << err << "\n";
            fs::remove(tmpBin, ec);
            return 1;
        }
        fs::rename(tmpBin, outPath, ec);
        return ec ? 1 : 0;
    }
#endif

    std::string llcCmd = llcCommand() + " -filetype=obj -relocation-model=pic -O2 "
                       + irPath + " -o " + tmpObj;
    if (verbose) std::cerr << "build: " << llcCmd << "\n";
    if (qplat::run_shell(llcCmd.c_str()) != 0) {
        std::error_code ec; fs::remove(tmpObj, ec);
        return 1;
    }
    std::string runtimeDir = fs::path(runtimePath).parent_path().string();
    std::string linkCmd = "gcc " + tmpObj + " " + runtimePath
                        + " " + gcLinkArg(runtimePath) + " -lm -lpthread -o " + tmpBin
                        + " -Wl,-rpath," + runtimeDir;
    if (verbose) std::cerr << "build: " << linkCmd << "\n";
    if (qplat::run_shell(linkCmd.c_str()) != 0) {
        std::error_code ec;
        fs::remove(tmpObj, ec);
        fs::remove(tmpBin, ec);
        return 1;
    }
    // Atomic swap into place.
    std::error_code ec;
    fs::rename(tmpBin, outPath, ec);
    fs::remove(tmpObj, ec);
    if (ec) return 1;
    return 0;
}

// Start `quirk --build <file>` detached (double fork: no zombie even if
// this run lasts long; own session; output discarded; lower priority).
// Skipped when the binary exists, or an earlier build for this key failed
// (`<bin>.failed` — e.g. llc / gcc missing) so runs don't keep retrying.
static void spawnBackgroundAotBuild(const std::string& input, const std::string& key, int optLevel) {
    fs::path bin = buildCachePathFor(key);
    std::error_code ec;
    if (fs::exists(bin, ec) || fs::exists(bin.string() + ".failed", ec)) return;
    std::string self = qpm::self_binary();
    std::string inputAbs = fs::absolute(input, ec).string();
    qplat::spawn_detached({self, "--build", "-O" + std::to_string(optLevel), inputAbs});
}

// `execv` the cached binary, forwarding the script's argv. Returns
// only on failure — on success this process is replaced. argv[0] is
// the script path (matches the JIT convention so `sys.argv()[0]`
// stays consistent across JIT and AOT modes).
static int execCachedBinary(const std::string& binPath,
                            const std::string& inputFile,
                            const std::vector<std::string>& scriptArgs) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(inputFile.c_str()));
    for (const auto& a : scriptArgs) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    // The program's own `sys.executable()` should name quirk, not the
    // cached binary it now runs as.
#if defined(__linux__)
    {
        char self[4096];
        ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (n > 0) { self[n] = '\0'; setenv("QUIRK_EXE", self, 1); }
    }
#endif
#ifdef _WIN32
    {
        // No exec on Windows: run it as a child with our console and pass
        // its exit code through. runtime.dll (and the GC / pthread / SSL
        // DLLs) sit next to it in quirk's bin dir; put that on PATH.
        std::string self = qplat::self_exe();
        _putenv_s("QUIRK_EXE", self.c_str());
        std::string binDir = fs::path(self).parent_path().string();
        const char* path = std::getenv("PATH");
        std::string newPath = binDir + ";" + (path ? path : "");
        _putenv_s("PATH", newPath.c_str());
        std::vector<std::string> args;
        for (size_t i = 0; argv[i]; i++) args.push_back(argv[i]);
        int rc = qplat::spawn_wait(binPath, args);
        if (rc == -1) std::cerr << "quirk: couldn't run cached binary " << binPath << "\n";
        return rc;
    }
#endif
    execv(binPath.c_str(), argv.data());
    // execv only returns on failure.
    std::perror("quirk: execv cached binary");
    return 1;
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

    for (const char* gcLib : qplat::GC_LIBS) {
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
#ifdef _WIN32
    // Bind the C library names generated code calls to the runtime's
    // quirk_crt_* wrappers (one CRT, one stdout buffer — see win_compat.h).
    // Defined in the JITDylib itself, so they win over the process search.
    {
        std::string libErr;
        auto rtLib = llvm::sys::DynamicLibrary::getPermanentLibrary(runtimePath.c_str(), &libErr);
        llvm::orc::SymbolMap crt;
        for (const char* n : {"printf", "sprintf", "fopen", "fclose", "fwrite", "fread", "fseek",
                              "ftell", "strlen", "malloc", "free", "exit", "write"}) {
            void* p = rtLib.getAddressOfSymbol((std::string("quirk_crt_") + n).c_str());
            if (p) crt[JIT->mangleAndIntern(n)] = llvm::JITEvaluatedSymbol(
                llvm::pointerToJITTargetAddress(p), llvm::JITSymbolFlags::Exported);
        }
        if (!crt.empty()) llvm::cantFail(JD.define(llvm::orc::absoluteSymbols(std::move(crt))));
    }
#endif
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

    // Run the JIT'd main on a worker thread with a 64 MB
    // stack. The main thread's stack is ~8 MB (Linux default)
    // which deeply-recursive selfhost code — esp. sema/codegen
    // chasing nested method-call expressions through
    // typing/collections/list.quirk — exhausts. Running on a
    // larger stack via pthread sidesteps the issue without
    // making the regression on already-flat code worse.
    // (POSIX: a pthread with a 64 MB stack; Windows: CreateThread.)
    int programResult = qplat::run_on_big_stack(mainFn, (int)scriptArgv.size(),
                                                scriptArgv.data(), 64 * 1024 * 1024);
    // Forward the program's exit code to the shell verbatim. A
    // nonzero return from `main` is a program-level signal, NOT a
    // compiler error — the driver used to shout `Error: Program
    // exited with code N` on any nonzero, which conflated normal
    // program semantics with compilation failure. The user can read
    // the exit code via `$?` if they need it.
    return programResult;
}

// ==========================================================
//  SYMBOL TABLE DUMP  (--symbols-json)
// ==========================================================
//
// Walks the analyzed AST and emits one NDJSON record per declaration
// to stdout. Designed for the LSP — scope-aware completion needs to
// know what names are in scope at any given file:line, and a future
// semantic-rename pass needs the canonical decl site for each symbol.
//
// Record shape (extra keys ignored by consumers):
//   {"kind":"function","name":"greet","scope":"module",
//    "file":"/abs/path.quirk","line":3,"col":8,"type":"String"}
//
// `scope` is the enclosing context:
//   - "module" for top-level decls
//   - "<struct_name>" for fields and methods
//   - "<function_name>" for parameters and local variables
// Nested scopes (closures, lambdas) inherit their parent function's
// name today — finer-grained scope tracking can layer on later.

static std::string symJsonEscape(const std::string& s) {
    std::string out; out.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else out += c;
        }
    }
    return out;
}

// Specialization name → display form (`Box__Int` → `Box[Int]`), filled
// by emitSymbols so hover types read the way the user wrote them.
static std::map<std::string, std::string> g_symDisplayNames;

// A symbol name / scope as written in the source: `Button_in_ui` → `Button`,
// also as a prefix (`Button_in_ui_draw` → `Button_draw`).
static std::string sourceTypeName(const std::string& s) {
    for (const auto& [internal, shown] : quirk::typeDisplayNames()) {
        if (s.rfind(internal, 0) != 0) continue;
        if (s.size() > internal.size() && s[internal.size()] != '_') continue;
        size_t dot = shown.rfind('.');
        return (dot == std::string::npos ? shown : shown.substr(dot + 1)) + s.substr(internal.size());
    }
    return s;
}

static void emitSymbol(const char* kind, const std::string& rawName,
                       const std::string& rawScope, const std::string& file,
                       int line, int col, const std::string& rawType = "") {
    const std::string name = sourceTypeName(rawName);
    const std::string scope = sourceTypeName(rawScope);
    // Temporaries the compiler generates (`assert`'s __qa_*, `quirk test`'s
    // __qt_*) aren't the user's names.
    if (name.rfind("__qa_", 0) == 0 || name.rfind("__qt_", 0) == 0) return;
    std::string type = rawType;
    {
        bool nullable = !type.empty() && type.back() == '?';
        auto it = g_symDisplayNames.find(nullable ? type.substr(0, type.size() - 1) : type);
        if (it != g_symDisplayNames.end()) type = it->second + (nullable ? "?" : "");
    }
    type = quirk::displayTypeNames(type);
    std::cout << "{\"kind\":\""  << kind
              << "\",\"name\":\""  << symJsonEscape(name)
              << "\",\"scope\":\"" << symJsonEscape(scope)
              << "\",\"file\":\""  << symJsonEscape(file)
              << "\",\"line\":"    << line
              << ",\"col\":"       << col;
    if (!type.empty())
        std::cout << ",\"type\":\""  << symJsonEscape(type) << "\"";
    std::cout << "}\n";
}

// Walk a function/method body looking for variable declarations. The
// existing AST already carries line/col on every node, so we just
// inspect the LHS of each VarDeclNode for a bare identifier (the
// common `x := value` shape). Destructuring (`(a, b) := tuple`) is
// rare today and can be folded in later.
// The type to show for a declaration: as written (`List[P]`, kept past
// the monomorphisation rewrite to `List`), else annotated, else inferred.
static std::string declTypeOf(const VarDeclNode* vd) {
    if (!vd->writtenAnnotation.empty()) return vd->writtenAnnotation;
    if (!vd->typeAnnotation.empty())    return vd->typeAnnotation;
    return vd->inferredType;
}

// Walk a body for variable declarations, descending into blocks (if /
// while / for / try / with) with the same scope — Quirk variables are
// function-scoped. `for` loop variables are reported with the type Sema
// gave them.
static void emitBodyVariables(const std::vector<std::unique_ptr<Node>>& body,
                              const std::string& scope);
static void emitStmtVariables(Node* n, const std::string& scope) {
    {
        if (auto vd = dynamic_cast<VarDeclNode*>(n)) {
            // Declarations only: `x = v` re-assigns an existing name.
            bool isDecl = vd->op == ":=" || !vd->typeAnnotation.empty();
            if (auto* lit = dynamic_cast<LiteralNode*>(vd->lhs.get()); lit && isDecl)
                emitSymbol("variable", lit->value, scope,
                           vd->filePath, vd->line, vd->col, declTypeOf(vd));
        } else if (auto f = dynamic_cast<ForNode*>(n)) {
            auto shown = [](const std::string& t) { return t == "Any" ? std::string() : t; };
            if (f->destructureVars.empty()) {
                emitSymbol("variable", f->varName, scope, f->filePath, f->line, f->col, shown(f->itemType));
                if (!f->varName2.empty())
                    emitSymbol("variable", f->varName2, scope, f->filePath, f->line, f->col, shown(f->itemType2));
            }
            emitBodyVariables(f->body, scope);
        } else if (auto i = dynamic_cast<IfNode*>(n)) {
            emitBodyVariables(i->thenBranch, scope);
            for (auto& b : i->elIfBranches) emitBodyVariables(b.body, scope);
            emitBodyVariables(i->elseBranch, scope);
        } else if (auto w = dynamic_cast<WhileNode*>(n)) {
            emitBodyVariables(w->body, scope);
        } else if (auto t = dynamic_cast<TryCatchNode*>(n)) {
            emitBodyVariables(t->tryBlock, scope);
            for (auto& cb : t->catchBlocks) emitBodyVariables(cb.body, scope);
            emitBodyVariables(t->finallyBlock, scope);
        } else if (auto w2 = dynamic_cast<WithNode*>(n)) {
            emitBodyVariables(w2->body, scope);
        }
    }
}
static void emitBodyVariables(const std::vector<std::unique_ptr<Node>>& body,
                              const std::string& scope) {
    for (auto& stmt : body) emitStmtVariables(stmt.get(), scope);
}

static void emitSymbols(const std::vector<std::unique_ptr<Node>>& ast) {
    for (auto& node : ast)
        if (auto s = dynamic_cast<StructNode*>(node.get()))
            if (!s->genericDisplayName.empty()) g_symDisplayNames[s->name] = s->genericDisplayName;
    // `async define f` is compiled as a wrapper `f` plus the body
    // `f__async_impl`; editors should see one `f` returning Task[T].
    static const std::string kAsync = "__async_impl";
    std::set<std::string> asyncNames;   // functions and `<Struct>_<method>`
    auto isAsyncImpl = [&](const std::string& n) {
        return n.size() > kAsync.size() && n.compare(n.size() - kAsync.size(), kAsync.size(), kAsync) == 0;
    };
    for (auto& node : ast)
        if (auto fn = dynamic_cast<FunctionNode*>(node.get()))
            if (isAsyncImpl(fn->name))
                asyncNames.insert(fn->name.substr(0, fn->name.size() - kAsync.size()));
    for (auto& node : ast) {
        if (node->isSpecialization) continue;   // monomorphized copies
        if (auto fn = dynamic_cast<FunctionNode*>(node.get())) {
            if (fn->isExtern) continue;     // external decls: declared, not defined here
            if (asyncNames.count(fn->name)) continue;   // the async wrapper
            if (fn->cls.empty() && isAsyncImpl(fn->name)) {
                const std::string shown = fn->name.substr(0, fn->name.size() - kAsync.size());
                const std::string ret = fn->writtenReturnType.empty() ? fn->returnType : fn->writtenReturnType;
                emitSymbol("function", shown, "module", fn->filePath, fn->line, fn->col,
                           ret.empty() || ret == "void" ? "Task" : "Task[" + ret + "]");
                for (const auto& p : fn->parameters)
                    emitSymbol("parameter", p.name, shown, fn->filePath, fn->line, fn->col,
                               p.writtenType.empty() ? p.type : p.writtenType);
                emitBodyVariables(fn->body, shown);
                continue;
            }
            // Top-level functions have empty `cls`; methods carry their
            // owning struct's name. Use that to pick "function" vs
            // "method" + the right scope.
            const bool isMethod = !fn->cls.empty();
            // The parser mangles method names as `<Struct>_<raw>` (or
            // `<Struct>__init` for constructors) so codegen can emit
            // them as flat LLVM symbols. The LSP needs the *raw* name
            // — that's what users type in `obj.<raw>(...)`. Demangle
            // by stripping the cls prefix where we recognise it.
            std::string displayName = fn->name;
            if (isMethod) {
                if (fn->name == fn->cls + "__init") displayName = "__init";
                else if (fn->name.rfind(fn->cls + "_", 0) == 0)
                    displayName = fn->name.substr(fn->cls.size() + 1);
            }
            std::string shownType = fn->returnType;
            if (isMethod && isAsyncImpl(displayName)) {
                // An async method's body: show it as the method, returning Task[T].
                displayName = displayName.substr(0, displayName.size() - kAsync.size());
                const std::string ret = fn->writtenReturnType.empty() ? fn->returnType : fn->writtenReturnType;
                shownType = ret.empty() || ret == "void" ? "Task" : "Task[" + ret + "]";
            }
            emitSymbol(isMethod ? "method" : "function",
                       displayName,
                       isMethod ? fn->cls : std::string("module"),
                       fn->filePath, fn->line, fn->col,
                       shownType);
            // Parameters: scope is the function's own name. Use the
            // *display* name for methods so the LSP can attach params
            // to the right outline node (`Point.norm`'s `self`).
            const std::string& paramScope = isMethod ? displayName : fn->name;
            for (const auto& p : fn->parameters) {
                emitSymbol("parameter", p.name, paramScope,
                           fn->filePath, fn->line, fn->col,
                           p.writtenType.empty() ? p.type : p.writtenType);
            }
            emitBodyVariables(fn->body, paramScope);
        }
        else if (auto sn = dynamic_cast<StructNode*>(node.get())) {
            emitSymbol("struct", sn->name, "module",
                       sn->filePath, sn->line, sn->col);
            for (const auto& f : sn->fields) {
                emitSymbol("field", f.name, sn->name,
                           sn->filePath, sn->line, sn->col, f.type);
            }
            // Methods are emitted as top-level FunctionNodes whose
            // `cls` matches `sn->name` — they get picked up in the
            // FunctionNode branch above. No duplicate emission here.
        }
        else if (auto en = dynamic_cast<EnumNode*>(node.get())) {
            emitSymbol("enum", en->name, "module",
                       en->filePath, en->line, en->col);
            for (const auto& v : en->variants) {
                emitSymbol("enumvariant", v, en->name,
                           en->filePath, en->line, en->col);
            }
        }
        else if (auto in = dynamic_cast<InterfaceNode*>(node.get())) {
            emitSymbol("interface", in->name, "module",
                       in->filePath, in->line, in->col);
            for (const auto& m : in->methods) {
                emitSymbol("method", m->name, in->name,
                           m->filePath, m->line, m->col, m->returnType);
            }
        }
        else if (auto vd = dynamic_cast<VarDeclNode*>(node.get())) {
            // Top-level `NAME := value` — module-level constant.
            if (auto* lit = dynamic_cast<LiteralNode*>(vd->lhs.get())) {
                emitSymbol("module_const", lit->value, "module",
                           vd->filePath, vd->line, vd->col,
                           declTypeOf(vd));
            }
        }
        else if (dynamic_cast<ForNode*>(node.get()) || dynamic_cast<IfNode*>(node.get()) ||
                 dynamic_cast<WhileNode*>(node.get()) || dynamic_cast<TryCatchNode*>(node.get()) ||
                 dynamic_cast<WithNode*>(node.get())) {
            // Top-level blocks in a script without main().
            emitStmtVariables(node.get(), "module");
        }
    }
    std::cout.flush();
}

// ==========================================================
//  MAIN
// ==========================================================

int main(int argc, char* argv[]) {
    qplat::utf8_console();   // Windows: UTF-8 output (├──, ✓, …); no-op elsewhere
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
        // QUIRK_NO_UPDATE_CHECK is set. Not after `quirk compiler …`:
        // `update` / `install` just replaced this binary (its version is
        // stale, so the notice would offer the release it installed), and
        // `check` / `list` already print the latest release.
        if (std::string(argv[1]) != "compiler") qpm::maybe_announce_update();
        return pmRc;
    }

    // Parse CLI flags. Anything after the input file (the first non-`-` arg)
    // is forwarded to the user script via sys.argv() — same convention as
    // python/node: `quirk [compiler-flags] script.quirk [script-args...]`.
    CompilerOptions opts;
    std::vector<std::string> scriptArgs;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        // (`quirk bundle app.qk -o dist`: there's no script argv, its options
        // can come after the file.)
        if (!opts.inputFile.empty() && !opts.bundle) {
            // Already seen script — everything else is the script's own argv.
            scriptArgs.push_back(arg);
            continue;
        }
        if      (arg == "-h" || arg == "--help") { printUsage(); return 0; }
        else if (arg == "--compile-only") opts.runImmediate = false;
        else if (arg == "--check")        { opts.checkOnly = true; opts.runImmediate = false; }
        else if (arg == "--watch")        opts.watch        = true;
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
        else if (arg == "--no-aot")       opts.useAotCache  = false;
        else if (arg == "--build" || arg == "build") {
            // `quirk build <file>` and `quirk --build <file>` both work.
            // The `build` form is a "soft subcommand" — it isn't in the
            // qpm dispatch table because the build pipeline shares all
            // of main()'s CLI surface (verbose/opt/cache flags), so
            // routing it through qpm would mean re-plumbing every
            // flag. Setting the flag here keeps the existing main()
            // path and just diverts at the very end.
            opts.buildOnly    = true;
            opts.runImmediate = false;
        }
        else if (arg == "bundle" || arg == "--bundle") {
            opts.bundle       = true;
            opts.runImmediate = false;
        }
        else if (arg == "--docker")       opts.bundleDocker = true;
        else if (arg == "--windowed")     opts.bundleWindowed = true;
        else if (arg == "--embed")        opts.bundleEmbed = true;
        else if (arg == "--name" || arg == "--icon") {
            if (++i >= argc) { std::cerr << "Error: " << arg << " needs a value\n"; return 1; }
            (arg == "--name" ? opts.bundleName : opts.bundleIcon) = argv[i];
        }
        else if (arg == "--include") {
            if (++i >= argc) { std::cerr << "Error: --include needs a path\n"; return 1; }
            opts.bundleIncludes.push_back(argv[i]);
        }
        else if (arg == "--stdin-source") g_stdin_source = true;
        else if (arg == "--test-harness") g_test_harness = true;
        else if (arg.rfind("--test-k=", 0) == 0) g_test_k = arg.substr(9);
        else if (arg == "--diagnostics-json") {
            // Structured error mode for tooling (LSP). Switches every
            // error printer to NDJSON-on-stdout; the bitcode cache is
            // disabled so each invocation emits fresh diagnostics
            // instead of silently short-circuiting on a stale hit.
            g_diagnostics_json = true;
            opts.useCache = false;
        }
        else if (arg == "--symbols-json") {
            // Symbol-table dump for tooling. Implies --check so we
            // don't pay the codegen cost; runs through Sema, then
            // walks the AST and emits NDJSON. Bitcode cache off for
            // the same reason as --diagnostics-json.
            opts.symbolsJson  = true;
            opts.checkOnly    = true;
            opts.runImmediate = false;
            g_diagnostics_json = true;     // route any errors to NDJSON too
            opts.useCache     = false;
        }
        else if (arg == "-o") {
            if (++i >= argc) { std::cerr << "Error: -o requires a filename\n"; return 1; }
            opts.outputFile    = argv[i];
            opts.runImmediate  = false;
        }
        else if (arg[0] != '-') {
            if (opts.bundle && !opts.inputFile.empty()) {
                std::cerr << "bundle: one file (the app's entry), got " << opts.inputFile << " and " << arg << "\n";
                return 1;
            }
            opts.inputFile = arg;
        }
        else {
            std::cerr << "Unknown option: " << arg << std::endl;
            printUsage();
            return 1;
        }
    }

    // `quirk build` with no file: the app as the project's quirk.toml
    // describes it ([build]) — `quirk bundle` with those options.
    if ((opts.buildOnly || opts.bundle) && opts.inputFile.empty()) {
        std::string err;
        if (!loadBuildConfig(opts, err)) {
            std::cerr << "build: " << err << "\n";
            return 1;
        }
        opts.buildOnly = false;
        opts.bundle = true;
    }
    if (opts.inputFile.empty()) {
        std::cerr << "Error: No input file specified." << std::endl;
        printUsage();
        return 1;
    }
    // `bundle`: -o is the folder (default dist/); the binary goes in it,
    // named after the entry file.
    std::string bundleDir;
    if (opts.bundle) {
        bundleDir = opts.outputFile.empty() ? "dist" : opts.outputFile;
        std::error_code ec;
        fs::create_directories(bundleDir, ec);
        if (ec) { std::cerr << "bundle: can't make " << bundleDir << ": " << ec.message() << "\n"; return 1; }
        std::string base = fs::path(opts.inputFile).stem().string();
        if (!opts.bundleName.empty()) {
#ifdef _WIN32
            base = opts.bundleName;                          // "Quirk IDE.exe"
            for (char& c : base) if (std::strchr("\\/:*?\"<>|", c)) c = '-';
#else
            base = bundleSlug(opts.bundleName);              // "quirk-ide"
#endif
        }
        if (!opts.bundleIcon.empty()) {
            int iw = 0, ih = 0;
            if (!fs::exists(opts.bundleIcon, ec)) {
                std::cerr << "bundle: --icon " << opts.bundleIcon << ": no such file\n";
                return 1;
            }
            if (!bundlePngSize(opts.bundleIcon, iw, ih)) {
                std::cerr << "bundle: --icon " << opts.bundleIcon << ": not a PNG file\n";
                return 1;
            }
            opts.bundleIcon = fs::absolute(opts.bundleIcon).string();
        }
#ifdef __APPLE__
        // A named app: Name.app, the program and its files in Contents/MacOS.
        if (!opts.bundleName.empty() || !opts.bundleIcon.empty()) {
            std::string app = (!opts.bundleName.empty() ? opts.bundleName : base) + ".app";
            bundleDir = (fs::path(bundleDir) / app / "Contents" / "MacOS").string();
            fs::create_directories(bundleDir, ec);
        }
#endif
        opts.outputFile = (fs::path(bundleDir) / base).string();
    }
    // `--watch`: this process supervises; the program runs as a child
    // (the same command without --watch), again on every change.
    if (opts.watch) {
        std::vector<std::string> childArgs;
        for (int i = 0; i < argc; ++i)
            if (i == 0 || std::string(argv[i]) != "--watch") childArgs.push_back(argv[i]);
        return qwatch::run(qpm::self_binary(), childArgs, opts.inputFile);
    }
    // `quirk run x_test.qk` of a test file (test_* functions, no main):
    // nothing runs, and nothing said why. Point to `quirk test`.
    if (opts.runImmediate && !opts.checkOnly && !opts.buildOnly && !g_test_harness && !g_diagnostics_json) {
        std::string base = fs::path(opts.inputFile).filename().string();
        if (base.find("_test.") != std::string::npos) {
            std::ifstream in(opts.inputFile);
            std::stringstream ss; ss << in.rdbuf();
            std::string src = ss.str();
            static const std::regex testFn(R"(^\s*define\s+test_\w+\s*\(\s*\))");
            static const std::regex mainFn(R"(^\s*define\s+main\s*\()");
            std::istringstream lines(src);
            std::string line;
            int tests = 0;
            bool hasMain = false;
            while (std::getline(lines, line)) {
                if (std::regex_search(line, testFn)) tests++;
                else if (std::regex_search(line, mainFn)) hasMain = true;
            }
            if (tests > 0 && !hasMain)
                std::cerr << "note: " << base << " has " << tests << " test" << (tests == 1 ? "" : "s")
                          << " and no main() — `quirk run` doesn't run them. Run them with:\n"
                          << "    quirk test " << opts.inputFile << "\n"
                          << "    quirk test --watch " << opts.inputFile << "    (again on every save)\n";
        }
    }
    // `--stdin-source`: the editor's unsaved text for the entry file.
    if (g_stdin_source && !opts.inputFile.empty()) {
        std::stringstream ss; ss << std::cin.rdbuf();
        g_stdin_source_text = ss.str();
        g_stdin_source_path = fs::absolute(opts.inputFile).string();
    }

    // Pipes / process substitution (`quirk <(...)`, `/dev/stdin`) can only
    // be read once, but the cache-key hash and the parser each open the
    // input — the parser then saw an empty program and exited 0 silently.
    // Spool non-regular inputs into a temp file so every read agrees.
    {
        std::error_code ec;
        if (fs::exists(opts.inputFile, ec) && !fs::is_regular_file(opts.inputFile, ec)
            && !fs::is_directory(opts.inputFile, ec)) {
            std::ifstream in(opts.inputFile, std::ios::binary);
            std::stringstream ss; ss << in.rdbuf();
            static std::string spooled;
            spooled = qplat::write_temp_file("quirk-stdin-", ".quirk", ss.str());
            if (!spooled.empty()) {
                opts.inputFile = spooled;
                std::atexit([] { std::remove(spooled.c_str()); });
            }
        }
    }

    // Derive base name for output files (e.g. "tests/strings.quirk" -> "strings")
    fs::path inputPath(opts.inputFile);
    std::string baseName = inputPath.stem().string();    // "strings"
    std::string baseDir  = inputPath.parent_path().string();
    if (baseDir.empty()) baseDir = ".";

    Logger log(opts.verbose);

    // =======================================================
    // 0a. AOT BUILD-CACHE FAST PATH  (v2.0)
    // =======================================================
    // Skip every other phase — including parse and Sema — when the
    // AOT cache already has a binary keyed to this (input + version +
    // opt + arch). `execv` replaces the current process so signals
    // and exit codes flow through cleanly. Gated by `useAotCache` so
    // users can opt out via `--no-aot` (or QUIRK_NO_AOT=1) for JIT
    // semantics. Skipped for `--build` (we're producing the binary),
    // `--debug` (the stepper needs the JIT's source map), `--check` /
    // `--emit-*` (no run expected), and explicit `-o` (the user is
    // already asking for a one-shot AOT build).
    if (const char* killSwitch = std::getenv("QUIRK_NO_AOT"))
        if (killSwitch[0] && killSwitch[0] != '0') opts.useAotCache = false;
    if (opts.useAotCache && opts.runImmediate && !opts.debugMode
        && !opts.checkOnly && !opts.emitIR && !opts.emitAST
        && !opts.buildOnly && opts.outputFile.empty()) {
        std::string aotKey = computeCacheKey(opts.inputFile, opts.optLevel);
        if (!aotKey.empty()) {
            fs::path binPath = buildCachePathFor(aotKey);
            if (fs::exists(binPath)) {
                log.debug("AOT cache HIT: " + binPath.string());
                touchCacheEntry(binPath);
                return execCachedBinary(binPath.string(), opts.inputFile, scriptArgs);
            }
            log.debug("AOT cache MISS: " + binPath.string());
        }
    }

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
                touchCacheEntry(cp);
                auto ctx = std::make_unique<llvm::LLVMContext>();
                if (auto cached = loadCachedBitcode(cp, *ctx)) {
                    // Second run of an unchanged script: build the native
                    // binary in the background so later runs exec it
                    // (~80 ms of JIT work → a few ms).
                    if (opts.useAotCache && !g_test_harness && !g_stdin_source)
                        spawnBackgroundAotBuild(opts.inputFile, key, opts.optLevel);
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

    g_entry_dir = fs::path(opts.inputFile).parent_path().string();
    g_entry_file = opts.inputFile;
    auto userNodes = processFile(opts.inputFile, log, sourceMap, true);
    for (auto& node : userNodes)
        ast.push_back(std::move(node));

    // Two files define the same type name → give the non-main ones internal
    // names and load the program again with the references rewritten.
    {
        std::string typingDir = corePath.empty() ? "" :
            fs::absolute(fs::path(corePath)).parent_path().generic_string() + "/";
        if (planTypeRewrites(ast, fs::absolute(opts.inputFile).string(), typingDir)) {
            ast.clear();
            sourceMap.clear();
            loadedModules.clear();
            if (!corePath.empty())
                for (auto& node : processFile(corePath, log, sourceMap))
                    ast.push_back(std::move(node));
            for (auto& node : processFile(opts.inputFile, log, sourceMap, true))
                ast.push_back(std::move(node));
        }
    }

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
        // Sema checks every statement even after an error, so the symbol
        // table is still worth having: editors keep hover / completion
        // types on a file with mistakes in it.
        if (opts.checkOnly && opts.symbolsJson) {
            emitSymbols(ast);
            std::cout.flush();
        }
        if (!g_diagnostics_json) std::cerr << "Compilation failed." << std::endl;
        return 1;
    }

    if (opts.checkOnly) {
        // --symbols-json runs through --check too. Sema's results are
        // recorded in node properties, so walking the AST here is
        // enough to produce the symbol table. After the decls, dump
        // Sema's recorded usage table so the LSP can do semantic
        // find-references and rename without re-resolving.
        if (opts.symbolsJson) {
            emitSymbols(ast);
            for (const auto& u : sema.usages) {
                emitSymbol("usage", u.name, u.scope,
                           u.filePath, u.line, u.col);
            }
            std::cout.flush();
        }
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
            : ((fs::temp_directory_path() / "quirk_").string() + baseName + ".ll");

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
        codegen.setEntryFile(fs::absolute(opts.inputFile).string());
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
#ifdef _WIN32
        {
            std::string irPath  = (fs::temp_directory_path() / "quirk_").string() + baseName + ".ll";
            std::string objPath = (fs::temp_directory_path() / "quirk_").string() + baseName + ".obj";
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
                codegen.setEntryFile(fs::absolute(opts.inputFile).string());
                        if (opts.bundle) codegen.setRelativeFilesRoot(fs::absolute(opts.inputFile).parent_path().string());
                        codegen.compile(ast, irDest);
            }
            std::string err;
            // The icon, in the .exe (Explorer, the taskbar): a resource, made
            // with the Windows SDK's rc.exe (or LLVM's llvm-rc).
            std::string resPath;
            if (opts.bundle && !opts.bundleIcon.empty()) {
                fs::path tmp = fs::temp_directory_path() / ("quirk_icon_" + std::to_string((long long)std::time(nullptr)));
                std::error_code iec;
                fs::create_directories(tmp, iec);
                std::string ico = (tmp / "app.ico").string(), rc = (tmp / "app.rc").string(), res = (tmp / "app.res").string();
                std::string rcTool = qplat::have_command("rc.exe") ? "rc.exe" : qplat::have_command("llvm-rc") ? "llvm-rc" : "";
                if (bundleWriteIco(opts.bundleIcon, ico) && !rcTool.empty()) {
                    std::ofstream(rc) << "1 ICON \"" << fs::path(ico).generic_string() << "\"\n";
                    if (qplat::run_shell(rcTool + " /nologo /fo \"" + res + "\" \"" + rc + "\"") == 0) resPath = res;
                }
                if (resPath.empty())
                    std::cerr << "bundle: the .exe keeps the default icon (no rc.exe / llvm-rc: run from a Visual Studio"
                                 " developer prompt); the window still shows the app's own\n";
            }
            std::string extra = resPath.empty() ? "" : "\"" + resPath + "\"";
            if (opts.bundle && opts.bundleEmbed) {
                auto files = embedFiles(opts);
                std::string ll = (fs::temp_directory_path() / ("quirk_embed_" + std::to_string((long long)std::time(nullptr)) + ".ll")).string();
                std::string eobj = ll.substr(0, ll.size() - 3) + ".obj";
                if (files.empty()) { std::cerr << "bundle: --embed: no files in the --include folders\n"; return 1; }
                if (!writeEmbedIR(files, ll)) { std::cerr << "bundle: --embed: can't write " << ll << "\n"; return 1; }
                if (!windowsEmitObject(ll, eobj, err)) {
                    std::cerr << "bundle: --embed: can't build the files into the program: " << err << "\n";
                    return 1;
                }
                extra += " \"" + eobj + "\"";
            }
            if (!windowsEmitObject(irPath, objPath, err) ||
                !windowsLinkExe(objPath, opts.outputFile, runtimePath, err, opts.bundle && opts.bundleWindowed, extra)) {
                std::cerr << "Error: " << err << std::endl;
                return 1;
            }
            log.debug("Binary written to " + opts.outputFile);
            if (opts.bundle) return finishBundle(opts, bundleDir, runtimePath, opts.outputFile + ".exe");
            return 0;
        }
#endif
        // Emit optimised IR to a temp file, then compile + link via llc-14 + gcc.
        // This is the same pipeline used by --emit-ir; it's reliable and avoids
        // having to manage TargetMachine lifetime alongside the running compiler.
        std::string irPath  = (fs::temp_directory_path() / "quirk_").string() + baseName + ".ll";
        std::string objPath = (fs::temp_directory_path() / "quirk_").string() + baseName + ".o";

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
            codegen.setEntryFile(fs::absolute(opts.inputFile).string());
            if (opts.bundle) codegen.setRelativeFilesRoot(fs::absolute(opts.inputFile).parent_path().string());
            codegen.compile(ast, irDest);
        }

        // Compile IR → object file
        std::string llcCmd = llcCommand() + " -filetype=obj -relocation-model=pic -O2 "
                           + irPath + " -o " + objPath;
        log.debug("Compiling: " + llcCmd);
        if (int r = qplat::run_shell(llcCmd.c_str())) {
            std::cerr << "Error: llc failed (exit " << r << ")" << std::endl;
            return 1;
        }

        // The --include files built into the program: their own object.
        std::string embedObj;
        if (opts.bundle && opts.bundleEmbed) {
            auto files = embedFiles(opts);
            std::string ll = objPath + ".embed.ll";
            embedObj = objPath + ".embed.o";
            if (files.empty()) { std::cerr << "bundle: --embed: no files in the --include folders\n"; return 1; }
            if (!writeEmbedIR(files, ll) ||
                qplat::run_shell((llcCommand() + " -filetype=obj -relocation-model=pic " + ll + " -o " + embedObj).c_str()) != 0) {
                std::cerr << "bundle: --embed: can't build the files into the program (" << ll << ")\n";
                return 1;
            }
            objPath += " " + embedObj;
        }

        // Link: object + runtime.so → output binary
        std::string runtimeDir = fs::path(runtimePath).parent_path().string();
        std::string linkCmd = "gcc " + objPath + " " + runtimePath
                            + " " + gcLinkArg(runtimePath) + " -lm -lpthread -o " + opts.outputFile
                            + " -Wl,-rpath," + runtimeDir;
        if (opts.bundle) {
            // The runtime next to the binary, wherever the folder goes.
#ifdef __APPLE__
            linkCmd = "gcc " + objPath + " " + runtimePath + " " + gcLinkArg(runtimePath) + " -lm -lpthread -o '" + opts.outputFile
                    + "' -Wl,-rpath,@executable_path";
#else
            linkCmd = "gcc " + objPath + " -L" + runtimeDir + " -l:runtime.so -lgc -lm -lpthread -o '" + opts.outputFile
                    + "' -Wl,-rpath,'$ORIGIN'";
#endif
        }
        log.debug("Linking: " + linkCmd);
        if (int r = qplat::run_shell(linkCmd.c_str())) {
            std::cerr << "Error: linker failed (exit " << r << ")" << std::endl;
            return 1;
        }
#ifdef __APPLE__
        if (opts.bundle) {
            // The binary names the runtime by its install name: point it next to itself.
            std::string fix = "install_name_tool -change \"$(otool -D '" + runtimePath + "' | tail -1)\" "
                              "@executable_path/runtime.so '" + opts.outputFile + "'";
            if (int r = qplat::run_shell(fix.c_str())) {
                std::cerr << "Error: install_name_tool failed (exit " << r << ")" << std::endl;
                return 1;
            }
        }
#endif

        log.debug("Binary written to " + opts.outputFile);
        if (opts.bundle) return finishBundle(opts, bundleDir, runtimePath, opts.outputFile);
        return 0;
    }

    // =======================================================
    // 6bx. AOT BUILD CACHE  (--build / `quirk build`)
    // =======================================================
    // Identical pipeline to the `-o` path above, but the output goes
    // into the shared `~/.quirk/cache/build/<key>-<arch>/` slot
    // instead of a user-named file. A subsequent `quirk <file>` hits
    // the AOT cache fast path at step 0a and exec's the binary —
    // typical second-run latency drops from ~80ms (JIT) to ~3ms
    // (process startup + dlopen).
    if (opts.buildOnly) {
        std::string aotKey = computeCacheKey(opts.inputFile, opts.optLevel);
        if (aotKey.empty()) {
            std::cerr << "build: couldn't hash input file\n";
            return 1;
        }
        fs::path binPath = buildCachePathFor(aotKey);

        // Already cached? Idempotent — return success without rebuilding.
        if (fs::exists(binPath)) {
            log.debug("build: cache hit, skipping rebuild");
            std::cout << "Built: " << binPath.string() << " (cache hit)\n";
            return 0;
        }

        // Per process: two builds of same-named scripts (or the same one,
        // from background builds) mustn't share the temp IR file.
        std::string irPath = (fs::temp_directory_path() / "quirk_").string() + baseName + "_aot." + std::to_string(getpid()) + ".ll";
        {
            std::error_code EC;
            llvm::raw_fd_ostream irDest(irPath, EC, llvm::sys::fs::OF_None);
            if (EC) {
                std::cerr << "build: cannot open temp IR file '" << irPath
                          << "': " << EC.message() << std::endl;
                return 1;
            }
            LLVMCodegen codegen;
            codegen.setVerbose(opts.verbose);
            codegen.setOptLevel(opts.optLevel);
            codegen.setDebugMode(opts.debugMode);
            codegen.setSourceMap(sourceMap);
            codegen.setEntryFile(fs::absolute(opts.inputFile).string());
            if (opts.bundle) codegen.setRelativeFilesRoot(fs::absolute(opts.inputFile).parent_path().string());
            codegen.compile(ast, irDest);
        }
        if (runAotPipeline(irPath, binPath.string(), runtimePath, opts.verbose) != 0) {
            std::cerr << "build: AOT pipeline failed — leaving cache untouched\n";
            std::error_code ec; fs::remove(irPath, ec);
            // Remember it, so background builds (spawnBackgroundAotBuild)
            // don't retry this key on every run.
            fs::create_directories(binPath.parent_path(), ec);
            std::ofstream(binPath.string() + ".failed") << "AOT build failed\n";
            return 1;
        }
        std::error_code ec; fs::remove(irPath, ec);
        pruneCaches(binPath);
        std::cout << "Built: " << binPath.string() << "\n";
        std::cout << "Now `quirk " << opts.inputFile
                  << "` will exec this binary directly (~10× faster startup).\n";
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
        codegen.setEntryFile(fs::absolute(opts.inputFile).string());
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
                    pruneCaches(cp);
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