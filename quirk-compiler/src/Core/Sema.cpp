#include <iostream>
#include <algorithm>
#include <functional>
#include <sstream>
#include "sema.hpp"

std::string Sema::currentClass = "";

// Strip generic type args: "List[T]" -> "List", "Map[K, V]" -> "Map", "Int" -> "Int"
static std::string baseType(const std::string& t) {
    auto pos = t.find('[');
    return pos != std::string::npos ? t.substr(0, pos) : t;
}

// Shared formatting helper — prints one error to stderr.
static void printSemaError(const std::string& msg, int line, int col,
                           const std::string& path,
                           const std::map<std::string, std::string>& sourceMap) {
    std::cerr << "\033[1;31m[ERROR]\033[0m " << msg << "\n";
    if (line > 0) {
        std::cerr << " --> ";
        if (!path.empty()) std::cerr << path << ":";
        std::cerr << line;
        if (col > 0) std::cerr << ":" << col;
        std::cerr << "\n";
        if (!path.empty() && sourceMap.count(path)) {
            const std::string& src = sourceMap.at(path);
            int cur = 1; std::string lineText;
            std::istringstream ss(src);
            while (std::getline(ss, lineText)) { if (cur++ == line) break; }
            std::string ln = std::to_string(line);
            std::cerr << std::string(ln.size(), ' ') << " |\n";
            std::cerr << ln << " | " << lineText << "\n";
            int off = (col > 1) ? col - 1 : 0;
            std::cerr << std::string(ln.size(), ' ') << " | "
                      << std::string(off, ' ')
                      << "\033[1;33m^--- here\033[0m\n\n";
        }
    }
}

[[noreturn]] void Sema::fatalError(const std::string& msg, int line, int col,
                                    const std::string& filePath) {
    if (line <= 0 && lastNode && lastNode->line > 0) {
        line = lastNode->line; col = lastNode->col;
    }
    std::string path = (!filePath.empty())                          ? filePath
                     : (lastNode && !lastNode->filePath.empty()) ? lastNode->filePath
                     : currentFilePath;
    // Flush any previously accumulated errors first, then print this one and exit
    flushErrors();
    printSemaError(msg, line, col, path, sourceMap);
    exit(1);
}

void Sema::reportError(const std::string& msg, int line, int col,
                       const std::string& filePath) {
    if (line <= 0 && lastNode && lastNode->line > 0) {
        line = lastNode->line; col = lastNode->col;
    }
    std::string path = (!filePath.empty())                          ? filePath
                     : (lastNode && !lastNode->filePath.empty()) ? lastNode->filePath
                     : currentFilePath;
    errors.push_back({msg, path, line, col});
}

void Sema::flushErrors() {
    for (auto& e : errors)
        printSemaError(e.msg, e.line, e.col, e.filePath, sourceMap);
    errors.clear();
}

void Sema::reportWarning(const std::string& msg, int line, int col,
                         const std::string& filePath) {
    if (line <= 0 && lastNode && lastNode->line > 0) {
        line = lastNode->line; col = lastNode->col;
    }
    std::string path = (!filePath.empty())                             ? filePath
                     : (lastNode && !lastNode->filePath.empty()) ? lastNode->filePath
                     : currentFilePath;
    warnings.push_back({msg, path, line, col});
}

void Sema::flushWarnings() {
    for (auto& w : warnings)
        std::cerr << "\033[1;33m[WARNING]\033[0m " << w.msg
                  << "\n --> " << w.filePath << ":" << w.line << ":" << w.col << "\n";
    warnings.clear();
}

bool Sema::analyze(const std::vector<std::unique_ptr<Node>> &nodes)
{
    if (scopeStack.empty())
        enterScope();

    // Pass 0: Register Global Module Aliases (Synchronized with Codegen)
    for (const auto &node : nodes) {
        if (auto use = dynamic_cast<UseNode*>(node.get())) {
            if (use->filterList.empty()) {
                std::string alias = use->moduleName;
                size_t lastDot = alias.rfind('.');
                if (lastDot == std::string::npos) lastDot = alias.rfind('/');
                if (lastDot != std::string::npos) alias = alias.substr(lastDot + 1);
                globalModuleAliases[alias] = "MODULE$" + use->moduleName;
            }
        }
    }

    // Register built-in Type struct (used by self.__class)
    static StructNode builtinTypeNode;
    if (builtinTypeNode.name.empty()) {
        builtinTypeNode.name = "Type";
        StructField nf; nf.name = "name";   nf.type = "String";
        StructField pf; pf.name = "parent"; pf.type = "String";
        builtinTypeNode.fields.push_back(std::move(nf));
        builtinTypeNode.fields.push_back(std::move(pf));
    }
    if (!structRegistry.count("Type")) structRegistry["Type"] = &builtinTypeNode;


    // Pass 1: Register Structs, Interfaces, and Signatures
    for (const auto &node : nodes)
    {
        if (auto s = dynamic_cast<StructNode *>(node.get()))
        {
            structRegistry[s->name] = s;
        }
        else if (auto iface = dynamic_cast<InterfaceNode*>(node.get()))
        {
            interfaceRegistry[iface->name] = iface;
        }
        else if (auto f = dynamic_cast<FunctionNode *>(node.get()))
        {
            if (f->returnType.empty())
                f->returnType = "auto";
            defineVariable(f->name, f->returnType);
            if (!f->cls.empty())
                methodRegistry[f->cls][f->name] = f;
            else
                methodRegistry[""][f->name] = f;
        }
        else if (auto e = dynamic_cast<EnumNode*>(node.get())) {
            enumRegistry[e->name] = e;
        }
        else if (auto v = dynamic_cast<VarDeclNode*>(node.get())) {
            // Top-level `NAME := value` bindings — track them so
            // `from M use { NAME }` can resolve them across modules.
            // The LHS is a LiteralNode whose value is the name.
            if (auto* lhs = dynamic_cast<LiteralNode*>(v->lhs.get())) {
                moduleConstRegistry[lhs->value] = v;
            }
        }
    }

    // Pass 2: Analyze Bodies
    enterScope();

    for (const auto &node : nodes)
    {
        std::string mod = node->moduleName;
        if (moduleVisibility.find(mod) == moduleVisibility.end())
        {
            moduleVisibility[mod].visibleModules.insert("typing");
        }

        // Validate inheritance tree and interface conformance for structs
        if (auto s = dynamic_cast<StructNode*>(node.get())) {
            std::vector<std::string> structParents;
            for (const std::string& parentName : s->parents) {
                if (interfaceRegistry.count(parentName)) {
                    // It's an interface — record conformance and check it
                    s->interfaces.push_back(parentName);
                    checkInterfaceConformance(s, interfaceRegistry[parentName]);
                } else if (!structRegistry.count(parentName)) {
                    fatalError("struct '" + s->name + "' inherits from undefined type '" + parentName + "'",
                               s->line, s->col, s->filePath);
                } else {
                    structParents.push_back(parentName);
                }
            }
            s->parents = structParents; // keep only struct parents (not interfaces)

            // Validate generic where constraints: each bound must name an interface, not a struct
            for (const auto& [typeVar, bounds] : s->genericConstraints) {
                for (const auto& bound : bounds) {
                    if (structRegistry.count(bound) && !interfaceRegistry.count(bound)) {
                        reportWarning(
                            "generic constraint '" + bound + "' is a concrete type, not an interface. "
                            "Use an interface or remove the constraint and use '" + bound + "' directly.",
                            s->line, s->col, s->filePath);
                    } else if (!interfaceRegistry.count(bound) && bound != "Any" && bound != "Primitive") {
                        reportWarning(
                            "generic constraint '" + bound + "' is not a known interface.",
                            s->line, s->col, s->filePath);
                    }
                }
            }
        }

        if (!node->filePath.empty()) currentFilePath = node->filePath;

        if (auto f = dynamic_cast<FunctionNode *>(node.get()))
        {
            checkFunction(f);
        }
        else if (auto use = dynamic_cast<UseNode *>(node.get()))
        {
            checkUse(use);
        }
        else if (dynamic_cast<InterfaceNode*>(node.get()))
        {
            // Interface declarations are compile-time only — no body to check
        }
        else if (!dynamic_cast<StructNode *>(node.get()))
        {
            checkStatement(node.get());
        }
    }
    exitScope();
    if (!warnings.empty()) flushWarnings();
    if (!errors.empty()) {
        flushErrors();
        return false;
    }
    return true;
}

void Sema::checkUse(UseNode *node)
{
    std::string sourceModule = static_cast<Node *>(node)->moduleName;
    VisibilityContext &ctx = moduleVisibility[sourceModule];

    if (!node->filterList.empty())
    {
        for (const auto &item : node->filterList)
        {
            bool found = structRegistry.count(item) || methodRegistry[""].count(item)
                      || interfaceRegistry.count(item) || enumRegistry.count(item)
                      || moduleConstRegistry.count(item);
            if (!found)
                fatalError("module '" + node->moduleName + "' does not export symbol '" + item + "'",
                           node->line, node->col, node->filePath);
            ctx.visibleSymbols.insert(item);
        }
    }
    else
    {
        std::string alias = node->alias;
        if (alias.empty()) {
            alias = node->moduleName;
            size_t lastDot = alias.rfind('.');
            if (lastDot == std::string::npos) lastDot = alias.rfind('/');
            if (lastDot != std::string::npos) alias = alias.substr(lastDot + 1);
        }

        defineVariable(alias, "MODULE$" + node->moduleName);

        std::string modName = node->moduleName;
        std::replace(modName.begin(), modName.end(), '/', '.');
        ctx.visibleModules.insert(modName);
    }
}

bool Sema::isVisible(const std::string &name, const std::string &symbolModule, const std::string &currentModule)
{
    if (currentModule != "main")
        return true;
    if (symbolModule.find("typing") == 0 || symbolModule.find("core") == 0)
        return true;
    if (symbolModule == currentModule)
        return true;

    if (moduleVisibility.count(currentModule))
    {
        const auto &ctx = moduleVisibility[currentModule];
        if (ctx.visibleModules.count(symbolModule))
            return true;
        if (ctx.visibleSymbols.count(name))
            return true;
    }
    return false;
}

void Sema::checkInterfaceConformance(StructNode* s, InterfaceNode* iface) {
    for (const auto& method : iface->methods) {
        // method->name is already mangled as "InterfaceName_methodName"
        // For the struct, the method would be "StructName_methodName"
        std::string rawName = method->name;
        // Strip the interface prefix to get the raw method name
        if (rawName.size() > iface->name.size() + 1)
            rawName = rawName.substr(iface->name.size() + 1);
        std::string structMethodName = s->name + "_" + rawName;

        bool found = methodRegistry.count(s->name) &&
                     methodRegistry[s->name].count(structMethodName);
        if (!found) {
            reportError("struct '" + s->name + "' does not implement interface method '" +
                        rawName + "' (required by '" + iface->name + "')",
                        s->line, s->col, s->filePath);
        }
    }
    // Also check inherited interface methods
    for (const auto& ext : iface->extends) {
        if (interfaceRegistry.count(ext))
            checkInterfaceConformance(s, interfaceRegistry[ext]);
    }
}

void Sema::checkFunction(FunctionNode *f)
{
    // Abstract interface method signatures have no body to check
    if (f->isAbstract) return;

    std::string prevClass = currentClass;
    FunctionNode *prevFunc = currentFunctionNode;

    currentClass = f->cls;
    currentFunctionNode = f;
    if (!f->filePath.empty()) currentFilePath = f->filePath;

    // Push generic type params as type aliases for "Any" so that annotations
    // like `item: T` and `-> T` are accepted without "unknown type" errors.
    std::vector<std::string> pushedParams;
    auto pushTypeParam = [&](const std::string& tp) {
        if (!typeAliases.count(tp)) {
            typeAliases[tp] = "Any";
            pushedParams.push_back(tp);
        }
    };
    for (const auto& tp : f->typeParams) pushTypeParam(tp);
    // Also push the enclosing struct's type params (e.g. T in struct Stack[T])
    if (!f->cls.empty() && structRegistry.count(f->cls))
        for (const auto& tp : structRegistry[f->cls]->typeParams) pushTypeParam(tp);

    // Validate generic where constraints: each bound must name an interface, not a struct
    for (const auto& [typeVar, bounds] : f->genericConstraints) {
        for (const auto& bound : bounds) {
            if (structRegistry.count(bound) && !interfaceRegistry.count(bound)) {
                reportWarning(
                    "generic constraint '" + bound + "' is a concrete type, not an interface. "
                    "Use an interface or remove the constraint and use '" + bound + "' directly.",
                    f->line, f->col, f->filePath);
            } else if (!interfaceRegistry.count(bound) && bound != "Any" && bound != "Primitive") {
                reportWarning(
                    "generic constraint '" + bound + "' is not a known interface.",
                    f->line, f->col, f->filePath);
            }
        }
    }

    enterScope();

    if (!f->cls.empty() && !f->isStatic)
        defineVariable("self", f->cls, false, true);
    for (const auto &param : f->parameters)
        defineVariable(param.name, param.type.empty() ? "Any" : param.type, false, true);

    if (f->whereClause)
        checkExpression(f->whereClause.get());

    if (!f->isExtern)
    {
        for (const auto &statement : f->body)
        {
            checkStatement(statement.get());
        }
    }

    if (f->returnType == "auto")
    {
        // No return statement found — default to void
        f->returnType = "void";
        if (auto* m = findMethod(f->cls, f->name)) m->returnType = "void";
    }
    else if (!f->cls.empty() && f->returnType != "void")
    {
        // Sync inferred return type back to registry (handles duplicate parsing)
        if (auto* m = findMethod(f->cls, f->name)) m->returnType = f->returnType;
    }

    exitScope();

    // Remove generic type param aliases we pushed
    for (const auto& tp : pushedParams)
        typeAliases.erase(tp);

    currentClass = prevClass;
    currentFunctionNode = prevFunc;
}

void Sema::checkVarDecl(VarDeclNode *node)
{
    lastNode = node;

    // Const mutation check: direct rebind AND index/member mutation on a const binding.
    if (node->op == "=" || node->op == "+=" || node->op == "-=" ||
        node->op == "*=" || node->op == "/=") {
        // Extract the root variable name from whatever LHS shape we have:
        //   m = ...          → LiteralNode("m")
        //   m["k"] = ...     → BinaryOpNode("[]", LiteralNode("m"), ...)
        //   m.field = ...    → MemberAccessNode(LiteralNode("m"), ...)
        std::string rootVar;
        if (auto lit = dynamic_cast<LiteralNode *>(node->lhs.get())) {
            rootVar = lit->value;
        } else if (auto bin = dynamic_cast<BinaryOpNode *>(node->lhs.get())) {
            if (bin->op == "[]")
                if (auto lit = dynamic_cast<LiteralNode *>(bin->left.get()))
                    rootVar = lit->value;
        } else if (auto mem = dynamic_cast<MemberAccessNode *>(node->lhs.get())) {
            if (auto lit = dynamic_cast<LiteralNode *>(mem->object.get()))
                rootVar = lit->value;
        }
        if (!rootVar.empty()) {
            for (int i = (int)scopeStack.size() - 1; i >= 0; i--) {
                if (scopeStack[i].count(rootVar)) {
                    if (scopeStack[i][rootVar].isConst)
                        fatalError("cannot mutate const variable '" + rootVar + "'",
                                   node->line, node->col, node->filePath);
                    break;
                }
            }
        }
    }

    std::string exprType = checkExpression(node->expression.get());

    // A node is a declaration if it uses ':=' OR if it has a type annotation (e.g. `x: Type = val`)
    bool isDecl = (node->op == ":=") || !node->typeAnnotation.empty();

    if (auto tup = dynamic_cast<TupleLiteralNode *>(node->lhs.get()))
    {
        // Tuple destructuring: (a, b) := tuple_expr
        for (auto& elem : tup->elements) {
            if (auto* nameNode = dynamic_cast<LiteralNode*>(elem.get()))
                defineVariable(nameNode->value, "Any");
        }
    }
    else if (auto lit = dynamic_cast<LiteralNode *>(node->lhs.get()))
    {
        // Check if variable already exists in any scope
        bool alreadyDefined = false;
        for (int i = (int)scopeStack.size() - 1; i >= 0; i--) {
            if (scopeStack[i].count(lit->value)) { alreadyDefined = true; break; }
        }

        if (isDecl || !alreadyDefined) {
            std::string finalType =
                node->typeAnnotation.empty() ? exprType : node->typeAnnotation;
            defineVariable(lit->value, finalType, node->isConst);
        } else {
            // Reassignment — mark the existing binding as used
            resolveVariable(lit->value);
        }
    }
    else if (auto member = dynamic_cast<MemberAccessNode *>(node->lhs.get()))
    {
        std::string objType = checkExpression(member->object.get());
        if (resolveMember(objType, member->memberName) == "unknown")
            fatalError("member '" + member->memberName + "' not found in '" + objType + "'",
                       member->line, member->col, member->filePath);
    }
}

void Sema::checkStatement(Node *node)
{
    if (auto v = dynamic_cast<VarDeclNode *>(node))
        checkVarDecl(v);
    else if (auto u = dynamic_cast<UseNode *>(node))
        checkUse(u);
    else if (auto i = dynamic_cast<IfNode *>(node))
        checkIf(i);
    else if (auto wi = dynamic_cast<WithNode *>(node))
        checkWith(wi);
    else if (auto w = dynamic_cast<WhileNode *>(node))
        checkWhile(w);
    else if (auto f = dynamic_cast<ForNode *>(node))
        checkFor(f);
    else if (auto c = dynamic_cast<CallNode *>(node))
        checkExpression(c);
    else if (auto t = dynamic_cast<TryCatchNode*>(node)) {
        enterScope();
        for (auto& s : t->tryBlock) checkStatement(s.get());
        exitScope();

        for (size_t i = 0; i < t->catchBlocks.size(); ++i) {
            auto& cb = t->catchBlocks[i];
            enterScope();
            for (const std::string& typeName : cb.types) {
                if (!structRegistry.count(typeName))
                    fatalError("catch type '" + typeName + "' is not defined",
                               t->line, t->col, t->filePath);
                if (structRegistry.count("Exception") && !inheritsFromException(typeName))
                    fatalError("catch type '" + typeName + "' does not inherit from 'Exception'",
                               t->line, t->col, t->filePath);
                for (size_t j = 0; j < i; ++j) {
                    for (const std::string& prevType : t->catchBlocks[j].types) {
                        if (inheritsFromException(typeName, prevType)) {
                            std::cerr << "\033[1;33m[WARNING]\033[0m catch (" << cb.varName
                                      << ": " << typeName << ") is unreachable — '"
                                      << prevType << "' in block " << (j + 1)
                                      << " already handles it\n";
                        }
                    }
                }
            }
            if (!cb.varName.empty()) defineVariable(cb.varName, cb.types[0]);
            for (auto& s : cb.body) checkStatement(s.get());
            exitScope();
        }

        for (auto& s : t->finallyBlock) checkStatement(s.get());
    }
    else if (auto ta = dynamic_cast<TypeAliasNode*>(node)) {
        typeAliases[ta->name] = ta->target;
        defineVariable(ta->name, "Type");
    }
    else if (auto m = dynamic_cast<MatchNode*>(node)) {
        checkExpression(m->scrutinee.get());
        for (auto& arm : m->arms) {
            if (arm.isTypeMatch) {
                enterScope();
                if (!arm.bindName.empty()) defineVariable(arm.bindName, arm.typeNames[0], false, true);
                if (arm.guard) checkExpression(arm.guard.get());
                for (auto& s : arm.body) checkStatement(s.get());
                exitScope();
            } else {
                for (auto& pat : arm.patterns) checkExpression(pat.get());
                enterScope();
                // Wildcard-with-bind (`case x`) makes `x` visible in both
                // the guard and the body. Same pattern as `as x` above.
                if (arm.isWildcard && !arm.bindName.empty()) {
                    defineVariable(arm.bindName, "Any", false, true);
                }
                // Tuple destructure (`case (a, b)`): each name is bound to
                // the corresponding tuple slot at codegen time.
                for (auto& n : arm.bindNames) {
                    defineVariable(n, "Any", false, true);
                }
                if (arm.guard) checkExpression(arm.guard.get());
                for (auto& s : arm.body) checkStatement(s.get());
                exitScope();
            }
        }
    }
    else if (auto th = dynamic_cast<ThrowNode*>(node)) {
        if (th->expression) {
            std::string type = checkExpression(th->expression.get());
            if (!structRegistry.count(type))
                fatalError("can only throw struct objects, got '" + type + "'",
                           th->line, 0, th->filePath);
        }
        if (th->cause) checkExpression(th->cause.get());
        // bare throw (nullptr expression): re-raises current exception — no type check needed
    }
    else if (auto r = dynamic_cast<ReturnNode *>(node))
        checkReturn(r);
    else if (auto nl = dynamic_cast<NonlocalNode*>(node)) {
        // Mark nonlocal vars as known so references inside closures type-check cleanly.
        for (const auto& v : nl->vars)
            defineVariable(v, "Any", false, true);
    }
    else if (dynamic_cast<BreakNode*>(node)) { /* no-op */ }
    else if (dynamic_cast<ContinueNode*>(node)) { /* no-op */ }
}

// Walk an if-condition collecting (varName, typeName) pairs from `x is T`
// checks joined by `and`. Pairs are applied as shadowed bindings in the
// then-branch scope so member access / method lookup on `x` inside the
// block resolves against `T`. We skip `or` joins (either side could hold),
// and skip negations.
static void collectNarrowings(Node* cond, std::vector<std::pair<std::string,std::string>>& out) {
    if (!cond) return;
    if (auto* bin = dynamic_cast<BinaryOpNode*>(cond)) {
        if (bin->op == "and") {
            collectNarrowings(bin->left.get(), out);
            collectNarrowings(bin->right.get(), out);
            return;
        }
        if (bin->op == "is") {
            auto* lhs = dynamic_cast<LiteralNode*>(bin->left.get());
            auto* rhs = dynamic_cast<LiteralNode*>(bin->right.get());
            if (lhs && rhs && !lhs->value.empty() && !rhs->value.empty()) {
                char c0 = lhs->value[0];
                // LHS must look like an identifier, not a literal value.
                if (!std::isdigit(static_cast<unsigned char>(c0)) && c0 != '"' && c0 != '\''
                    && lhs->value != "true" && lhs->value != "false" && lhs->value != "null") {
                    out.emplace_back(lhs->value, rhs->value);
                }
            }
        }
    }
}

void Sema::checkIf(IfNode *node)
{
    lastNode = node;
    std::string condType = checkExpression(node->condition.get());
    if (condType != "Bool" && condType != "unknown" && condType != "Any")
        reportError("'if' condition must be 'Bool', got '" + condType + "'",
                    node->line, node->col, node->filePath);
    enterScope();
    {
        std::vector<std::pair<std::string,std::string>> narrowings;
        collectNarrowings(node->condition.get(), narrowings);
        for (auto& [var, type] : narrowings) defineVariable(var, type, false, false);
    }
    for (auto &s : node->thenBranch)
        checkStatement(s.get());
    exitScope();
    for (auto &b : node->elIfBranches)
    {
        std::string elifType = checkExpression(b.condition.get());
        if (elifType != "Bool" && elifType != "unknown")
            reportError("'elif' condition must be 'Bool'", node->line, node->col, node->filePath);
        enterScope();
        {
            std::vector<std::pair<std::string,std::string>> narrowings;
            collectNarrowings(b.condition.get(), narrowings);
            for (auto& [var, type] : narrowings) defineVariable(var, type, false, false);
        }
        for (auto &s : b.body)
            checkStatement(s.get());
        exitScope();
    }
    if (!node->elseBranch.empty())
    {
        enterScope();
        for (auto &s : node->elseBranch)
            checkStatement(s.get());
        exitScope();
    }
}

void Sema::checkWhile(WhileNode *node)
{
    lastNode = node;
    std::string wCondType = checkExpression(node->condition.get());
    if (wCondType != "Bool" && wCondType != "unknown" && wCondType != "Any")
        reportError("'while' condition must be 'Bool'", node->line, node->col, node->filePath);
    enterScope();
    for (auto &s : node->body)
        checkStatement(s.get());
    exitScope();
}

void Sema::checkFor(ForNode *node)
{
    std::string iterType = checkExpression(node->iterable.get());
    std::string itemType = "Any";

    if (iterType == "String")
        itemType = "String";    // iterating a String yields length-1 Strings
    else if (iterType == "File")
        itemType = "String";
    else if (structRegistry.count(iterType))
    {
        if (auto* iterFn = findMethod(iterType, iterType + "___iter")) {
            const std::string& iterStruct = iterFn->returnType;
            if (auto* nextFn = findMethod(iterStruct, iterStruct + "___next"))
                itemType = nextFn->returnType;
        }
    }

    enterScope();
    defineVariable(node->varName, itemType);
    if (!node->varName2.empty()) defineVariable(node->varName2, "Any", false, true);
    for (const auto& dv : node->destructureVars)
        defineVariable(dv, "Any", false, true);
    for (auto &s : node->body)
        checkStatement(s.get());
    exitScope();
}

std::string Sema::checkExpression(Node *node)
{
    if (auto lit = dynamic_cast<LiteralNode *>(node))
        return checkLiteral(lit);
    if (auto binOp = dynamic_cast<BinaryOpNode *>(node))
        return checkBinaryOp(binOp);
    if (auto c = dynamic_cast<ConstructorNode *>(node))
        return checkConstructor(c);
    if (auto m = dynamic_cast<MemberAccessNode *>(node))
        return checkMemberAccess(m);
    if (auto c = dynamic_cast<CallNode *>(node))
        return checkCall(c);
    if (auto arr = dynamic_cast<ListLiteralNode *>(node))
        return checkListLiteral(arr);
    if (auto map = dynamic_cast<MapLiteralNode *>(node))
        return checkMapLiteral(map);
    if (auto s = dynamic_cast<SetLiteralNode *>(node)) {
        for (auto& e : s->elements) checkExpression(e.get());
        return "Set";
    }
    if (auto comp = dynamic_cast<ListComprehensionNode *>(node)) {
        checkExpression(comp->iterable.get());
        enterScope();
        defineVariable(comp->varName, "Any", false, true);
        if (!comp->varName2.empty()) defineVariable(comp->varName2, "Any", false, true);
        if (comp->condition) checkExpression(comp->condition.get());
        checkExpression(comp->expr.get());
        exitScope();
        return "List";
    }
    if (auto comp = dynamic_cast<MapComprehensionNode *>(node)) {
        checkExpression(comp->iterable.get());
        enterScope();
        defineVariable(comp->varName, "Any", false, true);
        if (!comp->varName2.empty()) defineVariable(comp->varName2, "Any", false, true);
        if (comp->condition) checkExpression(comp->condition.get());
        checkExpression(comp->keyExpr.get());
        checkExpression(comp->valExpr.get());
        exitScope();
        return "Map";
    }
    if (auto tup = dynamic_cast<TupleLiteralNode *>(node)) {
        for (auto& elem : tup->elements) checkExpression(elem.get());
        return "Tuple";
    }
    if (auto lambda = dynamic_cast<LambdaNode *>(node)) {
        FunctionNode* savedFn = currentFunctionNode;
        FunctionNode lambdaStub;
        lambdaStub.name = "<lambda>";
        lambdaStub.returnType = "auto";
        currentFunctionNode = &lambdaStub;
        enterScope();
        for (const auto& p : lambda->params) {
            std::string t = p.isVariadic ? "List" : (p.type.empty() ? "Any" : p.type);
            defineVariable(p.name, t, false, true);
        }
        if (lambda->isExpression && lambda->exprBody) {
            lambda->inferredReturnType = checkExpression(lambda->exprBody.get());
        } else {
            for (auto& s : lambda->stmtBody) checkStatement(s.get());
            if (!lambdaStub.returnType.empty() && lambdaStub.returnType != "auto" && lambdaStub.returnType != "void")
                lambda->inferredReturnType = lambdaStub.returnType;
        }
        exitScope();
        currentFunctionNode = savedFn;
        return "Callable";
    }
    if (auto tern = dynamic_cast<TernaryNode*>(node)) {
        checkExpression(tern->condition.get());
        std::string thenType = checkExpression(tern->thenExpr.get());
        std::string elseType = checkExpression(tern->elseExpr.get());
        if (thenType == elseType) return thenType;
        // Strip optional markers for comparison
        auto strip = [](const std::string& s) {
            return (!s.empty() && s.back() == '?') ? s.substr(0, s.size() - 1) : s;
        };
        if (strip(thenType) == strip(elseType)) return strip(thenType);
        return thenType;
    }
    if (auto sl = dynamic_cast<SliceNode*>(node)) {
        lastNode = sl;
        std::string objType = checkExpression(sl->object.get());
        if (sl->start) checkExpression(sl->start.get());
        if (sl->end)   checkExpression(sl->end.get());
        if (objType == "String") return "String";
        if (objType == "List")   return "List";
        fatalError("slice '[:]' not supported on type '" + objType + "'",
                   sl->line, sl->col, sl->filePath);
    }
    return "unknown";
}

std::string Sema::checkLiteral(LiteralNode *node)
{
    lastNode = node;
    if (std::isdigit(node->value[0]))
        return (node->value.find('.') != std::string::npos) ? "Double" : "Int";
    if (node->value[0] == '"' || node->value[0] == '\'')
        return "String";        // 'x' and "x" both produce length-1 strings
    if (node->value == "true" || node->value == "false")
        return "Bool";
    if (node->value == "null")
        return "Null";
    return resolveVariable(node->value);
}

std::string Sema::checkBinaryOp(BinaryOpNode *node)
{
    lastNode = node;
    if (node->op == "not")
    {
        std::string t = checkExpression(node->left.get());
        if (t != "Bool" && t != "Any" && t != "Int")
            fatalError("'not' operand must be 'Bool', got '" + t + "'",
                       node->line, node->col, node->filePath);
        return "Bool";
    }

    if (node->op == "?")
    {
        checkExpression(node->left.get());
        return "Bool";
    }

    if (node->op == "is")
    {
        checkExpression(node->left.get());
        return "Bool";
    }

    if (node->op == "as")
    {
        checkExpression(node->left.get());
        // RHS is always a LiteralNode holding the target type name
        if (auto lit = dynamic_cast<LiteralNode*>(node->right.get()))
            return lit->value;
        return "unknown";
    }

    if (node->op == "in" || node->op == "not in")
    {
        checkExpression(node->left.get());
        checkExpression(node->right.get());
        return "Bool";
    }

    std::string lType = checkExpression(node->left.get());
    std::string rType = checkExpression(node->right.get());

    if (node->op == "and" || node->op == "or")
        return "Bool";

    // Null-coalesce: result is the non-optional type of the LHS
    if (node->op == "??") {
        // Strip trailing ? from lType to get the base type
        std::string base = lType;
        if (!base.empty() && base.back() == '?') base.pop_back();
        return base.empty() ? rType : base;
    }

    // Array Access
    if (node->op == "[]")
    {
        lType = baseType(lType); // strip generic args: "List[T]" -> "List"
        if (structRegistry.count(lType))
        {
            std::string funcName = lType + "___get";
            if (methodRegistry[lType].count(funcName))
            {
                FunctionNode *func = methodRegistry[lType][funcName];
                if (!func->parameters.empty())
                {
                    std::string expectedKeyType = func->parameters[0].type;
                    bool validKey = (rType == expectedKeyType);
                    if (!validKey)
                        fatalError("type mismatch for '" + lType + "[]': expected '" +
                                   expectedKeyType + "' index, got '" + rType + "'",
                                   node->line, node->col, node->filePath);
                }
                return func->returnType;
            }
        }
        if (rType != "Int")
            fatalError("array index must be 'Int', got '" + rType + "'",
                       node->line, node->col, node->filePath);
        if (lType == "Any" || lType == "String")
            return (lType == "String") ? "String" : "Any";
        fatalError("type '" + lType + "' does not support indexing with '[]'",
                   node->line, node->col, node->filePath);
    }

    // Operator Overloading
    if (structRegistry.count(lType))
    {
        std::string magic;
        if (node->op == "+")       magic = "__add";
        else if (node->op == "-")  magic = "__sub";
        else if (node->op == "*")  magic = "__mul";
        else if (node->op == "/")  magic = "__div";
        else if (node->op == "%")  magic = "__mod";
        else if (node->op == "==") magic = "__eq";
        else if (node->op == "!=") magic = "__ne";
        else if (node->op == "<")  magic = "__lt";
        else if (node->op == "<=") magic = "__le";
        else if (node->op == ">")  magic = "__gt";
        else if (node->op == ">=") magic = "__ge";

        if (!magic.empty())
        {
            FunctionNode* fn = findMethod(lType, lType + "_" + magic);
            // For !=, fall back to __eq if __ne is not defined
            if (!fn && node->op == "!=") fn = findMethod(lType, lType + "___eq");
            if (fn) {
                static const std::set<std::string> boolOps = {"==","!=","<","<=",">",">="};
                if (boolOps.count(node->op)) return "Bool";
                return fn->returnType;
            }
        }
    }

    // Primitives
    if (node->op == "+")
    {
        if (lType == "String" || rType == "String")
            return "String";
        if (lType == "Double" || rType == "Double")
            return "Double";
        return "Int";
    }
    if (node->op == "-" || node->op == "*" || node->op == "/" || node->op == "%")
    {
        if (lType == "Double" || rType == "Double")
            return "Double";
        return "Int";
    }
    // Allow comparison between same enum types
    if (node->op == "==" || node->op == "!=") {
        if (enumRegistry.count(lType) && lType == rType) return "Bool";
        if (enumRegistry.count(lType) || enumRegistry.count(rType)) return "Bool";
    }

    if (node->op == ">" || node->op == "<" || node->op == ">=" ||
        node->op == "<=" || node->op == "==" || node->op == "!=")
    {
        return "Bool";
    }
    fatalError("unsupported operator '" + node->op + "' on types '" + lType + "' and '" + rType + "'",
               node->line, node->col, node->filePath);
}

std::string Sema::checkMemberAccess(MemberAccessNode *node)
{
    lastNode = node;
    // Enum variant access: Direction.North
    if (auto lit = dynamic_cast<LiteralNode*>(node->object.get())) {
        if (enumRegistry.count(lit->value)) {
            EnumNode* en = enumRegistry[lit->value];
            if (node->memberName == "str" || node->memberName == "name") return "String";
            auto it = std::find(en->variants.begin(), en->variants.end(), node->memberName);
            if (it == en->variants.end())
                fatalError("'" + node->memberName + "' is not a variant of enum '" + lit->value + "'",
                           node->line, node->col, node->filePath);
            return en->name;
        }
    }

    std::string objType = checkExpression(node->object.get());
    // Strip Optional marker and generic type args before method lookup
    if (!objType.empty() && objType.back() == '?') objType.pop_back();
    objType = baseType(objType);

    // Magic attributes
    if (node->memberName == "__name")   return "String";
    if (node->memberName == "__parent") return "String";
    if (node->memberName == "__class")  return "Type";

    if (objType.rfind("MODULE$", 0) == 0)
    {
        std::string modName = objType.substr(7);
        const std::string& funcName = node->memberName;
        if (auto* fn = findMethod("", funcName))
            return fn->returnType;
        fatalError("module '" + modName + "' has no function '" + funcName + "'",
                   node->line, node->col, node->filePath);
    }

    std::string type = resolveMember(objType, node->memberName);
    if (type == "unknown")
        fatalError("'" + objType + "' has no member '" + node->memberName + "'",
                   node->line, node->col, node->filePath);
    if (type == "method") {
        if (auto* fn = findMethod(objType, objType + "_" + node->memberName)) {
            const std::string& ret = fn->returnType;
            if (ret.empty() || ret == "auto") return "Any";
            return ret;
        }
        if (structRegistry.count(objType)) {
            for (const auto& par : structRegistry[objType]->parents) {
                std::string pf = par + "_" + node->memberName;
                if (methodRegistry[par].count(pf)) {
                    std::string ret = methodRegistry[par][pf]->returnType;
                    if (ret.empty() || ret == "auto") return "Any";
                    return ret;
                }
            }
        }
        return "Any";
    }
    return type;
}

void Sema::checkInitArgCount(const std::string& name, FunctionNode* init,
                              int provided, int line, int col, const std::string& filePath) {
    int required = 0, total = 0;
    bool hasVariadic = false;
    for (const auto& p : init->parameters) {
        if (p.isVariadic) { hasVariadic = true; break; }
        total++;
        if (!p.defaultValue) required++;
    }
    if (hasVariadic || (provided >= required && provided <= total)) return;

    std::string expect = (required == total)
        ? std::to_string(required) + " argument" + (required == 1 ? "" : "s")
        : "between " + std::to_string(required) + " and " + std::to_string(total) + " arguments";
    fatalError(name + "() takes " + expect +
               " but " + std::to_string(provided) + " " + (provided == 1 ? "was" : "were") + " given",
               line, col, filePath);
}

std::string Sema::checkConstructor(ConstructorNode *node)
{
    lastNode = node;
    if (!structRegistry.count(node->structName)) return "unknown";

    std::string initName = node->structName + "__init";
    if (methodRegistry.count(node->structName) &&
        methodRegistry[node->structName].count(initName)) {
        checkInitArgCount(node->structName, methodRegistry[node->structName][initName],
                          (int)node->args.size(), node->line, node->col, node->filePath);
    }

    return node->structName;
}

std::string Sema::checkCall(CallNode *node)
{
    lastNode = node;
    if (auto l = dynamic_cast<LiteralNode *>(node->callee.get())) {
        if (l->value == "super") {
            if (currentClass.empty())
                fatalError("'super' used outside of a struct method", node->line, node->col, node->filePath);
            if (structRegistry[currentClass]->parents.empty())
                fatalError("'" + currentClass + "' has no parent — cannot use 'super'",
                           node->line, node->col, node->filePath);
            return structRegistry[currentClass]->parents[0];
        }

        // Always check all argument expressions so variables inside them are marked used
        for (auto& a : node->args) checkExpression(a.value.get());

        // Positional constructor call: Foo(...) where Foo is a known struct.
        // Validate argument count against __init (self already stripped by parser).
        if (structRegistry.count(l->value)) {
            std::string initName = l->value + "__init";
            if (methodRegistry.count(l->value) &&
                methodRegistry[l->value].count(initName)) {
                checkInitArgCount(l->value, methodRegistry[l->value][initName],
                                  (int)node->args.size(), node->line, node->col, node->filePath);
            }
            return l->value;
        }

        {
            std::string vtype = resolveVariable(l->value);

            // Hard error: bare call into a module that was imported with `use X`
            // but NOT explicitly named via `from X use { name }`. Forbids:
            //     use slug; slug(base)          // ← error
            // while still allowing the two valid forms:
            //     use slug; slug.slug(base)     // dotted access
            //     from slug use { slug }; slug(base)
            if (vtype.rfind("MODULE$", 0) == 0) {
                const std::string& ctxMod = currentFunctionNode ? currentFunctionNode->moduleName : "main";
                // Single map lookup instead of count+[] (each was O(log n)).
                bool explicitlyImported = false;
                auto mvIt = moduleVisibility.find(ctxMod);
                if (mvIt != moduleVisibility.end()) {
                    explicitlyImported = mvIt->second.visibleSymbols.count(l->value) > 0;
                }
                if (!explicitlyImported) {
                    std::string mod = vtype.substr(7);
                    std::replace(mod.begin(), mod.end(), '/', '.');
                    fatalError(
                        "cannot call module '" + mod + "' directly. Use '" + mod + "." + l->value
                        + "(...)' or import the function with 'from " + mod + " use { " + l->value + " }'",
                        node->line, node->col, node->filePath);
                }
                // Explicitly imported AND module-aliased — prefer the function.
                auto fnIt = methodRegistry[""].find(l->value);
                if (fnIt != methodRegistry[""].end()) {
                    return fnIt->second->returnType.empty() ? "void" : fnIt->second->returnType;
                }
            }

            // Calling a Callable variable or a generic-param value — return Any
            if (vtype == "Callable" || isGenericParam(vtype)) return "Any";
            return vtype;
        }
    }

    if (auto m = dynamic_cast<MemberAccessNode *>(node->callee.get()))
    {
        std::string objType = checkExpression(m->object.get());
        // Strip Optional marker and generic type args before method lookup
        if (!objType.empty() && objType.back() == '?') objType.pop_back();
        objType = baseType(objType);

        // --- THE FIX: Handle Module Constructor Calls (e.g. io.File) ---
        if (objType.rfind("MODULE$", 0) == 0) {
            for (auto& a : node->args) checkExpression(a.value.get());
            std::string funcName = m->memberName;

            // 1. Is it a Struct Constructor?
            if (structRegistry.count(funcName)) {
                return funcName;
            }

            // 2. Is it a standard Module Function?
            if (methodRegistry[""].count(funcName)) {
                return methodRegistry[""][funcName]->returnType;
            }

            return "void";
        }
        // ---------------------------------------------------------------

        if (objType == "int") objType = "Int";
        else if (objType == "double") objType = "Double";
        else if (objType == "bool") objType = "Bool";
        else if (objType == "cstring" || objType == "string" || objType == "char") objType = "String";

        // Builtin method return types for core primitives.
        // These are defined in Quirk's core library files which may not always
        // be loaded, so we hard-code their return types here as a fallback to
        // prevent false type errors (e.g. String.to_int() resolving as 'void').
        static const std::map<std::string, std::map<std::string, std::string>> builtinMethods = {
            {"String", {
                {"to_int",     "Int"},
                {"to_float",   "Double"},
                {"to_double",  "Double"},
                {"to_bool",    "Bool"},
                {"lower",      "String"},
                {"upper",      "String"},
                {"is_alpha",   "Bool"},
                {"is_digit",   "Bool"},
                {"is_space",   "Bool"},
                {"is_upper",   "Bool"},
                {"is_lower",   "Bool"},
                {"trim",       "String"},
                {"strip",      "String"},
                {"split",      "List"},
                {"join",       "String"},
                {"find",       "Int"},
                {"contains",   "Bool"},
                {"startswith", "Bool"},
                {"endswith",   "Bool"},
                {"replace",    "String"},
                {"substring",  "String"},
                {"str",        "String"},
                {"format",     "String"},
            }},
            {"Int", {
                {"str",        "String"},
                {"to_float",   "Double"},
                {"to_double",  "Double"},
                {"parse",      "Int"},
            }},
            {"Double", {
                {"str",        "String"},
                {"to_int",     "Int"},
                {"parse",      "Double"},
            }},
            {"Bool", {
                {"str",        "String"},
                {"parse",      "Bool"},
            }},
            {"List", {
                {"get",        "Any"},
                {"length",     "Int"},
                {"append",     "void"},
                {"pop",        "Any"},
                {"join",       "String"},
            }},
            {"Map", {
                {"get",        "Any"},
                {"put",        "void"},
                {"contains",   "Bool"},
                {"length",     "Int"},
            }},
        };
        auto bmIt = builtinMethods.find(objType);
        if (bmIt != builtinMethods.end()) {
            auto mIt = bmIt->second.find(m->memberName);
            if (mIt != bmIt->second.end()) {
                for (auto& a : node->args) checkExpression(a.value.get());
                return mIt->second;
            }
        }

        if (structRegistry.count(objType))
        {
            auto searchMethod = [&](const std::string& currentType, auto& self) -> std::string {
                auto sIt = structRegistry.find(currentType);
                if (sIt == structRegistry.end()) return "";

                std::string funcName = currentType + "_" + m->memberName;
                if (FunctionNode* func = findMethod(currentType, funcName)) {
                    for (size_t i = 0; i < node->args.size() && i < func->parameters.size(); ++i) {
                        if (func->parameters[i].isVariadic) break;
                        std::string argType = checkExpression(node->args[i].value.get());
                        const std::string& paramType = func->parameters[i].type;
                        if (!isCompatibleTypes(paramType, argType))
                            fatalError("argument " + std::to_string(i + 1) + " of '" + funcName +
                                       "' expected '" + paramType + "' but got '" + argType + "'",
                                       node->line, node->col, node->filePath);
                    }
                    const std::string& ret = func->returnType;
                    return (ret.empty() || ret == "auto") ? std::string("Any") : ret;
                }

                for (const std::string& parent : sIt->second->parents) {
                    std::string res = self(parent, self);
                    if (!res.empty()) return res;
                }
                return "";
            };

            std::string retType = searchMethod(objType, searchMethod);
            if (!retType.empty()) return retType;
        }
    }
    // Last resort: the callee is some other expression form, e.g. a chained
    // call like `dec(foo)(arg)` produced by decorator desugaring. Type-check
    // the callee; if it yields a Callable, treat the call's return type as
    // `Any` (same policy as calling a Callable variable above). Restricted
    // to callees we haven't already classified to avoid re-entering the
    // MemberAccess/Literal paths and changing existing error reporting.
    if (!dynamic_cast<LiteralNode*>(node->callee.get())
     && !dynamic_cast<MemberAccessNode*>(node->callee.get())) {
        for (auto& a : node->args) checkExpression(a.value.get());
        std::string calleeTy = checkExpression(node->callee.get());
        // Calling something whose type is `Callable` or `Any` produces an
        // unknown (Any) value. Calling something of any other concrete type
        // is a type error elsewhere; here we just bail out as void.
        if (calleeTy == "Callable" || calleeTy == "Any") return "Any";
    }
    return "void";
}

std::string Sema::checkListLiteral(ListLiteralNode *node)
{
    lastNode = node;
    if (!structRegistry.count("List"))
        fatalError("'List' type not available — is core loaded?", node->line, node->col, node->filePath);
    for (auto &elem : node->elements)
        checkExpression(elem.get());
    return "List";
}

std::string Sema::checkMapLiteral(MapLiteralNode *node)
{
    lastNode = node;
    if (!structRegistry.count("Map"))
        fatalError("'Map' type not available — is core loaded?", node->line, node->col, node->filePath);
    for (auto &pair : node->elements)
    {
        std::string keyType = checkExpression(pair.first.get());
        if (keyType != "String")
            fatalError("map keys must be 'String', got '" + keyType + "'",
                       node->line, node->col, node->filePath);
        checkExpression(pair.second.get());
    }
    return "Map";
}


// Primitive/builtin type names that are always "known"
static bool isKnownType(const std::string& t) {
    static const std::set<std::string> known = {
        "Int","Double","Bool","Char","String","Any","void","List","Map","Tuple",
        "Set","Queue","File","Callable","auto","unknown",
        "int","double","bool","char","string","cstring",
        "Null","null"
    };
    return known.count(t) > 0;
}

bool Sema::isGenericParam(const std::string& t) const {
    if (typeAliases.count(t) && typeAliases.at(t) == "Any") return true;
    return !isKnownType(t) && !structRegistry.count(t) && !enumRegistry.count(t);
}

bool Sema::isCompatibleTypes(const std::string &expected, const std::string &actual)
{
    if (expected == actual) return true;
    if (isGenericParam(expected) || isGenericParam(actual)) return true;
    if (enumRegistry.count(expected) || enumRegistry.count(actual)) return true;
    if (expected == "Any" || actual == "Any") return true;
    if (expected == "Null" || actual == "Null") return true;

    // Implicit widening coercions
    if (expected == "Double" && actual == "Int") return true;
    if (expected == "double" && actual == "Int") return true;
    if (expected == "Char"   && actual == "Int") return true;

    // Pointer compatibility
    bool expIsPtr = (expected == "Any" || expected == "String");
    bool actIsPtr = (actual   == "Any" || actual   == "String");
    if (expIsPtr && actIsPtr) return true;

    return false;
}

bool Sema::inheritsFromException(const std::string& typeName, const std::string& baseType)
{
    if (typeName == baseType) return true;
    if (!structRegistry.count(typeName)) return false;
    for (const auto& parent : structRegistry.at(typeName)->parents) {
        if (inheritsFromException(parent, baseType)) return true;
    }
    return false;
}

void Sema::enterScope() { scopeStack.push_back({}); }
void Sema::exitScope()
{
    if (scopeStack.empty()) return;
    scopeStack.pop_back();
}

void Sema::defineVariable(const std::string &name, const std::string &type, bool isConst, bool isParam)
{
    if (scopeStack.empty())
        enterScope();
    scopeStack.back()[name] = VarInfo{type, isConst, false, isParam, currentFilePath};
}

std::string Sema::resolveVariable(const std::string &name)
{
    // 1. Check local scopes first.
    // Single find() per scope avoids the double-lookup (count + [] pattern)
    // — resolveVariable is hit on every identifier reference, so this is hot.
    for (int i = scopeStack.size() - 1; i >= 0; i--) {
        auto it = scopeStack[i].find(name);
        if (it != scopeStack[i].end()) {
            it->second.used = true;
            return it->second.type;
        }
    }

    // 2. Check Global Module Aliases
    auto gmaIt = globalModuleAliases.find(name);
    if (gmaIt != globalModuleAliases.end()) return gmaIt->second;

    std::string contextModule = currentFunctionNode ? currentFunctionNode->moduleName : "main";

    if (structRegistry.count(name))
    {
        StructNode *s = structRegistry[name];
        if (!isVisible(name, s->moduleName, contextModule))
            fatalError("symbol '" + name + "' (from '" + s->moduleName + "') is not visible here — did you 'use' it?");
        return name;
    }

    if (methodRegistry[""].count(name))
    {
        FunctionNode *f = methodRegistry[""][name];
        if (!isVisible(name, f->moduleName, contextModule))
            fatalError("function '" + name + "' (from '" + f->moduleName + "') is not visible here — did you 'use' it?");
        std::string ret = f->returnType;
        return ret.empty() ? "void" : ret;
    }

    // Builtins
    if (name == "print" || name == "printf" || name == "free" || name == "exit"
        || name == "write" || name == "writeln")
        return "void";
    if (name == "type" || name == "str")
        return "String";
    if (name == "char_at")
        return "String";
    if (name == "set_char_at")
        return "void";
    if (name == "malloc" || name == "realloc")
        return "Any";
    if (name == "strlen")
        return "Int";
        
    // --- NEW: Super keyword support ---
    if (name == "super") {
        if (!currentClass.empty() && structRegistry.count(currentClass) && !structRegistry[currentClass]->parents.empty()) {
            return structRegistry[currentClass]->parents[0];
        }
        return "void";
    }
    // ----------------------------------

    if (!currentClass.empty())
    {
        if (auto* fn = findMethod(currentClass, currentClass + "_" + name)) {
            const std::string& ret = fn->returnType;
            return ret.empty() ? "void" : ret;
        }
    }

    reportError("undefined variable or function '" + name + "'");
    return "unknown";  // allow analysis to continue
}

std::string Sema::resolveMember(const std::string &sName, const std::string &mName)
{
    // Strip generic type args before lookup: "List[T]" -> "List"
    const std::string sBase = baseType(sName);

    // Tuple numeric index access: t.0, t.1, ...
    if (sBase == "Tuple" && !mName.empty()) {
        bool isNumeric = true;
        for (char c : mName) if (!std::isdigit((unsigned char)c)) { isNumeric = false; break; }
        if (isNumeric) return "Any";
    }

    // Built-in C-runtime struct fields (from types.h) with no Quirk-side declarations.
    // str.length, list.size etc. used as bare properties resolve correctly to "Int".
    static const std::map<std::string, std::map<std::string, std::string>> builtinFields = {
        {"String", {{"_length", "Int"}, {"_buffer", "Any"}}},
        {"List",   {{"_size",   "Int"}, {"_capacity", "Int"}}},
        {"Map",    {{"_size",   "Int"}, {"_capacity", "Int"}}},
        {"Tuple",  {{"_size", "Int"}}},
        {"File",   {{"_handle", "Any"}, {"is_open", "Bool"}}},
        {"Any",    {{"tag",    "Int"}, {"ival", "Int"}, {"dval", "Double"}}},
    };
    auto bIt = builtinFields.find(sBase);
    if (bIt != builtinFields.end()) {
        auto fIt = bIt->second.find(mName);
        if (fIt != bIt->second.end()) return fIt->second;
    }

    std::string lookupName = sBase;

    if (sBase == "int") lookupName = "Int";
    else if (sBase == "double") lookupName = "Double";
    else if (sBase == "bool") lookupName = "Bool";
    else if (sBase == "char") lookupName = "Char";
    else if (sBase == "string" || sBase == "cstring") lookupName = "String";

    if (!structRegistry.count(lookupName))
        return "unknown";

    auto searchMember = [&](const std::string& currentType, auto& self) -> std::string {
        auto sIt = structRegistry.find(currentType);
        if (sIt == structRegistry.end()) return "unknown";
        StructNode* st = sIt->second;

        for (const auto& f : st->fields)
            if (f.name == mName) return f.type;

        if (findMethod(currentType, currentType + "_" + mName))
            return "method";

        for (const std::string& parentName : st->parents) {
            std::string res = self(parentName, self);
            if (res != "unknown") return res;
        }
        return "unknown";
    };

    return searchMember(lookupName, searchMember);
}

void Sema::checkWith(WithNode *node)
{
    lastNode = node;
    std::string resType = checkExpression(node->resource.get());
    if (!structRegistry.count(resType))
        fatalError("'with' resource must be a struct, got '" + resType + "'",
                   node->line, node->col, node->filePath);
    if (!findMethod(resType, resType + "___enter") ||
        !findMethod(resType, resType + "___exit"))
        fatalError("'" + resType + "' must implement __enter and __exit for use in 'with'",
                   node->line, node->col, node->filePath);
    enterScope();
    defineVariable(node->varName, resType);
    for (auto &stmt : node->body)
        checkStatement(stmt.get());
    exitScope();
}

void Sema::checkReturn(ReturnNode *node)
{
    std::string actual = node->expression ? checkExpression(node->expression.get()) : "void";
    std::string &target = currentFunctionNode->returnType;

    // If the declared return type is a generic type param (e.g. "T"), treat it as "Any"
    // for the purpose of compatibility checking (type erasure).
    if (typeAliases.count(target)) {
        const std::string resolved = typeAliases[target];
        if (resolved == "Any") return; // any return value satisfies a generic return type
    }

    // Infer return type from first return statement when no annotation given
    if (target == "auto" || target.empty())
    {
        target = actual;
        if (!currentFunctionNode->cls.empty())
            methodRegistry[currentFunctionNode->cls][currentFunctionNode->name]->returnType = actual;
        return;
    }

    // "void" here means checkFunction already defaulted it before seeing this return —
    // treat it as infer-able rather than crashing (handles duplicate file loads)
    if (target == "void" && actual != "void")
    {
        target = actual;
        if (!currentFunctionNode->cls.empty())
            methodRegistry[currentFunctionNode->cls][currentFunctionNode->name]->returnType = actual;
        return;
    }

    if (!isCompatibleTypes(target, actual))
    {
        std::cerr << "Error: Function " << currentFunctionNode->name
                << " expected " << target << " but got " << actual << std::endl;
        exit(1);
    }
}