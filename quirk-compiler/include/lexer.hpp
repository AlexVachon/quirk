#ifndef LEXER_HPP
#define LEXER_HPP

#include <string>
#include <vector>

enum class TokenType {
    // Keywords
    DEFINE,
    STRUCT,
    INIT,
    EXTERN,
    EXTEND,
    CONST,
    RETURN,
    IF,
    ELSE,
    ELIF,
    AND,
    OR,
    NOT,
    WHILE,
    FOR,
    REF,
    MOVE,
    DEL,
    USE,
    IN,
    WITH,
    AS,

    // Literals
    IDENTIFIER,
    INT_LITERAL,
    STRING_LITERAL,
    TRUE,
    FALSE,
    PIPE,

    // Operators
    ASSIGN_INIT,
    ASSIGN,
    EQUAL,
    ARROW,
    LBRACE,
    RBRACE,
    LPAREN,
    RPAREN,
    LBRACKET,
    RBRACKET,
    COLON,
    SEMICOLON,
    COMMA,
    PLUS,
    MINUS,
    STAR,
    SLASH,
    GREATER,
    LESS,
    GREATER_EQUAL,
    LESS_EQUAL,
    NOT_EQUAL,
    DOT,
    MINUS_ASSIGN,

    // NEW: Ellipsis "..."
    ELLIPSIS,

    // Special
    EOF_TOKEN,
    ERROR
};

struct Token {
    TokenType type;
    std::string value;
    int line;
};

class Lexer {
   public:
    Lexer(const std::string& source);
    std::vector<Token> tokenize();

   private:
    std::string src;
    size_t pos = 0;
    int line = 1;

    char peek() { return (pos < src.length()) ? src[pos] : '\0'; }
    char advance() { return src[pos++]; }
};

#endif