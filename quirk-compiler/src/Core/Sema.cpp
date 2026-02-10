#include <iostream>
#include <algorithm> // <--- ADDED THIS for std::replace
#include "sema.hpp"

std::string Sema::currentClass = "";

bool Sema::analyze(const std::vector<std::unique_ptr<Node>>& nodes) {
    if (scopeStack.empty()) enterScope();

    // Pass 1: Register Structs and Signatures
    for (const auto& node : nodes) {
        if (auto s = dynamic_cast<StructNode*>(node.get())) {
            structRegistry[s->name] = s;
        } else if (auto f = dynamic_cast<FunctionNode*>(node.get())) {
            if (f->returnType.empty()) f->returnType = "auto";
            defineVariable(f->name, f->returnType);
            if (!f->cls.empty()) methodRegistry[f->cls][f->name] = f;
            else methodRegistry[""][f->name] = f;
        }
    }

    // Pass 2: Analyze Bodies
    enterScope();
    
    for (const auto& node : nodes) {
        // --- NEW: Context-Aware Analysis ---
        // Ensure every module (including 'main') implicitly sees 'core'
        std::string mod = node->moduleName;
        if (moduleVisibility.find(mod) == moduleVisibility.end()) {
             moduleVisibility[mod].visibleModules.insert("core");
             moduleVisibility[mod].visibleModules.insert("core.sys");
             moduleVisibility[mod].visibleModules.insert("core.string");
             moduleVisibility[mod].visibleModules.insert("core.collections.list");
        }

        if (auto f = dynamic_cast<FunctionNode*>(node.get())) {
            checkFunction(f);
        } else if (auto use = dynamic_cast<UseNode*>(node.get())) {
            checkUse(use);
        } else if (!dynamic_cast<StructNode*>(node.get())) {
            checkStatement(node.get());
        }
    }
    exitScope();
    return true;
}

void Sema::checkUse(UseNode* node) {
    // FIX: Cast to Node* to access the SOURCE module ("main"), 
    // not the UseNode's target module ("math").
    std::string sourceModule = static_cast<Node*>(node)->moduleName;
    
    VisibilityContext& ctx = moduleVisibility[sourceModule]; // <--- FIXED

    if (!node->filterList.empty()) {
        for (const auto& item : node->filterList) {
            bool found = structRegistry.count(item) || methodRegistry[""].count(item);
            if (!found) {
                std::cerr << "Error: Module '" << node->moduleName 
                          << "' does not export symbol '" << item << "'" << std::endl;
                exit(1);
            }
            ctx.visibleSymbols.insert(item);
        }
    } else {
        std::string modName = node->moduleName;
        std::replace(modName.begin(), modName.end(), '/', '.');
        ctx.visibleModules.insert(modName);
    }
}

// --- NEW VISIBILITY CHECK ---
bool Sema::isVisible(const std::string& name, const std::string& symbolModule, const std::string& currentModule) {
    // 1. Library Strictness Check:
    //    If we are inside a library (not "main"), we currently allow access to everything 
    //    to simplify internal dependencies. We ONLY enforce strictness for User Code ("main").
    if (currentModule != "main") return true;

    // 2. Core is always visible
    if (symbolModule.find("core") == 0) return true;

    // 3. Same Module? (main accessing main)
    if (symbolModule == currentModule) return true;

    // 4. Check Context
    if (moduleVisibility.count(currentModule)) {
        const auto& ctx = moduleVisibility[currentModule];
        
        // A. Is the whole module imported?
        if (ctx.visibleModules.count(symbolModule)) return true;
        
        // B. Is the specific symbol imported?
        if (ctx.visibleSymbols.count(name)) return true;
    }

    return false;
}

void Sema::checkFunction(FunctionNode* f) {
    std::string prevClass = currentClass;
    FunctionNode* prevFunc = currentFunctionNode;

    currentClass = f->cls;
    currentFunctionNode = f;

    enterScope();

    if (!f->cls.empty() && !f->isStatic)
        defineVariable("self", f->cls);
    for (const auto& param : f->parameters) {
        defineVariable(param.name, param.type);
    }

    if (!f->isExtern) {
        for (const auto& statement : f->body) {
            checkStatement(statement.get());
        }
    }

    if (f->returnType == "auto") {
        f->returnType = "void";
        if (!f->cls.empty())
            methodRegistry[f->cls][f->name]->returnType = "void";
    }

    exitScope();
    currentClass = prevClass;
    currentFunctionNode = prevFunc;
}

void Sema::checkVarDecl(VarDeclNode* node) {
    std::string exprType = checkExpression(node->expression.get());

    if (auto lit = dynamic_cast<LiteralNode*>(node->lhs.get())) {
        std::string finalType =
            node->typeAnnotation.empty() ? exprType : node->typeAnnotation;

        if (!node->typeAnnotation.empty() && finalType != "any" &&
            finalType != exprType) {
            bool isPtrTypes = (finalType == "ptr" || finalType == "cstring" ||
                               finalType == "string");
            bool isExprPtr = (exprType == "ptr" || exprType == "cstring" ||
                              exprType == "string");

            bool valid = (isPtrTypes && isExprPtr);
            if (finalType == "String" &&
                (exprType == "cstring" || exprType == "string"))
                valid = true;
            if ((finalType == "ptr" && exprType == "int") ||
                (finalType == "int" && exprType == "ptr"))
                valid = true;

            if (!valid) {
                std::cerr << "Error: Cannot assign '" << exprType << "' to '"
                          << finalType << "' for variable '" << lit->value
                          << "'" << std::endl;
                exit(1);
            }
        }
        defineVariable(lit->value, finalType);

    } else if (auto member = dynamic_cast<MemberAccessNode*>(node->lhs.get())) {
        std::string objType = checkExpression(member->object.get());
        if (resolveMember(objType, member->memberName) == "unknown") {
            std::cerr << "Error: Member '" << member->memberName
                      << "' not found in " << objType << std::endl;
            exit(1);
        }
    } else if (auto binOp = dynamic_cast<BinaryOpNode*>(node->lhs.get())) {
        if (binOp->op != "[]") {
            std::cerr << "Error: Invalid assignment target." << std::endl;
            exit(1);
        }
        checkExpression(binOp->left.get());
    }
}

void Sema::checkStatement(Node* node) {
    if (auto v = dynamic_cast<VarDeclNode*>(node))
        checkVarDecl(v);
    else if (auto u = dynamic_cast<UseNode*>(node))
        checkUse(u);
    else if (auto i = dynamic_cast<IfNode*>(node))
        checkIf(i);
    else if (auto wi = dynamic_cast<WithNode*>(node))
        checkWith(wi);
    else if (auto w = dynamic_cast<WhileNode*>(node))
        checkWhile(w);
    else if (auto f = dynamic_cast<ForNode*>(node))
        checkFor(f);
    else if (auto c = dynamic_cast<CallNode*>(node))
        checkExpression(c);
    else if (auto r = dynamic_cast<ReturnNode*>(node))
        checkReturn(r);
}

void Sema::checkIf(IfNode* node) {
    std::string condType = checkExpression(node->condition.get());
    if (condType != "bool") {
        std::cerr << "[Sema Error] 'if' condition must be 'bool', but got '"
                  << condType << "'" << std::endl;
        exit(1);
    }
    enterScope();
    for (auto& s : node->thenBranch) checkStatement(s.get());
    exitScope();
    for (auto& b : node->elIfBranches) {
        if (checkExpression(b.condition.get()) != "bool") exit(1);
        enterScope();
        for (auto& s : b.body) checkStatement(s.get());
        exitScope();
    }
    if (!node->elseBranch.empty()) {
        enterScope();
        for (auto& s : node->elseBranch) checkStatement(s.get());
        exitScope();
    }
}

void Sema::checkWhile(WhileNode* node) {
    if (checkExpression(node->condition.get()) != "bool") exit(1);
    enterScope();
    for (auto& s : node->body) checkStatement(s.get());
    exitScope();
}

void Sema::checkFor(ForNode* node) {
    std::string iterType = checkExpression(node->iterable.get());
    std::string itemType = "any";

    if (iterType == "cstring" || iterType == "string")
        itemType = node->isRef ? "ptr" : "char";
    else if (iterType == "String")
        itemType = "char";
    else if (iterType == "File")
        itemType = "String";
    else if (structRegistry.count(iterType)) {
        std::string iterMethod = iterType + "___iter";
        if (methodRegistry[iterType].count(iterMethod)) {
            std::string iterStruct =
                methodRegistry[iterType][iterMethod]->returnType;
            std::string nextMethod = iterStruct + "___next";
            if (methodRegistry[iterStruct].count(nextMethod)) {
                itemType = methodRegistry[iterStruct][nextMethod]->returnType;
            }
        }
    }

    enterScope();
    defineVariable(node->varName, itemType);
    for (auto& s : node->body) checkStatement(s.get());
    exitScope();
}

std::string Sema::checkExpression(Node* node) {
    if (auto lit = dynamic_cast<LiteralNode*>(node)) return checkLiteral(lit);
    if (auto binOp = dynamic_cast<BinaryOpNode*>(node)) return checkBinaryOp(binOp);
    if (auto c = dynamic_cast<ConstructorNode*>(node)) return checkConstructor(c);
    if (auto m = dynamic_cast<MemberAccessNode*>(node)) return checkMemberAccess(m);
    if (auto c = dynamic_cast<CallNode*>(node)) return checkCall(c);
    if (auto arr = dynamic_cast<ListLiteralNode*>(node)) return checkListLiteral(arr);
    if (auto map = dynamic_cast<MapLiteralNode*>(node)) return checkMapLiteral(map);
    return "unknown";
}

std::string Sema::checkLiteral(LiteralNode* node) {
    if (std::isdigit(node->value[0]))
        return (node->value.find('.') != std::string::npos) ? "double" : "int";
    if (node->value[0] == '"') return "cstring";
    if (node->value == "true" || node->value == "false") return "bool";
    return resolveVariable(node->value);
}

std::string Sema::checkBinaryOp(BinaryOpNode* node) {
    if (node->op == "not") {
        if (checkExpression(node->left.get()) != "bool") exit(1);
        return "bool";
    }

    std::string lType = checkExpression(node->left.get());
    std::string rType = checkExpression(node->right.get());

    if (node->op == "and" || node->op == "or") return "bool";

    // Array Access
    if (node->op == "[]") {
        if (structRegistry.count(lType)) {
            std::string funcName = lType + "___get";
            if (methodRegistry[lType].count(funcName)) {
                FunctionNode* func = methodRegistry[lType][funcName];
                if (!func->parameters.empty()) {
                    std::string expectedKeyType = func->parameters[0].type;
                    bool validKey = (rType == expectedKeyType);
                    if (expectedKeyType == "String" &&
                        (rType == "cstring" || rType == "string")) validKey = true;
                    if (!validKey) {
                        std::cerr << "Error: Type mismatch for '" << lType
                                  << "[]'. Expected '" << expectedKeyType
                                  << "' index, got '" << rType << "'." << std::endl;
                        exit(1);
                    }
                }
                return func->returnType;
            }
        }
        if (rType != "int") {
            std::cerr << "Error: Array index must be 'int'." << std::endl;
            exit(1);
        }
        if (lType == "ptr" || lType == "cstring" || lType == "string" || lType == "any")
            return (lType == "cstring" || lType == "string") ? "char" : "any";
        exit(1);
    }

    // Operator Overloading
    if (structRegistry.count(lType)) {
        std::string magic;
        if (node->op == "+") magic = "__add";
        else if (node->op == "-") magic = "__sub";
        else if (node->op == "*") magic = "__mul";
        else if (node->op == "/") magic = "__div";
        else if (node->op == "==" || node->op == "!=") magic = "__eq";

        if (!magic.empty()) {
            std::string funcName = lType + "_" + magic;
            if (methodRegistry[lType].count(funcName)) {
                if (node->op == "==" || node->op == "!=") return "bool";
                return methodRegistry[lType][funcName]->returnType;
            }
        }
    }

    // Primitives
    if (node->op == "+") {
        if (lType == "cstring" || rType == "cstring" || lType == "string") return "cstring";
        if (lType == "double" || rType == "double") return "double";
        return "int";
    }
    if (node->op == "-" || node->op == "*" || node->op == "/") {
        if (lType == "double" || rType == "double") return "double";
        return "int";
    }
    if (node->op == ">" || node->op == "<" || node->op == ">=" ||
        node->op == "<=" || node->op == "==" || node->op == "!=") {
        return "bool";
    }
    exit(1);
}

std::string Sema::checkMemberAccess(MemberAccessNode* node) {
    std::string objType = checkExpression(node->object.get());
    std::string type = resolveMember(objType, node->memberName);
    if (type == "unknown") {
        std::cerr << "Error: Member '" << node->memberName << "' not found on "
                  << objType << std::endl;
        exit(1);
    }
    return type;
}

std::string Sema::checkConstructor(ConstructorNode* node) {
    return structRegistry.count(node->structName) ? node->structName : "unknown";
}

std::string Sema::checkCall(CallNode* node) {
    if (auto l = dynamic_cast<LiteralNode*>(node->callee.get()))
        return resolveVariable(l->value);

    if (auto m = dynamic_cast<MemberAccessNode*>(node->callee.get())) {
        std::string objType = checkExpression(m->object.get());
        if (objType == "int") objType = "Int";
        else if (objType == "double") objType = "Double";
        else if (objType == "cstring") objType = "String";

        if (structRegistry.count(objType)) {
            std::string funcName = objType + "_" + m->memberName;
            if (methodRegistry[objType].count(funcName)) {
                std::string ret = methodRegistry[objType][funcName]->returnType;
                return ret.empty() ? "void" : ret;
            }
        }
    }
    return "void";
}

std::string Sema::checkListLiteral(ListLiteralNode* node) {
    if (!structRegistry.count("List")) exit(1);
    for (auto& elem : node->elements) checkExpression(elem.get());
    return "List";
}

std::string Sema::checkMapLiteral(MapLiteralNode* node) {
    if (!structRegistry.count("Map")) {
        std::cerr << "Error: Map type not defined." << std::endl;
        exit(1);
    }
    for (auto& pair : node->elements) {
        std::string keyType = checkExpression(pair.first.get());
        if (keyType != "String" && keyType != "cstring" && keyType != "string") {
            std::cerr << "Error: Map keys must be String." << std::endl;
            exit(1);
        }
        checkExpression(pair.second.get());
    }
    return "Map";
}

void Sema::enterScope() { scopeStack.push_back({}); }
void Sema::exitScope() { if (!scopeStack.empty()) scopeStack.pop_back(); }

void Sema::defineVariable(const std::string& name, const std::string& type) {
    if (scopeStack.empty()) enterScope();
    scopeStack.back()[name] = type;
}

// --- UPDATED RESOLVE VARIABLE ---
std::string Sema::resolveVariable(const std::string& name) {
    for (int i = scopeStack.size() - 1; i >= 0; i--)
        if (scopeStack[i].count(name)) return scopeStack[i][name];

    // Determine context
    std::string contextModule = currentFunctionNode ? currentFunctionNode->moduleName : "main";

    // Check Structs
    if (structRegistry.count(name)) {
        StructNode* s = structRegistry[name];
        if (!isVisible(name, s->moduleName, contextModule)) {
            std::cerr << "Error: Symbol '" << name << "' defined in '" 
                      << s->moduleName << "' is not visible in '" << contextModule << "'." << std::endl;
            exit(1);
        }
        return name;
    }

    // Check Global Functions
    if (methodRegistry[""].count(name)) {
        FunctionNode* f = methodRegistry[""][name];
        if (!isVisible(name, f->moduleName, contextModule)) {
             std::cerr << "Error: Function '" << name << "' defined in '" 
                       << f->moduleName << "' is not visible in '" << contextModule << "'." << std::endl;
             exit(1);
        }
        std::string ret = f->returnType;
        return ret.empty() ? "void" : ret;
    }

    // Builtins
    if (name == "print" || name == "printf" || name == "free" || name == "exit") return "void";
    if (name == "char_at") return "char";
    if (name == "set_char_at") return "void";
    if (name == "malloc" || name == "realloc") return "ptr";
    if (name == "strlen") return "int";

    if (!currentClass.empty()) {
        std::string staticName = currentClass + "_" + name;
        if (methodRegistry[currentClass].count(staticName)) {
            std::string ret = methodRegistry[currentClass][staticName]->returnType;
            return ret.empty() ? "void" : ret;
        }
    }

    std::cerr << "Error: Undefined variable '" << name << "'" << std::endl;
    exit(1);
}

std::string Sema::resolveMember(const std::string& sName, const std::string& mName) {
    std::string lookupName = sName;
    if (sName == "int") lookupName = "Int";
    else if (sName == "double") lookupName = "Double";
    else if (sName == "bool") lookupName = "Bool";

    if (!structRegistry.count(lookupName)) return "unknown";

    for (const auto& f : structRegistry[lookupName]->fields)
        if (f.name == mName) return f.type;

    std::string funcName = lookupName + "_" + mName;
    if (methodRegistry[lookupName].count(funcName)) return "method";

    return "unknown";
}

void Sema::checkWith(WithNode* node) {
    std::string resType = checkExpression(node->resource.get());
    if (!structRegistry.count(resType)) exit(1);
    if (!methodRegistry[resType].count(resType + "___enter") ||
        !methodRegistry[resType].count(resType + "___exit")) {
        exit(1);
    }
    enterScope();
    defineVariable(node->varName, resType);
    for (auto& stmt : node->body) checkStatement(stmt.get());
    exitScope();
}

void Sema::checkReturn(ReturnNode* node) {
    std::string actual = node->expression ? checkExpression(node->expression.get()) : "void";
    std::string& target = currentFunctionNode->returnType;

    if (target == "auto") {
        target = actual;
        if (!currentFunctionNode->cls.empty())
            methodRegistry[currentFunctionNode->cls][currentFunctionNode->name]->returnType = actual;
        return;
    }

    if (target != actual && target != "any") {
        bool targetIsPtr = (target == "ptr" || target == "cstring" || target == "string");
        bool actualIsPtr = (actual == "ptr" || actual == "cstring" || actual == "string");
        bool valid = (targetIsPtr && actualIsPtr);
        if (target == "double" && actual == "int") valid = true;
        if (target == "char" && actual == "int") valid = true;
        if (actual == "any") valid = true;
        if (actual == "int" && (targetIsPtr || structRegistry.count(target) || target == "any")) valid = true;
        if (target == "String" && (actual == "cstring" || actual == "string")) valid = true;

        if (!valid) {
            std::cerr << "Error: Function " << currentFunctionNode->name
                      << " expected " << target << " but got " << actual << std::endl;
            exit(1);
        }
    }
}