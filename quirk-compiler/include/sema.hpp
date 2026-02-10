#ifndef SEMA_HPP
#define SEMA_HPP

#include <string>
#include <map>
#include <vector>
#include "ast.hpp"

struct Symbol {
    std::string name;
    std::string type;
};

class Sema {
   public:
    bool analyze(const std::vector<std::unique_ptr<Node>>& nodes);
    static std::string currentClass;

   private:
    FunctionNode* currentFunctionNode = nullptr;

    std::map<std::string, Symbol> globalSymbols;
    std::map<std::string, StructNode*> structRegistry;
    std::map<std::string, std::map<std::string, FunctionNode*>> methodRegistry;
    std::vector<std::map<std::string, std::string>> scopeStack;

    // Scope Helpers
    void enterScope();
    void exitScope();
    void defineVariable(const std::string& name, const std::string& type);
    void loadBuiltinModule(const std::string& name);

    std::string resolveVariable(const std::string& name);
    std::string resolveMember(const std::string& structName,
                              const std::string& memberName);

    void checkFunction(FunctionNode* node);
    void checkVarDecl(VarDeclNode* node);
    void checkStatement(Node* node);
    void checkIf(IfNode* node);
    void checkWhile(WhileNode* node);
    void checkFor(ForNode* node);
    void checkWith(WithNode* node);
    void checkReturn(ReturnNode* node);

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