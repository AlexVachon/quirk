#include "lexer.hpp"
#include <cctype>

Lexer::Lexer(const std::string& source) : src(source) {}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;

    while (pos < src.length()) {
        char c = peek();

        // 1. Skip Whitespace
        if (isspace(c)) {
            if (c == '\n')
                line++;
            advance();
            continue;
        }

        // 2. Handle Strings
        if (c == '"') {
            std::string literal;
            literal += advance();  // Keep the starting "
            while (peek() != '"' && pos < src.length()) {
                if (peek() == '\n')
                    line++;

                // --- FIX: Handle Escape Sequences ---
                if (peek() == '\\') {
                    literal += advance();  // Consume '\'
                    if (pos < src.length()) {
                        literal += advance();  // Consume the escaped character
                                               // (e.g. '"')
                    }
                    continue;
                }
                // ------------------------------------

                literal += advance();
            }
            if (peek() == '"')
                literal += advance();  // Keep the ending "

            tokens.push_back({TokenType::STRING_LITERAL, literal, line});
            continue;
        }

        // ... (Rest of the file remains exactly the same) ...

        // 3. Handle Identifiers and Keywords
        if (isalpha(c) || c == '_') {
            std::string ident;
            while (isalnum(peek()) || peek() == '_')
                ident += advance();

            if (ident == "define")
                tokens.push_back({TokenType::DEFINE, ident, line});
            else if (ident == "struct")
                tokens.push_back({TokenType::STRUCT, ident, line});
            else if (ident == "use")
                tokens.push_back({TokenType::USE, ident, line});
            else if (ident == "extend")
                tokens.push_back({TokenType::EXTEND, ident, line});
            else if (ident == "init")
                tokens.push_back({TokenType::INIT, ident, line});
            else if (ident == "extern")
                tokens.push_back({TokenType::EXTERN, ident, line});
            else if (ident == "return")
                tokens.push_back({TokenType::RETURN, ident, line});
            else if (ident == "if")
                tokens.push_back({TokenType::IF, ident, line});
            else if (ident == "else")
                tokens.push_back({TokenType::ELSE, ident, line});
            else if (ident == "elif")
                tokens.push_back({TokenType::ELIF, ident, line});
            else if (ident == "and")
                tokens.push_back({TokenType::AND, ident, line});
            else if (ident == "or")
                tokens.push_back({TokenType::OR, ident, line});
            else if (ident == "not")
                tokens.push_back({TokenType::NOT, ident, line});
            else if (ident == "while")
                tokens.push_back({TokenType::WHILE, ident, line});
            else if (ident == "for")
                tokens.push_back({TokenType::FOR, ident, line});
            else if (ident == "in")
                tokens.push_back({TokenType::IN, ident, line});
            else if (ident == "ref")
                tokens.push_back({TokenType::REF, ident, line});
            else if (ident == "with")
                tokens.push_back({TokenType::WITH, ident, line});
            else if (ident == "as")
                tokens.push_back({TokenType::AS, ident, line});
            else if (ident == "del")
                tokens.push_back({TokenType::DEL, ident, line});
            else if (ident == "true")
                tokens.push_back({TokenType::TRUE, ident, line});
            else if (ident == "false")
                tokens.push_back({TokenType::FALSE, ident, line});
            else
                tokens.push_back({TokenType::IDENTIFIER, ident, line});
            continue;
        }

        // 4. Handle Numbers
        if (isdigit(c)) {
            std::string num;
            while (isdigit(peek()))
                num += advance();
            if (peek() == '.') {
                num += advance();
                while (isdigit(peek()))
                    num += advance();
            }
            tokens.push_back({TokenType::INT_LITERAL, num, line});
            continue;
        }

        // 5. Multi-character Operators (:= and ->)
        if (c == ':' && pos + 1 < src.length() && src[pos + 1] == '=') {
            advance();
            advance();
            tokens.push_back({TokenType::ASSIGN_INIT, ":=", line});
            continue;
        }

        // 6. Equality (==)
        if (c == '=') {
            if (pos + 1 < src.length() && src[pos + 1] == '=') {
                advance();
                advance();
                tokens.push_back({TokenType::EQUAL, "==", line});
                continue;
            }
        }

        // Arrow (->) and Minus-Assign (-=)
        if (c == '-') {
            if (pos + 1 < src.length() && src[pos + 1] == '>') {
                advance();
                advance();
                tokens.push_back({TokenType::ARROW, "->", line});
                continue;
            }
            if (pos + 1 < src.length() && src[pos + 1] == '=') {
                advance();
                advance();
                tokens.push_back({TokenType::MINUS_ASSIGN, "-=", line});
                continue;
            }
        }

        // 7. Comments
        if (c == '/') {
            if (pos + 1 < src.length() && src[pos + 1] == '/') {
                while (pos < src.length() && peek() != '\n') {
                    advance();
                }
                continue;
            }
        }

        // Comparison Operators (>=, >, <=, <, !=)
        if (c == '>') {
            if (pos + 1 < src.length() && src[pos + 1] == '=') {
                advance();
                advance();
                tokens.push_back({TokenType::GREATER_EQUAL, ">=", line});
            } else {
                advance();
                tokens.push_back({TokenType::GREATER, ">", line});
            }
            continue;
        }

        if (c == '<') {
            if (pos + 1 < src.length() && src[pos + 1] == '=') {
                advance();
                advance();
                tokens.push_back({TokenType::LESS_EQUAL, "<=", line});
            } else {
                advance();
                tokens.push_back({TokenType::LESS, "<", line});
            }
            continue;
        }

        if (c == '!') {
            if (pos + 1 < src.length() && src[pos + 1] == '=') {
                advance();
                advance();
                tokens.push_back({TokenType::NOT_EQUAL, "!=", line});
                continue;
            }
        }

        // 8. Single-character Tokens
        switch (c) {
            case '(':
                tokens.push_back({TokenType::LPAREN, "(", line});
                break;
            case ')':
                tokens.push_back({TokenType::RPAREN, ")", line});
                break;
            case '{':
                tokens.push_back({TokenType::LBRACE, "{", line});
                break;
            case '}':
                tokens.push_back({TokenType::RBRACE, "}", line});
                break;
            case '[':
                tokens.push_back({TokenType::LBRACKET, "[", line});
                break;
            case ']':
                tokens.push_back({TokenType::RBRACKET, "]", line});
                break;
            case ':':
                tokens.push_back({TokenType::COLON, ":", line});
                break;
            case ';':
                tokens.push_back({TokenType::SEMICOLON, ";", line});
                break;
            case ',':
                tokens.push_back({TokenType::COMMA, ",", line});
                break;
            case '+':
                tokens.push_back({TokenType::PLUS, "+", line});
                break;
            case '-':
                tokens.push_back({TokenType::MINUS, "-", line});
                break;
            case '*':
                tokens.push_back({TokenType::STAR, "*", line});
                break;
            case '/':
                tokens.push_back({TokenType::SLASH, "/", line});
                break;
            case '=':
                tokens.push_back({TokenType::ASSIGN, "=", line});
                break;
            case '.':
                if (pos + 1 < src.length() && src[pos] == '.' &&
                    src[pos + 1] == '.') {
                    advance();  // 2nd dot
                    advance();  // 3rd dot
                    tokens.push_back({TokenType::ELLIPSIS, "...", line});
                } else {
                    tokens.push_back({TokenType::DOT, ".", line});
                }
                break;
            case '|':
                tokens.push_back({TokenType::PIPE, "|", line});
                break;
            default:
                tokens.push_back({TokenType::ERROR, std::string(1, c), line});
                break;
        }
        advance();
    }

    tokens.push_back({TokenType::EOF_TOKEN, "", line});
    return tokens;
}