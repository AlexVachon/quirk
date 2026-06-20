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
    // For each named symbol the user explicitly imported via
    // `from X use { name }`, record the package it came from.
    // `lookupTopLevel` uses this to break collisions when two
    // packages export the same function name and the caller's
    // import list says which one they meant.
    std::map<std::string, std::string> visibleSymbolSources; // name → module
    // Per-module alias map (`from X use { y as local }` records
    // `local → y`). Sema dereferences the alias to the source name
    // when looking the symbol up so Codegen sees the canonical
    // FunctionNode / StructNode and the linkage name is preserved.
    // Scoped per-module so two different files can alias the same
    // symbol to different local names without colliding.
    std::map<std::string, std::string> importAliases; // local → source
};
// -------------------------------

class Sema {
   public:
    // Non-const so Pass 0 (monomorphization) can synthesize +
    // append specialised StructNodes that downstream passes
    // (including Codegen, which receives the same AST) then see.
    bool analyze(std::vector<std::unique_ptr<Node>>& nodes);
    void setSourceMap(const std::map<std::string, std::string>& sm) { sourceMap = sm; }
    void setUserFile(const std::string& path) { userInputFile = path; }
    static std::string currentClass;

   private:
    std::map<std::string, std::string> sourceMap;
    std::string currentFilePath;   // set per top-level node; used by fatalError
    std::string userInputFile;     // only warn about unused vars in this file
    Node* lastNode = nullptr;      // updated as we walk — used by fatalError for location
    FunctionNode* currentFunctionNode = nullptr;

    // `suggestions` carries "did you mean … ?" candidates the LSP can
    // turn into code actions. Empty on warnings; populated for the
    // undefined-name error path by `Sema::suggestNames`.
    struct ErrorRecord {
        std::string msg, filePath;
        int line, col;
        std::vector<std::string> suggestions;
    };
    std::vector<ErrorRecord> errors;
    std::vector<ErrorRecord> warnings;

    // Per-identifier usage table. Filled by resolveVariable on every
    // successful lookup, exposed (via `--symbols-json`) to the LSP for
    // semantic find-references and rename. `scope` mirrors what the
    // declaration-side records use — "module", a struct name, or an
    // enclosing function name — so consumers can match a usage to
    // the right binding without redoing resolution.
   public:
    struct UsageRecord {
        std::string name;
        std::string scope;
        std::string filePath;
        int line = 0;
        int col  = 0;
    };
    std::vector<UsageRecord> usages;
   private:
    // The function currently being checked; usages encountered while
    // walking its body get this as their `scope`. Empty == "module".
    std::string currentScope = "module";
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
    // Non-fatal: record the error and return so analysis can continue.
    // The trailing `suggestions` flavor attaches "did you mean … ?" hints
    // to the diagnostic — the LSP turns these into one CodeAction per
    // suggestion when the diagnostic surfaces in the editor.
    void reportError(const std::string& msg, int line = 0, int col = 0,
                     const std::string& filePath = "");
    void reportError(const std::string& msg,
                     const std::vector<std::string>& suggestions,
                     int line = 0, int col = 0,
                     const std::string& filePath = "");

    // Top-N closest names to `query` from every known scope (locals,
    // params, globals, registered functions, structs, enums,
    // interfaces). Edit-distance cutoff is intentionally small to
    // avoid wild "did you mean Direction" for `print`. Caller is the
    // undefined-name error path.
    std::vector<std::string> suggestNames(const std::string& query, size_t maxN = 3);
    // Top-N closest field/method names on a given struct (walks the
    // parent chain). Used by the `member 'X' not found in 'Y'` path.
    std::vector<std::string> suggestMembers(const std::string& structName,
                                            const std::string& query,
                                            size_t maxN = 3);
    // Top-N closest top-level function names in a specific module.
    // Used by the `module 'X' has no function 'Y'` path so a typo
    // like `net.gte("...")` surfaces `did you mean 'get'?`.
    std::vector<std::string> suggestModuleFunctions(const std::string& modName,
                                                    const std::string& query,
                                                    size_t maxN = 3);
    // Top-N closest variant names for an enum. Used by the
    // `'X' is not a variant of enum 'Y'` path so `Color.Reed`
    // surfaces `did you mean 'Red'?`.
    std::vector<std::string> suggestEnumVariants(const std::string& enumName,
                                                 const std::string& query,
                                                 size_t maxN = 3);
    void checkInitArgCount(const std::string& name, FunctionNode* init,
                           int provided, int line, int col, const std::string& filePath);
    // Type-check each positional arg against the corresponding init param.
    // Catches things like `User(name, age_string, gender_string)` where
    // age/gender are declared `Int`/`Gender` — without this, the mismatch
    // falls through to Codegen and surfaces as a malformed-IR crash.
    // `argTypes` is pre-resolved via checkExpression so the caller can use
    // either CallNode::Arg or ConstructorArg without templating.
    void checkInitArgTypes(const std::string& name, FunctionNode* init,
                           const std::vector<std::string>& argTypes,
                           int line, int col, const std::string& filePath);

    // --- NEW: Per-Module Visibility Map ---
    std::map<std::string, VisibilityContext> moduleVisibility;
    
    bool isVisible(const std::string& name, const std::string& symbolModule, const std::string& currentModule);
    // --------------------------------------

    std::map<std::string, Symbol> globalSymbols;
    std::map<std::string, StructNode*> structRegistry;
    std::map<std::string, EnumNode*> enumRegistry;
    std::map<std::string, InterfaceNode*> interfaceRegistry;
    std::map<std::string, std::map<std::string, FunctionNode*>> methodRegistry;
    // When two packages define the same top-level function name
    // (e.g. `console.input` and `html.input`), `methodRegistry[""][name]`
    // only stores the last one Pass 1 walked. The losing package's
    // own internal calls to its `input(...)` then route through the
    // winner's signature and trip the arity / type gate with a
    // confusing "expected List but got String" error in unrelated code.
    //
    // This side table keeps every candidate so the call-site lookup
    // can prefer one whose `moduleName` is visible from the calling
    // function's module (via isVisible). Single-candidate names stay
    // out of here — only true collisions are tracked.
    std::map<std::string, std::vector<FunctionNode*>> topLevelOverloads;
    FunctionNode* lookupTopLevel(const std::string& name);
    // Top-level `NAME := value` bindings that should be exportable across
    // modules. Sema fills this during the first walk; `from M use { NAME }`
    // accepts a name iff it's in here (or one of the other registries).
    std::map<std::string, VarDeclNode*> moduleConstRegistry;
    // Tagged-union variant sets (v2.4). Maps the union name to the
    // ordered list of variant identifiers it declares. Used by the
    // match-arm exhaustiveness check — if the scrutinee's declared
    // type is a tagged-union root and one or more variants are
    // missing from the arms (and there's no `_` wildcard), Sema
    // warns. The actual struct registration (one StructNode per
    // variant, parent=union) happens at parse time via desugaring.
    std::map<std::string, std::vector<std::string>> taggedUnionVariants;

    // v3.1.0 — per-instantiation Codegen monomorphization. Runs as
    // a Pass-0 (before structRegistry is built): walk the AST,
    // collect every `StructName[ConcreteArgs]` in a type annotation,
    // synthesize one specialized StructNode per (StructName, [Args])
    // pair with `T`-substituted field types, and rewrite the
    // annotations to the mangled name. The downstream passes see
    // only concrete structs.
    //
    // Method specialization is the harder companion piece (requires
    // deep-cloning FunctionNode bodies); deferred to a follow-up.
    // Today's slice: field-level type safety + packed layouts for
    // explicit-annotation use; method calls on the specialized
    // type fall back to the erased version via bitcast.
    std::set<std::pair<std::string, std::vector<std::string>>> monoInstantiations;
    void runMonomorphizePrePass(std::vector<std::unique_ptr<Node>>& nodes);
    void collectInstantiationsFromType(const std::string& type);
    void collectInstantiationsInNode(Node* n);
    void rewriteTypeAnnotations(Node* n);
    FunctionNode* findMethod(const std::string& cls, const std::string& name) {
        auto cit = methodRegistry.find(cls);
        if (cit == methodRegistry.end()) return nullptr;
        auto fit = cit->second.find(name);
        return fit == cit->second.end() ? nullptr : fit->second;
    }
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