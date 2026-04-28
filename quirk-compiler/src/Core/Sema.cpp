#include <iostream>
#include <algorithm>
#include <functional>
#include <sstream>
#include "sema.hpp"

std::string Sema::currentClass = "";

[[noreturn]] void Sema::fatalError(const std::string& msg, int line, int col,
                                    const std::string& filePath) {
    std::cerr << "\033[1;31m[ERROR]\033[0m " << msg << "\n";

    // Fallback to lastNode if no explicit location given
    if (line <= 0 && lastNode && lastNode->line > 0) {
        line     = lastNode->line;
        col      = lastNode->col;
    }
    std::string path = (!filePath.empty())           ? filePath
                       : (lastNode && !lastNode->filePath.empty()) ? lastNode->filePath
                       : currentFilePath;

    if (line > 0) {
        std::cerr << " --> ";
        if (!path.empty()) std::cerr << path << ":";
        std::cerr << line;
        if (col > 0) std::cerr << ":" << col;
        std::cerr << "\n";

        if (!path.empty() && sourceMap.count(path)) {
            const std::string& src = sourceMap.at(path);
            int cur = 1;
            std::string lineText;
            std::istringstream ss(src);
            while (std::getline(ss, lineText)) {
                if (cur++ == line) break;
            }
            std::string ln = std::to_string(line);
            std::cerr << "   |\n";
            std::cerr << ln << " | " << lineText << "\n";
            int off = (col > 1) ? col - 1 : 0;
            std::cerr << std::string(ln.size(), ' ') << " | "
                      << std::string(off, ' ')
                      << "\033[1;33m^--- here\033[0m\n\n";
        }
    }
    exit(1);
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

    // Register built-in Callable struct (used by lambda expressions)
    static StructNode builtinCallableNode;
    if (builtinCallableNode.name.empty()) {
        builtinCallableNode.name = "Callable";
        StructField ff; ff.name = "fn";  ff.type = "Any";
        StructField ef; ef.name = "env"; ef.type = "Any";
        builtinCallableNode.fields.push_back(std::move(ff));
        builtinCallableNode.fields.push_back(std::move(ef));
    }
    if (!structRegistry.count("Callable")) structRegistry["Callable"] = &builtinCallableNode;

    // Pass 1: Register Structs and Signatures
    for (const auto &node : nodes)
    {
        if (auto s = dynamic_cast<StructNode *>(node.get()))
        {
            structRegistry[s->name] = s;
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
    }

    // Pass 2: Analyze Bodies
    enterScope();

    for (const auto &node : nodes)
    {
        std::string mod = node->moduleName;
        if (moduleVisibility.find(mod) == moduleVisibility.end())
        {
            moduleVisibility[mod].visibleModules.insert("core");
        }

        // --- NEW: Validate Inheritance Tree for Structs ---
        if (auto s = dynamic_cast<StructNode*>(node.get())) {
            for (const std::string& parentName : s->parents) {
                if (!structRegistry.count(parentName))
                    fatalError("struct '" + s->name + "' inherits from undefined type '" + parentName + "'",
                               s->line, s->col, s->filePath);
            }
        }
        // --------------------------------------------------

        if (!node->filePath.empty()) currentFilePath = node->filePath;

        if (auto f = dynamic_cast<FunctionNode *>(node.get()))
        {
            checkFunction(f);
        }
        else if (auto use = dynamic_cast<UseNode *>(node.get()))
        {
            checkUse(use);
        }
        else if (!dynamic_cast<StructNode *>(node.get()))
        {
            checkStatement(node.get());
        }
    }
    exitScope();
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
            bool found = structRegistry.count(item) || methodRegistry[""].count(item);
            if (!found)
                fatalError("module '" + node->moduleName + "' does not export symbol '" + item + "'",
                           node->line, node->col, node->filePath);
            ctx.visibleSymbols.insert(item);
        }
    }
    else
    {
        std::string alias = node->moduleName;
        size_t lastDot = alias.rfind('.'); 
        if (lastDot == std::string::npos)
            lastDot = alias.rfind('/');
        if (lastDot != std::string::npos)
            alias = alias.substr(lastDot + 1);

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
    if (symbolModule.find("core") == 0)
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

void Sema::checkFunction(FunctionNode *f)
{
    std::string prevClass = currentClass;
    FunctionNode *prevFunc = currentFunctionNode;

    currentClass = f->cls;
    currentFunctionNode = f;
    if (!f->filePath.empty()) currentFilePath = f->filePath;

    enterScope();

    if (!f->cls.empty() && !f->isStatic)
        defineVariable("self", f->cls);
    for (const auto &param : f->parameters)
    {
        defineVariable(param.name, param.type);
    }

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
        if (!f->cls.empty() && methodRegistry[f->cls].count(f->name))
            methodRegistry[f->cls][f->name]->returnType = "void";
    }
    else if (!f->cls.empty() && f->returnType != "void")
    {
        // Sync inferred return type back to registry (handles duplicate parsing)
        if (methodRegistry[f->cls].count(f->name))
            methodRegistry[f->cls][f->name]->returnType = f->returnType;
    }

    exitScope();
    currentClass = prevClass;
    currentFunctionNode = prevFunc;
}

void Sema::checkVarDecl(VarDeclNode *node)
{
    lastNode = node;
    std::string exprType = checkExpression(node->expression.get());

    if (auto lit = dynamic_cast<LiteralNode *>(node->lhs.get()))
    {
        std::string finalType =
            node->typeAnnotation.empty() ? exprType : node->typeAnnotation;
        defineVariable(lit->value, finalType);
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
    else if (auto m = dynamic_cast<MatchNode*>(node)) {
        checkExpression(m->scrutinee.get());
        for (auto& arm : m->arms) {
            for (auto& pat : arm.patterns) checkExpression(pat.get());
            enterScope();
            for (auto& s : arm.body) checkStatement(s.get());
            exitScope();
        }
    }
    else if (auto th = dynamic_cast<ThrowNode*>(node)) {
        if (th->expression) {
            std::string type = checkExpression(th->expression.get());
            if (!structRegistry.count(type))
                fatalError("can only throw struct objects, got '" + type + "'",
                           th->line, 0, th->filePath);
        }
        // bare throw (nullptr expression): re-raises current exception — no type check needed
    }
    else if (auto tr = dynamic_cast<TriggerNode*>(node)) {
        std::string varType;
        std::string objType; // Store the object type
        
        size_t dotPos = tr->varName.find('.');
        if (dotPos != std::string::npos) {
            std::string objName = tr->varName.substr(0, dotPos);
            std::string propName = tr->varName.substr(dotPos + 1);
            
            objType = resolveVariable(objName);
            varType = resolveMember(objType, propName);
            
            if (varType == "unknown")
                fatalError("cannot trigger on unknown member '" + tr->varName + "'",
                           tr->line, tr->col, tr->filePath);
        } else {
            varType = resolveVariable(tr->varName);
        }

        // --- NEW: Update all 3 parameter types ---
        if (tr->handlerNode) {
            auto& params = tr->handlerNode->parameters;
            if (dotPos != std::string::npos && params.size() >= 3) {
                // [0] = Object Context, [1] = New Value, [2] = Old Value
                params[0].type = objType;
                params[1].type = varType;
                params[2].type = varType;
            } else if (params.size() >= 2) {
                // Local Variable: [0] = New Value, [1] = Old Value
                params[0].type = varType;
                params[1].type = varType;
            }
        }
    }
    else if (auto r = dynamic_cast<ReturnNode *>(node))
        checkReturn(r);
}

void Sema::checkIf(IfNode *node)
{
    lastNode = node;
    std::string condType = checkExpression(node->condition.get());
    if (condType != "Bool")
        fatalError("'if' condition must be 'Bool', got '" + condType + "'",
                   node->line, node->col, node->filePath);
    enterScope();
    for (auto &s : node->thenBranch)
        checkStatement(s.get());
    exitScope();
    for (auto &b : node->elIfBranches)
    {
        if (checkExpression(b.condition.get()) != "Bool")
            fatalError("'elif' condition must be 'Bool'", node->line, node->col, node->filePath);
        enterScope();
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
    if (checkExpression(node->condition.get()) != "Bool")
        fatalError("'while' condition must be 'Bool'", node->line, node->col, node->filePath);
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
        itemType = "Char";
    else if (iterType == "File")
        itemType = "String";
    else if (structRegistry.count(iterType))
    {
        std::string iterMethod = iterType + "___iter";
        if (methodRegistry[iterType].count(iterMethod))
        {
            std::string iterStruct =
                methodRegistry[iterType][iterMethod]->returnType;
            std::string nextMethod = iterStruct + "___next";
            if (methodRegistry[iterStruct].count(nextMethod))
            {
                itemType = methodRegistry[iterStruct][nextMethod]->returnType;
            }
        }
    }

    enterScope();
    defineVariable(node->varName, itemType);
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
    if (auto lambda = dynamic_cast<LambdaNode *>(node)) {
        // Infer return type by type-checking the body with params in scope
        enterScope();
        for (const auto& p : lambda->params)
            if (!p.type.empty()) defineVariable(p.name, p.type);
        if (lambda->isExpression && lambda->exprBody)
            lambda->inferredReturnType = checkExpression(lambda->exprBody.get());
        exitScope();
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
    return "unknown";
}

std::string Sema::checkLiteral(LiteralNode *node)
{
    if (std::isdigit(node->value[0]))
        return (node->value.find('.') != std::string::npos) ? "Double" : "Int";
    if (node->value[0] == '"')
        return "String";
    if (node->value[0] == '\'') // <-- ADD THIS
        return "Char";          // <-- ADD THIS
    if (node->value == "true" || node->value == "false")
        return "Bool";
    if (node->value == "null")
        return "Null";
    return resolveVariable(node->value);
}

std::string Sema::checkBinaryOp(BinaryOpNode *node)
{
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

    if (node->op == "in")
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
            return (lType == "String") ? "Char" : "Any";
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
        else if (node->op == "==") magic = "__eq";
        else if (node->op == "!=") magic = "__ne";
        else if (node->op == "<")  magic = "__lt";
        else if (node->op == "<=") magic = "__le";
        else if (node->op == ">")  magic = "__gt";
        else if (node->op == ">=") magic = "__ge";

        if (!magic.empty())
        {
            // For !=, fall back to __eq if __ne is not defined
            std::string funcName = lType + "_" + magic;
            if (!methodRegistry[lType].count(funcName) && node->op == "!=")
                funcName = lType + "_" + "__eq";

            if (methodRegistry[lType].count(funcName))
            {
                static const std::set<std::string> boolOps = {"==","!=","<","<=",">",">="};
                if (boolOps.count(node->op)) return "Bool";
                return methodRegistry[lType][funcName]->returnType;
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
    if (node->op == "-" || node->op == "*" || node->op == "/")
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
    // Strip Optional marker before method lookup
    if (!objType.empty() && objType.back() == '?') objType.pop_back();

    // Magic attributes
    if (node->memberName == "__name")   return "String";
    if (node->memberName == "__parent") return "String";
    if (node->memberName == "__class")  return "Type";

    if (objType.rfind("MODULE$", 0) == 0)
    {
        std::string modName = objType.substr(7); 
        std::string funcName = node->memberName;

        if (methodRegistry[""].count(funcName))
        {
            return methodRegistry[""][funcName]->returnType;
        }
        fatalError("module '" + modName + "' has no function '" + funcName + "'",
                   node->line, node->col, node->filePath);
    }

    std::string type = resolveMember(objType, node->memberName);
    if (type == "unknown")
        fatalError("'" + objType + "' has no member '" + node->memberName + "'",
                   node->line, node->col, node->filePath);
    if (type == "method") {
        std::string mfName = objType + "_" + node->memberName;
        if (methodRegistry[objType].count(mfName)) {
            std::string ret = methodRegistry[objType][mfName]->returnType;
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
    if (auto l = dynamic_cast<LiteralNode *>(node->callee.get())) {
        if (l->value == "super") {
            if (currentClass.empty())
                fatalError("'super' used outside of a struct method", node->line, node->col, node->filePath);
            if (structRegistry[currentClass]->parents.empty())
                fatalError("'" + currentClass + "' has no parent — cannot use 'super'",
                           node->line, node->col, node->filePath);
            return structRegistry[currentClass]->parents[0];
        }

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

        return resolveVariable(l->value);
    }

    if (auto m = dynamic_cast<MemberAccessNode *>(node->callee.get()))
    {
        std::string objType = checkExpression(m->object.get());
        // Strip Optional marker before method lookup (e.g. "String?" -> "String")
        if (!objType.empty() && objType.back() == '?') objType.pop_back();

        // --- THE FIX: Handle Module Constructor Calls (e.g. io.File) ---
        if (objType.rfind("MODULE$", 0) == 0) {
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
        else if (objType == "char") objType = "Char";
        else if (objType == "cstring" || objType == "string") objType = "String";

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
                {"to_char",    "Char"},
                {"lower",      "String"},
                {"upper",      "String"},
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
            {"Char", {
                {"str",        "String"},
                {"to_int",     "Int"},
                {"parse",      "Char"},
            }},
            {"List", {
                {"get",        "Any"},
                {"length",     "Int"},
                {"append",     "void"},
                {"push",       "void"},
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
                // Skip arg type-checking for builtins — their params aren't in the registry.
                return mIt->second;
            }
        }

        if (structRegistry.count(objType))
        {
            std::function<std::string(const std::string&)> searchMethod = [&](const std::string& currentType) -> std::string {
                if (!structRegistry.count(currentType)) return "";
                
                std::string funcName = currentType + "_" + m->memberName;
                if (methodRegistry[currentType].count(funcName)) {
                    FunctionNode *func = methodRegistry[currentType][funcName];

                    // Validate argument types against parameters.
                    // Note: 'self' is already stripped from func->parameters by the parser
                    // (Parser.cpp erases parameters[0] when name == "self"), so no offset needed.
                    // Stop early if we hit a variadic parameter — the remaining args are
                    // bundled into a List at codegen time.
                    for (size_t i = 0; i < node->args.size() && i < func->parameters.size(); ++i) {
                        if (func->parameters[i].isVariadic) break;

                        std::string argType = checkExpression(node->args[i].value.get());
                        const std::string &paramType = func->parameters[i].type;
                        if (!isCompatibleTypes(paramType, argType))
                            fatalError("argument " + std::to_string(i + 1) + " of '" + funcName +
                                       "' expected '" + paramType + "' but got '" + argType + "'",
                                       node->line, node->col, node->filePath);
                    }

                    std::string ret = func->returnType;
                    if (ret.empty() || ret == "auto") return "Any";
                    return ret;
                }
                
                for (const std::string& parent : structRegistry[currentType]->parents) {
                    std::string res = searchMethod(parent);
                    if (!res.empty()) return res;
                }
                
                return "";
            };

            std::string retType = searchMethod(objType);
            if (!retType.empty()) return retType;
        }
    }
    return "void";
}

std::string Sema::checkListLiteral(ListLiteralNode *node)
{
    if (!structRegistry.count("List"))
        fatalError("'List' type not available — is core loaded?", node->line, node->col, node->filePath);
    for (auto &elem : node->elements)
        checkExpression(elem.get());
    return "List";
}

std::string Sema::checkMapLiteral(MapLiteralNode *node)
{
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


bool Sema::isCompatibleTypes(const std::string &expected, const std::string &actual)
{
    if (expected == actual) return true;
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
    if (!scopeStack.empty())
        scopeStack.pop_back();
}

void Sema::defineVariable(const std::string &name, const std::string &type)
{
    if (scopeStack.empty())
        enterScope();
    scopeStack.back()[name] = type;
}

std::string Sema::resolveVariable(const std::string &name)
{
    // 1. Check local scopes first
    for (int i = scopeStack.size() - 1; i >= 0; i--)
        if (scopeStack[i].count(name))
            return scopeStack[i][name];

    // 2. Check Global Module Aliases
    if (globalModuleAliases.count(name)) {
        return globalModuleAliases[name];
    }

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
    if (name == "print" || name == "printf" || name == "free" || name == "exit")
        return "void";
    if (name == "char_at")
        return "Char";
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
        std::string staticName = currentClass + "_" + name;
        if (methodRegistry[currentClass].count(staticName))
        {
            std::string ret = methodRegistry[currentClass][staticName]->returnType;
            return ret.empty() ? "void" : ret;
        }
    }

    fatalError("undefined variable or function '" + name + "'");
}

std::string Sema::resolveMember(const std::string &sName, const std::string &mName)
{
    // Built-in C-runtime struct fields (from types.h) with no Quirk-side declarations.
    // str.length, list.size etc. used as bare properties resolve correctly to "Int".
    static const std::map<std::string, std::map<std::string, std::string>> builtinFields = {
        {"String", {{"length", "Int"}, {"buffer", "Any"}}},
        {"List",   {{"size",   "Int"}, {"capacity", "Int"}}},
        {"Map",    {{"size",   "Int"}, {"capacity", "Int"}}},
        {"File",   {{"is_open","Bool"}}},
        {"Any",    {{"tag",    "Int"}, {"ival", "Int"}, {"dval", "Double"}}},
    };
    auto bIt = builtinFields.find(sName);
    if (bIt != builtinFields.end()) {
        auto fIt = bIt->second.find(mName);
        if (fIt != bIt->second.end()) return fIt->second;
    }

    std::string lookupName = sName;
    
    if (sName == "int") lookupName = "Int";
    else if (sName == "double") lookupName = "Double";
    else if (sName == "bool") lookupName = "Bool";
    else if (sName == "char") lookupName = "Char";
    else if (sName == "string" || sName == "cstring") lookupName = "String";

    if (!structRegistry.count(lookupName))
        return "unknown";

    std::function<std::string(const std::string&)> searchMember = [&](const std::string& currentType) -> std::string {
        if (!structRegistry.count(currentType)) return "unknown";
        
        StructNode* st = structRegistry[currentType];
        
        for (const auto &f : st->fields) {
            if (f.name == mName) return f.type;
        }
        
        std::string funcName = currentType + "_" + mName;
        if (methodRegistry[currentType].count(funcName)) {
            return "method";
        }
        
        for (const std::string& parentName : st->parents) {
            std::string res = searchMember(parentName);
            if (res != "unknown") return res;
        }
        
        return "unknown";
    };

    return searchMember(lookupName);
}

void Sema::checkWith(WithNode *node)
{
    lastNode = node;
    std::string resType = checkExpression(node->resource.get());
    if (!structRegistry.count(resType))
        fatalError("'with' resource must be a struct, got '" + resType + "'",
                   node->line, node->col, node->filePath);
    if (!methodRegistry[resType].count(resType + "___enter") ||
        !methodRegistry[resType].count(resType + "___exit"))
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