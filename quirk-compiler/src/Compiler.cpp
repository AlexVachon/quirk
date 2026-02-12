#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>  // for std::replace
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <vector>

#include "lexer.hpp"
#include "parser.hpp"
#include "sema.hpp"

// Include Codegen (which includes StructGen, BuiltinGen, etc.)
#include "Backend/Codegen.cpp"

namespace fs = std::filesystem;

// ==========================================================
//  AST PRINTER (Dumps AST to ast.log)
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
        if (!node)
            return;

        printIndent(indent);

        if (auto f = dynamic_cast<FunctionNode*>(node)) {
            out << "[Function] " << f->name << (f->isExtern ? " (extern)" : "")
                << " -> " << (f->returnType.empty() ? "void" : f->returnType)
                << "\n";
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
            out << "[VarDecl] " << v->op << " Type: " << v->typeAnnotation
                << "\n";
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
//  IMPORT RESOLUTION LOGIC (Virtual Env Support)
// ==========================================================

std::vector<std::string> getSearchPaths() {
    std::vector<std::string> paths;

    // 1. Check for Virtual Environment (QUIRK_HOME)
    const char* envHome = std::getenv("QUIRK_HOME");

    if (envHome) {
        std::string venvBase = std::string(envHome);
        
        // Priority 1: Installed Packages (e.g. use requests -> packages/requests.qk)
        paths.push_back(venvBase + "/lib/quirk/packages/");
        
        // Priority 2: Standard Library PARENT
        // We look in 'quirk' so that 'use core.sys' resolves to 'quirk/core/sys.qk'
        paths.push_back(venvBase + "/lib/quirk/"); 
        
        // Priority 3: Root libs (Legacy/Fallback)
        paths.push_back(venvBase + "/libs/");
    }

    // 2. Local Project 'libs' (Manual overrides)
    paths.push_back("./libs/");

    // 3. System Global Fallback
    if (!envHome) {
        paths.push_back("/usr/local/lib/quirk/packages/");
        // Same logic here: point to parent of core
        paths.push_back("/usr/local/lib/quirk/"); 
    }

    return paths;
}

std::string resolveImportPath(const std::string& moduleName, const std::string& relativeTo = "") {
    
    // 1. Handle Relative Imports (starts with .)
    if (!moduleName.empty() && moduleName[0] == '.') {
        if (relativeTo.empty()) {
            std::cerr << "Error: Relative import '" << moduleName << "' used without context." << std::endl;
            exit(1);
        }

        // Count leading dots: "." = current, ".." = parent
        // FIX: Changed int to size_t to fix signed/unsigned comparison warning
        size_t dotCount = 0;
        while (dotCount < moduleName.size() && moduleName[dotCount] == '.') {
            dotCount++;
        }

        // Get the directory of the file doing the import
        fs::path baseDir = fs::path(relativeTo).parent_path();

        // Move up directory tree for each extra dot (start at 1 for current dir)
        for (size_t i = 1; i < dotCount; i++) {
            baseDir = baseDir.parent_path();
        }

        // Extract the rest of the path (e.g., "sys" from ".sys")
        std::string subPath = moduleName.substr(dotCount);
        
        // Construct candidates
        // Case A: .sys -> base/sys.qk
        fs::path candidateFile = baseDir / (subPath + ".qk");
        if (fs::exists(candidateFile)) return candidateFile.string();

        // Case B: .collections -> base/collections/__init.qk
        fs::path candidateInit = baseDir / subPath / "__init.qk";
        if (fs::exists(candidateInit)) return candidateInit.string();

        return ""; // Not found
    }

    // 2. Absolute Imports (Existing Logic)
    std::string relPath = moduleName;
    std::replace(relPath.begin(), relPath.end(), '.', '/'); // ensure standard format

    std::vector<std::string> variants = {
        relPath + ".qk",
        relPath + "/__init.qk"
    };

    auto searchPaths = getSearchPaths();
    for (const auto& root : searchPaths) {
        for (const auto& variant : variants) {
            fs::path fullPath = fs::path(root) / variant;
            if (fs::exists(fullPath)) return fullPath.string();
        }
    }

    return "";
}

// ==========================================================
//  MAIN COMPILER LOGIC
// ==========================================================

std::set<std::string> loadedModules;

std::string getModuleName(const std::string& path) {
    std::string mod = path;

    // 1. Remove leading "./"
    if (mod.find("./") == 0) {
        mod = mod.substr(2);
    }

    // 2. Remove "libs/" prefix (Standard Library)
    if (mod.find("libs/") == 0) {
        mod = mod.substr(5);
    }
    // 3. Remove "src/" prefix (User source code)
    else if (mod.find("src/") == 0) {
        mod = mod.substr(4);
    }
    // 4. Remove ".venv/lib/quirk/packages/" (Virtual Env)
    else {
        std::string venvPkg = ".venv/lib/quirk/packages/";
        size_t pos = mod.find(venvPkg);
        if (pos != std::string::npos) {
            mod = mod.substr(pos + venvPkg.length());
        }
        
        std::string venvCore = ".venv/lib/quirk/";
        pos = mod.find(venvCore);
        if (pos != std::string::npos) {
            mod = mod.substr(pos + venvCore.length());
        }
    }

    // 5. Remove extension
    size_t lastDot = mod.find_last_of('.');
    if (lastDot != std::string::npos) {
        mod = mod.substr(0, lastDot);
    }

    // 6. Normalize separators
    std::replace(mod.begin(), mod.end(), '/', '.');
    std::replace(mod.begin(), mod.end(), '\\', '.');

    return mod;
}

std::vector<std::unique_ptr<Node>> processFile(const std::string& filePath,
                                               bool isMainFile = false) {
    std::cerr << "[DEBUG] Processing file: " << filePath << std::endl;

    std::string absPath = fs::absolute(filePath).string();
    if (loadedModules.count(absPath))
        return {};
    loadedModules.insert(absPath);

    std::ifstream file(filePath);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open module '" << filePath << "'"
                  << std::endl;
        exit(1);
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string source = buffer.str();

    Lexer lexer(source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens, source);
    auto nodes = parser.parse();

    // Determine Module Name
    std::string currentModule = isMainFile ? "main" : getModuleName(filePath);
    std::vector<std::unique_ptr<Node>> allNodes;

    for (auto& node : nodes) {
        node->moduleName = currentModule;

        if (auto use = dynamic_cast<UseNode*>(node.get())) {
            // Use Helper to Resolve Path (passing context)
            std::string importPath = resolveImportPath(use->moduleName, filePath);

            if (importPath.empty()) {
                std::cerr << "Error: Could not resolve module '"
                          << use->moduleName << "' imported from " << filePath << std::endl;
                std::cerr
                    << "Checked paths (ensure QUIRK_HOME is set if using venv):"
                    << std::endl;
                for (auto& p : getSearchPaths())
                    std::cerr << " - " << p << std::endl;
                exit(1);
            }

            // Recurse
            auto importedNodes = processFile(importPath, false);
            for (auto& importedNode : importedNodes) {
                allNodes.push_back(std::move(importedNode));
            }
            allNodes.push_back(std::move(node));
        } else {
            allNodes.push_back(std::move(node));
        }
    }
    return allNodes;
}

int main(int argc, char* argv[]) {
    // 1. Redirect Debug Logs to compiler.log
    std::ofstream logFile("compiler.log");
    std::streambuf* cerr_buffer = std::cerr.rdbuf();
    std::cerr.rdbuf(logFile.rdbuf());

    if (argc < 2) {
        std::cout << "Usage: quirk [-r] <file.qk>" << std::endl;
        return 1;
    }

    std::string filename = (std::string(argv[1]) == "-r") ? argv[2] : argv[1];
    bool runImmediate = (std::string(argv[1]) == "-r");

    // Master AST container
    std::vector<std::unique_ptr<Node>> ast;

    // =======================================================
    // 1. IMPLICITLY LOAD STANDARD LIBRARY (PRELUDE)
    // =======================================================
    std::cerr << "[DEBUG] Loading Standard Library (Prelude)..." << std::endl;
    
    // This looks for 'core/__init.qk' or 'core.qk'
    // Passing "" as context since 'core' is an absolute path
    std::string corePath = resolveImportPath("core", "");

    if (!corePath.empty()) {
        auto coreNodes = processFile(corePath);
        for (auto& node : coreNodes) {
            ast.push_back(std::move(node));
        }
        std::cerr << "[DEBUG] Loaded " << coreNodes.size() << " nodes from Core." << std::endl;
    } else {
        std::cerr << "[WARNING] 'core' library not found! Standard types (String, List) will fail." << std::endl;
        
        const char* env = std::getenv("QUIRK_HOME");
        std::cerr << "  QUIRK_HOME: " << (env ? env : "(unset)") << std::endl;
        std::cerr << "  Search Paths:" << std::endl;
        for (const auto& p : getSearchPaths()) {
            std::cerr << "   - " << p << std::endl;
        }
    }

    // =======================================================
    // 2. LOAD USER FILE
    // =======================================================
    std::cerr << "[DEBUG] Loading user file: " << filename << std::endl;
    
    auto userNodes = processFile(filename, true);
    for (auto& node : userNodes) {
        ast.push_back(std::move(node));
    }

    // =======================================================
    // 3. DUMP AST (Debugging)
    // =======================================================
    std::cerr << "[DEBUG] Dumping AST to ast.log..." << std::endl;
    std::ofstream astLog("ast.log");
    if (astLog.is_open()) {
        ASTPrinter printer(astLog);
        printer.printAll(ast);
        astLog.close();
    }

    // =======================================================
    // 4. SEMANTIC ANALYSIS
    // =======================================================
    std::cerr << "[DEBUG] Running Semantic Analysis..." << std::endl;
    Sema sema;
    if (!sema.analyze(ast)) {
        std::cerr.rdbuf(cerr_buffer);
        std::cout << "Compilation Failed. See compiler.log for details." << std::endl;
        return 1;
    }

    // =======================================================
    // 5. CODE GENERATION
    // =======================================================
    std::cerr << "[DEBUG] Starting Code Generation..." << std::endl;
    LLVMCodegen codegen;

    std::string irPath = "output.ll";
    std::error_code EC;
    raw_fd_ostream dest(irPath, EC, sys::fs::F_None);
    if (EC) {
        std::cerr.rdbuf(cerr_buffer);
        std::cerr << "Error opening output file: " << EC.message() << std::endl;
        return 1;
    }

    codegen.compile(ast, dest);
    dest.flush();
    dest.close();

    // =======================================================
    // 6. EXECUTION (JIT / LLI)
    // =======================================================
    if (runImmediate) {
        std::cerr << "[DEBUG] Compilation finished. Executing..." << std::endl;

        std::string runtimePath = "./bin/runtime.so";
        const char* envHome = std::getenv("QUIRK_HOME");
        if (envHome) {
            std::string venvRuntime = std::string(envHome) + "/bin/runtime.so";
            if (fs::exists(venvRuntime)) {
                runtimePath = venvRuntime;
            }
        }

        std::string cmd = "lli -load=" + runtimePath + " " + irPath;
        int ret = system(cmd.c_str());

        if (ret != 0)
            std::cerr << "[DEBUG] Execution failed or returned non-zero." << std::endl;
    }

    std::cerr << "[DEBUG] Done!" << std::endl;
    std::cerr.rdbuf(cerr_buffer);

    return 0;
}