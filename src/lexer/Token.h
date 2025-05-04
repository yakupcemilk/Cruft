#pragma once

#include <vector>
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