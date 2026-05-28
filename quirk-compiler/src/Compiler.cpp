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
            p.push_back(venvBase + "/libs/");
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

        p.push_back("./libs/");

        if (!envHome) {
            p.push_back("/usr/local/lib/quirk/packages/");
            p.push_back("/usr/local/lib/quirk/");
        }
        return p;
    }();
    return paths;
}

std::string resolveImportPath(const std::string& moduleName, const std::string& relativeTo = "") {

    // Relative imports (start with '.')
    if (!moduleName.empty() && moduleName[0] == '.') {
        if (relativeTo.empty()) {
            std::cerr << "Error: Relative import '" << moduleName << "' used without context." << std::endl;
            exit(1);
        }

        size_t dotCount = 0;
        while (dotCount < moduleName.size() && moduleName[dotCount] == '.')
            dotCount++;

        fs::path baseDir = fs::path(relativeTo).parent_path();
        for (size_t i = 1; i < dotCount; i++)
            baseDir = baseDir.parent_path();

        std::string subPath = moduleName.substr(dotCount);

        fs::path candidateFile = baseDir / (subPath + ".quirk");
        if (fs::exists(candidateFile)) return candidateFile.string();

        fs::path candidateInit = baseDir / subPath / "index.quirk";
        if (fs::exists(candidateInit)) return candidateInit.string();

        return "";
    }

    // Self-resolution: if the importing file lives inside a project whose
    // quirk.toml declares `name = <moduleName>`, resolve to that project's
    // own entry point. Wins over the standard search paths so that local
    // edits aren't shadowed by an installed copy of the same package.
    if (!relativeTo.empty()) {
        std::string selfPath = qpm::resolve_self_package(moduleName, relativeTo);
        if (!selfPath.empty()) return selfPath;
    }

    // Absolute imports
    std::string relPath = moduleName;
    std::replace(relPath.begin(), relPath.end(), '.', '/');

    std::vector<std::string> variants = {
        relPath + ".quirk",
        relPath + "/index.quirk",
        relPath + "/src/index.quirk",                       // package layout: pkg/src/index.quirk
        relPath + "/src/" + relPath + ".quirk",
    };

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

    if (mod.find("libs/") == 0) mod = mod.substr(5);
    else if (mod.find("src/") == 0) mod = mod.substr(4);

    const char* envHome = std::getenv("QUIRK_HOME");
    if (envHome) {
        std::string base       = std::string(envHome);
        std::string venvPkg    = base + "/lib/quirk/packages/";
        std::string venvStdlib = base + "/lib/quirk/stdlib/";   // new venv layout
        std::string venvCore   = base + "/lib/quirk/";          // legacy + dev install
        std::string venvLibs   = base + "/libs/";               // local dev layout

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
        std::cerr << "Compilation failed." << std::endl;
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
    std::cout << "Usage: quirk [options] <file.quirk> [script-args...]\n"
              << "\n"
              << "Run options:\n"
              << "  --compile-only      Compile only, do not run\n"
              << "  --check             Type-check only (no codegen)\n"
              << "  -o <file>           Compile to native binary\n"
              << "  -v                  Verbose: show debug output\n"
              << "  --emit-ir           Write LLVM IR to <file>.ll\n"
              << "  --emit-ast          Write AST dump to <file>.ast.log\n"
              << "  --debug             Step through execution in the qdb prompt\n"
              << "\n"
              << "Package management:\n"
              << "  quirk install [-r <file>] [pkg ...]   install dependencies\n"
              << "  quirk upgrade [pkg ...]               bump to latest versions\n"
              << "  quirk remove <pkg> [pkg ...]          uninstall packages\n"
              << "  quirk list (or `packages`, `-p`)      show installed packages\n"
              << "  quirk show <pkg>                      package details\n"
              << "  quirk init                            scaffold quirk.toml\n"
              << "  quirk venv <name>                     create an isolated env\n"
              << "  quirk --version                       print Quirk version\n"
              << std::endl;
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
    if (qpm::dispatch(argc, argv, pmRc)) return pmRc;

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
        std::cerr << "Compilation failed." << std::endl;
        return 1;
    }

    if (opts.checkOnly) {
        std::cout << opts.inputFile << ": OK\n";
        return 0;
    }

    // =======================================================
    // 5. CODE GENERATION
    // =======================================================
    log.debug("Starting Code Generation...");

    // Resolve runtime.so path: prefer the one next to the binary itself
    // (works from any CWD and from inside a venv), then $QUIRK_HOME/bin/,
    // then a CWD-relative fallback for legacy dev invocations.
    std::string runtimePath;
    {
        char buf[4096];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            fs::path next = fs::path(buf).parent_path() / "runtime.so";
            if (fs::exists(next)) runtimePath = next.string();
        }
        const char* envHome = std::getenv("QUIRK_HOME");
        if (envHome) {
            std::string venvRuntime = std::string(envHome) + "/bin/runtime.so";
            if (fs::exists(venvRuntime)) runtimePath = venvRuntime;
        }
        if (runtimePath.empty()) runtimePath = "./bin/runtime.so";
    }

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
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::InitializeNativeTargetAsmParser();

        // Load libgc explicitly with RTLD_GLOBAL so GC_malloc is visible to the JIT.
        // runtime.so depends on libgc but loads it RTLD_LOCAL; the JIT needs it globally.
        for (const char* gcLib : {"libgc.so.1", "libgc.so"}) {
            std::string errMsg;
            if (!llvm::sys::DynamicLibrary::LoadLibraryPermanently(gcLib, &errMsg)) break;
        }

        // Build IR + run O2 passes, get ownership of the module
        LLVMCodegen codegen;
        codegen.setVerbose(opts.verbose);
        codegen.setOptLevel(opts.optLevel);
        codegen.setDebugMode(opts.debugMode);
        codegen.setSourceMap(sourceMap);
        auto [module, ctx] = codegen.compileAndRelease(ast);

        // Catch malformed IR early (e.g. missing terminators, type mismatches)
        // before the JIT tries to compile it and produces a cryptic crash.
        {
            std::string verifyErrors;
            llvm::raw_string_ostream verifyStream(verifyErrors);
            if (llvm::verifyModule(*module, &verifyStream)) {
                std::cerr << "Internal compiler error: malformed IR\n"
                          << verifyStream.str() << std::endl;
                return 1;
            }
        }

        // Create LLJIT instance
        auto jitOrErr = llvm::orc::LLJITBuilder().create();
        if (!jitOrErr) {
            std::cerr << "Error: failed to create JIT: "
                      << llvm::toString(jitOrErr.takeError()) << std::endl;
            return 1;
        }
        auto& JIT = *jitOrErr;
        auto& JD  = JIT->getMainJITDylib();

        // Expose current-process symbols (printf, malloc, etc.)
        {
            auto genOrErr = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
                JIT->getDataLayout().getGlobalPrefix());
            if (!genOrErr) {
                std::cerr << "Error: failed to create process symbol generator: "
                          << llvm::toString(genOrErr.takeError()) << std::endl;
                return 1;
            }
            JD.addGenerator(std::move(*genOrErr));
        }

        // Load runtime.so — provides all Core_* and quirk_* symbols
        {
            auto genOrErr = llvm::orc::DynamicLibrarySearchGenerator::Load(
                runtimePath.c_str(), JIT->getDataLayout().getGlobalPrefix());
            if (!genOrErr) {
                std::cerr << "Error: could not load runtime.so (" << runtimePath << "): "
                          << llvm::toString(genOrErr.takeError()) << std::endl;
                return 1;
            }
            JD.addGenerator(std::move(*genOrErr));
        }

        // Add the compiled module
        if (auto err = JIT->addIRModule(
                llvm::orc::ThreadSafeModule(std::move(module), std::move(ctx)))) {
            std::cerr << "Error: JIT addIRModule failed: "
                      << llvm::toString(std::move(err)) << std::endl;
            return 1;
        }

        // Resolve and call main(argc, argv)
        auto mainSym = JIT->lookup("main");
        if (!mainSym) {
            std::cerr << "Error: JIT could not find 'main': "
                      << llvm::toString(mainSym.takeError()) << std::endl;
            return 1;
        }
        auto mainFn = (int(*)(int, char**))mainSym->getAddress();
        // Build the argv visible to the running script: [scriptPath, ...scriptArgs]
        // — argv[0] is the source file path; scriptArgs is everything after.
        std::vector<char*> scriptArgv;
        scriptArgv.push_back(const_cast<char*>(opts.inputFile.c_str()));
        for (auto& a : scriptArgs) scriptArgv.push_back(const_cast<char*>(a.c_str()));
        int ret = mainFn((int)scriptArgv.size(), scriptArgv.data());
        if (ret != 0) {
            std::cerr << "Error: Program exited with code " << ret << std::endl;
            return ret;
        }
    }

    log.debug("Done.");
    return 0;
}