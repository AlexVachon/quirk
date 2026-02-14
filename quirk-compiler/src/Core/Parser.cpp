#include <iostream>
#include "parser.hpp"

Parser::Parser(const std::vector<Token>& tokens, const std::string& source)
    : tokens(tokens), source(source) {}

Token Parser::peek() const {
    return tokens[pos];
}
bool Parser::isAtEnd() const {
    return peek().type == TokenType::EOF_TOKEN;
}

Token Parser::advance() {
    if (!isAtEnd())
        pos++;
    return tokens[pos - 1];
}

bool Parser::match(TokenType type) {
    if (peek().type == type) {
        advance();
        return true;
    }
    return false;
}

void Parser::consume(TokenType type, const std::string& message) {
    if (peek().type == type) {
        advance();
        return;
    }
    reportError(message + " (found '" + peek().value + "')", peek());
}

// --- Top Level Parsing ---
std::vector<std::unique_ptr<Node>> Parser::parse() {
    std::vector<std::unique_ptr<Node>> nodes;
    while (!isAtEnd()) {
        TokenType type = peek().type;
        if (type == TokenType::DEFINE || type == TokenType::INIT ||
            type == TokenType::EXTERN) {
            nodes.push_back(parseFunction());
        } else if (type == TokenType::STRUCT) {
            nodes.push_back(parseStruct());
            for (auto& extra : extraNodes)
                nodes.push_back(std::move(extra));
            extraNodes.clear();
        } else if (type == TokenType::EXTEND) {
            advance();
            std::string targetStruct = advance().value;
            consume(TokenType::LBRACE, "Expected '{'");
            while (peek().type != TokenType::RBRACE && !isAtEnd()) {
                auto func = parseFunction();
                func->cls = targetStruct;
                nodes.push_back(std::move(func));
            }
            consume(TokenType::RBRACE, "Expected '}'");
        } else {
            nodes.push_back(parseStatement());
        }
    }
    return nodes;
}

// --- Expression Parsing ---
int getPrecedence(TokenType type) {
    switch (type) {
        case TokenType::OR:
            return 5;
        case TokenType::AND:
            return 6;
        case TokenType::EQUAL:
        case TokenType::GREATER:
        case TokenType::LESS:
        case TokenType::GREATER_EQUAL:
        case TokenType::LESS_EQUAL:
        case TokenType::NOT_EQUAL:
            return 10;
        case TokenType::PLUS:
        case TokenType::MINUS:
            return 20;
        case TokenType::STAR:
        case TokenType::SLASH:
            return 30;
        case TokenType::DOT:
            return 40;
        case TokenType::LBRACKET:
            return 50;
        case TokenType::LBRACE:
            return 0;
        default:
            return 0;
    }
}

std::unique_ptr<Node> Parser::parseExpression(int min_precedence) {
    std::unique_ptr<Node> left;
    Token t = advance();

    // 1. Primary
    if (t.type == TokenType::INT_LITERAL) {
        left = std::make_unique<LiteralNode>(t.value);
    } else if (t.type == TokenType::FLOAT_LITERAL) {
        left = std::make_unique<LiteralNode>(t.value);
    } else if (t.type == TokenType::STRING_LITERAL) {
        left = std::make_unique<LiteralNode>(t.value);
    } else if (t.type == TokenType::TRUE) {
        left = std::make_unique<LiteralNode>("true");
    } else if (t.type == TokenType::FALSE) {
        left = std::make_unique<LiteralNode>("false");
    } else if (t.type == TokenType::IDENTIFIER) {
        left = std::make_unique<LiteralNode>(t.value);
    } else if (t.type == TokenType::NOT) {
        auto operand = parseExpression(0);
        left =
            std::make_unique<BinaryOpNode>("not", std::move(operand), nullptr);
    } else if (t.type == TokenType::MINUS) {
        auto operand = parseExpression(50);
        auto zero = std::make_unique<LiteralNode>("0");
        left = std::make_unique<BinaryOpNode>("-", std::move(zero),
                                              std::move(operand));
    } else if (t.type == TokenType::LBRACKET) {
        std::vector<std::unique_ptr<Node>> elements;
        if (peek().type != TokenType::RBRACKET) {
            do {
                elements.push_back(parseExpression(0));
            } while (match(TokenType::COMMA));
        }
        consume(TokenType::RBRACKET, "Expected ']' after list elements");
        left = std::make_unique<ListLiteralNode>(std::move(elements));
    } else if (t.type == TokenType::LBRACE) {
        auto node = std::make_unique<MapLiteralNode>();

        if (peek().type == TokenType::RBRACE) {
            advance();
        } else {
            do {
                auto key = parseExpression(0);
                consume(TokenType::COLON, "Expected ':' after map key");
                auto value = parseExpression(0);
                node->elements.push_back({std::move(key), std::move(value)});
            } while (match(TokenType::COMMA));
            consume(TokenType::RBRACE, "Expected '}' after map literal");
        }
        left = std::move(node);
    }else if (t.type == TokenType::LPAREN) {
        left = parseExpression(0);
        consume(TokenType::RPAREN, "Expected ')'");
    } else if (t.type == TokenType::NOT) {
        auto operand = parseExpression(40);
        left =
            std::make_unique<BinaryOpNode>("not", std::move(operand), nullptr);
    } else {
        reportError("Unexpected token: " + t.value, t);
    }

    // 2. Infix Loop
    while (!isAtEnd()) {
        TokenType type = peek().type;
        int next_prec = getPrecedence(type);
        if (type == TokenType::LPAREN)
            next_prec = 50;
        if (next_prec <= min_precedence)
            break;

        Token opToken = advance();

        if (opToken.type == TokenType::DOT) {
            Token member = advance();
            left = std::make_unique<MemberAccessNode>(std::move(left),
                                                      member.value);
        } else if (opToken.type == TokenType::LBRACKET) {
            auto indexExpr = parseExpression(0);
            consume(TokenType::RBRACKET, "Expected ']'");
            left = std::make_unique<BinaryOpNode>("[]", std::move(left),
                                                  std::move(indexExpr));
        } else if (opToken.type == TokenType::LPAREN) {
            if (dynamic_cast<LiteralNode*>(left.get())) {
                if (peek().type == TokenType::IDENTIFIER &&
                    tokens[pos + 1].type == TokenType::COLON) {
                    pos--;
                    pos--;
                    left = parseConstructor();
                    continue;
                }
            }
            auto call = std::make_unique<CallNode>(std::move(left));
            while (peek().type != TokenType::RPAREN && !isAtEnd()) {
                std::string argName = "";
                if (peek().type == TokenType::IDENTIFIER &&
                    tokens[pos + 1].type == TokenType::COLON) {
                    argName = advance().value;
                    advance();
                }
                call->args.push_back({argName, parseExpression(0)});
                if (peek().type == TokenType::COMMA)
                    advance();
            }
            consume(TokenType::RPAREN, "Expected ')'");
            left = std::move(call);
        } else {
            auto right = parseExpression(next_prec);
            left = std::make_unique<BinaryOpNode>(
                opToken.value, std::move(left), std::move(right));
        }
    }
    return left;
}

// --- Statement & Control Flow ---
std::unique_ptr<Node> Parser::parseStatement() {
    TokenType type = peek().type;
    int currentLine = peek().line;

    if (type == TokenType::USE)
        return parseUse();
    if (type == TokenType::FROM)
        return parseUse();
    if (type == TokenType::IF)
        return parseIf();
    if (type == TokenType::WITH)
        return parseWith();
    if (type == TokenType::WHILE)
        return parseWhile();
    if (type == TokenType::FOR)
        return parseFor();
    if (type == TokenType::TRY) 
        return parseTry();
    if (type == TokenType::THROW) 
        return parseThrow();
    if (peek().type == TokenType::DEL) {
        advance();
        return std::make_unique<DeleteNode>(parseExpression(0));
    }
    if (peek().type == TokenType::RETURN) {
        advance();
        std::unique_ptr<Node> expr = nullptr;
        if (peek().type != TokenType::RBRACE)
            expr = parseExpression(0);
        return std::make_unique<ReturnNode>(std::move(expr));
    }
    if (type == TokenType::IDENTIFIER) {
        if (pos + 1 < static_cast<int>(tokens.size())) {
            TokenType next = tokens[pos + 1].type;
            if (next == TokenType::ASSIGN_INIT || next == TokenType::ASSIGN ||
                next == TokenType::MINUS_ASSIGN || next == TokenType::COLON) {
                return parseVarDecl();
            }
        }
        size_t lookahead = pos;
        int balance = 0;
        bool isAssignment = false;
        while (lookahead < tokens.size()) {
            if (tokens[lookahead].line > currentLine)
                break;
            TokenType t = tokens[lookahead].type;
            if (t == TokenType::LBRACKET || t == TokenType::LPAREN)
                balance++;
            else if (t == TokenType::RBRACKET || t == TokenType::RPAREN)
                balance--;
            if (balance == 0 &&
                (t == TokenType::ASSIGN_INIT || t == TokenType::MINUS_ASSIGN ||
                 t == TokenType::ASSIGN)) {
                isAssignment = true;
                break;
            }
            if (t == TokenType::SEMICOLON || t == TokenType::RBRACE)
                break;
            lookahead++;
        }
        if (isAssignment)
            return parseVarDecl();
        if (pos + 1 < static_cast<int>(tokens.size()) &&
            tokens[pos + 1].type == TokenType::LPAREN)
            return parseCall();
    }
    return parseExpression(0);
}

std::unique_ptr<Node> Parser::parseIf() {
    consume(TokenType::IF, "Expected 'if'");
    auto node = std::make_unique<IfNode>();
    node->condition = parseExpression(0);
    consume(TokenType::LBRACE, "Expected '{'");
    while (peek().type != TokenType::RBRACE && !isAtEnd())
        node->thenBranch.push_back(parseStatement());
    consume(TokenType::RBRACE, "Expected '}'");
    while (peek().type == TokenType::ELIF) {
        advance();
        ElIfBlock ei;
        ei.condition = parseExpression(0);
        consume(TokenType::LBRACE, "Expected '{'");
        while (peek().type != TokenType::RBRACE && !isAtEnd())
            ei.body.push_back(parseStatement());
        consume(TokenType::RBRACE, "Expected '}'");
        node->elIfBranches.push_back(std::move(ei));
    }
    if (peek().type == TokenType::ELSE) {
        advance();
        consume(TokenType::LBRACE, "Expected '{'");
        while (peek().type != TokenType::RBRACE && !isAtEnd())
            node->elseBranch.push_back(parseStatement());
        consume(TokenType::RBRACE, "Expected '}'");
    }
    return node;
}

std::unique_ptr<Node> Parser::parseWhile() {
    consume(TokenType::WHILE, "Expected 'while'");
    auto node = std::make_unique<WhileNode>();
    node->condition = parseExpression(0);
    consume(TokenType::LBRACE, "Expected '{'");
    while (peek().type != TokenType::RBRACE && !isAtEnd())
        node->body.push_back(parseStatement());
    consume(TokenType::RBRACE, "Expected '}'");
    return node;
}

std::unique_ptr<Node> Parser::parseFor() {
    consume(TokenType::FOR, "Expected 'for'");
    bool isRef = match(TokenType::REF);
    std::string varName = advance().value;
    consume(TokenType::IN, "Expected 'in'");
    auto node = std::make_unique<ForNode>(varName, isRef, parseExpression(0));
    consume(TokenType::LBRACE, "Expected '{'");
    while (peek().type != TokenType::RBRACE && !isAtEnd())
        node->body.push_back(parseStatement());
    consume(TokenType::RBRACE, "Expected '}'");
    return node;
}

std::unique_ptr<FunctionNode> Parser::parseFunction() {
    bool isExtern = false;
    if (peek().type == TokenType::EXTERN) {
        advance();
        isExtern = true;
    }

    // Check for DEFINE or INIT
    bool isInit = false;
    if (peek().type == TokenType::INIT) {
        advance();
        isInit = true;
    } else {
        consume(TokenType::DEFINE, "Expected 'define' or 'init'");
    }

    std::string name;
    if (isInit) {
        name = "init";
    } else {
        // FIXED: We must explicitly consume the IDENTIFIER token for the name
        if (peek().type == TokenType::IDENTIFIER) {
            name = advance().value;
        } else {
            consume(TokenType::IDENTIFIER,
                    "Expected function name after 'define'");
        }
    }

    auto node = std::make_unique<FunctionNode>();
    node->name = name;
    node->isExtern = isExtern;

    consume(TokenType::LPAREN, "Expected '('");
    while (peek().type != TokenType::RPAREN && !isAtEnd()) {
        Parameter param;

        // 1. Check for Variadic '...'
        if (peek().type == TokenType::ELLIPSIS) {
            advance();
            param.isVariadic = true;
        }

        if (peek().type == TokenType::REF) {
            advance();
            param.isRef = true;
        }

        param.name = advance().value;

        // 2. Parse Type Annotation
        if (peek().type == TokenType::COLON) {
            advance();
            if (peek().type == TokenType::REF) {
                advance();
                param.isRef = true;
            }
            param.type = advance().value;
            while (peek().type == TokenType::PIPE) {
                advance();
                param.type += "|" + advance().value;
            }
        }

        // 3. Validation: Variadic arg must be List
        // The fix is here: we pass `peek()` as the second argument to
        // reportError
        if (param.isVariadic && param.type != "List") {
            reportError("Variadic argument must be typed as List (found '" +
                            param.type + "')",
                        peek());
        }

        if (match(TokenType::ASSIGN)) {
            param.defaultValue = parseExpression(0);
        }

        node->parameters.push_back(std::move(param));
        if (peek().type == TokenType::COMMA)
            advance();
    }
    consume(TokenType::RPAREN, "Expected ')'");
    if (match(TokenType::ARROW))
        node->returnType = advance().value;
    if (isExtern)
        return node;

    consume(TokenType::LBRACE, "Expected '{'");
    while (peek().type != TokenType::RBRACE && !isAtEnd())
        node->body.push_back(parseStatement());
    consume(TokenType::RBRACE, "Expected '}'");
    return node;
}

std::unique_ptr<CallNode> Parser::parseCall() {
    std::string name = advance().value;
    auto node = std::make_unique<CallNode>(std::make_unique<LiteralNode>(name));
    consume(TokenType::LPAREN, "Expected '('");
    while (peek().type != TokenType::RPAREN && !isAtEnd()) {
        std::string argName = "";

        if (peek().type == TokenType::IDENTIFIER &&
            tokens[pos + 1].type == TokenType::COLON) {
            argName = advance().value;
            advance();
        }

        node->args.push_back({argName, parseExpression(0)});
        if (peek().type == TokenType::COMMA)
            advance();
    }
    consume(TokenType::RPAREN, "Expected ')'");
    return node;
}

std::unique_ptr<VarDeclNode> Parser::parseVarDecl() {
    auto lhs = parseExpression(35);
    std::string typeStr = "";
    if (match(TokenType::COLON))
        typeStr = advance().value;
    std::string opStr = advance().value;
    return std::make_unique<VarDeclNode>(std::move(lhs), parseExpression(0),
                                         opStr, typeStr);
}

std::unique_ptr<ConstructorNode> Parser::parseConstructor() {
    std::string typeName = advance().value;
    consume(TokenType::LPAREN, "Expected '('");
    auto node = std::make_unique<ConstructorNode>(typeName);
    while (peek().type != TokenType::RPAREN && !isAtEnd()) {
        std::string field = advance().value;
        consume(TokenType::COLON, "Expected ':'");
        node->args.push_back({field, parseExpression(0)});
        if (peek().type == TokenType::COMMA)
            advance();
    }
    consume(TokenType::RPAREN, "Expected ')'");
    return node;
}

std::unique_ptr<StructNode> Parser::parseStruct() {
    consume(TokenType::STRUCT, "Expected 'struct'");
    auto node = std::make_unique<StructNode>();
    node->name = advance().value;
    consume(TokenType::LBRACE, "Expected '{'");

    while (peek().type != TokenType::RBRACE && !isAtEnd()) {
        if (peek().type == TokenType::USE) {
            advance();
            do {
                node->parents.push_back(advance().value);
            } while (match(TokenType::COMMA));
        }
        // --- UPDATED: Allow EXTERN / DEFINE / INIT ---
        else if (peek().type == TokenType::DEFINE ||
                 peek().type == TokenType::INIT ||
                 peek().type == TokenType::EXTERN) {
            bool isExtern = (peek().type == TokenType::EXTERN);

            // Check if it is an init (either 'init' or 'extern init')
            bool isInit = (peek().type == TokenType::INIT) ||
                          (isExtern && pos + 1 < (int)tokens.size() &&
                           tokens[pos + 1].type == TokenType::INIT);

            // parseFunction handles consuming 'extern', 'define', and 'init'
            // tokens
            auto func = parseFunction();
            func->cls = node->name;

            // Name Mangling
            if (isInit) {
                func->name = node->name + "__init";
            } else {
                // If user wrote 'extern define format', name becomes
                // 'String_format'
                func->name = node->name + "_" + func->name;
            }

            // Safety catch for init naming
            if (!isInit && func->name.find("__init") != std::string::npos)
                func->name = node->name + "__init";

            // Static vs Instance logic (check for 'self')
            if (!func->parameters.empty() &&
                func->parameters[0].name == "self") {
                func->parameters.erase(func->parameters.begin());
                func->isStatic = false;
            } else {
                func->isStatic = true;
            }
            extraNodes.push_back(std::move(func));
        }
        // --- End Update ---
        else {
            std::string fName = advance().value;
            if (peek().type == TokenType::ASSIGN_INIT) {
                advance();
                auto val = parseExpression(0);
                std::string iType = "int";
                if (auto lit = dynamic_cast<LiteralNode*>(val.get())) {
                    if (lit->value[0] == '"')
                        iType = "string";
                }
                node->fields.push_back({fName, iType, std::move(val)});
            } else {
                consume(TokenType::COLON, "Expected ':'");
                node->fields.push_back({fName, advance().value, nullptr});
            }
        }
    }
    consume(TokenType::RBRACE, "Expected '}'");
    return node;
}


std::unique_ptr<Node> Parser::parseUse() {
    std::string path = "";
    std::vector<std::string> filters;

    // Helper to parse the path "core.sys" -> "core/sys" OR ".sys" -> ".sys"
    auto parsePath = [&]() {
        // 1. Handle Leading Dots (Relative Imports)
        while (match(TokenType::DOT)) {
            path += ".";
        }

        // 2. Read first identifier segment
        if (peek().type == TokenType::IDENTIFIER) {
            path += advance().value;
        }

        // 3. Read subsequent segments
        while (match(TokenType::DOT)) {
            // We use '/' for internal representation of separators, 
            // but keep the leading '.' for relative detection.
            path += "/" + advance().value;
        }
    };

    if (match(TokenType::FROM)) {
        parsePath();
        consume(TokenType::USE, "Expected 'use' after module path");
        consume(TokenType::LBRACE, "Expected '{'");
        do {
            filters.push_back(advance().value);
        } while (match(TokenType::COMMA));
        consume(TokenType::RBRACE, "Expected '}'");
    } 
    else {
        consume(TokenType::USE, "Expected 'use'");
        parsePath();
    }

    return std::make_unique<UseNode>(path, filters);
}

std::unique_ptr<Node> Parser::parseWith() {
    consume(TokenType::WITH, "Expected 'with'");
    auto node = std::make_unique<WithNode>();

    // 1. Parse Resource (e.g., File.open(...))
    node->resource = parseExpression(0);

    // 2. Parse 'as <identifier>'
    consume(TokenType::AS, "Expected 'as'");
    node->varName = advance().value;  // Get the variable name

    // 3. Parse Body
    consume(TokenType::LBRACE, "Expected '{'");
    while (peek().type != TokenType::RBRACE && !isAtEnd()) {
        node->body.push_back(parseStatement());
    }
    consume(TokenType::RBRACE, "Expected '}'");

    return node;
}

std::unique_ptr<Node> Parser::parseMapLiteral() {
    auto node = std::make_unique<MapLiteralNode>();
    consume(TokenType::LBRACE, "Expected '{'");

    // Check for empty map "{}"
    if (match(TokenType::RBRACE)) {
        return node;
    }

    do {
        // Parse Key
        auto key = parseExpression(0);
        consume(TokenType::COLON, "Expected ':' after map key");
        
        // Parse Value
        auto value = parseExpression(0);
        
        node->elements.push_back({std::move(key), std::move(value)});

    } while (match(TokenType::COMMA)); // Continue if comma found

    consume(TokenType::RBRACE, "Expected '}' after map literal");
    return node;
}

void Parser::reportError(const std::string& message, const Token& token) {
    std::cerr << "\033[1;31m[ERROR]\033[0m " << message << " at line "
              << token.line << ":\n\n";
    size_t lineStart = 0;
    int currentLine = 1;
    for (size_t i = 0; i < source.length(); i++) {
        if (currentLine == token.line) {
            lineStart = i;
            break;
        }
        if (source[i] == '\n')
            currentLine++;
    }
    size_t lineEnd = source.find('\n', lineStart);
    if (lineEnd == std::string::npos)
        lineEnd = source.length();
    std::string lineCode = source.substr(lineStart, lineEnd - lineStart);
    std::cerr << "    " << lineCode << "\n";
    std::cerr << "    \033[1;33m^--- Here\033[0m\n\n";
    exit(1);
}

std::unique_ptr<Node> Parser::parseTry() {
    consume(TokenType::TRY, "Expected 'try'");
    auto node = std::make_unique<TryCatchNode>();
    
    consume(TokenType::LBRACE, "Expected '{' after try");
    while (peek().type != TokenType::RBRACE && !isAtEnd()) {
        node->tryBlock.push_back(parseStatement());
    }
    consume(TokenType::RBRACE, "Expected '}'");

    consume(TokenType::CATCH, "Expected 'catch'");
    consume(TokenType::LPAREN, "Expected '('");
    node->catchVar = advance().value;
    consume(TokenType::COLON, "Expected ':'");
    node->catchType = advance().value;
    consume(TokenType::RPAREN, "Expected ')'");

    consume(TokenType::LBRACE, "Expected '{' after catch");
    while (peek().type != TokenType::RBRACE && !isAtEnd()) {
        node->catchBlock.push_back(parseStatement());
    }
    consume(TokenType::RBRACE, "Expected '}'");
    
    return node;
}

std::unique_ptr<Node> Parser::parseThrow() {
    consume(TokenType::THROW, "Expected 'throw'");
    return std::make_unique<ThrowNode>(parseExpression(0));
}
