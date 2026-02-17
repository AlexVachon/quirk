#include <iostream>
#include <algorithm>
#include <functional>
#include "sema.hpp"

std::string Sema::currentClass = "";

bool Sema::analyze(const std::vector<std::unique_ptr<Node>> &nodes)
{
    if (scopeStack.empty())
        enterScope();

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
            {
                std::cerr << "Error: Module '" << node->moduleName
                          << "' does not export symbol '" << item << "'" << std::endl;
                exit(1);
            }
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

    enterScope();

    if (!f->cls.empty() && !f->isStatic)
        defineVariable("self", f->cls);
    for (const auto &param : f->parameters)
    {
        defineVariable(param.name, param.type);
    }

    // // Implicitly define 'it' for trigger handlers
    // if (f->name.find("__quirk_trigger_") == 0) {
    //     defineVariable("it", "Any"); // 'it' represents the new value
    //     // --- NEW LOG ---
    //     std::cerr << "[DEBUG] Sema: Implicitly defined 'it' for " << f->name << std::endl;
    // }

    if (!f->isExtern)
    {
        for (const auto &statement : f->body)
        {
            checkStatement(statement.get());
        }
    }

    if (f->returnType == "auto")
    {
        f->returnType = "void";
        if (!f->cls.empty())
            methodRegistry[f->cls][f->name]->returnType = "void";
    }

    exitScope();
    currentClass = prevClass;
    currentFunctionNode = prevFunc;
}

void Sema::checkVarDecl(VarDeclNode *node)
{
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
        {
            std::cerr << "Error: Member '" << member->memberName
                      << "' not found in " << objType << std::endl;
            exit(1);
        }
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

        // --- UPDATED: Validate all catch blocks and all types within them ---
        for (auto& cb : t->catchBlocks) {
            enterScope();
            for (const std::string& typeName : cb.types) {
                if (!structRegistry.count(typeName)) {
                    std::cerr << "Error: Catch type '" << typeName << "' is undefined." << std::endl; 
                    exit(1);
                }
            }
            // Define the local exception variable using the FIRST valid type in the catch signature
            defineVariable(cb.varName, cb.types[0]);
            for (auto& s : cb.body) checkStatement(s.get());
            exitScope();
        }
    }
    else if (auto th = dynamic_cast<ThrowNode*>(node)) {
        std::string type = checkExpression(th->expression.get());
        if (!structRegistry.count(type)) {
            std::cerr << "Error: Can only throw Struct objects, got '" << type << "'." << std::endl; exit(1);
        }
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
            
            if (varType == "unknown") {
                std::cerr << "Error: Cannot trigger on unknown struct member '" << tr->varName << "'" << std::endl;
                exit(1);
            }
        } else {
            varType = resolveVariable(tr->varName);
        }

        // --- NEW: Update both parameter types ---
        if (tr->handlerNode) {
            if (dotPos != std::string::npos && tr->handlerNode->parameters.size() >= 2) {
                // Parameter 0 is the object (e.g., self : Player)
                tr->handlerNode->parameters[0].type = objType;
                // Parameter 1 is the new value (e.g., it : Int)
                tr->handlerNode->parameters[1].type = varType;
            } else if (!tr->handlerNode->parameters.empty()) {
                tr->handlerNode->parameters[0].type = varType;
            }
        }
    }
    else if (auto r = dynamic_cast<ReturnNode *>(node))
        checkReturn(r);
}

void Sema::checkIf(IfNode *node)
{
    std::string condType = checkExpression(node->condition.get());
    if (condType != "Bool")
    {
        std::cerr << "[Sema Error] 'if' condition must be 'Bool', but got '"
                  << condType << "'" << std::endl;
        exit(1);
    }
    enterScope();
    for (auto &s : node->thenBranch)
        checkStatement(s.get());
    exitScope();
    for (auto &b : node->elIfBranches)
    {
        if (checkExpression(b.condition.get()) != "Bool")
            exit(1);
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
    if (checkExpression(node->condition.get()) != "Bool")
        exit(1);
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
    return "unknown";
}

std::string Sema::checkLiteral(LiteralNode *node)
{
    if (std::isdigit(node->value[0]))
        return (node->value.find('.') != std::string::npos) ? "Double" : "Int";
    if (node->value[0] == '"')
        return "String";
    if (node->value == "true" || node->value == "false")
        return "Bool";
    return resolveVariable(node->value);
}

std::string Sema::checkBinaryOp(BinaryOpNode *node)
{
    if (node->op == "not")
    {
        if (checkExpression(node->left.get()) != "Bool")
            exit(1);
        return "Bool";
    }

    std::string lType = checkExpression(node->left.get());
    std::string rType = checkExpression(node->right.get());

    if (node->op == "and" || node->op == "or")
        return "Bool";

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
                    {
                        std::cerr << "Error: Type mismatch for '" << lType
                                  << "[]'. Expected '" << expectedKeyType
                                  << "' index, got '" << rType << "'." << std::endl;
                        exit(1);
                    }
                }
                return func->returnType;
            }
        }
        if (rType != "Int")
        {
            std::cerr << "Error: Array index must be 'Int'." << std::endl;
            exit(1);
        }
        if (lType == "Any" || lType == "String")
            return (lType == "String") ? "Char" : "Any";
        exit(1);
    }

    // Operator Overloading
    if (structRegistry.count(lType))
    {
        std::string magic;
        if (node->op == "+")
            magic = "__add";
        else if (node->op == "-")
            magic = "__sub";
        else if (node->op == "*")
            magic = "__mul";
        else if (node->op == "/")
            magic = "__div";
        else if (node->op == "==" || node->op == "!=")
            magic = "__eq";

        if (!magic.empty())
        {
            std::string funcName = lType + "_" + magic;
            if (methodRegistry[lType].count(funcName))
            {
                if (node->op == "==" || node->op == "!=")
                    return "Bool";
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
    if (node->op == ">" || node->op == "<" || node->op == ">=" ||
        node->op == "<=" || node->op == "==" || node->op == "!=")
    {
        return "Bool";
    }
    exit(1);
}

std::string Sema::checkMemberAccess(MemberAccessNode *node)
{
    std::string objType = checkExpression(node->object.get());

    if (objType.rfind("MODULE$", 0) == 0)
    {
        std::string modName = objType.substr(7); 
        std::string funcName = node->memberName;

        if (methodRegistry[""].count(funcName))
        {
            return methodRegistry[""][funcName]->returnType;
        }
        std::cerr << "Error: Module '" << modName << "' has no function '" << funcName << "'" << std::endl;
        exit(1);
    }

    std::string type = resolveMember(objType, node->memberName);
    if (type == "unknown")
    {
        std::cerr << "Error: Member '" << node->memberName << "' not found on " << objType << std::endl;
        exit(1);
    }
    return type;
}

std::string Sema::checkConstructor(ConstructorNode *node)
{
    return structRegistry.count(node->structName) ? node->structName : "unknown";
}

std::string Sema::checkCall(CallNode *node)
{
    if (auto l = dynamic_cast<LiteralNode *>(node->callee.get()))
        return resolveVariable(l->value);

    if (auto m = dynamic_cast<MemberAccessNode *>(node->callee.get()))
    {
        std::string objType = checkExpression(m->object.get());

        if (objType == "int") objType = "Int";
        else if (objType == "double") objType = "Double";
        else if (objType == "bool") objType = "Bool";
        else if (objType == "char") objType = "Char";
        else if (objType == "cstring" || objType == "string") objType = "String";

        if (structRegistry.count(objType))
        {
            std::function<std::string(const std::string&)> searchMethod = [&](const std::string& currentType) -> std::string {
                if (!structRegistry.count(currentType)) return "";
                
                std::string funcName = currentType + "_" + m->memberName;
                if (methodRegistry[currentType].count(funcName)) {
                    std::string ret = methodRegistry[currentType][funcName]->returnType;
                    return ret.empty() ? "void" : ret;
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
        exit(1);
    for (auto &elem : node->elements)
        checkExpression(elem.get());
    return "List";
}

std::string Sema::checkMapLiteral(MapLiteralNode *node)
{
    if (!structRegistry.count("Map"))
    {
        std::cerr << "Error: Map type not defined." << std::endl;
        exit(1);
    }
    for (auto &pair : node->elements)
    {
        std::string keyType = checkExpression(pair.first.get());
        if (keyType != "String")
        {
            std::cerr << "Error: Map keys must be String." << std::endl;
            exit(1);
        }
        checkExpression(pair.second.get());
    }
    return "Map";
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
    for (int i = scopeStack.size() - 1; i >= 0; i--)
        if (scopeStack[i].count(name))
            return scopeStack[i][name];

    std::string contextModule = currentFunctionNode ? currentFunctionNode->moduleName : "main";

    if (structRegistry.count(name))
    {
        StructNode *s = structRegistry[name];
        if (!isVisible(name, s->moduleName, contextModule))
        {
            std::cerr << "Error: Symbol '" << name << "' defined in '"
                      << s->moduleName << "' is not visible in '" << contextModule << "'." << std::endl;
            exit(1);
        }
        return name;
    }

    if (methodRegistry[""].count(name))
    {
        FunctionNode *f = methodRegistry[""][name];
        if (!isVisible(name, f->moduleName, contextModule))
        {
            std::cerr << "Error: Function '" << name << "' defined in '"
                      << f->moduleName << "' is not visible in '" << contextModule << "'." << std::endl;
            exit(1);
        }
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

    if (!currentClass.empty())
    {
        std::string staticName = currentClass + "_" + name;
        if (methodRegistry[currentClass].count(staticName))
        {
            std::string ret = methodRegistry[currentClass][staticName]->returnType;
            return ret.empty() ? "void" : ret;
        }
    }

    std::cerr << "Error: Undefined variable '" << name << "'" << std::endl;
    exit(1);
}

std::string Sema::resolveMember(const std::string &sName, const std::string &mName)
{
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
    std::string resType = checkExpression(node->resource.get());
    if (!structRegistry.count(resType))
        exit(1);
    if (!methodRegistry[resType].count(resType + "___enter") ||
        !methodRegistry[resType].count(resType + "___exit"))
    {
        exit(1);
    }
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

    if (target == "auto")
    {
        target = actual;
        if (!currentFunctionNode->cls.empty())
            methodRegistry[currentFunctionNode->cls][currentFunctionNode->name]->returnType = actual;
        return;
    }

    if (target != actual && target != "Any")
    {
        bool targetIsPtr = (target == "Any" || target == "String");
        bool actualIsPtr = (actual == "Any" || actual == "String");
        
        bool valid = (targetIsPtr && actualIsPtr);
        if (target == "Double" && actual == "Int") valid = true;
        if (target == "Char" && actual == "Int") valid = true;
        if (actual == "Any") valid = true;
        
        if (actual == "Int" && (targetIsPtr || structRegistry.count(target) || target == "Any")) valid = true;

        if (!valid)
        {
            std::cerr << "Error: Function " << currentFunctionNode->name
                      << " expected " << target << " but got " << actual << std::endl;
            exit(1);
        }
    }
}
