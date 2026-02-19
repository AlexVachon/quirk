#include <cctype>
#include <iostream>
#include "lexer.hpp"

// =========================================================
//  HELPER METHODS
// =========================================================

char Lexer::peek(int offset) const
{
    if (pos + offset >= src.length())
        return '\0';
    return src[pos + offset];
}

char Lexer::advance()
{
    if (pos >= src.length())
        return '\0';
    char c = src[pos++];
    return c;
}

bool Lexer::match(char expected)
{
    if (peek() == expected)
    {
        advance();
        return true;
    }
    return false;
}

// =========================================================
//  MAIN TOKENIZE LOOP
// =========================================================

std::vector<Token> Lexer::tokenize()
{
    std::vector<Token> tokens;
    while (true)
    {
        // 1. Drain Buffer first (from String Interpolation desugaring)
        if (!tokenBuffer.empty())
        {
            tokens.push_back(tokenBuffer.front());
            tokenBuffer.pop_front();
            if (tokens.back().type == TokenType::EOF_TOKEN)
                break;
            continue;
        }

        // 2. Scan next raw token
        Token t = nextToken();
        tokens.push_back(t);
        if (t.type == TokenType::EOF_TOKEN)
            break;
    }
    return tokens;
}

// =========================================================
//  TOKEN SCANNER
// =========================================================

Token Lexer::nextToken()
{
    // 1. Skip Whitespace
    while (pos < src.length() && isspace(peek()))
    {
        if (peek() == '\n')
            line++;
        advance();
    }

    if (pos >= src.length())
        return {TokenType::EOF_TOKEN, "", line};

    char c = peek();

    // ---------------------------------------------------------
    //  Skip Markdown-style Docstrings (--- ... ---)
    // ---------------------------------------------------------
    if (c == '-' && peek(1) == '-' && peek(2) == '-')
    {
        advance(); // consume 1st '-'
        advance(); // consume 2nd '-'
        advance(); // consume 3rd '-'

        // We are now inside a doc block. Skip EVERYTHING until we see the next ---
        while (pos < src.length())
        {
            if (peek() == '\n')
                line++;

            // Check for closing ---
            if (peek() == '-' && peek(1) == '-' && peek(2) == '-')
            {
                advance(); // consume 1st '-'
                advance(); // consume 2nd '-'
                advance(); // consume 3rd '-'

                // We successfully skipped the whole block.
                // Recursively call nextToken() to get the actual next piece of code!
                return nextToken();
            }
            advance();
        }

        // If we hit EOF before closing the doc block, just return EOF.
        return {TokenType::EOF_TOKEN, "", line};
    }

    // --- FIX: Handle BOTH Double and Single Quotes ---
    if (c == '"' || c == '\'')
    {
        tokenizeString(); // Pushes tokens to tokenBuffer
        // Return the first token immediately
        Token t = tokenBuffer.front();
        tokenBuffer.pop_front();
        return t;
    }

    // 3. Handle Identifiers and Keywords
    if (isalpha(c) || c == '_')
    {
        std::string ident;
        while (isalnum(peek()) || peek() == '_')
            ident += advance();

        if (ident == "define")
            return {TokenType::DEFINE, ident, line};
        if (ident == "struct")
            return {TokenType::STRUCT, ident, line};
        if (ident == "super")
            return {TokenType::SUPER, ident, line};
        if (ident == "use")
            return {TokenType::USE, ident, line};
        if (ident == "from")
            return {TokenType::FROM, ident, line};
        if (ident == "extend")
            return {TokenType::EXTEND, ident, line};
        if (ident == "init")
            return {TokenType::INIT, ident, line};
        if (ident == "extern")
            return {TokenType::EXTERN, ident, line};
        if (ident == "return")
            return {TokenType::RETURN, ident, line};
        if (ident == "if")
            return {TokenType::IF, ident, line};
        if (ident == "else")
            return {TokenType::ELSE, ident, line};
        if (ident == "elif")
            return {TokenType::ELIF, ident, line};
        if (ident == "and")
            return {TokenType::AND, ident, line};
        if (ident == "or")
            return {TokenType::OR, ident, line};
        if (ident == "not")
            return {TokenType::NOT, ident, line};
        if (ident == "while")
            return {TokenType::WHILE, ident, line};
        if (ident == "for")
            return {TokenType::FOR, ident, line};
        if (ident == "in")
            return {TokenType::IN, ident, line};
        if (ident == "ref")
            return {TokenType::REF, ident, line};
        if (ident == "with")
            return {TokenType::WITH, ident, line};
        if (ident == "as")
            return {TokenType::AS, ident, line};
        if (ident == "del")
            return {TokenType::DEL, ident, line};
        if (ident == "true")
            return {TokenType::TRUE, ident, line};
        if (ident == "false")
            return {TokenType::FALSE, ident, line};
        if (ident == "try")
            return {TokenType::TRY, ident, line};
        if (ident == "catch")
            return {TokenType::CATCH, ident, line};
        if (ident == "throw")
            return {TokenType::THROW, ident, line};

        if (ident == "trigger")
            return {TokenType::TRIGGER, ident, line};
            
        return {TokenType::IDENTIFIER, ident, line};
    }

    // 4. Handle Numbers
    if (isdigit(c))
    {
        std::string num;
        while (isdigit(peek()))
            num += advance();

        // ONLY consume dot if the NEXT char is a digit
        // This allows "10.str()" to work (dot belongs to member access)
        // But "10.5" will still be a double.
        if (peek() == '.' && isdigit(peek(1)))
        {
            num += advance();
            while (isdigit(peek()))
                num += advance();
            return {TokenType::FLOAT_LITERAL, num, line}; // Return Double
        }

        return {TokenType::INT_LITERAL, num, line}; // Return Int
    }

    // 5. Multi-character Operators
    if (c == ':' && peek(1) == '=') {
        advance(); advance();
        return {TokenType::ASSIGN_INIT, ":=", line};
    }
    if (c == '=' && peek(1) == '=') {
        advance(); advance();
        return {TokenType::EQUAL, "==", line};
    }
    if (c == '-' && peek(1) == '>') {
        advance(); advance();
        return {TokenType::ARROW, "->", line};
    }
    if (c == '-' && peek(1) == '=') {
        advance(); advance();
        return {TokenType::MINUS_ASSIGN, "-=", line};
    }
    
    if (c == '+' && peek(1) == '=') {
        advance(); advance();
        return {TokenType::PLUS_ASSIGN, "+=", line};
    }
    if (c == '*' && peek(1) == '=') {
        advance(); advance();
        return {TokenType::STAR_ASSIGN, "*=", line};
    }
    if (c == '/' && peek(1) == '=') {
        advance(); advance();
        return {TokenType::SLASH_ASSIGN, "/=", line};
    }

    // 6. Comments
    if (c == '/')
    {
        if (peek(1) == '/')
        {
            while (pos < src.length() && peek() != '\n')
            {
                advance();
            }
            return nextToken(); // Recursively call to get the real next token
        }
    }

    // Comparison Operators
    if (c == '>')
    {
        if (peek(1) == '=')
        {
            advance();
            advance();
            return {TokenType::GREATER_EQUAL, ">=", line};
        }
        advance();
        return {TokenType::GREATER, ">", line};
    }
    if (c == '<')
    {
        if (peek(1) == '=')
        {
            advance();
            advance();
            return {TokenType::LESS_EQUAL, "<=", line};
        }
        advance();
        return {TokenType::LESS, "<", line};
    }
    if (c == '!')
    {
        if (peek(1) == '=')
        {
            advance();
            advance();
            return {TokenType::NOT_EQUAL, "!=", line};
        }
    }

    // 7. Single-character Tokens
    advance(); // Consume the char
    switch (c)
    {
    case '(':
        return {TokenType::LPAREN, "(", line};
    case ')':
        return {TokenType::RPAREN, ")", line};
    case '{':
        return {TokenType::LBRACE, "{", line};
    case '}':
        return {TokenType::RBRACE, "}", line};
    case '[':
        return {TokenType::LBRACKET, "[", line};
    case ']':
        return {TokenType::RBRACKET, "]", line};
    case ':':
        return {TokenType::COLON, ":", line};
    case ';':
        return {TokenType::SEMICOLON, ";", line};
    case ',':
        return {TokenType::COMMA, ",", line};
    case '+':
        return {TokenType::PLUS, "+", line};
    case '-':
        return {TokenType::MINUS, "-", line};
    case '*':
        return {TokenType::STAR, "*", line};
    case '/':
        return {TokenType::SLASH, "/", line};
    case '=':
        return {TokenType::ASSIGN, "=", line};
    case '.':
        return {TokenType::DOT, ".", line};
    case '|':
        return {TokenType::PIPE, "|", line};
    default:
        return {TokenType::ERROR, std::string(1, c), line};
    }
}

// =========================================================
//  STRING INTERPOLATION LOGIC
// =========================================================

void Lexer::tokenizeString()
{
    int startLine = line;
    
    // --- FIX: Capture the quote type so we know when to stop ---
    char quoteType = peek(); 
    advance(); // Skip opening quote

    std::vector<std::string> args;
    std::string format_builder = "";

    // Read until we hit the SAME quote type
    while (pos < src.length() && peek() != quoteType)
    {
        char c = peek();

        // Handle Escapes
        if (c == '\\')
        {
            advance(); // Consume '\'
            if (pos < src.length())
            {
                char esc = advance();
                format_builder += '\\';
                format_builder += esc;
            }
            continue;
        }

        // 1. Found '$' - Start Interpolation
        if (c == '$')
        {
            advance(); // Skip '$'

            // Case A: ${expression % fmt}
            if (peek() == '{')
            {
                advance(); // Skip '{'
                std::string expr = "";
                std::string fmt = "";
                bool readingFmt = false;

                // Read until '}'
                while (pos < src.length() && peek() != '}')
                {
                    // Check for Modulo/Format separator '%' or Pipe '|'
                    if (!readingFmt && (peek() == '%' || peek() == '|'))
                    {
                        readingFmt = true;
                        advance();
                        continue;
                    }

                    if (readingFmt)
                        fmt += advance();
                    else
                        expr += advance();
                }
                if (peek() == '}')
                    advance(); // Skip '}'

                // Add placeholder to string: "{ % .2f}" or "{}"
                format_builder += "{";
                if (!fmt.empty())
                {
                    format_builder += " % "; // Normalized separator for Runtime
                    format_builder += fmt;
                }
                format_builder += "}";

                // Capture expression
                size_t first = expr.find_first_not_of(" \t");
                if (std::string::npos != first)
                {
                    size_t last = expr.find_last_not_of(" \t");
                    expr = expr.substr(first, (last - first + 1));
                }
                args.push_back(expr);
            }
            // Case B: $variable (Simple Identifier)
            else if (isalpha(peek()) || peek() == '_')
            {
                std::string id = "";
                while (isalnum(peek()) || peek() == '_')
                {
                    id += advance();
                }
                format_builder += "{}";
                args.push_back(id);
            }
            // Case C: Just a '$' symbol (escaped or trailing)
            else
            {
                format_builder += "$";
            }
        }
        // 2. Normal Character
        else
        {
            format_builder += advance();
        }
    }

    if (peek() == quoteType)
        advance(); // Skip closing quote


    // Exactly 1 char and no interpolation? It's a C-style Char!
    if (quoteType == '\'' && format_builder.length() == 1 && args.empty()) {
        tokenBuffer.push_back({TokenType::CHAR_LITERAL, "'" + format_builder + "'", startLine});
    } else {
        // Otherwise, it's a full String. Force double quotes for downstream safety.
        tokenBuffer.push_back({TokenType::STRING_LITERAL, "\"" + format_builder + "\"", startLine});

        // If interpolations exist, generate: .format(arg1, arg2)
        if (!args.empty())
        {
            tokenBuffer.push_back({TokenType::DOT, ".", startLine});
            tokenBuffer.push_back({TokenType::IDENTIFIER, "format", startLine});
            tokenBuffer.push_back({TokenType::LPAREN, "(", startLine});

            for (size_t i = 0; i < args.size(); i++)
            {
                Lexer subLexer(args[i]);
                std::vector<Token> subTokens = subLexer.tokenize();

                for (const auto &t : subTokens)
                {
                    if (t.type != TokenType::EOF_TOKEN)
                    {
                        Token adjustedToken = t;
                        adjustedToken.line = startLine; 
                        tokenBuffer.push_back(adjustedToken);
                    }
                }

                if (i < args.size() - 1)
                {
                    tokenBuffer.push_back({TokenType::COMMA, ",", startLine});
                }
            }
            tokenBuffer.push_back({TokenType::RPAREN, ")", startLine});
        }
    }
}