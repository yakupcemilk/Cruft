#pragma once
#include "Token.h"
#include <string>
#include <vector>

namespace cruft {

class Lexer {
public:
    explicit Lexer(const std::string& source);

    Token nextToken();
    std::vector<Token> getTokens() {
        return tokenize();
    }

private:
    std::string input;
    size_t pos = 0;
    int line = 1;

    void advance();
    bool matchNext(char expected) const;
    void skipWhitespace();
    void skipBlockComment();

    Token lexLineComment();
    Token lexIdentifierOrKeyword();
    Token lexNumber();
    Token lexString();
    std::vector<Token> tokenize();
};

} // namespace cruft