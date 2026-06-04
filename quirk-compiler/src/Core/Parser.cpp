#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include "parser.hpp"
#include "../PackageManager.hpp"

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
        // Stray semicolons act as empty statement separators — lets one-liners
        // like `a := 1; print(a)` work (matching Python's same-line statement
        // separator). The Lexer emits SEMICOLON tokens; the Parser is
        // newline-driven otherwise, so we just eat them here.
        while (peek().type == TokenType::SEMICOLON) advance();
        if (isAtEnd()) break;

        // Decorator lines: `@expr` (zero or more) attach to the next `define`.
        // The expression after `@` is a normal expression — bare name, member
        // access, or call — terminated by EOL or the next `@`/`define`.
        std::vector<std::unique_ptr<Node>> pendingDecorators;
        try {
            while (peek().type == TokenType::AT) {
                Token at = advance();
                auto dec = parseExpression(0);
                if (!dec) {
                    reportError("Expected expression after '@'", at);
                }
                pendingDecorators.push_back(std::move(dec));
                while (peek().type == TokenType::SEMICOLON) advance();
            }
            if (!pendingDecorators.empty()) {
                // Decorators must be followed by a `define` (or `extern define`).
                TokenType nxt = peek().type;
                if (nxt != TokenType::DEFINE && nxt != TokenType::EXTERN) {
                    reportError("Decorators must precede a `define` (found "
                                + peek().value + ")", peek());
                }
            }
        } catch (ParseError&) {
            pendingDecorators.clear();
            sync();
            continue;
        }

        TokenType type = peek().type;
        try {
            if (type == TokenType::DEFINE || type == TokenType::INIT ||
                type == TokenType::EXTERN) {
                auto fn = parseFunction();
                fn->decorators = std::move(pendingDecorators);
                if (!fn->decorators.empty()) {
                    // ─── Decorator rewriting ───────────────────────────────
                    // `@a @b define f(x: T) -> R { body }` desugars to
                    //   define f__inner__(x: T) -> R { body }
                    //   define f(x: T) -> R { return a(b(f__inner__))(x) }
                    //
                    // Decorators apply innermost-first (bottom-up): the LAST
                    // decorator in source order wraps the inner function, then
                    // each earlier decorator wraps that. Mirrors Python.
                    //
                    // Cost: the decorator chain is re-evaluated on every call.
                    // That's correct for stateless decorators (@logged, @timed)
                    // but resets per-call state in stateful ones (@cached).
                    // A future Phase 5b can cache the wrapped Callable in a
                    // module-level global initialized at startup.
                    std::string innerName = fn->name + "__inner__";

                    // 1. Inner function: same signature + body, renamed.
                    auto inner = std::make_unique<FunctionNode>();
                    inner->name       = innerName;
                    inner->cls        = fn->cls;
                    inner->returnType = fn->returnType;
                    inner->isExtern   = fn->isExtern;
                    inner->isStatic   = fn->isStatic;
                    inner->isAbstract = fn->isAbstract;
                    inner->linkageName = "";  // let codegen mangle from name
                    inner->parameters = std::move(fn->parameters);
                    inner->body       = std::move(fn->body);

                    // 2. Rebuild wrapper's parameter list (same names/types,
                    // no default values — those live on the inner).
                    std::vector<Parameter> wrapperParams;
                    for (auto& p : inner->parameters) {
                        Parameter q;
                        q.name = p.name;
                        q.type = p.type;
                        q.isRef = p.isRef;
                        q.isVariadic = p.isVariadic;
                        wrapperParams.push_back(std::move(q));
                    }
                    fn->parameters = std::move(wrapperParams);

                    // 3. Build the decorator chain: innermost is a reference to
                    // <innerName>, then each decorator (in reverse source
                    // order) wraps the prior chain. Result: `a(b(c(inner)))`.
                    std::unique_ptr<Node> chain = std::make_unique<LiteralNode>(innerName);
                    for (auto it = fn->decorators.rbegin();
                              it != fn->decorators.rend(); ++it) {
                        auto callExpr = std::make_unique<CallNode>(std::move(*it));
                        Arg argEntry;
                        argEntry.value = std::move(chain);
                        callExpr->args.push_back(std::move(argEntry));
                        chain = std::move(callExpr);
                    }
                    fn->decorators.clear();

                    // 4. Mark the wrapper and stash the chain expression for
                    // Codegen. The wrapper's *body* is left empty — Codegen
                    // synthesizes a lazy-init + cached-dispatch IR sequence
                    // around a module-internal Callable* global. That keeps
                    // stateful decorators (`@cached`) working across calls.
                    fn->body.clear();
                    fn->isDecoratorWrapper = true;
                    fn->decoratorChainExpr = std::move(chain);

                    nodes.push_back(std::move(inner));
                }
                nodes.push_back(std::move(fn));
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
            } else if (type == TokenType::TYPE_KW) {
                advance(); // consume 'type'
                std::string aliasName = advance().value;
                consume(TokenType::ASSIGN, "Expected '=' after type alias name");
                // Collect the target type (may include | for unions)
                std::string target = advance().value;
                while (peek().type == TokenType::PIPE) {
                    advance(); // consume '|'
                    target += "|" + advance().value;
                }
                auto node = std::make_unique<TypeAliasNode>();
                node->name = aliasName;
                node->target = target;
                nodes.push_back(std::move(node));
            } else if (type == TokenType::INTERFACE) {
                nodes.push_back(parseInterface());
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
        } catch (ParseError&) {
            extraNodes.clear();
            sync();
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
        case TokenType::IF:
            return 2;
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
        case TokenType::PERCENT:
            return 30;
        case TokenType::DOTDOT:
            return 4; // range: binds loosely so `1+2..5+3` works as (1+2)..(5+3)
        case TokenType::AS:
            return 35;
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
    } else if (t.type == TokenType::IDENTIFIER || t.type == TokenType::TYPE_KW) {
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
        if (peek().type == TokenType::RBRACKET) {
            advance();
            left = std::make_unique<ListLiteralNode>(std::vector<std::unique_ptr<Node>>{});
        } else {
            auto firstExpr = parseExpression(0);
            if (peek().type == TokenType::FOR) {
                // List comprehension: [expr for var in iterable (where cond)]
                advance(); // consume 'for'
                std::string varName = advance().value;
                std::string varName2;
                if (peek().type == TokenType::COMMA) { advance(); varName2 = advance().value; }
                consume(TokenType::IN, "Expected 'in' in list comprehension");
                // Parse the iterable at precedence 3 so a trailing `if`
                // (precedence 2) is left for the filter clause rather than
                // being eaten by the ternary-style `expr if cond else …`
                // operator. Anything tighter (`==`, `+`, `.`) still binds.
                auto iterable = parseExpression(3);
                std::unique_ptr<Node> condition;
                // Both `where` and `if` introduce the filter clause —
                // `where` was original, `if` matches Python and reads more
                // naturally inside `[…]`.
                if (peek().type == TokenType::WHERE || peek().type == TokenType::IF) {
                    advance();
                    condition = parseExpression(0);
                }
                consume(TokenType::RBRACKET, "Expected ']' after list comprehension");
                auto comp = std::make_unique<ListComprehensionNode>();
                comp->expr      = std::move(firstExpr);
                comp->varName   = varName;
                comp->varName2  = varName2;
                comp->iterable  = std::move(iterable);
                comp->condition = std::move(condition);
                comp->line = t.line; comp->col = t.col; comp->filePath = filePath;
                left = std::move(comp);
            } else {
                std::vector<std::unique_ptr<Node>> elements;
                elements.push_back(std::move(firstExpr));
                while (match(TokenType::COMMA))
                    elements.push_back(parseExpression(0));
                consume(TokenType::RBRACKET, "Expected ']' after list elements");
                left = std::make_unique<ListLiteralNode>(std::move(elements));
            }
        }
    } else if (t.type == TokenType::LBRACE) {
        if (peek().type == TokenType::RBRACE) {
            advance();
            left = std::make_unique<MapLiteralNode>();
        } else {
            auto keyExpr = parseExpression(0);
            // Set-style comprehension: {expr for var in iterable (where cond)} → produces a List
            if (peek().type == TokenType::FOR) {
                advance(); // consume 'for'
                std::string varName = advance().value;
                std::string varName2;
                if (peek().type == TokenType::COMMA) { advance(); varName2 = advance().value; }
                consume(TokenType::IN, "Expected 'in' in comprehension");
                auto iterable = parseExpression(0);
                std::unique_ptr<Node> condition;
                if (peek().type == TokenType::WHERE) {
                    advance();
                    condition = parseExpression(0);
                }
                consume(TokenType::RBRACE, "Expected '}' after comprehension");
                auto comp = std::make_unique<ListComprehensionNode>();
                comp->expr      = std::move(keyExpr);
                comp->varName   = varName;
                comp->varName2  = varName2;
                comp->iterable  = std::move(iterable);
                comp->condition = std::move(condition);
                comp->line = t.line; comp->col = t.col; comp->filePath = filePath;
                left = std::move(comp);
            } else if (peek().type == TokenType::COLON) {
                // Map literal: {key: val, ...}
                advance(); // consume ':'
                auto valExpr = parseExpression(0);
                if (peek().type == TokenType::FOR) {
                    // Map comprehension: {key: val for var in iterable}
                    advance();
                    std::string varName = advance().value;
                    std::string varName2;
                    if (peek().type == TokenType::COMMA) { advance(); varName2 = advance().value; }
                    consume(TokenType::IN, "Expected 'in' in map comprehension");
                    // See list-comprehension counterpart above for the
                    // precedence-3 rationale (keeps a trailing `if` for us).
                    auto iterable = parseExpression(3);
                    std::unique_ptr<Node> condition;
                    if (peek().type == TokenType::WHERE || peek().type == TokenType::IF) {
                        advance(); condition = parseExpression(0);
                    }
                    consume(TokenType::RBRACE, "Expected '}' after map comprehension");
                    auto comp = std::make_unique<MapComprehensionNode>();
                    comp->keyExpr = std::move(keyExpr); comp->valExpr = std::move(valExpr);
                    comp->varName = varName; comp->varName2 = varName2;
                    comp->iterable = std::move(iterable); comp->condition = std::move(condition);
                    comp->line = t.line; comp->col = t.col; comp->filePath = filePath;
                    left = std::move(comp);
                } else {
                    auto node = std::make_unique<MapLiteralNode>();
                    node->elements.push_back({std::move(keyExpr), std::move(valExpr)});
                    while (match(TokenType::COMMA)) {
                        auto k = parseExpression(0);
                        consume(TokenType::COLON, "Expected ':' after map key");
                        auto v = parseExpression(0);
                        node->elements.push_back({std::move(k), std::move(v)});
                    }
                    consume(TokenType::RBRACE, "Expected '}' after map literal");
                    left = std::move(node);
                }
            } else {
                // Set literal: {val, val, ...}
                auto node = std::make_unique<SetLiteralNode>();
                node->elements.push_back(std::move(keyExpr));
                node->line = t.line; node->col = t.col; node->filePath = filePath;
                while (match(TokenType::COMMA)) {
                    if (peek().type == TokenType::RBRACE) break; // trailing comma
                    node->elements.push_back(parseExpression(0));
                }
                consume(TokenType::RBRACE, "Expected '}' after set literal");
                left = std::move(node);
            }
        }
    } else if (t.type == TokenType::LPAREN) {
        if (peek().type == TokenType::RPAREN) {
            // Empty tuple: ()
            advance();
            left = std::make_unique<TupleLiteralNode>(std::vector<std::unique_ptr<Node>>{});
        } else {
            auto first = parseExpression(0);
            if (peek().type == TokenType::COMMA) {
                // Tuple literal: (a, b, ...) or (a,) single-element
                std::vector<std::unique_ptr<Node>> elements;
                elements.push_back(std::move(first));
                while (match(TokenType::COMMA)) {
                    if (peek().type == TokenType::RPAREN) break; // trailing comma
                    elements.push_back(parseExpression(0));
                }
                consume(TokenType::RPAREN, "Expected ')'");
                left = std::make_unique<TupleLiteralNode>(std::move(elements));
            } else {
                // Parenthesized expression
                consume(TokenType::RPAREN, "Expected ')'");
                left = std::move(first);
            }
        }
    } else if (t.type == TokenType::NOT) {
        auto operand = parseExpression(40);
        left =
            std::make_unique<BinaryOpNode>("not", std::move(operand), nullptr);
    } else if (t.type == TokenType::FN) {
        auto lambda = std::make_unique<LambdaNode>();
        consume(TokenType::LPAREN, "Expected '(' after 'fn'");
        while (peek().type != TokenType::RPAREN && !isAtEnd()) {
            LambdaParam p;
            if (peek().type == TokenType::ELLIPSIS) {
                advance();
                p.isVariadic = true;
            }
            p.name = advance().value;
            if (match(TokenType::COLON))
                p.type = advance().value;
            if (p.isVariadic && p.type.empty())
                p.type = "List";
            lambda->params.push_back(std::move(p));
            if (!match(TokenType::COMMA)) break;
        }
        consume(TokenType::RPAREN, "Expected ')' after lambda params");

        // Optional `-> ReturnType` annotation between params and body.
        // Sema overwrites `inferredReturnType`, so we stash the annotation
        // in `declaredReturnType` for future type-check use.
        if (peek().type == TokenType::ARROW) {
            advance(); // consume `->`
            if (peek().type != TokenType::IDENTIFIER) {
                reportError("Expected return type after '->' in lambda", peek());
            }
            lambda->declaredReturnType = advance().value;
        }

        // Body: `=> expr`, `=> { stmts }`, or `{ stmts }`. The `=> { ... }`
        // form looks like a block but parseExpression would otherwise try to
        // read it as a set/map literal — peek ahead and route to the stmt
        // body instead.
        if (peek().type == TokenType::FAT_ARROW) {
            advance();
            if (peek().type == TokenType::LBRACE) {
                advance(); // consume `{`
                lambda->isExpression = false;
                while (peek().type != TokenType::RBRACE && !isAtEnd())
                    lambda->stmtBody.push_back(parseStatement());
                consume(TokenType::RBRACE, "Expected '}' to close lambda body");
            } else {
                lambda->isExpression = true;
                lambda->exprBody = parseExpression(0);
            }
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

    // Stamp position onto every primary node so sema can show source context in errors.
    if (left && left->line == 0) {
        left->line     = t.line;
        left->col      = t.col;
        left->filePath = filePath;
    }

    // 2. Infix Loop
    while (!isAtEnd()) {
        TokenType type = peek().type;
        int next_prec = getPrecedence(type);
        if (type == TokenType::LPAREN)
            next_prec = 50;
        // `not in` compound operator: same precedence as `in`
        if (type == TokenType::NOT && pos + 1 < (int)tokens.size() && tokens[pos + 1].type == TokenType::IN)
            next_prec = 10;
        // Python-style ternary `val if cond else other`: only infix on same line
        if (type == TokenType::IF && pos > 0 && tokens[pos].line != tokens[pos - 1].line)
            next_prec = 0;
        if (next_prec <= min_precedence)
            break;

        Token opToken = advance();

        if (opToken.type == TokenType::NOT) {
            // `not in` — consume the `in` and build BinaryOpNode("not in", ...)
            consume(TokenType::IN, "Expected 'in' after 'not'");
            auto right = parseExpression(next_prec);
            left = std::make_unique<BinaryOpNode>("not in", std::move(left), std::move(right));
            continue;
        }

        if (opToken.type == TokenType::IF) {
            // Python-style ternary: val if cond else other
            auto cond = parseExpression(0);
            consume(TokenType::ELSE, "Expected 'else' in ternary expression");
            auto other = parseExpression(0);
            left = std::make_unique<TernaryNode>(std::move(cond), std::move(left), std::move(other));
            continue;
        }

        if (opToken.type == TokenType::DOT || opToken.type == TokenType::QUESTION_DOT) {
            Token member = advance();
            auto memberNode = std::make_unique<MemberAccessNode>(std::move(left), member.value);
            memberNode->line     = member.line;
            memberNode->col      = member.col;
            memberNode->filePath = filePath;
            memberNode->isSafeAccess = (opToken.type == TokenType::QUESTION_DOT);
            left = std::move(memberNode);
        } else if (opToken.type == TokenType::LBRACKET) {
            // Slice: s[start:end], s[:end], s[start:]
            if (peek().type == TokenType::COLON) {
                advance(); // consume ':'
                std::unique_ptr<Node> endExpr;
                if (peek().type != TokenType::RBRACKET)
                    endExpr = parseExpression(0);
                consume(TokenType::RBRACKET, "Expected ']'");
                auto sl = std::make_unique<SliceNode>(std::move(left), nullptr, std::move(endExpr));
                sl->line = opToken.line; sl->col = opToken.col; sl->filePath = filePath;
                left = std::move(sl);
            } else {
                auto indexExpr = parseExpression(0);
                if (peek().type == TokenType::COLON) {
                    advance(); // consume ':'
                    std::unique_ptr<Node> endExpr;
                    if (peek().type != TokenType::RBRACKET)
                        endExpr = parseExpression(0);
                    consume(TokenType::RBRACKET, "Expected ']'");
                    auto sl = std::make_unique<SliceNode>(std::move(left), std::move(indexExpr), std::move(endExpr));
                    sl->line = opToken.line; sl->col = opToken.col; sl->filePath = filePath;
                    left = std::move(sl);
                } else {
                    consume(TokenType::RBRACKET, "Expected ']'");
                    left = std::make_unique<BinaryOpNode>("[]", std::move(left),
                                                          std::move(indexExpr));
                }
            }
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
            call->line     = opToken.line;
            call->col      = opToken.col;
            call->filePath = filePath;
            while (peek().type != TokenType::RPAREN && !isAtEnd()) {
                std::string argName = "";
                bool isSpread = false;

                if (peek().type == TokenType::ELLIPSIS) {
                    advance(); // consume '...'
                    isSpread = true;
                } else if (peek().type == TokenType::IDENTIFIER &&
                    tokens[pos + 1].type == TokenType::ASSIGN) {
                    argName = advance().value;
                    advance(); // consume '='
                }

                Arg newArg;
                newArg.name = argName;
                newArg.isSpread = isSpread;
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
        } else if (opToken.type == TokenType::DOTDOT) {
            // Range literal: start..end
            auto right = parseExpression(next_prec);
            auto range = std::make_unique<RangeLiteralNode>();
            range->start = std::move(left);
            range->end   = std::move(right);
            range->line = opToken.line; range->col = opToken.col; range->filePath = filePath;
            left = std::move(range);
        } else if (opToken.type == TokenType::AS) {
            // `val as TypeName` — type cast; RHS is the target type name
            Token typeName = advance();
            auto typeNode = std::make_unique<LiteralNode>(typeName.value);
            left = std::make_unique<BinaryOpNode>("as", std::move(left), std::move(typeNode));
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
            auto binOp = std::make_unique<BinaryOpNode>(
                opToken.value, std::move(left), std::move(right));
            binOp->line = opToken.line;
            binOp->col  = opToken.col;
            binOp->filePath = filePath;
            left = std::move(binOp);
        }
    }
    return left;
}

// --- Statement & Control Flow ---
std::unique_ptr<Node> Parser::parseStatement() {
    // Stamp every returned statement with the start-of-statement line/col so
    // the --debug stepper has something to show at every pause. Many of the
    // narrow paths below (e.g. `(a, b) := rhs`) don't bother setting line on
    // the node they build; doing it once here keeps them honest.
    while (peek().type == TokenType::SEMICOLON) advance();
    int stmtLine = peek().line;
    int stmtCol  = peek().col;
    auto stmt = parseStatementImpl();
    if (stmt && stmt->line == 0) {
        stmt->line = stmtLine;
        stmt->col  = stmtCol;
        if (stmt->filePath.empty()) stmt->filePath = filePath;
    }
    return stmt;
}

std::unique_ptr<Node> Parser::parseStatementImpl() {
    // Skip any leading `;` — they act as empty statement separators, so a
    // line like `a := 1; b := 2` parses two statements without complaint.
    while (peek().type == TokenType::SEMICOLON) advance();
    TokenType type = peek().type;
    int currentLine = peek().line;

    if (type == TokenType::NONLOCAL || type == TokenType::GLOBAL) {
        bool isGlobal = (type == TokenType::GLOBAL);
        advance(); // consume 'nonlocal' or 'global'
        auto node = std::make_unique<NonlocalNode>();
        node->isGlobal = isGlobal;
        node->vars.push_back(advance().value);
        while (peek().type == TokenType::COMMA) {
            advance(); // consume ','
            node->vars.push_back(advance().value);
        }
        return node;
    }
    if (type == TokenType::CONST) {
        advance(); // consume 'const'
        auto decl = parseVarDecl();
        decl->isConst = true;
        return decl;
    }
    // Nested `define` inside a function body — desugar to a local Callable
    // binding (`name := fn(args) -> Ret { body }`). Captures the enclosing
    // scope automatically via the lambda's free-var collection, which is
    // what users expect when they write Python-style nested defs.
    if (type == TokenType::DEFINE) {
        auto fn = parseFunction();
        auto lambda = std::make_unique<LambdaNode>();
        lambda->isExpression       = false;
        lambda->declaredReturnType = fn->returnType;
        for (auto& p : fn->parameters) {
            LambdaParam lp;
            lp.name       = p.name;
            lp.type       = p.type;
            lp.isVariadic = p.isVariadic;
            lambda->params.push_back(std::move(lp));
        }
        lambda->stmtBody = std::move(fn->body);
        lambda->line = fn->line;
        lambda->col  = fn->col;

        auto lhs  = std::make_unique<LiteralNode>(fn->name);
        lhs->line = fn->line;
        lhs->col  = fn->col;
        auto decl = std::make_unique<VarDeclNode>(std::move(lhs), std::move(lambda), ":=", "");
        decl->line = fn->line;
        decl->col  = fn->col;
        return decl;
    }
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
        // Tuple destructuring without parens: x, y := expr
        // Lookahead: IDENT (COMMA IDENT)+ ASSIGN_INIT
        {
            size_t ahead = pos;
            int nameCount = 0;
            while (ahead < tokens.size() &&
                   tokens[ahead].type == TokenType::IDENTIFIER &&
                   tokens[ahead].line == currentLine) {
                nameCount++;
                ahead++;
                if (ahead < tokens.size() && tokens[ahead].type == TokenType::COMMA)
                    ahead++;
                else
                    break;
            }
            if (nameCount >= 2 && ahead < tokens.size() &&
                tokens[ahead].type == TokenType::ASSIGN_INIT) {
                std::vector<std::unique_ptr<Node>> names;
                for (int i = 0; i < nameCount; i++) {
                    names.push_back(std::make_unique<LiteralNode>(advance().value));
                    if (i < nameCount - 1) advance(); // consume comma
                }
                advance(); // consume :=
                auto tupLhs = std::make_unique<TupleLiteralNode>(std::move(names));
                return std::make_unique<VarDeclNode>(std::move(tupLhs), parseExpression(0), ":=", "");
            }
        }

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
                 t == TokenType::STAR_ASSIGN || t == TokenType::SLASH_ASSIGN ||
                 t == TokenType::PERCENT_ASSIGN)) {
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
    // Tuple destructuring: (a, b) := rhs
    if (type == TokenType::LPAREN) {
        auto expr = parseExpression(0);
        if (dynamic_cast<TupleLiteralNode*>(expr.get()) &&
            peek().type == TokenType::ASSIGN_INIT) {
            advance(); // consume :=
            return std::make_unique<VarDeclNode>(std::move(expr), parseExpression(0), ":=", "");
        }
        return expr;
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

    std::vector<std::string> destructureVars;
    std::string varName, varName2;

    if (peek().type == TokenType::LPAREN) {
        // for (a, b) in items — tuple destructuring
        advance(); // consume '('
        while (peek().type != TokenType::RPAREN && !isAtEnd()) {
            destructureVars.push_back(advance().value);
            if (!match(TokenType::COMMA)) break;
        }
        consume(TokenType::RPAREN, "Expected ')' after destructure variables");
        varName = "__for_item"; // synthetic iteration variable
    } else {
        varName = advance().value;
        if (peek().type == TokenType::COMMA) {
            advance(); // consume ','
            varName2 = advance().value;
        }
    }

    consume(TokenType::IN, "Expected 'in'");
    auto node = std::make_unique<ForNode>(varName, isRef, parseExpression(0));
    node->varName2 = varName2;
    node->destructureVars = destructureVars;
    consume(TokenType::LBRACE, "Expected '{'");
    while (peek().type != TokenType::RBRACE && !isAtEnd())
        node->body.push_back(parseStatement());
    consume(TokenType::RBRACE, "Expected '}'");
    return node;
}

// Computes the full hierarchical module prefix from this->filePath.
// libs/core/string.quirk           -> "Core_String"
// libs/core/collections/list.quirk -> "Core_Collections_List"
// libs/sys/index.quirk             -> "Sys"
// userfile.quirk                   -> "Userfile"
std::string Parser::computeModulePrefix() const {
    std::string p = this->filePath;
    if (p.size() >= 6 && p.substr(p.size() - 6) == ".quirk")
        p = p.substr(0, p.size() - 6);
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

    // Strip everything up to and including the stdlib-root marker. We
    // recognize THREE layouts:
    //   .../libs/<module>/...               (dev tree)
    //   .../lib/quirk/<module>/...          (installed / venv via QUIRK_HOME)
    //   .../packages/<module>/...           (3rd-party packages)
    // All three should produce the same prefix for the same module, so a
    // stdlib resolved through any of them gets the same `Core_String_*`
    // linkage names.
    auto findRoot = [&](const std::string& a, const std::string& b = "") -> std::vector<std::string>::iterator {
        for (auto it = parts.begin(); it != parts.end(); ++it) {
            if (*it == a && (b.empty() || (it + 1 != parts.end() && *(it + 1) == b)))
                return it;
        }
        return parts.end();
    };
    auto libsIt     = findRoot("libs");
    auto libQuirkIt = findRoot("lib", "quirk");
    auto packagesIt = findRoot("packages");

    // Pick the deepest match (the rightmost root marker in the path) so a
    // venv layout `lib/quirk/packages/foo` still resolves under `packages`.
    auto rootIt = parts.end();
    int rootSkip = 0;
    if (libsIt     != parts.end() && (rootIt == parts.end() || libsIt > rootIt))         { rootIt = libsIt;     rootSkip = 1; }
    if (libQuirkIt != parts.end() && (rootIt == parts.end() || libQuirkIt > rootIt))     { rootIt = libQuirkIt; rootSkip = 2; }
    if (packagesIt != parts.end() && (rootIt == parts.end() || packagesIt > rootIt))     { rootIt = packagesIt; rootSkip = 1; }

    if (rootIt != parts.end()) {
        parts.erase(parts.begin(), rootIt + rootSkip);
        // Venv layout adds a `stdlib/` or `packages/` bucket after lib/quirk.
        // Strip it so stdlib paths still produce `Core_*` linkage names.
        if (!parts.empty() && (parts.front() == "stdlib" || parts.front() == "packages"))
            parts.erase(parts.begin());
        if (!parts.empty() && parts.back() == "index")
            parts.pop_back();
    } else {
        // No standard layout marker in the path. Fall back to the manifest:
        // if the file lives inside a project rooted at `quirk.toml`, the
        // declared `name` drives the linkage prefix (lets a library's own
        // `src/index.quirk` emit `MyLib_*` linkage names without being
        // installed). Otherwise, use the bare filename as a one-component
        // prefix — that's the user-script case.
        std::string pkgName = qpm::project_name_for_file(this->filePath);
        if (!pkgName.empty()) {
            if (!pkgName.empty()) pkgName[0] = (char)std::toupper((unsigned char)pkgName[0]);
            return pkgName;
        }
        std::string last = parts.empty() ? "" : parts.back();
        if (last == "index" && parts.size() > 1) last = parts[parts.size() - 2];
        parts = { last };
    }

    // Normalize "typing" → "core" so libs/typing/string.quirk still emits Core_String_* linkage names.
    for (auto& part : parts)
        if (part == "typing") part = "core";

    // Drop the typing/primitives/ subdir component so linkage names match the C runtime,
    // which keeps string in core/string.c and int/double/bool/char in core/primitives.c.
    if (parts.size() >= 3 && parts[0] == "core" && parts[1] == "primitives")
        parts.erase(parts.begin() + 1);

    // Normalize split primitive files back to "primitives" so the C runtime symbols match.
    // typing/int.quirk, typing/double.quirk, typing/bool.quirk, typing/char.quirk all live under
    // core/primitives.c in the runtime.
    for (auto& part : parts)
        if (part == "int" || part == "double" || part == "bool" || part == "char")
            part = "primitives";

    for (auto& part : parts)
        if (!part.empty()) part[0] = (char)std::toupper((unsigned char)part[0]);

    std::string result;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i > 0) result += "_";
        result += parts[i];
    }
    return result;
}

std::unique_ptr<FunctionNode> Parser::parseFunction(bool allowAbstract) {
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
    Token nameTokFn = peek();
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
    node->line = nameTokFn.line; node->col = nameTokFn.col; node->filePath = filePath;
    node->name = name;
    node->isExtern = isExtern;

    // Generic type params: define map[T, U](...)
    if (peek().type == TokenType::LBRACKET) {
        advance(); // consume '['
        while (peek().type != TokenType::RBRACKET && !isAtEnd()) {
            node->typeParams.push_back(advance().value);
            if (peek().type == TokenType::COMMA) advance();
        }
        consume(TokenType::RBRACKET, "Expected ']' after type parameters");
    }

    // --- AUTOMATIC FFI MANGLING ---
    // Derives the full hierarchical module prefix from this->filePath.
    //
    // Naming convention (Option B — struct name always included):
    //   libs/core/string.quirk           -> modulePrefix = "Core_String"
    //   libs/core/primitives.quirk       -> modulePrefix = "Core_Primitives"
    //   libs/core/collections/list.quirk -> modulePrefix = "Core_Collections_List"
    //   libs/core/collections/map.quirk  -> modulePrefix = "Core_Collections_Map"
    //   libs/sys/index.quirk             -> modulePrefix = "Sys"
    //   libs/io/file.quirk               -> modulePrefix = "Io_File"
    //   userfile.quirk  (non-libs)       -> modulePrefix = "Userfile"
    //
    // Global extern:  linkageName = modulePrefix + "_" + name
    //   e.g. extern define float_to_str  ->  "Core_String_float_to_str"
    //        extern define arg_count      ->  "Sys_arg_count"
    //
    // Extern struct method (handled in parseStruct):
    //   linkageName = modulePrefix + "_" + structName + "_" + rawMethodName
    //   e.g. Core_String_String_to_float, Core_Collections_List_List_append
    std::string modulePrefix = computeModulePrefix();

    // Library functions ALWAYS get a module-prefixed linkage name. User
    // scripts (main module + user files) keep bare linkage so the JIT can
    // find `main` and existing lookups don't change.
    //
    // Top-level library functions use the `<mp>$<name>` form so they can't
    // collide with struct-method linkage `<structName>_<methodName>` —
    // before this, a top-level `define test` in regex/index.quirk and the
    // method `Regex.test` both wanted `Regex_test`. The `$` separator is
    // legal in LLVM identifiers but never produced by anything else here.
    // Extern declarations stay on `<mp>_<name>` because their linkage has
    // to match the C runtime symbol they bind to.
    bool isLibFunction = filePath.find("libs/") != std::string::npos
                      || filePath.find("libs\\") != std::string::npos
                      || filePath.find("lib/quirk/") != std::string::npos
                      || filePath.find("lib\\quirk\\") != std::string::npos
                      || filePath.find("packages/") != std::string::npos
                      || filePath.find("packages\\") != std::string::npos;
    if (isExtern) {
        node->linkageName = modulePrefix + "_" + name;
    } else if (isLibFunction) {
        node->linkageName = modulePrefix + "$" + name;
    } else if (name == "main") {
        // The program entry must link as plain `main` for the C runtime.
        node->linkageName = name;
    } else {
        // User functions: module-prefix them too so two files that both
        // declare `info` (or any other common name) get distinct LLVM
        // symbols. Without this, `module_a.info` and `module_b.info`
        // both resolve to the same getFunction("info") at the call site
        // and we end up calling the wrong one.
        node->linkageName = modulePrefix + "$" + name;
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
            param.type = parseTypeString();
        }

        if (param.isVariadic) {
            if (param.type.empty())
                param.type = "List";  // default type for ...args
            else if (param.type != "List")
                reportError("Variadic argument must be typed as List (found '" + param.type + "')", peek());
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
        node->returnType = parseTypeString();
    }
    if (isExtern)
        return node;

    if (peek().type == TokenType::WHERE) {
        advance();
        // Generic constraint form: where T: Interface — first token is a known type param
        if (!node->typeParams.empty() &&
            peek().type == TokenType::IDENTIFIER &&
            std::find(node->typeParams.begin(), node->typeParams.end(), peek().value) != node->typeParams.end()) {
            node->genericConstraints = parseGenericWhere(node->typeParams);
        } else {
            node->whereClause = parseExpression(0);
        }
    }

    if (peek().type != TokenType::LBRACE) {
        if (allowAbstract) {
            node->isAbstract = true;
            return node;
        }
        reportError("Expected '{' to open function body", peek());
    }
    advance(); // consume '{'
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
        bool isSpread = false;

        if (peek().type == TokenType::ELLIPSIS) {
            advance(); // consume '...'
            isSpread = true;
        } else if (peek().type == TokenType::IDENTIFIER &&
            tokens[pos + 1].type == TokenType::ASSIGN) {
            argName = advance().value;
            advance(); // consume '='
        }

        Arg newArg;
        newArg.name = argName;
        newArg.isSpread = isSpread;
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
    Token nameTokSt = peek();
    node->name = advance().value;
    node->line = nameTokSt.line; node->col = nameTokSt.col; node->filePath = filePath;

    // Generic type params: struct Stack[T]
    if (peek().type == TokenType::LBRACKET) {
        advance(); // consume '['
        while (peek().type != TokenType::RBRACKET && !isAtEnd()) {
            node->typeParams.push_back(advance().value);
            if (peek().type == TokenType::COMMA) advance();
        }
        consume(TokenType::RBRACKET, "Expected ']' after type parameters");
    }

    if (match(TokenType::COLON)) {
        do {
            node->parents.push_back(advance().value);
        } while (match(TokenType::COMMA));
    }

    // Generic constraints: struct Foo[T] where T: Comparable (after inheritance clause)
    if (!node->typeParams.empty() && peek().type == TokenType::WHERE) {
        advance();
        node->genericConstraints = parseGenericWhere(node->typeParams);
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
            // e.g. extern define to_float in struct String in libs/core/string.quirk
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
                std::string fType = parseTypeString();
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

    // --- Implicit field inference ---
    // Scan __init body for `self.X = Y` where X is not an explicit field.
    // Infer the field type from Y: parameter type > literal type > Any.
    // This lets users write `self.side = side` in __init without declaring
    // `side: Int` at the top of the struct.
    {
        std::set<std::string> declaredFields;
        for (const auto& f : node->fields) declaredFields.insert(f.name);

        for (auto& extra : extraNodes) {
            auto* func = dynamic_cast<FunctionNode*>(extra.get());
            if (!func || func->name != node->name + "__init") continue;

            // Map parameter names → declared types (self already stripped)
            std::map<std::string, std::string> paramTypes;
            for (const auto& p : func->parameters) paramTypes[p.name] = p.type;

            for (const auto& stmt : func->body) {
                auto* vd = dynamic_cast<VarDeclNode*>(stmt.get());
                if (!vd || vd->op != "=") continue;

                auto* mem = dynamic_cast<MemberAccessNode*>(vd->lhs.get());
                if (!mem) continue;

                auto* selfLit = dynamic_cast<LiteralNode*>(mem->object.get());
                if (!selfLit || selfLit->value != "self") continue;

                const std::string& fieldName = mem->memberName;
                if (declaredFields.count(fieldName)) continue;

                // Infer type from the RHS
                std::string inferredType = "Any";
                if (auto* rhs = dynamic_cast<LiteralNode*>(vd->expression.get())) {
                    if (paramTypes.count(rhs->value)) {
                        inferredType = paramTypes[rhs->value]; // matches a param name
                    } else if (!rhs->value.empty()) {
                        char c = rhs->value[0];
                        if (c == '"' || c == '\'')                               inferredType = "String";
                        else if (rhs->value == "true" || rhs->value == "false")  inferredType = "Bool";
                        else if (std::isdigit(c) && rhs->value.find('.') != std::string::npos) inferredType = "Double";
                        else if (std::isdigit(c))                                inferredType = "Int";
                    }
                }
                // `paramName.method()` where paramName is a String — most
                // String methods return a new String, so a small hardcoded
                // table covers the common case (`self.x = name.title()`).
                // Without this the field defaults to Any, and Any→String
                // returns elsewhere (e.g. `__str`) end up as garbage memory.
                else if (auto* callExpr = dynamic_cast<CallNode*>(vd->expression.get())) {
                    if (auto* mem = dynamic_cast<MemberAccessNode*>(callExpr->callee.get())) {
                        if (auto* recv = dynamic_cast<LiteralNode*>(mem->object.get())) {
                            auto pit = paramTypes.find(recv->value);
                            if (pit != paramTypes.end() && pit->second == "String") {
                                static const std::set<std::string> stringToString = {
                                    "title", "upper", "lower", "capitalize",
                                    "strip", "lstrip", "rstrip", "trim",
                                    "replace", "format", "slice", "substring",
                                    "concat", "reverse", "padLeft", "padRight",
                                    "str", "repr", "lstripChars", "rstripChars",
                                };
                                if (stringToString.count(mem->memberName))
                                    inferredType = "String";
                            }
                        }
                    }
                }

                node->fields.push_back({fieldName, inferredType, nullptr});
                declaredFields.insert(fieldName);
            }
            break; // only scan the first __init
        }
    }
    // --------------------------------

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
        if (peek().type == TokenType::AS) {
            // from .path as alias — whole-module import with explicit alias
            advance();
            std::string alias = advance().value;
            return std::make_unique<UseNode>(path, std::vector<std::string>{}, alias);
        }
        consume(TokenType::USE, "Expected 'use' or 'as' after module path");
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

    node->resource = parseExpression(35);
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
    // Extract the source line for display in flushErrors()
    size_t lineStart = 0;
    int currentLine = 1;
    for (size_t i = 0; i < source.length(); i++) {
        if (currentLine == token.line) { lineStart = i; break; }
        if (source[i] == '\n') currentLine++;
    }
    size_t lineEnd = source.find('\n', lineStart);
    if (lineEnd == std::string::npos) lineEnd = source.length();
    std::string lineCode = source.substr(lineStart, lineEnd - lineStart);

    errors.push_back({message, filePath, lineCode, token.line, token.col});
    throw ParseError{};
}

void Parser::flushErrors() const {
    for (const auto& e : errors) {
        if (g_diagnostics_json) {
            // NDJSON record per diagnostic. The escape pattern mirrors
            // the one in Sema.cpp — kept inline rather than shared so
            // the parser stays free of Sema/header coupling.
            std::string esc; esc.reserve(e.msg.size() + 8);
            for (char c : e.msg) {
                switch (c) {
                    case '"':  esc += "\\\""; break;
                    case '\\': esc += "\\\\"; break;
                    case '\n': esc += "\\n";  break;
                    case '\r': esc += "\\r";  break;
                    case '\t': esc += "\\t";  break;
                    default:
                        if (static_cast<unsigned char>(c) < 0x20) {
                            char buf[8];
                            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                            esc += buf;
                        } else esc += c;
                }
            }
            std::cout << "{\"level\":\"error\",\"msg\":\"" << esc
                      << "\",\"path\":\""                 << e.filePath
                      << "\",\"line\":"                   << e.line
                      << ",\"col\":"                      << e.col
                      << "}\n";
            std::cout.flush();
            continue;
        }
        std::cerr << "\033[1;31m[ERROR]\033[0m " << e.msg << "\n";
        if (!e.filePath.empty())
            std::cerr << " --> " << e.filePath << ":" << e.line << ":" << e.col << "\n";
        std::string ln = std::to_string(e.line);
        int caretOffset = (e.col > 1) ? e.col - 1 : 0;
        std::cerr << std::string(ln.size(), ' ') << " |\n";
        std::cerr << ln << " | " << e.lineCode << "\n";
        std::cerr << std::string(ln.size(), ' ') << " | "
                  << std::string(caretOffset, ' ')
                  << "\033[1;33m^--- here\033[0m\n\n";
    }
}

void Parser::sync() {
    while (!isAtEnd()) {
        TokenType t = peek().type;
        if (t == TokenType::DEFINE || t == TokenType::STRUCT ||
            t == TokenType::ENUM   || t == TokenType::USE    ||
            t == TokenType::FROM   || t == TokenType::EXTEND ||
            t == TokenType::INIT)
            return;
        advance();
    }
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
        // Type-match: `case TypeName =>` or `case TypeName as var =>`
        // Detect: identifier starts with uppercase and next token is =>, |, as, comma, or {
        auto isTypeName = [&](const Token& tok) -> bool {
            if (tok.type != TokenType::IDENTIFIER) return false;
            if (tok.value.empty() || !std::isupper((unsigned char)tok.value[0])) return false;
            // Peek ahead: must be followed by =>, {, |, as, comma, or RBRACE (not '(' or operators)
            int ahead = pos + 1; // one past the peeked token
            TokenType nx = tokens[ahead].type;
            return nx == TokenType::FAT_ARROW || nx == TokenType::LBRACE ||
                   nx == TokenType::PIPE     || nx == TokenType::AS    ||
                   nx == TokenType::COMMA    || nx == TokenType::RBRACE;
        };

        if (peek().type == TokenType::IDENTIFIER && peek().value == "_") {
            advance();
            arm.isWildcard = true;
        } else if (isTypeName(peek())) {
            arm.isTypeMatch = true;
            arm.typeNames.push_back(advance().value);
            while (peek().type == TokenType::PIPE) {
                advance(); // consume '|'
                arm.typeNames.push_back(advance().value);
            }
            if (peek().type == TokenType::AS) {
                advance(); // consume 'as'
                arm.bindName = advance().value;
            }
        } else {
            do {
                arm.patterns.push_back(parseExpression(2));
            } while (match(TokenType::COMMA));
            // Single bare lowercase identifier: `case x` — interpret as a
            // binding wildcard rather than equality against a named value.
            // This is the standard pattern-matching shape from Python /
            // Rust / OCaml and what users reach for when writing guards
            // like `case x if x > 0 =>`. UPPERCASE identifiers already go
            // through the type-match branch above; comma-separated
            // patterns stay as value-equality lists.
            if (arm.patterns.size() == 1) {
                if (auto lit = dynamic_cast<LiteralNode*>(arm.patterns[0].get())) {
                    const std::string& v = lit->value;
                    if (!v.empty() && (std::isalpha((unsigned char)v[0]) || v[0] == '_')
                        && std::islower((unsigned char)v[0])
                        && v != "true" && v != "false" && v != "null") {
                        arm.isWildcard = true;
                        arm.bindName = v;
                        arm.patterns.clear();
                    }
                }
                // Tuple destructure: `case (a, b) =>` — recognise a single
                // tuple-literal pattern whose elements are all lowercase
                // identifiers as a positional binding form. Each element
                // is bound to the corresponding scrutinee tuple slot.
                else if (auto tup = dynamic_cast<TupleLiteralNode*>(arm.patterns[0].get())) {
                    bool allBindings = !tup->elements.empty();
                    std::vector<std::string> names;
                    for (auto& elem : tup->elements) {
                        auto* lit = dynamic_cast<LiteralNode*>(elem.get());
                        if (!lit) { allBindings = false; break; }
                        const std::string& v = lit->value;
                        if (v.empty() || !(std::isalpha((unsigned char)v[0]) || v[0] == '_')) {
                            allBindings = false; break;
                        }
                        if (!std::islower((unsigned char)v[0])
                            && v[0] != '_') { allBindings = false; break; }
                        names.push_back(v);
                    }
                    if (allBindings) {
                        arm.isWildcard = true;
                        arm.bindNames = std::move(names);
                        arm.patterns.clear();
                    }
                }
                // List destructure: `case [a, b] =>` — same shape as the
                // tuple-destructure above, but for List scrutinees. Codegen
                // reads via `List.get(i)` instead of `Tuple.get(i)`.
                else if (auto lst = dynamic_cast<ListLiteralNode*>(arm.patterns[0].get())) {
                    bool allBindings = !lst->elements.empty();
                    std::vector<std::string> names;
                    for (auto& elem : lst->elements) {
                        auto* lit = dynamic_cast<LiteralNode*>(elem.get());
                        if (!lit) { allBindings = false; break; }
                        const std::string& v = lit->value;
                        if (v.empty() || !(std::isalpha((unsigned char)v[0]) || v[0] == '_')) {
                            allBindings = false; break;
                        }
                        if (!std::islower((unsigned char)v[0])
                            && v[0] != '_') { allBindings = false; break; }
                        names.push_back(v);
                    }
                    if (allBindings) {
                        arm.isWildcard = true;
                        arm.bindNames = std::move(names);
                        arm.bindsList = true;
                        arm.patterns.clear();
                    }
                }
            }
        }

        // Optional guard: `case x if x > 0 =>`. The guard is parsed at
        // precedence 0 — anything goes — but it stops at `=>` because the
        // FAT_ARROW token has no infix meaning. Reusing TokenType::IF
        // works here because it can't appear at this position outside of
        // a guard.
        if (match(TokenType::IF)) {
            arm.guard = parseExpression(0);
        }

        // Body: `=> stmt`, `=> { stmts }` (block after arrow), or `{ stmts }` (block only)
        if (match(TokenType::FAT_ARROW)) {
            if (peek().type == TokenType::LBRACE) {
                // `=> { stmts }` — treat { } as a block, not a map literal
                advance(); // consume '{'
                while (peek().type != TokenType::RBRACE && !isAtEnd())
                    arm.body.push_back(parseStatement());
                consume(TokenType::RBRACE, "Expected '}'");
            } else {
                arm.body.push_back(parseStatement());
            }
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

// =========================================================
// GENERICS + INTERFACES
// =========================================================

bool Parser::isGenericArgList() const {
    // Lookahead from current pos (which must be LBRACKET).
    // Returns true if tokens form: [ IDENTIFIER (COMMA IDENTIFIER)* ]
    // with no literals or operators inside — unambiguously a type-arg list.
    int i = pos + 1;
    int n = (int)tokens.size();
    if (i >= n || tokens[pos].type != TokenType::LBRACKET) return false;
    while (i < n && tokens[i].type != TokenType::RBRACKET) {
        if (tokens[i].type == TokenType::IDENTIFIER) { i++; }
        else if (tokens[i].type == TokenType::COMMA)  { i++; }
        else return false; // literal, operator, etc. — not a type-arg list
    }
    return i < n; // found closing ]
}

std::string Parser::parseTypeString() {
    std::string t = advance().value; // base type identifier
    if (peek().type == TokenType::QUESTION) { advance(); t += "?"; }
    // Generic args: List[T]  or  Map[K, V]  — only when [ contains only identifiers
    if (peek().type == TokenType::LBRACKET && isGenericArgList()) {
        advance(); // consume '['
        t += "[";
        bool first = true;
        while (peek().type != TokenType::RBRACKET && !isAtEnd()) {
            if (!first) { consume(TokenType::COMMA, "Expected ','"); t += ", "; }
            first = false;
            t += parseTypeString();
        }
        consume(TokenType::RBRACKET, "Expected ']' after type arguments");
        t += "]";
    }
    // Union types: T | U
    while (peek().type == TokenType::PIPE) {
        advance();
        t += "|" + advance().value;
    }
    return t;
}

std::unique_ptr<InterfaceNode> Parser::parseInterface() {
    consume(TokenType::INTERFACE, "Expected 'interface'");
    auto node = std::make_unique<InterfaceNode>();
    node->name = advance().value;
    node->line = tokens[pos - 1].line;
    node->col  = tokens[pos - 1].col;
    node->filePath = filePath;
    node->moduleName = computeModulePrefix();

    // Optional interface inheritance: interface Comparable : Equatable
    if (match(TokenType::COLON)) {
        do {
            node->extends.push_back(advance().value);
        } while (match(TokenType::COMMA));
    }

    consume(TokenType::LBRACE, "Expected '{' after interface name");
    while (peek().type != TokenType::RBRACE && !isAtEnd()) {
        if (peek().type == TokenType::DEFINE) {
            auto sig = parseFunction(/*allowAbstract=*/true);
            sig->cls = node->name;
            // Strip leading 'self' param — same convention as struct methods
            if (!sig->parameters.empty() && sig->parameters[0].name == "self")
                sig->parameters.erase(sig->parameters.begin());
            sig->isStatic = false;
            sig->name = node->name + "_" + sig->name;
            sig->linkageName = sig->name;
            node->methods.push_back(std::move(sig));
        } else {
            reportError("Expected 'define' inside interface body", peek());
            advance(); // recover
        }
    }
    consume(TokenType::RBRACE, "Expected '}' to close interface");
    return node;
}

// Parse: T: Interface1 & Interface2, U: Interface3
// Called after `where` has already been consumed and the first token is a known type param.
std::map<std::string, std::vector<std::string>> Parser::parseGenericWhere(const std::vector<std::string>& typeParams) {
    std::map<std::string, std::vector<std::string>> constraints;
    while (true) {
        if (peek().type != TokenType::IDENTIFIER) break;
        std::string typeVar = advance().value;
        consume(TokenType::COLON, "Expected ':' after type parameter in where clause");
        std::vector<std::string> ifaces;
        ifaces.push_back(parseTypeString());
        while (peek().type == TokenType::AMPERSAND) {
            advance();
            ifaces.push_back(parseTypeString());
        }
        constraints[typeVar] = std::move(ifaces);
        if (peek().type != TokenType::COMMA) break;
        advance(); // consume ','
    }
    return constraints;
}

