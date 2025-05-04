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