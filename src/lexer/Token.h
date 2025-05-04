#pragma once

#include <string>

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


// include/lexer/Lexer.h
#pragma once

#include <string>
#include <vector>
#include "Token.h"

class Lexer {
public:
    explicit Lexer(const std::string& input);
    Token nextToken();

private:
    std::string input;
    size_t index = 0;
    size_t line = 1;
    size_t column = 1;

    char currentChar() const;
    void advance();
    void skipWhitespace();
    void skipComment();
    Token generateKeywordOrIdentifier();
    Token generateInteger();
    Token generateOperator();
    Token generatePunctuation();
    Token generateType();
    std::vector<Token> tokenize();
};