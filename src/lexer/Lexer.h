#pragma once
#include <string>
#include <vector>
#include "Token.h"

class Lexer {
public:
    explicit Lexer(const std::string& input);
    Token nextToken();
    std::vector<Token> tokenize();    // now implemented

private:
    std::string input;
    size_t index = 0, line = 1, column = 1;

    char currentChar() const;
    char peek() const;
    void advance();
    void skipWhitespace();
    void skipComment();
    Token generateKeywordOrIdentifier();
    Token generateInteger();
    Token generateOperator();
    Token generatePunctuation();
};