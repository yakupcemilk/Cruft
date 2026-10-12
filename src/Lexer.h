// Lexer.h - tokenizer for Cruft source text.
#pragma once

#include "Diagnostics.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cruft {

enum class TokenKind {
  // ---- keywords -------------------------------------------------------
  KwPackage,
  KwImport,
  KwPub,
  KwPri,
  KwFn,
  KwExtern,
  KwVar,
  KwConst,
  KwRet,
  KwEnd,
  KwIf,
  KwThen,
  KwElse,
  KwWhile,
  KwDo,
  KwType,
  KwMatch,
  KwCase,
  KwAnd,
  KwOr,
  KwNot,
  KwTrue,
  KwFalse,
  KwScope,  ///< `scope arena` — lexical bump region
  KwArena,
  KwOwned,  ///< explicit (and default) linear ownership
  KwShared, ///< reference-counted multi-ownership

  // primitive / built-in type names (lexed as keywords so the parser can
  // tell type positions from identifiers without a symbol table)
  KwI32,
  KwI64,
  KwF32,
  KwF64,
  KwBool,
  KwInterval, ///< `I`     - the cubical interval
  KwUniverse, ///< `Type`  - universe of (small) types
  KwPath,     ///< `Path`
  KwEquiv,    ///< `Equiv`
  KwI0,       ///< `i0`
  KwI1,       ///< `i1`

  // ---- literals -------------------------------------------------------
  Identifier,
  IntLiteral,
  FloatLiteral,
  StringLiteral,

  // ---- punctuation / operators ---------------------------------------
  LParen,
  RParen,
  LBracket,
  RBracket,
  Comma,
  Colon,
  Dot,
  Assign,       ///< =
  Plus,
  Minus,
  Star,
  Slash,
  Percent,
  EqEq,         ///< ==
  BangEq,       ///< !=
  Less,         ///< <   (also opens a path abstraction `<i> e`)
  LessEq,
  Greater,      ///< >
  GreaterEq,
  At,           ///< @   path application
  Tilde,        ///< ~   interval reversal
  Meet,         ///< /\  interval min
  Join,         ///< \/  interval max
  Arrow,        ///< ->  face mapping
  FatArrow,     ///< =>  lambda body

  EndOfFile,
  Invalid
};

std::string_view tokenKindName(TokenKind k);

struct Token {
  TokenKind kind = TokenKind::Invalid;
  std::string text; ///< lexeme (for literals/identifiers)
  SourceLocation loc;

  [[nodiscard]] bool is(TokenKind k) const { return kind == k; }
};

class Lexer {
public:
  Lexer(std::string source, std::string filename, DiagnosticEngine &diags);

  /// Tokenizes the whole file. Always ends with an EndOfFile token.
  std::vector<Token> tokenize();

private:
  [[nodiscard]] bool atEnd() const { return pos_ >= src_.size(); }
  [[nodiscard]] char peek(size_t ahead = 0) const {
    return pos_ + ahead < src_.size() ? src_[pos_ + ahead] : '\0';
  }
  char advance();
  [[nodiscard]] SourceLocation here() const;

  void skipTrivia();
  Token lexIdentifierOrKeyword();
  Token lexNumber();
  Token lexString();
  Token make(TokenKind k, std::string text, SourceLocation loc) const;

  std::string src_;
  std::shared_ptr<const std::string> file_;
  DiagnosticEngine &diags_;
  size_t pos_ = 0;
  unsigned line_ = 1;
  unsigned column_ = 1;
};

} // namespace cruft
