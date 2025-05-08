// src/lexer/Token.h
#pragma once
#include <string>

namespace cruft {

enum class TokenType {
  // Keywords
  PACKAGE,
  SEC,
  TO,
  EXPORT,
  IMPORT,
  USE,
  PUB,
  PRI,
  FUNC,
  RETURN,
  END,
  CLASS,
  IF,
  ELSEIF,
  THEN,
  ELSE,
  SWI,
  CASE,
  WHILE,
  DO,
  PRINT,
  CONST,
  VAR,
  MUT,
  ARRAY,
  BOOL,
  TRUE,
  FALSE,
  ANY,
  VOID,
  OBJECT,
  I_64,
  I_32,
  I_16,
  I_8,
  F_64,
  F_32,
  F_16,
  F_8,
  NULL_T,
  BREAK,
  AND,
  OR,
  XOR,
  NOT,
  NAND,
  NOR,
  XNOR,
  IMPLIES,
  IFF,

  // Literals
  IDENTIFIER,
  NUM,
  STRING,

  // Operators and Punctuation
  ADD,
  SUB,
  MUL,
  DIV,
  EQU,
  SMOL,
  BIG,
  SMOL_OR_EQU,
  BIG_OR_EQU,
  UNION,
  COLON,
  COMMA,
  LEFT_PAREN,
  RIGHT_PAREN,
  LEFT_BRACK,
  RIGHT_BRACK,
  LEFT_BRACE,
  RIGHT_BRACE,

  COMMENT,
  OTHER,
  OPERATOR,
  END_OF_FILE,
  BAD
};

class Token {
    public:
        Token() : type(TokenType::OTHER), lexeme(""), line(0) {} // Default constructor
        Token(TokenType t, const std::string& lex, int l) : type(t), lexeme(lex), line(l) {} // Existing constructor
    
        TokenType type;
        std::string lexeme;
        int line;
    };

} // namespace cruft
