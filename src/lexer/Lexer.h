#pragma once
#include <string>
#include <vector>
#include <cctype>
#include <memory>

enum class TokenType {
    Identifier,
    Keyword,
    Integer,
    Operator,
    Punctuation,
    Type,
    EndOfFile,
    Invalid
};

struct Token {
    TokenType type;
    std::string value;
    size_t line;
    size_t column;

    Token(TokenType t, const std::string& v, size_t l, size_t c)
        : type(t), value(v), line(l), column(c) {}

    Token() : type(TokenType::Invalid), value(""), line(0), column(0) {}
};

class Lexer {
public:
    Lexer(const std::string& input);

    Token nextToken();

private:
    std::string input;
    size_t index;
    size_t line;
    size_t column;

    char currentChar();
    void advance();
    void skipWhitespace();
    void skipComment();  // Bildirim burada
    Token generateKeywordOrIdentifier(); // Bildirim burada
    Token generateInteger();              // Bildirim burada
    Token generateOperator();             // Bildirim burada
    Token generatePunctuation();         // Bildirim burada
    Token generateType();                // Bildirim burada
    std::vector<Token> tokenize();        // Bildirim burada
};
