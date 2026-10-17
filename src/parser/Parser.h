// Parser.h - recursive-descent parser producing the Cruft AST.
#pragma once

#include "ast/AST.h"
#include "Diagnostics.h"
#include "lexer/Lexer.h"

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
  [[nodiscard]] bool isRecoveryBoundary(TokenKind k, bool topLevel) const;
  [[nodiscard]] bool isTopLevelStart(TokenKind k) const;
  /// Panic-mode synchronization. Stops before a boundary (`end`, `;`, `}`,
  /// `fn`, `package`, and the other declaration or statement starters) so
  /// parsing can resume. Semicolons are consumed. `topLevel` also guarantees
  /// the cursor moves when the boundary is not itself a declaration.
  void synchronize(bool topLevel);
  void noteImport(ast::File &file, std::string package, SourceLocation loc);

  // --- declarations ---
  void parseTopLevel(ast::File &file);
  std::unique_ptr<ast::FnDecl> parseFunction(ast::Visibility vis, bool isExtern, bool isCofn);
  std::unique_ptr<ast::GlobalDecl> parseGlobal(ast::Visibility vis);
  std::unique_ptr<ast::AxiomDecl> parseAxiom(ast::Visibility vis);
  std::unique_ptr<ast::TypeDecl> parseTypeDecl(ast::Visibility vis);
  std::vector<ast::Param> parseParamList();
  std::vector<ast::Param> parseLambdaParams();
  [[nodiscard]] bool aheadIsLambda() const;

  // --- statements ---
  ast::Block parseBlock(std::initializer_list<TokenKind> terminators);
  ast::StmtPtr parseStatement();
  ast::StmtPtr parseStatementBody();

  // --- types ---
  ast::TypeExprPtr parseType();
  [[nodiscard]] bool atTypeStart() const;
  struct EffectClause {
    ast::EffectSet effects = 0;
    std::string var;
    bool declared = false;
  };
  EffectClause parseEffectClause();

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
  /// Effect variable in scope while parsing a signature (`fn name[e]`).
  std::string effectBinder_;
};

} // namespace cruft
