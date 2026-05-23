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
    if (c == '\n') { line++; col = 1; } else { col++; }
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
    // 1. Skip Whitespace (advance() tracks line/col internally)
    while (pos < src.length() && isspace(peek()))
        advance();

    if (pos >= src.length())
        return {TokenType::EOF_TOKEN, "", line, col};

    int startLine = line;
    int startCol  = col;

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

        if (ident == "define")   return {TokenType::DEFINE,     ident, startLine, startCol};
        if (ident == "struct")   return {TokenType::STRUCT,     ident, startLine, startCol};
        if (ident == "super")    return {TokenType::SUPER,      ident, startLine, startCol};
        if (ident == "use")      return {TokenType::USE,        ident, startLine, startCol};
        if (ident == "from")     return {TokenType::FROM,       ident, startLine, startCol};
        if (ident == "extend")   return {TokenType::EXTEND,     ident, startLine, startCol};
        if (ident == "init")     return {TokenType::INIT,       ident, startLine, startCol};
        if (ident == "extern")   return {TokenType::EXTERN,     ident, startLine, startCol};
        if (ident == "return")   return {TokenType::RETURN,     ident, startLine, startCol};
        if (ident == "if")       return {TokenType::IF,         ident, startLine, startCol};
        if (ident == "else")     return {TokenType::ELSE,       ident, startLine, startCol};
        if (ident == "elif")     return {TokenType::ELIF,       ident, startLine, startCol};
        if (ident == "and")      return {TokenType::AND,        ident, startLine, startCol};
        if (ident == "or")       return {TokenType::OR,         ident, startLine, startCol};
        if (ident == "not")      return {TokenType::NOT,        ident, startLine, startCol};
        if (ident == "while")    return {TokenType::WHILE,      ident, startLine, startCol};
        if (ident == "for")      return {TokenType::FOR,        ident, startLine, startCol};
        if (ident == "in")       return {TokenType::IN,         ident, startLine, startCol};
        // `ref` was reserved for future pass-by-reference semantics, but
        // codegen never used the flag. Treat as a plain identifier so
        // names like `ref` aren't blocked from real code.
        if (ident == "with")     return {TokenType::WITH,       ident, startLine, startCol};
        if (ident == "as")       return {TokenType::AS,         ident, startLine, startCol};
        if (ident == "del")      return {TokenType::DEL,        ident, startLine, startCol};
        if (ident == "true")     return {TokenType::TRUE,       ident, startLine, startCol};
        if (ident == "false")    return {TokenType::FALSE,      ident, startLine, startCol};
        if (ident == "null")     return {TokenType::QUIRK_NULL, ident, startLine, startCol};
        if (ident == "is")       return {TokenType::IS,         ident, startLine, startCol};
        if (ident == "try")      return {TokenType::TRY,        ident, startLine, startCol};
        if (ident == "catch")    return {TokenType::CATCH,      ident, startLine, startCol};
        if (ident == "throw")    return {TokenType::THROW,      ident, startLine, startCol};
        if (ident == "finally")  return {TokenType::FINALLY,    ident, startLine, startCol};
        if (ident == "break")    return {TokenType::BREAK,      ident, startLine, startCol};
        if (ident == "continue") return {TokenType::CONTINUE,   ident, startLine, startCol};
        if (ident == "enum")     return {TokenType::ENUM,       ident, startLine, startCol};
        if (ident == "match")    return {TokenType::MATCH,      ident, startLine, startCol};
        if (ident == "case")     return {TokenType::CASE,       ident, startLine, startCol};
        if (ident == "fn")       return {TokenType::FN,         ident, startLine, startCol};
        if (ident == "where")    return {TokenType::WHERE,      ident, startLine, startCol};
        if (ident == "const")    return {TokenType::CONST,      ident, startLine, startCol};
        if (ident == "type")     return {TokenType::TYPE_KW,    ident, startLine, startCol};
        if (ident == "nonlocal")  return {TokenType::NONLOCAL,   ident, startLine, startCol};
        if (ident == "global")    return {TokenType::GLOBAL,     ident, startLine, startCol};
        if (ident == "interface") return {TokenType::INTERFACE,  ident, startLine, startCol};
        return {TokenType::IDENTIFIER, ident, startLine, startCol};
    }

    // 4. Handle Numbers
    if (isdigit(c))
    {
        // Hex literal: 0xFF, 0xCafe — converted to a decimal string token
        // so the rest of the pipeline (Sema, Codegen) doesn't need to know
        // about the base. Keep the loop tight; bail to decimal scanning if
        // the leading `0x`/`0b` is followed by no valid digits.
        if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
            advance(); advance(); // consume "0x"
            std::string hex;
            auto isHex = [](char ch) {
                return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
            };
            while (isHex(peek())) hex += advance();
            if (hex.empty()) return {TokenType::ERROR, "0x", startLine, startCol};
            long long v = std::stoll(hex, nullptr, 16);
            return {TokenType::INT_LITERAL, std::to_string(v), startLine, startCol};
        }
        // Binary literal: 0b1010 — same decimal-normalisation strategy.
        if (peek() == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
            advance(); advance(); // consume "0b"
            std::string bin;
            while (peek() == '0' || peek() == '1') bin += advance();
            if (bin.empty()) return {TokenType::ERROR, "0b", startLine, startCol};
            long long v = std::stoll(bin, nullptr, 2);
            return {TokenType::INT_LITERAL, std::to_string(v), startLine, startCol};
        }

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
            return {TokenType::FLOAT_LITERAL, num, startLine, startCol};
        }

        return {TokenType::INT_LITERAL, num, startLine, startCol};
    }

    // 5. Multi-character Operators
    // Optional/safe operators must come before single '?'
    if (c == '?' && peek(1) == '?') {
        advance(); advance();
        return {TokenType::NULL_COALESCE, "??", startLine, startCol};
    }
    if (c == '?' && peek(1) == '.') {
        advance(); advance();
        return {TokenType::QUESTION_DOT, "?.", startLine, startCol};
    }
    // Ellipsis '...' must be checked before '..' and single DOT
    if (c == '.' && peek(1) == '.' && peek(2) == '.') {
        advance(); advance(); advance();
        return {TokenType::ELLIPSIS, "...", startLine, startCol};
    }
    // Range operator '..' (two dots, not three)
    if (c == '.' && peek(1) == '.' && peek(2) != '.') {
        advance(); advance();
        return {TokenType::DOTDOT, "..", startLine, startCol};
    }
    if (c == ':' && peek(1) == '=') {
        advance(); advance();
        return {TokenType::ASSIGN_INIT, ":=", startLine, startCol};
    }
    if (c == '=' && peek(1) == '=') {
        advance(); advance();
        return {TokenType::EQUAL, "==", startLine, startCol};
    }
    if (c == '-' && peek(1) == '>') {
        advance(); advance();
        return {TokenType::ARROW, "->", startLine, startCol};
    }
    if (c == '=' && peek(1) == '>') {
        advance(); advance();
        return {TokenType::FAT_ARROW, "=>", startLine, startCol};
    }
    if (c == '-' && peek(1) == '=') {
        advance(); advance();
        return {TokenType::MINUS_ASSIGN, "-=", startLine, startCol};
    }
    if (c == '+' && peek(1) == '=') {
        advance(); advance();
        return {TokenType::PLUS_ASSIGN, "+=", startLine, startCol};
    }
    if (c == '*' && peek(1) == '=') {
        advance(); advance();
        return {TokenType::STAR_ASSIGN, "*=", startLine, startCol};
    }
    if (c == '/' && peek(1) == '=') {
        advance(); advance();
        return {TokenType::SLASH_ASSIGN, "/=", startLine, startCol};
    }
    if (c == '%' && peek(1) == '=') {
        advance(); advance();
        return {TokenType::PERCENT_ASSIGN, "%=", startLine, startCol};
    }

    // 6. Comments
    if (c == '/' && peek(1) == '/') {
        while (pos < src.length() && peek() != '\n')
            advance();
        return nextToken();
    }

    // Comparison Operators
    if (c == '>') {
        if (peek(1) == '=') { advance(); advance(); return {TokenType::GREATER_EQUAL, ">=", startLine, startCol}; }
        advance();
        return {TokenType::GREATER, ">", startLine, startCol};
    }
    if (c == '<') {
        if (peek(1) == '=') { advance(); advance(); return {TokenType::LESS_EQUAL, "<=", startLine, startCol}; }
        advance();
        return {TokenType::LESS, "<", startLine, startCol};
    }
    if (c == '!' && peek(1) == '=') {
        advance(); advance();
        return {TokenType::NOT_EQUAL, "!=", startLine, startCol};
    }

    // 7. Single-character Tokens
    advance(); // Consume the char
    switch (c)
    {
    case '(':  return {TokenType::LPAREN,     "(", startLine, startCol};
    case ')':  return {TokenType::RPAREN,     ")", startLine, startCol};
    case '{':  return {TokenType::LBRACE,     "{", startLine, startCol};
    case '}':  return {TokenType::RBRACE,     "}", startLine, startCol};
    case '[':  return {TokenType::LBRACKET,   "[", startLine, startCol};
    case ']':  return {TokenType::RBRACKET,   "]", startLine, startCol};
    case ':':  return {TokenType::COLON,      ":", startLine, startCol};
    case ';':  return {TokenType::SEMICOLON,  ";", startLine, startCol};
    case ',':  return {TokenType::COMMA,      ",", startLine, startCol};
    case '@':  return {TokenType::AT,         "@", startLine, startCol};
    case '+':  return {TokenType::PLUS,       "+", startLine, startCol};
    case '-':  return {TokenType::MINUS,      "-", startLine, startCol};
    case '*':  return {TokenType::STAR,       "*", startLine, startCol};
    case '/':  return {TokenType::SLASH,      "/", startLine, startCol};
    case '%':  return {TokenType::PERCENT,    "%", startLine, startCol};
    case '=':  return {TokenType::ASSIGN,     "=", startLine, startCol};
    case '.':  return {TokenType::DOT,        ".", startLine, startCol};
    case '&':  return {TokenType::AMPERSAND,  "&", startLine, startCol};
    case '|':  return {TokenType::PIPE,       "|", startLine, startCol};
    case '?':  return {TokenType::QUESTION,   "?", startLine, startCol};
    default:   return {TokenType::ERROR, std::string(1, c), startLine, startCol};
    }
}

// =========================================================
//  STRING INTERPOLATION LOGIC
// =========================================================

void Lexer::tokenizeString()
{
    int startLine = line;
    int startCol  = col;

    // --- FIX: Capture the quote type so we know when to stop ---
    char quoteType = peek();
    advance(); // Skip opening quote

    // Triple-quoted string: `"""..."""` or `'''...'''`. Reads as a single
    // String literal that spans multiple lines and treats newlines as
    // content. Interpolation (`${expr}`) still works inside. Terminated by
    // a matching triple-quote sequence.
    bool tripleQuoted = (peek() == quoteType && peek(1) == quoteType);
    if (tripleQuoted) {
        advance(); advance(); // consume the remaining two quote chars
    }

    std::vector<std::string> args;
    std::string format_builder = "";

    // Read until we hit the closing delimiter. For triple-quoted strings,
    // that's a `"""` (or `'''`) sequence; for normal strings, a single
    // matching quote.
    auto atTerminator = [&]() -> bool {
        if (pos >= src.length()) return false;
        if (peek() != quoteType) return false;
        if (!tripleQuoted) return true;
        return peek(1) == quoteType && peek(2) == quoteType;
    };

    while (pos < src.length() && !atTerminator())
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

            // Case A: ${expression : fmt}   (or `%`/`|` as legacy separators)
            if (peek() == '{')
            {
                advance(); // Skip '{'
                std::string expr = "";
                std::string fmt = "";
                bool readingFmt = false;
                // Track bracket / paren / quote depth so a `:` inside a
                // slice (`arr[0:5]`) or string literal stays inside the
                // expression rather than triggering the format-spec switch.
                int depth = 0;
                char quoteCh = 0;

                // Read until '}'
                while (pos < src.length() && peek() != '}')
                {
                    char ch = peek();
                    if (!readingFmt) {
                        if (quoteCh == 0 && (ch == '"' || ch == '\'')) { quoteCh = ch; expr += advance(); continue; }
                        if (quoteCh != 0) {
                            if (ch == '\\' && pos + 1 < src.length()) { expr += advance(); expr += advance(); continue; }
                            if (ch == quoteCh) quoteCh = 0;
                            expr += advance();
                            continue;
                        }
                        if (ch == '(' || ch == '[') { depth++; expr += advance(); continue; }
                        if (ch == ')' || ch == ']') { depth--; expr += advance(); continue; }
                        // Format separator: `%`/`|` (legacy), or `:` at
                        // top-level brace depth (Python-style).
                        if (depth == 0 && (ch == '%' || ch == '|' || ch == ':'))
                        {
                            readingFmt = true;
                            advance();
                            continue;
                        }
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

    if (tripleQuoted) {
        // Consume the closing triple-quote (three chars). atTerminator()
        // already verified all three are present before exiting the loop.
        if (peek() == quoteType) { advance(); advance(); advance(); }

        // Dedent: triple-quoted strings get the common leading whitespace
        // stripped off every line so users can indent the literal to match
        // surrounding code without that indent leaking into the value.
        // Also trims a leading and trailing newline so the canonical form
        //     sql := """
        //         SELECT 1
        //     """
        // produces `"SELECT 1\n"` rather than `"\n        SELECT 1\n    "`.
        if (!format_builder.empty()) {
            // Strip exactly one leading \n if the literal opens with one.
            if (format_builder.front() == '\n') {
                format_builder.erase(0, 1);
            }
            // Find min leading whitespace across non-empty lines.
            size_t minIndent = std::string::npos;
            size_t i = 0;
            while (i < format_builder.size()) {
                size_t lineStart = i;
                while (i < format_builder.size() && format_builder[i] != '\n') i++;
                // measure leading whitespace on this line
                size_t ws = 0;
                while (lineStart + ws < i &&
                       (format_builder[lineStart + ws] == ' ' ||
                        format_builder[lineStart + ws] == '\t')) ws++;
                // ignore blank lines for the min calculation
                if (lineStart + ws < i) {
                    if (ws < minIndent) minIndent = ws;
                }
                if (i < format_builder.size()) i++; // skip the \n
            }
            if (minIndent != std::string::npos && minIndent > 0) {
                std::string out;
                out.reserve(format_builder.size());
                size_t j = 0;
                while (j < format_builder.size()) {
                    size_t lineStart = j;
                    while (j < format_builder.size() && format_builder[j] != '\n') j++;
                    size_t lineLen = j - lineStart;
                    // Strip up to minIndent leading whitespace; lines that
                    // are pure whitespace and shorter just become empty.
                    size_t skip = std::min(minIndent, lineLen);
                    out.append(format_builder, lineStart + skip, lineLen - skip);
                    if (j < format_builder.size()) { out.push_back('\n'); j++; }
                }
                format_builder = std::move(out);
            }
            // Strip one trailing \n if the closing """ was on its own line —
            // detected by the last char being \n after the dedent pass.
            if (!format_builder.empty() && format_builder.back() == '\n') {
                format_builder.pop_back();
            }
        }
    } else if (peek() == quoteType) {
        advance(); // Skip closing quote
    }


    // Exactly 1 char and no interpolation? It's a C-style Char!
    if (quoteType == '\'' && format_builder.length() == 1 && args.empty()) {
        tokenBuffer.push_back({TokenType::CHAR_LITERAL, "'" + format_builder + "'", startLine, startCol});
    } else {
        // Otherwise, it's a full String. Force double quotes for downstream safety.
        tokenBuffer.push_back({TokenType::STRING_LITERAL, "\"" + format_builder + "\"", startLine, startCol});

        // If interpolations exist, generate: .format(arg1, arg2)
        if (!args.empty())
        {
            tokenBuffer.push_back({TokenType::DOT,        ".",      startLine, startCol});
            tokenBuffer.push_back({TokenType::IDENTIFIER,  "format", startLine, startCol});
            tokenBuffer.push_back({TokenType::LPAREN,      "(",      startLine, startCol});

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
                        adjustedToken.col  = startCol;
                        tokenBuffer.push_back(adjustedToken);
                    }
                }

                if (i < args.size() - 1)
                    tokenBuffer.push_back({TokenType::COMMA, ",", startLine, startCol});
            }
            tokenBuffer.push_back({TokenType::RPAREN, ")", startLine, startCol});
        }
    }
}