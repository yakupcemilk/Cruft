// Parser.h - recursive-descent parser producing the Cruft AST.
#pragma once

#include "AST.h"
#include "Diagnostics.h"
#include "Lexer.h"

#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace cruft {

class Parser {
public:
  Parser(std::vector<Token> tokens, std::string filename, DiagnosticEngine &diags);

  /// Parses a whole file. Never returns null; on syntax errors the returned
  /// file is partial and `diags` carries the errors.
  std::unique_ptr<ast::File> parseFile();

private:
  struct ParseError {};

  // --- token cursor ---
  [[nodiscard]] const Token &peek(size_t ahead = 0) const;
  [[nodiscard]] bool at(TokenKind k) const { return peek().kind == k; }
  [[nodiscard]] bool atIdent(std::string_view text) const;
  const Token &advance();
  bool accept(TokenKind k);
  const Token &expect(TokenKind k, const char *context);
  std::string expectIdentifier(const char *context);
  [[noreturn]] void fail(const Token &tok, std::string message);
  void synchronizeTopLevel();

  // --- declarations ---
  void parseTopLevel(ast::File &file);
  std::unique_ptr<ast::FnDecl> parseFunction(ast::Visibility vis, bool isExtern);
  std::unique_ptr<ast::GlobalDecl> parseGlobal(ast::Visibility vis);
  std::unique_ptr<ast::TypeDecl> parseTypeDecl(ast::Visibility vis);
  std::vector<ast::Param> parseParamList();
  std::vector<ast::Param> parseLambdaParams();
  [[nodiscard]] bool aheadIsLambda() const;

  // --- statements ---
  ast::Block parseBlock(std::initializer_list<TokenKind> terminators);
  ast::StmtPtr parseStatement();

  // --- types ---
  ast::TypeExprPtr parseType();
  [[nodiscard]] bool atTypeStart() const;

  // --- expressions (precedence climbing) ---
  ast::ExprPtr parseExpr();
  ast::ExprPtr parseOr();
  ast::ExprPtr parseAnd();
  ast::ExprPtr parseEquality();
  ast::ExprPtr parseComparison();
  ast::ExprPtr parseAdditive();
  ast::ExprPtr parseMultiplicative();
  ast::ExprPtr parseInterval();     ///< \/ level
  ast::ExprPtr parseIntervalMeet(); ///< /\ level
  ast::ExprPtr parseUnary();
  ast::ExprPtr parsePostfix();
  ast::ExprPtr parsePrimary();
  ast::ExprPtr parseLambda();
  ast::ExprPtr parseMatch();
  ast::ExprPtr parseHcomp(SourceLocation loc);
  std::vector<ast::ExprPtr> parseArgs();

  std::vector<Token> tokens_;
  size_t pos_ = 0;
  std::string filename_;
  DiagnosticEngine &diags_;
  const ast::FnDecl *currentFn_ = nullptr; ///< to know whether `ret` takes a value
};

} // namespace cruft
