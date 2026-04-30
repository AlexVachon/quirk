#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/IR/Verifier.h"
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
    bool runImmediate = true;
    bool verbose      = false;
    bool emitIR       = false;
    bool emitAST      = false;
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

std::vector<std::string> getSearchPaths() {
    std::vector<std::string> paths;

    const char* envHome = std::getenv("QUIRK_HOME");
    if (envHome) {
        std::string venvBase = std::string(envHome);
        paths.push_back(venvBase + "/lib/quirk/packages/");
        paths.push_back(venvBase + "/lib/quirk/");
        paths.push_back(venvBase + "/libs/");
    }

    paths.push_back("./libs/");

    if (!envHome) {
        paths.push_back("/usr/local/lib/quirk/packages/");
        paths.push_back("/usr/local/lib/quirk/");
    }

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

        fs::path candidateFile = baseDir / (subPath + ".qk");
        if (fs::exists(candidateFile)) return candidateFile.string();

        fs::path candidateInit = baseDir / subPath / "__init.qk";
        if (fs::exists(candidateInit)) return candidateInit.string();

        return "";
    }

    // Absolute imports
    std::string relPath = moduleName;
    std::replace(relPath.begin(), relPath.end(), '.', '/');

    std::vector<std::string> variants = {
        relPath + ".qk",
        relPath + "/__init.qk"
    };

    for (const auto& root : getSearchPaths()) {
        for (const auto& variant : variants) {
            fs::path fullPath = fs::path(root) / variant;
            if (fs::exists(fullPath)) return fullPath.string();
        }
    }

    return "";
}

// ==========================================================
//  MODULE LOADING
// ==========================================================

std::set<std::string> loadedModules;

std::string getModuleName(const std::string& path) {
    std::string mod = path;

    if (mod.find("./") == 0) mod = mod.substr(2);

    if (mod.find("libs/") == 0) mod = mod.substr(5);
    else if (mod.find("src/") == 0) mod = mod.substr(4);

    const char* envHome = std::getenv("QUIRK_HOME");
    if (envHome) {
        std::string base     = std::string(envHome);
        std::string venvPkg  = base + "/lib/quirk/packages/";
        std::string venvCore = base + "/lib/quirk/";
        std::string venvLibs = base + "/libs/";   // local dev layout: $QUIRK_HOME/libs/

        size_t pos;
        if ((pos = mod.find(venvPkg)) != std::string::npos)
            mod = mod.substr(pos + venvPkg.length());
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
    std::cout << "Usage: quirk [options] <file.qk>\n"
              << "\n"
              << "Options:\n"
              << "  --compile-only  Compile only, do not run\n"
              << "  -v              Verbose: show debug output\n"
              << "  --emit-ir       Write LLVM IR to <file>.ll\n"
              << "  --emit-ast      Write AST dump to <file>.ast.log\n"
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

    // Parse CLI flags
    CompilerOptions opts;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if      (arg == "-h" || arg == "--help") { printUsage(); return 0; }
        else if (arg == "--compile-only") opts.runImmediate = false;
        else if (arg == "-v")             opts.verbose      = true;
        else if (arg == "--emit-ir")      opts.emitIR       = true;
        else if (arg == "--emit-ast")     opts.emitAST      = true;
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

    // Derive base name for output files (e.g. "tests/strings.qk" -> "strings")
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
    if (!sema.analyze(ast)) {
        std::cerr << "Compilation failed." << std::endl;
        return 1;
    }

    // =======================================================
    // 5. CODE GENERATION
    // =======================================================
    log.debug("Starting Code Generation...");

    // Resolve runtime.so path (needed by both emit-ir and JIT paths)
    std::string runtimePath = "./bin/runtime.so";
    {
        const char* envHome = std::getenv("QUIRK_HOME");
        if (envHome) {
            std::string venvRuntime = std::string(envHome) + "/bin/runtime.so";
            if (fs::exists(venvRuntime)) runtimePath = venvRuntime;
        }
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
        codegen.setSourceMap(sourceMap);
        codegen.compile(ast, dest);
        dest.flush();

        if (opts.emitIR) {
            log.debug("LLVM IR written to " + irPath);
            return 0;
        }
    }

    // =======================================================
    // 6b. JIT COMPILE + RUN (runImmediate, no subprocess)
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
        int ret = mainFn(argc, argv);
        if (ret != 0) {
            std::cerr << "Error: Program exited with code " << ret << std::endl;
            return ret;
        }
    }

    log.debug("Done.");
    return 0;
}