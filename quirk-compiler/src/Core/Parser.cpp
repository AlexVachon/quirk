#include <algorithm>
#include <iostream>
#include "parser.hpp"

Parser::Parser(const std::vector<Token>& tokens, const std::string& source, const std::string& filePath)
    : tokens(tokens), source(source), filePath(filePath) {}

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

            for (auto& extra : extraNodes)
                nodes.push_back(std::move(extra));
            extraNodes.clear();

        } else if (type == TokenType::ENUM) {
            nodes.push_back(parseEnum());
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
            for (auto& extra : extraNodes)
                nodes.push_back(std::move(extra));
            extraNodes.clear();
        }
    }
    return nodes;
}

// --- Expression Parsing ---
static bool canStartTernaryExpr(TokenType t) {
    return t == TokenType::INT_LITERAL    || t == TokenType::FLOAT_LITERAL  ||
           t == TokenType::STRING_LITERAL || t == TokenType::CHAR_LITERAL   ||
           t == TokenType::TRUE           || t == TokenType::FALSE          ||
           t == TokenType::QUIRK_NULL     || t == TokenType::SUPER          ||
           t == TokenType::IDENTIFIER     || t == TokenType::NOT            ||
           t == TokenType::MINUS          || t == TokenType::LBRACKET       ||
           t == TokenType::LPAREN         || t == TokenType::FN;
    // LBRACE excluded: avoids ambiguity with block `{` in `if cond? { ... }`
}

int getPrecedence(TokenType type) {
    switch (type) {
        case TokenType::NULL_COALESCE:
            return 3;
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
        case TokenType::IS:
        case TokenType::IN:
            return 10;
        case TokenType::PLUS:
        case TokenType::MINUS:
            return 20;
        case TokenType::STAR:
        case TokenType::SLASH:
            return 30;
        case TokenType::DOT:
        case TokenType::QUESTION_DOT:
        case TokenType::QUESTION:
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
    } else if (t.type == TokenType::CHAR_LITERAL) {
        left = std::make_unique<LiteralNode>(t.value);
    } else if (t.type == TokenType::TRUE) {
        left = std::make_unique<LiteralNode>("true");
    } else if (t.type == TokenType::FALSE) {
        left = std::make_unique<LiteralNode>("false");
    } else if (t.type == TokenType::QUIRK_NULL) {
        left = std::make_unique<LiteralNode>("null");
    } else if (t.type == TokenType::SUPER) {
        left = std::make_unique<LiteralNode>("super");
    }else if (t.type == TokenType::IDENTIFIER) {
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
    } else if (t.type == TokenType::FN) {
        auto lambda = std::make_unique<LambdaNode>();
        consume(TokenType::LPAREN, "Expected '(' after 'fn'");
        while (peek().type != TokenType::RPAREN && !isAtEnd()) {
            LambdaParam p;
            p.name = advance().value;
            if (match(TokenType::COLON))
                p.type = advance().value;
            lambda->params.push_back(std::move(p));
            if (!match(TokenType::COMMA)) break;
        }
        consume(TokenType::RPAREN, "Expected ')' after lambda params");
        if (peek().type == TokenType::FAT_ARROW) {
            advance();
            lambda->isExpression = true;
            lambda->exprBody = parseExpression(0);
        } else {
            consume(TokenType::LBRACE, "Expected '=>' or '{' after lambda params");
            lambda->isExpression = false;
            while (peek().type != TokenType::RBRACE && !isAtEnd())
                lambda->stmtBody.push_back(parseStatement());
            consume(TokenType::RBRACE, "Expected '}' to close lambda body");
        }
        left = std::move(lambda);
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

        if (opToken.type == TokenType::DOT || opToken.type == TokenType::QUESTION_DOT) {
            Token member = advance();
            auto memberNode = std::make_unique<MemberAccessNode>(std::move(left), member.value);
            memberNode->line = member.line;
            memberNode->col  = member.col;
            memberNode->isSafeAccess = (opToken.type == TokenType::QUESTION_DOT);
            left = std::move(memberNode);
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
            call->line = opToken.line;
            call->col  = opToken.col;
            while (peek().type != TokenType::RPAREN && !isAtEnd()) {
                std::string argName = "";
                if (peek().type == TokenType::IDENTIFIER &&
                    tokens[pos + 1].type == TokenType::COLON) {
                    argName = advance().value;
                    advance();
                }
                
                Arg newArg;
                newArg.name = argName;
                newArg.value = parseExpression(0);
                call->args.push_back(std::move(newArg));
                
                if (peek().type == TokenType::COMMA)
                    advance();
            }
            consume(TokenType::RPAREN, "Expected ')'");
            left = std::move(call);
        } else if (opToken.type == TokenType::IS) {
            // `val is TypeName` — consume the type name as a string literal
            // so codegen can call Core_Primitives_Any_isinstance(val, "TypeName")
            Token typeName = advance();
            auto typeNode = std::make_unique<LiteralNode>(typeName.value);
            left = std::make_unique<BinaryOpNode>("is", std::move(left), std::move(typeNode));
        } else if (opToken.type == TokenType::QUESTION) {
            if (canStartTernaryExpr(peek().type)) {
                // Ternary: condition? thenExpr : elseExpr
                auto thenExpr = parseExpression(0);
                consume(TokenType::COLON, "Expected ':' in ternary expression");
                auto elseExpr = parseExpression(0);
                left = std::make_unique<TernaryNode>(std::move(left), std::move(thenExpr), std::move(elseExpr));
            } else {
                // Postfix `?` — null/zero check: expr? → true if expr is not null
                left = std::make_unique<BinaryOpNode>("?", std::move(left), nullptr);
            }
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
    if (type == TokenType::MATCH)
        return parseMatch();
    if (type == TokenType::THROW) 
        return parseThrow();
    if (type == TokenType::TRIGGER)
        return parseTrigger();
    if (type == TokenType::BREAK) {
        advance();
        return std::make_unique<BreakNode>();
    }
    if (type == TokenType::CONTINUE) {
        advance();
        return std::make_unique<ContinueNode>();
    }
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
                (t == TokenType::ASSIGN_INIT || t == TokenType::ASSIGN ||
                 t == TokenType::MINUS_ASSIGN || t == TokenType::PLUS_ASSIGN || 
                 t == TokenType::STAR_ASSIGN || t == TokenType::SLASH_ASSIGN)) {
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

// Computes the full hierarchical module prefix from this->filePath.
// libs/core/string.qk           -> "Core_String"
// libs/core/collections/list.qk -> "Core_Collections_List"
// libs/sys/__init.qk            -> "Sys"
// userfile.qk                   -> "Userfile"
std::string Parser::computeModulePrefix() const {
    std::string p = this->filePath;
    if (p.size() >= 3 && p.substr(p.size() - 3) == ".qk")
        p = p.substr(0, p.size() - 3);
    for (char& c : p) if (c == '\\') c = '/';

    std::vector<std::string> parts;
    std::string seg;
    for (char c : p) {
        if (c == '/') {
            // Skip empty segments and "." / ".." path components
            if (!seg.empty() && seg != "." && seg != "..")
                parts.push_back(seg);
            seg.clear();
        } else {
            seg += c;
        }
    }
    if (!seg.empty() && seg != "." && seg != "..") parts.push_back(seg);

    // Find "libs" anywhere in the path (handles ./libs/... or /abs/path/libs/...)
    auto libsIt = std::find(parts.begin(), parts.end(), "libs");
    if (libsIt != parts.end()) {
        parts.erase(parts.begin(), libsIt + 1);   // drop everything up to and including "libs"
        if (!parts.empty() && parts.back() == "__init")
            parts.pop_back();                      // drop trailing "__init"
    } else {
        // User file — just use the filename as a single-component prefix
        std::string last = parts.empty() ? "" : parts.back();
        if (last == "__init" && parts.size() > 1) last = parts[parts.size() - 2];
        parts = { last };
    }

    for (auto& part : parts)
        if (!part.empty()) part[0] = (char)std::toupper((unsigned char)part[0]);

    std::string result;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i > 0) result += "_";
        result += parts[i];
    }
    return result;
}

std::unique_ptr<FunctionNode> Parser::parseFunction() {
    bool isExtern = false;
    if (peek().type == TokenType::EXTERN) {
        advance();
        isExtern = true;
    }

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

    // --- AUTOMATIC FFI MANGLING ---
    // Derives the full hierarchical module prefix from this->filePath.
    //
    // Naming convention (Option B — struct name always included):
    //   libs/core/string.qk           -> modulePrefix = "Core_String"
    //   libs/core/primitives.qk       -> modulePrefix = "Core_Primitives"
    //   libs/core/collections/list.qk -> modulePrefix = "Core_Collections_List"
    //   libs/core/collections/map.qk  -> modulePrefix = "Core_Collections_Map"
    //   libs/sys/__init.qk            -> modulePrefix = "Sys"
    //   libs/io/file.qk               -> modulePrefix = "Io_File"
    //   userfile.qk  (non-libs)       -> modulePrefix = "Userfile"
    //
    // Global extern:  linkageName = modulePrefix + "_" + name
    //   e.g. extern define float_to_str  ->  "Core_String_float_to_str"
    //        extern define arg_count      ->  "Sys_arg_count"
    //
    // Extern struct method (handled in parseStruct):
    //   linkageName = modulePrefix + "_" + structName + "_" + rawMethodName
    //   e.g. Core_String_String_to_float, Core_Collections_List_List_append
    std::string modulePrefix = computeModulePrefix();

    if (isExtern) {
        node->linkageName = modulePrefix + "_" + name;
    } else {
        node->linkageName = name;
    }
    // --------------------------------

    consume(TokenType::LPAREN, "Expected '('");
    while (peek().type != TokenType::RPAREN && !isAtEnd()) {
        Parameter param;

        if (peek().type == TokenType::ELLIPSIS) {
            advance();
            param.isVariadic = true;
        }

        if (peek().type == TokenType::REF) {
            advance();
            param.isRef = true;
        }

        param.name = advance().value;

        if (peek().type == TokenType::COLON) {
            advance();
            if (peek().type == TokenType::REF) {
                advance();
                param.isRef = true;
            }
            param.type = advance().value;
            if (peek().type == TokenType::QUESTION) { advance(); param.type += "?"; }
            while (peek().type == TokenType::PIPE) {
                advance();
                param.type += "|" + advance().value;
            }
        }

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
    if (match(TokenType::ARROW)) {
        node->returnType = advance().value;
        if (peek().type == TokenType::QUESTION) { advance(); node->returnType += "?"; }
    }
    if (isExtern)
        return node;

    consume(TokenType::LBRACE, "Expected '{'");
    while (peek().type != TokenType::RBRACE && !isAtEnd())
        node->body.push_back(parseStatement());
    consume(TokenType::RBRACE, "Expected '}'");
    return node;
}

std::unique_ptr<CallNode> Parser::parseCall() {
    Token nameTok = advance();
    auto node = std::make_unique<CallNode>(std::make_unique<LiteralNode>(nameTok.value));
    node->line = nameTok.line;
    node->col  = nameTok.col;
    consume(TokenType::LPAREN, "Expected '('");
    while (peek().type != TokenType::RPAREN && !isAtEnd()) {
        std::string argName = "";

        if (peek().type == TokenType::IDENTIFIER &&
            tokens[pos + 1].type == TokenType::COLON) {
            argName = advance().value;
            advance();
        }

        Arg newArg;
        newArg.name = argName;
        newArg.value = parseExpression(0);
        node->args.push_back(std::move(newArg));
        
        if (peek().type == TokenType::COMMA)
            advance();
    }
    consume(TokenType::RPAREN, "Expected ')'");
    return node;
}

std::unique_ptr<VarDeclNode> Parser::parseVarDecl() {
    auto lhs = parseExpression(35);
    std::string typeStr = "";
    if (match(TokenType::COLON)) {
        typeStr = advance().value;
        if (peek().type == TokenType::QUESTION) { advance(); typeStr += "?"; }
    }
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
        
        ConstructorArg newArg;
        newArg.fieldName = field;
        newArg.value = parseExpression(0);
        node->args.push_back(std::move(newArg));
        
        if (peek().type == TokenType::COMMA)
            advance();
    }
    consume(TokenType::RPAREN, "Expected ')'");
    return node;
}

// [Parser.cpp] - inside parseStruct()

std::unique_ptr<StructNode> Parser::parseStruct() {
    consume(TokenType::STRUCT, "Expected 'struct'");
    auto node = std::make_unique<StructNode>();
    node->name = advance().value;

    if (match(TokenType::COLON)) {
        do {
            node->parents.push_back(advance().value);
        } while (match(TokenType::COMMA));
    }

    consume(TokenType::LBRACE, "Expected '{'");

    // --- NEW: Store dynamic default initializations ---
    std::vector<std::unique_ptr<Node>> defaultInits;
    // --------------------------------------------------

    while (peek().type != TokenType::RBRACE && !isAtEnd()) {
        
        if (peek().type == TokenType::USE) {
            advance();
            do {
                node->parents.push_back(advance().value);
            } while (match(TokenType::COMMA));
        }
        else if (peek().type == TokenType::DEFINE ||
                 peek().type == TokenType::INIT ||
                 peek().type == TokenType::EXTERN) {
            bool isExtern = (peek().type == TokenType::EXTERN);
            bool isInit = (peek().type == TokenType::INIT) ||
                          (isExtern && pos + 1 < (int)tokens.size() &&
                           tokens[pos + 1].type == TokenType::INIT);

            auto func = parseFunction();
            func->cls = node->name;

            // Save raw method name BEFORE func->name gets the struct prefix prepended.
            // For extern methods we reconstruct:
            //   linkageName = modulePrefix + "_" + structName + "_" + rawMethodName
            // e.g. extern define to_float in struct String in libs/core/string.qk
            //   -> "Core_String" + "_" + "String" + "_" + "to_float"
            //   -> "Core_String_String_to_float"
            std::string rawMethodName = func->name; // e.g. "to_float", "__add", "__init"

            if (isInit) {
                func->name = node->name + "__init";
            } else {
                func->name = node->name + "_" + func->name;
            }

            if (!isInit && func->name.find("__init") != std::string::npos) {
                func->name = node->name + "__init";
            }

            if (isExtern) {
                // modulePrefix is the same for all methods in this file.
                std::string mp = computeModulePrefix();
                func->linkageName = mp + "_" + node->name + "_" + rawMethodName;
            } else {
                func->linkageName = func->name;
            }

            if (!func->parameters.empty() &&
                func->parameters[0].name == "self") {
                func->parameters.erase(func->parameters.begin());
                func->isStatic = false;
            } else {
                func->isStatic = true;
            }
            extraNodes.push_back(std::move(func));
        }
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
                std::string fType = advance().value;
                node->fields.push_back({fName, fType, nullptr});

                // --- NEW: Parse default property values ---
                if (match(TokenType::ASSIGN)) {
                    auto defaultExpr = parseExpression(0);
                    
                    auto selfLit = std::make_unique<LiteralNode>("self");
                    auto memberAcc = std::make_unique<MemberAccessNode>(std::move(selfLit), fName);
                    auto varDecl = std::make_unique<VarDeclNode>(std::move(memberAcc), std::move(defaultExpr), "=", "");
                    
                    defaultInits.push_back(std::move(varDecl));
                }
                // ------------------------------------------
            }
        }
    }
    consume(TokenType::RBRACE, "Expected '}'");

    // --- NEW: Inject default initializations into __init AST ---
    if (!defaultInits.empty()) {
        FunctionNode* initFunc = nullptr;
        for (auto& extra : extraNodes) {
            if (auto func = dynamic_cast<FunctionNode*>(extra.get())) {
                if (func->name == node->name + "__init") {
                    initFunc = func;
                    break;
                }
            }
        }
        if (initFunc) {
            // Prepend defaults to existing explicit __init
            std::vector<std::unique_ptr<Node>> newBody;
            for (auto& n : defaultInits) newBody.push_back(std::move(n));
            for (auto& n : initFunc->body) newBody.push_back(std::move(n));
            initFunc->body = std::move(newBody);
        } else {
            // Synthesize a hidden __init if none exists
            auto synthInit = std::make_unique<FunctionNode>();
            synthInit->name = node->name + "__init";
            synthInit->linkageName = synthInit->name;
            synthInit->cls = node->name;
            synthInit->isStatic = false;
            synthInit->returnType = "void";
            synthInit->body = std::move(defaultInits);
            extraNodes.push_back(std::move(synthInit));
        }
    }
    // -----------------------------------------------------------

    return node;
}

std::unique_ptr<EnumNode> Parser::parseEnum() {
    consume(TokenType::ENUM, "Expected 'enum'");
    auto node = std::make_unique<EnumNode>();
    Token nameTok = peek();
    consume(TokenType::IDENTIFIER, "Expected enum name");
    node->name = nameTok.value;
    node->line = nameTok.line;
    node->col  = nameTok.col;
    consume(TokenType::LBRACE, "Expected '{' after enum name");
    while (peek().type == TokenType::IDENTIFIER) {
        node->variants.push_back(advance().value);
    }
    consume(TokenType::RBRACE, "Expected '}' to close enum");
    return node;
}

std::unique_ptr<Node> Parser::parseUse() {
    std::string path = "";
    std::vector<std::string> filters;

    auto parsePath = [&]() {
        // Consume leading dots for relative imports (e.g. '.' or '...' or ELLIPSIS)
        while (peek().type == TokenType::DOT || peek().type == TokenType::ELLIPSIS) {
            if (peek().type == TokenType::ELLIPSIS) {
                advance();
                path += "...";
            } else {
                advance();
                path += ".";
            }
        }
        if (peek().type == TokenType::IDENTIFIER) {
            path += advance().value;
        }
        while (match(TokenType::DOT)) {
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

    node->resource = parseExpression(0);
    consume(TokenType::AS, "Expected 'as'");
    node->varName = advance().value;

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

    if (match(TokenType::RBRACE)) {
        return node;
    }

    do {
        auto key = parseExpression(0);
        consume(TokenType::COLON, "Expected ':' after map key");
        auto value = parseExpression(0);
        node->elements.push_back({std::move(key), std::move(value)});

    } while (match(TokenType::COMMA));

    consume(TokenType::RBRACE, "Expected '}' after map literal");
    return node;
}

void Parser::reportError(const std::string& message, const Token& token) {
    std::cerr << "\033[1;31m[ERROR]\033[0m " << message
              << " at line " << token.line << ", col " << token.col << ":\n\n";
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
    int caretOffset = (token.col > 0) ? token.col - 1 : 0;
    std::cerr << "    " << std::string(caretOffset, ' ') << "\033[1;33m^--- Here\033[0m\n\n";
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

    while (peek().type == TokenType::CATCH) {
        advance();
        CatchBlock cb;

        consume(TokenType::LPAREN, "Expected '('");
        // Support both  catch (e: Type)  and type-only  catch (Type)
        std::string first = advance().value;
        if (first == "`") first = advance().value;
        if (match(TokenType::COLON)) {
            // Named form: catch (e: Type, Type2, ...)
            cb.varName = first;
            do {
                std::string t = advance().value;
                if (t == "`") t = advance().value;
                cb.types.push_back(t);
            } while (match(TokenType::COMMA));
        } else {
            // Anonymous form: catch (Type, Type2, ...) — no binding variable
            cb.varName = "";
            cb.types.push_back(first);
            while (match(TokenType::COMMA)) {
                std::string t = advance().value;
                if (t == "`") t = advance().value;
                cb.types.push_back(t);
            }
        }

        consume(TokenType::RPAREN, "Expected ')'");

        consume(TokenType::LBRACE, "Expected '{' after catch");
        while (peek().type != TokenType::RBRACE && !isAtEnd()) {
            cb.body.push_back(parseStatement());
        }
        consume(TokenType::RBRACE, "Expected '}'");

        node->catchBlocks.push_back(std::move(cb));
    }

    if (match(TokenType::FINALLY)) {
        consume(TokenType::LBRACE, "Expected '{' after finally");
        while (peek().type != TokenType::RBRACE && !isAtEnd()) {
            node->finallyBlock.push_back(parseStatement());
        }
        consume(TokenType::RBRACE, "Expected '}'");
    }

    return node;
}

std::unique_ptr<Node> Parser::parseMatch() {
    consume(TokenType::MATCH, "Expected 'match'");
    auto node = std::make_unique<MatchNode>();
    node->scrutinee = parseExpression(0);
    consume(TokenType::LBRACE, "Expected '{' after match expression");

    while (peek().type != TokenType::RBRACE && !isAtEnd()) {
        consume(TokenType::CASE, "Expected 'case' in match block");
        MatchArm arm;

        // Parse comma-separated patterns; _ is the wildcard
        do {
            if (peek().type == TokenType::IDENTIFIER && peek().value == "_") {
                advance();
                arm.isWildcard = true;
                break;  // nothing further to parse for patterns
            }
            arm.patterns.push_back(parseExpression(2));  // prec=2: allows member access, not comma
        } while (match(TokenType::COMMA));

        // Body: `=> stmt` (single statement) or `{ stmts }` (block)
        if (match(TokenType::FAT_ARROW)) {
            arm.body.push_back(parseStatement());
        } else {
            consume(TokenType::LBRACE, "Expected '=>' or '{' after case pattern");
            while (peek().type != TokenType::RBRACE && !isAtEnd())
                arm.body.push_back(parseStatement());
            consume(TokenType::RBRACE, "Expected '}'");
        }

        node->arms.push_back(std::move(arm));
    }

    consume(TokenType::RBRACE, "Expected '}' to close match block");
    return node;
}

std::unique_ptr<Node> Parser::parseThrow() {
    int lineNum = peek().line;
    consume(TokenType::THROW, "Expected 'throw'");

    // Bare throw: re-raises the current exception without arguments.
    // Detected when the next token cannot begin an expression.
    TokenType next = peek().type;
    bool canStartExpr = (next == TokenType::IDENTIFIER || next == TokenType::INT_LITERAL  ||
                         next == TokenType::FLOAT_LITERAL || next == TokenType::STRING_LITERAL ||
                         next == TokenType::CHAR_LITERAL || next == TokenType::LPAREN         ||
                         next == TokenType::LBRACKET      || next == TokenType::LBRACE         ||
                         next == TokenType::MINUS         || next == TokenType::NOT            ||
                         next == TokenType::TRUE          || next == TokenType::FALSE          ||
                         next == TokenType::QUIRK_NULL);
    if (!canStartExpr) {
        auto node = std::make_unique<ThrowNode>(nullptr, nullptr, lineNum);
        node->moduleName = this->filePath;
        return node;
    }

    auto expr = parseExpression(0);
    std::unique_ptr<Node> causeExpr = nullptr;
    
    if (match(TokenType::FROM)) {
        causeExpr = parseExpression(0);
    }
    
    auto node = std::make_unique<ThrowNode>(std::move(expr), std::move(causeExpr), lineNum);
    node->moduleName = this->filePath; 
    
    return node;
}

std::unique_ptr<Node> Parser::parseTrigger() {
    consume(TokenType::TRIGGER, "Expected 'trigger'");
    
    if (peek().type != TokenType::IDENTIFIER) {
        reportError("Expected variable name after 'trigger'", peek());
    }
    
    std::string varName = advance().value;
    while (match(TokenType::DOT)) {
        if (peek().type != TokenType::IDENTIFIER) {
            reportError("Expected property name after '.'", peek());
        }
        varName += "." + advance().value;
    }

    auto lambda = std::make_unique<FunctionNode>();
    static int triggerCount = 0;
    
    std::string safeVarName = varName;
    std::replace(safeVarName.begin(), safeVarName.end(), '.', '_');
    
    std::string safeHandlerName = "__quirk_trigger_" + safeVarName + "_" + std::to_string(triggerCount++);
    lambda->name = safeHandlerName;

    std::string newParamName = "it";
    std::string oldParamName = "was";

    if (match(TokenType::LPAREN)) {
        newParamName = advance().value; 
        if (match(TokenType::COMMA)) {
            oldParamName = advance().value; 
        }
        consume(TokenType::RPAREN, "Expected ')' after trigger parameters");
    }

    size_t dotPos = varName.find('.');
    if (dotPos != std::string::npos) {
        Parameter objParam;
        objParam.name = varName.substr(0, dotPos);
        objParam.type = "Any"; 
        lambda->parameters.push_back(std::move(objParam));
    }

    Parameter pNew;
    pNew.name = newParamName;
    pNew.type = "Any"; 
    lambda->parameters.push_back(std::move(pNew)); 

    Parameter pOld;
    pOld.name = oldParamName;
    pOld.type = "Any"; 
    lambda->parameters.push_back(std::move(pOld));

    consume(TokenType::LBRACE, "Expected '{'");
    while (peek().type != TokenType::RBRACE && !isAtEnd()) {
        lambda->body.push_back(parseStatement());
    }
    consume(TokenType::RBRACE, "Expected '}'");

    FunctionNode* rawLambda = lambda.get();
    extraNodes.push_back(std::move(lambda));

    return std::make_unique<TriggerNode>(varName, safeHandlerName, rawLambda);
}