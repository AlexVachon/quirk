#pragma once
#include <vector>
#include <string>
#include <memory>
#include "lexer.hpp"
#include "ast.hpp"

// Thrown by reportError; caught in parse() to recover at top-level boundaries.
struct ParseError {};

class Parser {
    const std::vector<Token>& tokens;
    const std::string& source;
    std::string filePath;
    int pos = 0;
    int lambdaCount = 0;

    std::vector<std::unique_ptr<Node>> extraNodes;

    struct ErrorRecord { std::string msg, filePath, lineCode; int line, col; };
    std::vector<ErrorRecord> errors;

    void reportError(const std::string& message, const Token& token); // records + throws ParseError
    void sync(); // advance to next top-level sync point
    // In-block statement resync — advance past one broken statement
    // (to newline, `;`, or a plausible statement-starter) so the
    // next line inside a `define` body can still be type-checked.
    // Used by every `while (peek() != RBRACE) parseStatement()` loop.
    void syncStatement();

public:
    Parser(const std::vector<Token>& tokens, const std::string& source, const std::string& filePath = "unknown_file");

    std::vector<std::unique_ptr<Node>> parse();

    bool hasErrors() const { return !errors.empty(); }
    void flushErrors() const;

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
    std::unique_ptr<Node> parseStatementImpl();
    std::unique_ptr<Node> parseIf();
    std::unique_ptr<Node> parseWhile();
    std::unique_ptr<Node> parseFor();

    // Parse a control-flow body — either a braced block `{ ... }` or
    // a single statement after `=>`. Used by if / elif / else /
    // while / for so `if x < 0 => return -x` reads the same shape
    // as a full braced body without special-casing at each site.
    void parseControlBody(std::vector<std::unique_ptr<Node>>& out);
    std::unique_ptr<Node> parseUse();
    std::unique_ptr<Node> parseWith();
    std::unique_ptr<Node> parseTry();
    std::unique_ptr<Node> parseThrow();
    std::unique_ptr<Node> parseMatch();
    
    // Definition parsing
    std::unique_ptr<FunctionNode> parseFunction(bool allowAbstract = false,
                                                bool assumeExtern = false);
    std::unique_ptr<StructNode> parseStruct();
    // Parse: where T: Interface1 & Interface2, U: Interface3
    std::map<std::string, std::vector<std::string>> parseGenericWhere(const std::vector<std::string>& typeParams);
    std::unique_ptr<EnumNode> parseEnum();
    std::unique_ptr<InterfaceNode> parseInterface();

    // Type annotation helper: reads "Type" or "Type[T, U]" or "T|U" into a string
    std::string parseTypeString();
    bool isGenericArgList() const; // lookahead: is [ ahead a type-arg list?
    
    // Helpers
    std::unique_ptr<CallNode> parseCall();
    std::unique_ptr<VarDeclNode> parseVarDecl();
    std::unique_ptr<ConstructorNode> parseConstructor();

    // std::unique_ptr<Node> parseListLiteral();    
    std::unique_ptr<Node> parseMapLiteral();
};