#pragma once
#include <vector>
#include <string>
#include <memory>
#include "lexer.hpp"
#include "ast.hpp"

class Parser {
    const std::vector<Token>& tokens; 
    const std::string& source;
    std::string filePath;
    int pos = 0;
    int lambdaCount = 0;
    
    std::vector<std::unique_ptr<Node>> extraNodes; 

    [[noreturn]] void reportError(const std::string& message, const Token& token);

public:
    Parser(const std::vector<Token>& tokens, const std::string& source, const std::string& filePath = "unknown_file");

    std::vector<std::unique_ptr<Node>> parse();

private:
    Token peek() const;
    bool isAtEnd() const;
    Token advance();
    bool match(TokenType type);
    void consume(TokenType type, const std::string& message);

    std::string computeModulePrefix() const;

    // Expression parsing
    std::unique_ptr<Node> parseExpression(int min_precedence);

    // Statement parsing
    std::unique_ptr<Node> parseStatement();
    std::unique_ptr<Node> parseIf();
    std::unique_ptr<Node> parseWhile();
    std::unique_ptr<Node> parseFor();
    std::unique_ptr<Node> parseUse();
    std::unique_ptr<Node> parseWith();
    std::unique_ptr<Node> parseTry();
    std::unique_ptr<Node> parseThrow();
    std::unique_ptr<Node> parseTrigger();
    
    // Definition parsing
    std::unique_ptr<FunctionNode> parseFunction();
    std::unique_ptr<StructNode> parseStruct();
    
    // Helpers
    std::unique_ptr<CallNode> parseCall();
    std::unique_ptr<VarDeclNode> parseVarDecl();
    std::unique_ptr<ConstructorNode> parseConstructor();

    // std::unique_ptr<Node> parseListLiteral();    
    std::unique_ptr<Node> parseMapLiteral();
};