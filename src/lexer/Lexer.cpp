#include "Lexer.h"  // Dosyanın en başında yer almalı

Lexer::Lexer(const std::string& input) : input(input), index(0), line(1), column(0) {}

Token Lexer::nextToken() {
    skipWhitespace();
    if (index >= input.size()) {
        return Token(TokenType::EndOfFile, "", line, column);
    }

    char current = currentChar();
    
    if (std::isalpha(current)) {
        return generateKeywordOrIdentifier();
    }
    if (std::isdigit(current)) {
        return generateInteger();
    }
    if (current == '+') {
        return generateOperator();
    }
    // Diğer token tiplerine göre işlemler burada yapılabilir.
    
    return Token(TokenType::Invalid, "", line, column); 
}

char Lexer::currentChar() {
    return input[index];
}

void Lexer::advance() {
    index++;
    column++;
}

void Lexer::skipWhitespace() {
    while (index < input.size() && std::isspace(input[index])) {
        if (input[index] == '\n') {
            line++;
            column = 0;
        }
        advance();
    }
}

void Lexer::skipComment() {
    // Yorumları geçmek için işlem yapılacak
}

Token Lexer::generateKeywordOrIdentifier() {
    // Anahtar kelime veya identifier üretme işlemi yapılacak
    return Token(TokenType::Identifier, "example", line, column);
}

Token Lexer::generateInteger() {
    // Integer token'ı üretme işlemi yapılacak
    return Token(TokenType::Integer, "123", line, column);
}

Token Lexer::generateOperator() {
    // Operator token'ı üretme işlemi yapılacak
    return Token(TokenType::Operator, "+", line, column);
}

Token Lexer::generatePunctuation() {
    // Punctuation token'ı üretme işlemi yapılacak
    return Token(TokenType::Punctuation, ".", line, column);
}

Token Lexer::generateType() {
    // Type token'ı üretme işlemi yapılacak
    return Token(TokenType::Type, "int", line, column);
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    Token tok = nextToken();
    while (tok.type != TokenType::EndOfFile) {
        tokens.push_back(tok);
        tok = nextToken();
    }
    return tokens;
}
