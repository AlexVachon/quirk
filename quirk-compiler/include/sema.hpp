// [include/sema.hpp]

#ifndef SEMA_HPP
#define SEMA_HPP

#include <string>
#include <map>
#include <vector>
#include <set>
#include <sstream>
#include "ast.hpp"

struct Symbol {
    std::string name;
    std::string type;
};

// --- NEW: Visibility Context ---
struct VisibilityContext {
    std::set<std::string> visibleModules; // e.g. "core", "math"
    std::set<std::string> visibleSymbols; // e.g. "Vector2"
};
// -------------------------------

class Sema {
   public:
    bool analyze(const std::vector<std::unique_ptr<Node>>& nodes);
    void setSourceMap(const std::map<std::string, std::string>& sm) { sourceMap = sm; }
    void setUserFile(const std::string& path) { userInputFile = path; }
    static std::string currentClass;

   private:
    std::map<std::string, std::string> sourceMap;
    std::string currentFilePath;   // set per top-level node; used by fatalError
    std::string userInputFile;     // only warn about unused vars in this file
    Node* lastNode = nullptr;      // updated as we walk — used by fatalError for location
    FunctionNode* currentFunctionNode = nullptr;

    struct ErrorRecord { std::string msg, filePath; int line, col; };
    std::vector<ErrorRecord> errors;
    std::vector<ErrorRecord> warnings;
    void flushErrors();    // print all accumulated errors
    void flushWarnings();  // print all accumulated warnings
    void reportWarning(const std::string& msg, int line = 0, int col = 0,
                       const std::string& filePath = "");

    [[noreturn]] void fatalError(const std::string& msg, int line = 0, int col = 0,
                                 const std::string& filePath = "");
    [[noreturn]] void fatalError(const std::string& msg, Node* node) {
        fatalError(msg, node ? node->line : 0, node ? node->col : 0,
                   node ? node->filePath : "");
    }
    // Non-fatal: record the error and return so analysis can continue
    void reportError(const std::string& msg, int line = 0, int col = 0,
                     const std::string& filePath = "");
    void checkInitArgCount(const std::string& name, FunctionNode* init,
                           int provided, int line, int col, const std::string& filePath);

    // --- NEW: Per-Module Visibility Map ---
    std::map<std::string, VisibilityContext> moduleVisibility;
    
    bool isVisible(const std::string& name, const std::string& symbolModule, const std::string& currentModule);
    // --------------------------------------

    std::map<std::string, Symbol> globalSymbols;
    std::map<std::string, StructNode*> structRegistry;
    std::map<std::string, EnumNode*> enumRegistry;
    std::map<std::string, InterfaceNode*> interfaceRegistry;
    std::map<std::string, std::map<std::string, FunctionNode*>> methodRegistry;
    struct VarInfo { std::string type; bool isConst = false; bool used = false; bool isParam = false; std::string filePath; };
    std::vector<std::map<std::string, VarInfo>> scopeStack;
    std::map<std::string, std::string> globalModuleAliases;
    std::map<std::string, std::string> typeAliases; // name → resolved type

    bool isCompatibleTypes(const std::string &expected, const std::string &actual);
    bool isGenericParam(const std::string &t) const;
    bool inheritsFromException(const std::string& typeName, const std::string& baseType = "Exception");
    void checkInterfaceConformance(StructNode* s, InterfaceNode* iface);

    // Active generic type params while checking a function/struct body
    std::set<std::string> activeTypeParams;
    
    // (Keep rest of the class unchanged)
    void enterScope();
    void exitScope();
    void defineVariable(const std::string& name, const std::string& type, bool isConst = false, bool isParam = false);

    std::string resolveVariable(const std::string& name);
    std::string resolveMember(const std::string& structName, const std::string& memberName);

    void checkFunction(FunctionNode* node);
    void checkVarDecl(VarDeclNode* node);
    void checkStatement(Node* node);
    void checkIf(IfNode* node);
    void checkWhile(WhileNode* node);
    void checkFor(ForNode* node);
    void checkWith(WithNode* node);
    void checkReturn(ReturnNode* node);
    
    void checkUse(UseNode* node);

    std::string checkExpression(Node* node);
    std::string checkLiteral(LiteralNode* node);
    std::string checkBinaryOp(BinaryOpNode* node);
    std::string checkMemberAccess(MemberAccessNode* node);
    std::string checkCall(CallNode* node);
    std::string checkListLiteral(ListLiteralNode* node);
    std::string checkMapLiteral(MapLiteralNode* node);
    std::string checkConstructor(ConstructorNode* node);
};

#endif