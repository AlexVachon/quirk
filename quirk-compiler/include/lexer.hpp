#ifndef LEXER_HPP
#define LEXER_HPP

#include <string>
#include <vector>
#include <deque>

enum class TokenType {
    // Keywords
    DEFINE,
    STRUCT,
    SUPER,
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
    FROM,
    IN,
    WITH,
    AS,
    IS,
    ENUM,
    MATCH,
    CASE,
    WHERE,
    TYPE_KW,

    // Literals
    IDENTIFIER,
    INT_LITERAL,
    FLOAT_LITERAL,
    STRING_LITERAL,
    CHAR_LITERAL,
    TRUE,
    FALSE,
    QUIRK_NULL,
    PIPE,
    AMPERSAND,

    TRY, CATCH, THROW, FINALLY,

    BREAK, CONTINUE,

    FN,
    FAT_ARROW,

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
    PLUS_ASSIGN,
    STAR_ASSIGN,
    SLASH_ASSIGN,

    ELLIPSIS,
    TRIPLE_MINUS,
    DOTDOT,          // ..  (range)

    QUESTION,        // ?
    QUESTION_DOT,    // ?.
    NULL_COALESCE,   // ??

    NONLOCAL,        // nonlocal keyword
    GLOBAL,          // global keyword
    INTERFACE,       // interface keyword

    // Special
    EOF_TOKEN,
    ERROR
};

struct Token {
    TokenType type;
    std::string value;
    int line;
    int col = 0;
};

class Lexer {
   public:
    Lexer(const std::string& source) : src(source), pos(0), line(1), col(1) {}
    std::vector<Token> tokenize();

   private:
    std::string src;
    size_t pos;
    int line;
    int col;

    std::deque<Token> tokenBuffer;

    char peek(int offset = 0) const;
    char advance();
    bool match(char expected);

    void tokenizeString();
    Token nextToken();
};

#endif