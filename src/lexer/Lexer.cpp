// src/lexer/Lexer.cpp
#include "Lexer.h"
#include <unordered_set>
#include <cctype>

static bool isKeyword(const std::string& s) {
    static const std::unordered_set<std::string> kws = {
        "package","var","const","mut","pub","fn","end","if","else","ret","print"
    };
    return kws.count(s) > 0;
}

Lexer::Lexer(const std::string& input)
  : input(input) {}

char Lexer::currentChar() const {
    return (index < input.size() ? input[index] : '\0');
}

char Lexer::peek() const {
    return (index+1 < input.size() ? input[index+1] : '\0');
}

void Lexer::advance() {
    if (currentChar() == '\n') {
        line++; column = 1;
    } else {
        column++;
    }
    index++;
}

void Lexer::skipWhitespace() {
    while (std::isspace(currentChar())) advance();
}

void Lexer::skipComment() {
    if (currentChar() == '/' && peek() == '/') {
        while (currentChar() && currentChar() != '\n') advance();
    }
}

Token Lexer::generateKeywordOrIdentifier() {
    size_t startCol = column;
    std::string val;
    while (std::isalnum(currentChar()) || currentChar() == '_') {
        val.push_back(currentChar());
        advance();
    }
    TokenType t = isKeyword(val)
                  ? TokenType::Keyword
                  : TokenType::Identifier;
    return {t, val, line, startCol};
}

Token Lexer::generateInteger() {
    size_t startCol = column;
    std::string val;
    while (std::isdigit(currentChar())) {
        val.push_back(currentChar());
        advance();
    }
    return {TokenType::Integer, val, line, startCol};
}

Token Lexer::generateOperator() {
    size_t startCol = column;
    std::string op;
    // handle ==, !=, >=, <=
    op.push_back(currentChar());
    if ((op=="="||op=="!"||op==">"||op=="<") && peek()=='=') {
        advance();
        op.push_back(currentChar());
    }
    advance();
    return {TokenType::Operator, op, line, startCol};
}

Token Lexer::generatePunctuation() {
    size_t startCol = column;
    std::string p(1, currentChar());
    advance();
    return {TokenType::Punctuation, p, line, startCol};
}

Token Lexer::nextToken() {
    // end‑of‑file
    if (!currentChar()) return {TokenType::EndOfFile, "", line, column};

    skipWhitespace();
    skipComment();
    skipWhitespace();

    if (!currentChar()) return {TokenType::EndOfFile, "", line, column};

    char c = currentChar();
    // identifier/keyword
    if (std::isalpha(c) || c=='_') return generateKeywordOrIdentifier();
    // integer literal
    if (std::isdigit(c)) return generateInteger();
    // operator chars
    if (c=='+'||c=='-'||c=='*'||c=='/'||c=='='||c=='>'||c=='<'||c=='!') 
        return generateOperator();
    // punctuation in Cruft: :, ,, (, )
    if (c==':'||c==','||c=='('||c==')')
        return generatePunctuation();
    // drop semicolons entirely (no ;) 
    if (c==';') {
        advance();
        return nextToken();
    }
    // unknown
    size_t col0 = column;
    std::string bad(1,c);
    advance();
    return {TokenType::Invalid, bad, line, col0};
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> out;
    while (true) {
        Token t = nextToken();
        out.push_back(t);
        if (t.type == TokenType::EndOfFile) break;
    }
    return out;
}