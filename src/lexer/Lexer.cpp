// src/lexer/Lexer.cpp
#include "Lexer.h"
#include <unordered_set>

// Yardımcı: bir string’in anahtar kelime olup olmadığını kontrol eder
static bool isKeyword(const std::string& s) {
    static const std::unordered_set<std::string> keywords = {
        "package", "var", "const"
    };
    return keywords.count(s) > 0;
}

Lexer::Lexer(const std::string& input)
    : input(input), index(0), line(1), column(1) {}

char Lexer::currentChar() const {
    if (index >= input.size()) return '\0';
    return input[index];
}

void Lexer::advance() {
    if (currentChar() == '\n') {
        line++;
        column = 1;
    } else {
        column++;
    }
    index++;
}

void Lexer::skipWhitespace() {
    while (std::isspace(currentChar())) {
        advance();
    }
}

void Lexer::skipComment() {
    // “//” ile başlayan satır sonuna kadar atla
    if (currentChar() == '/' && input[index+1] == '/') {
        while (currentChar() != '\0' && currentChar() != '\n') {
            advance();
        }
    }
}

Token Lexer::generateKeywordOrIdentifier() {
    size_t startCol = column;
    std::string value;
    // Harf veya alt çizgi ile başlar, sonra alfanümerik veya alt çizgi
    while (std::isalnum(currentChar()) || currentChar() == '_') {
        value.push_back(currentChar());
        advance();
    }
    TokenType type = isKeyword(value) ? TokenType::Keyword : TokenType::Identifier;
    return Token(type, value, line, startCol);
}

Token Lexer::generateInteger() {
    size_t startCol = column;
    std::string value;
    while (std::isdigit(currentChar())) {
        value.push_back(currentChar());
        advance();
    }
    return Token(TokenType::Integer, value, line, startCol);
}

Token Lexer::generateOperator() {
    size_t startCol = column;
    char c = currentChar();
    std::string op(1, c);
    // Basit: + - * / = 
    advance();
    return Token(TokenType::Operator, op, line, startCol);
}

Token Lexer::generatePunctuation() {
    size_t startCol = column;
    char c = currentChar();
    std::string p(1, c);
    // Basit: : ; , ( ) { } 
    advance();
    return Token(TokenType::Punctuation, p, line, startCol);
}

Token Lexer::nextToken() {
    // EOF kontrolü
    if (index >= input.size()) {
        return Token(TokenType::EndOfFile, "", line, column);
    }

    // Whitespace ve yorumları atla
    skipWhitespace();
    skipComment();
    skipWhitespace();

    // Tekrar EOF kontrolü (comment sonrası)
    if (index >= input.size()) {
        return Token(TokenType::EndOfFile, "", line, column);
    }

    char c = currentChar();

    // Harf veya '_' → identifier/keyword
    if (std::isalpha(c) || c == '_') {
        return generateKeywordOrIdentifier();
    }
    // Rakam → integer
    if (std::isdigit(c)) {
        return generateInteger();
    }
    // Operatör
    if (c=='+'||c=='-'||c=='*'||c=='/'||c=='=') {
        return generateOperator();
    }
    // Noktalama
    if (c==':'||c==';'||c==','||c=='('||c==')'||c=='{'||c=='}') {
        return generatePunctuation();
    }

    // Tanınmayan karakteri at ve Invalid token dön
    size_t startCol = column;
    std::string bad(1, c);
    advance();
    return Token(TokenType::Invalid, bad, line, startCol);
}
