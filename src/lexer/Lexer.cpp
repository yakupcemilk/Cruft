// src/lexer/Lexer.cpp
#include "Lexer.h"
#include <cctype>
#include <stdexcept>
#include <unordered_map>
#include <vector>

using namespace cruft;

Lexer::Lexer(const std::string &source) : input(source) {}

void Lexer::advance() {
  if (input[pos] == '\n') {
    ++line;
  }
  ++pos;
}

bool Lexer::matchNext(char expected) const {
  return pos + 1 < input.size() && input[pos + 1] == expected;
}

void Lexer::skipWhitespace() {
  while (pos < input.size() && std::isspace(input[pos])) {
    advance();
  }
}

void Lexer::skipBlockComment() {
  // assume we've seen '/*'
  pos += 2;
  while (pos + 1 < input.size() &&
         !(input[pos] == '*' && input[pos + 1] == '/')) {
    advance();
  }
  if (pos + 1 < input.size()) {
    pos += 2;
  }
}

Token Lexer::lexLineComment() {
  int start = pos;
  pos += 2; // skip "//"
  while (pos < input.size() && input[pos] != '\n') {
    advance();
  }
  return {TokenType::COMMENT, input.substr(start, pos - start), line};
}

Token Lexer::lexIdentifierOrKeyword() {
  std::size_t start = pos;
  while (pos < input.size() && (std::isalnum(input[pos]) || input[pos] == '_'))
    advance();
  std::string lex = input.substr(start, pos - start);

  static const std::unordered_map<std::string, TokenType> keywords = {
      {"package", TokenType::PACKAGE}, {"secure", TokenType::SEC},
      {"to", TokenType::TO},           {"export", TokenType::EXPORT},
      {"import", TokenType::IMPORT},   {"use", TokenType::USE},
      {"pub", TokenType::PUB},         {"pri", TokenType::PRI},
      {"fn", TokenType::FUNC},         {"ret", TokenType::RETURN},
      {"end", TokenType::END},         {"class", TokenType::CLASS},
      {"if", TokenType::IF},           {"elseif", TokenType::ELSEIF},
      {"then", TokenType::THEN},       {"else", TokenType::ELSE},
      {"switch", TokenType::SWI},      {"case", TokenType::CASE},
      {"while", TokenType::WHILE},     {"do", TokenType::DO},
      {"print", TokenType::PRINT},     {"const", TokenType::CONST},
      {"var", TokenType::VAR},         {"mut", TokenType::MUT},
      {"array", TokenType::ARRAY},     {"bool", TokenType::BOOL},
      {"true", TokenType::TRUE},       {"false", TokenType::FALSE},
      {"any", TokenType::ANY},         {"void", TokenType::VOID},
      {"object", TokenType::OBJECT},   {"i64", TokenType::I_64},
      {"i32", TokenType::I_32},        {"i16", TokenType::I_16},
      {"i8", TokenType::I_8},          {"f64", TokenType::F_64},
      {"f32", TokenType::F_32},        {"f16", TokenType::F_16},
      {"f8", TokenType::F_8},          {"null", TokenType::NULL_T},
      {"break", TokenType::BREAK},     {"and", TokenType::AND},
      {"or", TokenType::OR},           {"xor", TokenType::XOR},
      {"not", TokenType::NOT},         {"nand", TokenType::NAND},
      {"nor", TokenType::NOR},         {"xnor", TokenType::XNOR},
      {"implies", TokenType::IMPLIES}, {"iff", TokenType::IFF}};

  auto it = keywords.find(lex);
  return {it != keywords.end() ? it->second : TokenType::IDENTIFIER, lex, line};
}

Token Lexer::lexNumber() {
  int start = pos;
  while (pos < input.size() && std::isdigit(input[pos])) {
    advance();
  }
  return {TokenType::NUM, input.substr(start, pos - start), line};
}

Token Lexer::lexString() {
  // assume input[pos] == '"'
  advance();
  int start = pos;
  while (pos < input.size() && input[pos] != '"') {
    advance();
  }
  std::string str = input.substr(start, pos - start);
  if (pos < input.size()) {
    advance(); // skip closing "
  }
  return {TokenType::STRING, str, line};
}

Token Lexer::nextToken() {
  skipWhitespace();
  if (pos >= input.size()) {
    return {TokenType::END_OF_FILE, "", line};
  }

  // comments
  if (input[pos] == '/' && matchNext('*')) {
    skipBlockComment();
    return nextToken();
  }
  if (input[pos] == '/' && matchNext('/')) {
    return lexLineComment();
  }

  char c = input[pos];

  if (std::isalpha(c) || c == '_') {
    return lexIdentifierOrKeyword();
  }
  if (std::isdigit(c)) {
    return lexNumber();
  }
  if (c == '"') {
    return lexString();
  }

  // two-char operators
  if (c == '=' && matchNext('<')) {
    pos += 2;
    return {TokenType::SMOL_OR_EQU, "=<", line};
  }
  if (c == '>' && matchNext('=')) {
    pos += 2;
    return {TokenType::BIG_OR_EQU, ">=", line};
  }

  // single-char tokens
  advance();
  switch (c) {
  case '+':
    return {TokenType::ADD, "+", line};
  case '-':
    return {TokenType::SUB, "-", line};
  case '*':
    return {TokenType::MUL, "*", line};
  case '/':
    return {TokenType::DIV, "/", line};
  case '=':
    return {TokenType::EQU, "=", line};
  case '<':
    return {TokenType::SMOL, "<", line};
  case '>':
    return {TokenType::BIG, ">", line};
  case '!':
    return {TokenType::BAD, "!", line};
  case '|':
    return {TokenType::UNION, "|", line};
  case ':':
    return {TokenType::COLON, ":", line};
  case ',':
    return {TokenType::COMMA, ",", line};
  case '(':
    return {TokenType::LEFT_PAREN, "(", line};
  case ')':
    return {TokenType::RIGHT_PAREN, ")", line};
  case '[':
    return {TokenType::LEFT_BRACK, "[", line};
  case ']':
    return {TokenType::RIGHT_BRACK, "]", line};
  case '{':
    return {TokenType::LEFT_BRACE, "{", line};
  case '}':
    return {TokenType::RIGHT_BRACE, "}", line};
  default:
    return {TokenType::OTHER, std::string(1, c), line};
  }
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    Token tok(TokenType::OTHER, "", 0); // Initialize with a default invalid token
    do {
        tok = nextToken();  // Get the next token
        tokens.push_back(tok); // Add it to the tokens vector
    } while (tok.type != TokenType::END_OF_FILE);  // Keep going until the end of file
    return tokens;
}