// Parser.cpp - recursive-descent parser for Cruft.
//
// Grammar (informal):
//
//   File       := 'package' IDENT TopLevel*
//   TopLevel   := 'import' IDENT
//               | Vis? 'extern' 'fn' IDENT '(' Params ')' (':' Type)?
//               | Vis? 'fn' IDENT '(' Params ')' (':' Type)? Block 'end'
//               | Vis? ('var' | 'const') IDENT ':' Type '=' Expr
//               | Vis? 'type' IDENT Ctor* 'end'
//   Ctor       := IDENT ('(' Params ')')? (':' Type)?
//   Stmt       := ('var' | 'const') IDENT ':' Type '=' Expr
//               | 'ret' Expr?
//               | 'if' Expr 'then' Block ('else' Block)? 'end'
//               | 'while' Expr 'do' Block 'end'
//               | IDENT '=' Expr
//               | Expr
//   Type       := i32 | i64 | f32 | f64 | bool | I | Type
//               | Path '(' Type ',' Expr ',' Expr ')'
//               | Equiv '(' Type ',' Type ')'
//               | fn '(' Type* ')' (':' Type)?
//               | (IDENT '.')? IDENT
//   Expr       := precedence climbing, see parse* below
//   Primary    := literal | i0 | i1 | (IDENT '.')? IDENT Args?
//               | TypeAsTerm | '(' Expr ')' | '<' IDENT '>' Expr
//               | 'match' Expr ('case' IDENT ('(' IDENT* ')')? 'then' Expr)* 'end'
//               | 'hcomp' '(' Type ',' Expr ',' '[' Face* ']' ')'
//   Face       := IDENT '=' ('0'|'1') ('/\' IDENT '=' ('0'|'1'))* '->' Expr
#include "Parser.h"

#include <charconv>
#include <cstdlib>

namespace cruft {

using namespace ast;

Parser::Parser(std::vector<Token> tokens, std::string filename, DiagnosticEngine &diags)
    : tokens_(std::move(tokens)), filename_(std::move(filename)), diags_(diags) {}

// ---------------------------------------------------------------------------
// Token cursor helpers
// ---------------------------------------------------------------------------

const Token &Parser::peek(size_t ahead) const {
  size_t i = pos_ + ahead;
  if (i >= tokens_.size())
    return tokens_.back();
  return tokens_[i];
}

bool Parser::atIdent(std::string_view text) const {
  return at(TokenKind::Identifier) && peek().text == text;
}

const Token &Parser::advance() {
  const Token &t = peek();
  if (pos_ < tokens_.size() - 1)
    ++pos_;
  return t;
}

bool Parser::accept(TokenKind k) {
  if (at(k)) {
    advance();
    return true;
  }
  return false;
}

const Token &Parser::expect(TokenKind k, const char *context) {
  if (!at(k))
    fail(peek(), std::string("expected ") + std::string(tokenKindName(k)) + " " + context +
                     ", found " + std::string(tokenKindName(peek().kind)) +
                     (peek().text.empty() ? "" : " '" + peek().text + "'"));
  return advance();
}

std::string Parser::expectIdentifier(const char *context) {
  return expect(TokenKind::Identifier, context).text;
}

void Parser::fail(const Token &tok, std::string message) {
  diags_.error(tok.loc, std::move(message));
  throw ParseError{};
}

void Parser::synchronizeTopLevel() {
  while (!at(TokenKind::EndOfFile)) {
    switch (peek().kind) {
    case TokenKind::KwPub:
    case TokenKind::KwPri:
    case TokenKind::KwFn:
    case TokenKind::KwExtern:
    case TokenKind::KwType:
    case TokenKind::KwImport:
      return;
    default:
      advance();
    }
  }
}

// ---------------------------------------------------------------------------
// File / declarations
// ---------------------------------------------------------------------------

std::unique_ptr<File> Parser::parseFile() {
  auto file = std::make_unique<File>();
  file->filename = filename_;
  try {
    const Token &pkgTok = expect(TokenKind::KwPackage, "at start of file");
    file->packageLoc = pkgTok.loc;
    file->package = expectIdentifier("after 'package'");
  } catch (ParseError &) {
    synchronizeTopLevel();
  }

  while (!at(TokenKind::EndOfFile)) {
    try {
      parseTopLevel(*file);
    } catch (ParseError &) {
      synchronizeTopLevel();
    }
  }
  return file;
}

void Parser::parseTopLevel(File &file) {
  if (at(TokenKind::KwImport)) {
    SourceLocation loc = advance().loc;
    file.imports.push_back(ImportDecl{expectIdentifier("after 'import'"), loc});
    return;
  }

  Visibility vis = Visibility::Private;
  if (accept(TokenKind::KwPub))
    vis = Visibility::Public;
  else
    accept(TokenKind::KwPri);

  bool shared = false;
  if (accept(TokenKind::KwShared))
    shared = true;
  else
    accept(TokenKind::KwOwned);

  switch (peek().kind) {
  case TokenKind::KwExtern:
    advance();
    expect(TokenKind::KwFn, "after 'extern'");
    file.functions.push_back(parseFunction(vis, /*isExtern=*/true));
    return;
  case TokenKind::KwFn:
    advance();
    file.functions.push_back(parseFunction(vis, /*isExtern=*/false));
    return;
  case TokenKind::KwVar:
  case TokenKind::KwConst: {
    auto g = parseGlobal(vis);
    g->shared = shared;
    file.globals.push_back(std::move(g));
    return;
  }
  case TokenKind::KwType:
    advance();
    file.types.push_back(parseTypeDecl(vis));
    return;
  default:
    if (shared)
      fail(peek(), "expected 'var' or 'const' after 'shared'");
    fail(peek(), "expected a declaration ('fn', 'extern', 'var', 'const', 'type' or 'import'), found " +
                     std::string(tokenKindName(peek().kind)));
  }
}

std::vector<Param> Parser::parseParamList() {
  std::vector<Param> params;
  expect(TokenKind::LParen, "to open parameter list");
  if (!at(TokenKind::RParen)) {
    do {
      Param p;
      p.loc = peek().loc;
      p.name = expectIdentifier("as parameter name");
      expect(TokenKind::Colon, "after parameter name");
      p.type = parseType();
      params.push_back(std::move(p));
    } while (accept(TokenKind::Comma));
  }
  expect(TokenKind::RParen, "to close parameter list");
  return params;
}

std::unique_ptr<FnDecl> Parser::parseFunction(Visibility vis, bool isExtern) {
  auto fn = std::make_unique<FnDecl>();
  fn->visibility = vis;
  fn->isExtern = isExtern;
  fn->loc = peek().loc;
  fn->name = expectIdentifier("as function name");
  fn->params = parseParamList();
  if (accept(TokenKind::Colon))
    fn->ret = parseType();
  if (isExtern)
    return fn; // no body: foreign prototype

  const FnDecl *saved = currentFn_;
  currentFn_ = fn.get();
  fn->body = parseBlock({TokenKind::KwEnd});
  currentFn_ = saved;
  expect(TokenKind::KwEnd, "to close function body");
  return fn;
}

std::unique_ptr<GlobalDecl> Parser::parseGlobal(Visibility vis) {
  auto g = std::make_unique<GlobalDecl>();
  g->visibility = vis;
  g->loc = peek().loc;
  g->isConst = advance().kind == TokenKind::KwConst;
  g->name = expectIdentifier("as variable name");
  if (accept(TokenKind::Colon))
    g->type = parseType();
  expect(TokenKind::Assign, "before initializer");
  g->init = parseExpr();
  return g;
}

std::unique_ptr<TypeDecl> Parser::parseTypeDecl(Visibility vis) {
  auto td = std::make_unique<TypeDecl>();
  td->visibility = vis;
  td->loc = peek().loc;
  td->name = expectIdentifier("as type name");
  while (!at(TokenKind::KwEnd) && !at(TokenKind::EndOfFile)) {
    CtorDecl c;
    c.loc = peek().loc;
    c.name = expectIdentifier("as constructor name");
    if (at(TokenKind::LParen))
      c.params = parseParamList();
    if (accept(TokenKind::Colon))
      c.pathType = parseType();
    td->ctors.push_back(std::move(c));
  }
  expect(TokenKind::KwEnd, "to close type declaration");
  return td;
}

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

Block Parser::parseBlock(std::initializer_list<TokenKind> terminators) {
  Block block;
  auto isTerminator = [&] {
    if (at(TokenKind::EndOfFile))
      return true;
    for (TokenKind k : terminators)
      if (at(k))
        return true;
    return false;
  };
  while (!isTerminator())
    block.push_back(parseStatement());
  return block;
}

StmtPtr Parser::parseStatement() {
  bool shared = false;
  if (accept(TokenKind::KwShared))
    shared = true;
  else
    accept(TokenKind::KwOwned);

  const Token &tok = peek();
  if (shared && tok.kind != TokenKind::KwVar && tok.kind != TokenKind::KwConst)
    fail(tok, "'shared' applies to 'var' or 'const'");
  switch (tok.kind) {
  case TokenKind::KwScope: {
    SourceLocation loc = advance().loc;
    expect(TokenKind::KwArena, "after 'scope' (the only region form is 'scope arena')");
    Block body = parseBlock({TokenKind::KwEnd});
    expect(TokenKind::KwEnd, "to close 'scope arena'");
    return std::make_unique<ArenaStmt>(loc, std::move(body));
  }
  case TokenKind::KwVar:
  case TokenKind::KwConst: {
    SourceLocation loc = advance().loc;
    bool isConst = tok.kind == TokenKind::KwConst;
    std::string name = expectIdentifier("as variable name");
    TypeExprPtr type;
    if (accept(TokenKind::Colon))
      type = parseType();
    expect(TokenKind::Assign, "before initializer");
    ExprPtr init = parseExpr();
    auto decl = std::make_unique<VarDeclStmt>(loc, isConst, std::move(name), std::move(type),
                                              std::move(init));
    decl->shared = shared;
    return decl;
  }
  case TokenKind::KwRet: {
    SourceLocation loc = advance().loc;
    ExprPtr value;
    if (currentFn_ && currentFn_->ret)
      value = parseExpr();
    return std::make_unique<ReturnStmt>(loc, std::move(value));
  }
  case TokenKind::KwIf: {
    SourceLocation loc = advance().loc;
    ExprPtr cond = parseExpr();
    expect(TokenKind::KwThen, "after 'if' condition");
    Block thenBody = parseBlock({TokenKind::KwElse, TokenKind::KwEnd});
    Block elseBody;
    if (accept(TokenKind::KwElse))
      elseBody = parseBlock({TokenKind::KwEnd});
    expect(TokenKind::KwEnd, "to close 'if'");
    return std::make_unique<IfStmt>(loc, std::move(cond), std::move(thenBody), std::move(elseBody));
  }
  case TokenKind::KwWhile: {
    SourceLocation loc = advance().loc;
    ExprPtr cond = parseExpr();
    expect(TokenKind::KwDo, "after 'while' condition");
    Block body = parseBlock({TokenKind::KwEnd});
    expect(TokenKind::KwEnd, "to close 'while'");
    return std::make_unique<WhileStmt>(loc, std::move(cond), std::move(body));
  }
  default: {
    SourceLocation loc = tok.loc;
    ExprPtr e = parseExpr();
    if (at(TokenKind::Assign)) {
      if (e->kind != ExprKind::Ident || !static_cast<IdentExpr &>(*e).package.empty())
        fail(peek(), "left-hand side of assignment must be a local or module variable");
      advance();
      ExprPtr value = parseExpr();
      return std::make_unique<AssignStmt>(loc, static_cast<IdentExpr &>(*e).name, std::move(value));
    }
    return std::make_unique<ExprStmt>(loc, std::move(e));
  }
  }
}

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

bool Parser::atTypeStart() const {
  switch (peek().kind) {
  case TokenKind::KwI32:
  case TokenKind::KwI64:
  case TokenKind::KwF32:
  case TokenKind::KwF64:
  case TokenKind::KwBool:
  case TokenKind::KwInterval:
  case TokenKind::KwUniverse:
  case TokenKind::KwPath:
  case TokenKind::KwEquiv:
    return true;
  case TokenKind::KwFn:
    return peek(1).kind == TokenKind::LParen;
  default:
    return false;
  }
}

TypeExprPtr Parser::parseType() {
  const Token &tok = peek();
  switch (tok.kind) {
  case TokenKind::KwI32:
  case TokenKind::KwI64:
  case TokenKind::KwF32:
  case TokenKind::KwF64:
  case TokenKind::KwBool:
  case TokenKind::KwInterval:
  case TokenKind::KwUniverse: {
    auto t = std::make_unique<TypeExpr>(TypeExprKind::Named, tok.loc);
    t->name = tok.text;
    advance();
    return t;
  }
  case TokenKind::KwPath: {
    auto t = std::make_unique<TypeExpr>(TypeExprKind::Path, tok.loc);
    advance();
    expect(TokenKind::LParen, "after 'Path'");
    t->elem = parseType();
    expect(TokenKind::Comma, "after Path element type");
    t->lhs = parseExpr();
    expect(TokenKind::Comma, "after Path left endpoint");
    t->rhs = parseExpr();
    expect(TokenKind::RParen, "to close 'Path(...)'");
    return t;
  }
  case TokenKind::KwEquiv: {
    auto t = std::make_unique<TypeExpr>(TypeExprKind::Equiv, tok.loc);
    advance();
    expect(TokenKind::LParen, "after 'Equiv'");
    t->params.push_back(parseType());
    expect(TokenKind::Comma, "between Equiv arguments");
    t->params.push_back(parseType());
    expect(TokenKind::RParen, "to close 'Equiv(...)'");
    return t;
  }
  case TokenKind::KwFn: {
    auto t = std::make_unique<TypeExpr>(TypeExprKind::Fn, tok.loc);
    advance();
    expect(TokenKind::LParen, "after 'fn' in function type");
    if (!at(TokenKind::RParen)) {
      do {
        t->params.push_back(parseType());
      } while (accept(TokenKind::Comma));
    }
    expect(TokenKind::RParen, "to close function type parameters");
    if (accept(TokenKind::Colon))
      t->ret = parseType();
    return t;
  }
  case TokenKind::Identifier: {
    auto t = std::make_unique<TypeExpr>(TypeExprKind::Named, tok.loc);
    t->name = advance().text;
    if (at(TokenKind::Dot) && peek(1).kind == TokenKind::Identifier) {
      advance();
      t->package = t->name;
      t->name = advance().text;
    }
    if (at(TokenKind::LParen)) {
      advance();
      if (!at(TokenKind::RParen)) {
        do {
          t->params.push_back(parseType());
        } while (accept(TokenKind::Comma));
      }
      expect(TokenKind::RParen, "to close type arguments");
    }
    return t;
  }
  case TokenKind::LParen: {
    // `(T)` or the identity-type sugar `(a = b)` (equivalent to Path(_, a, b)).
    // Parentheses are required so `var x: i32 = 0` is not parsed as a path.
    SourceLocation loc = advance().loc;
    ExprPtr inner = parseExpr();
    if (accept(TokenKind::Assign)) {
      auto t = std::make_unique<TypeExpr>(TypeExprKind::Path, loc);
      t->lhs = std::move(inner);
      t->rhs = parseExpr();
      expect(TokenKind::RParen, "to close identity type '(a = b)'");
      return t;
    }
    expect(TokenKind::RParen, "to close parenthesised type");
    if (inner->kind == ExprKind::TypeLit)
      return std::move(static_cast<TypeLitExpr &>(*inner).typeExpr);
    if (inner->kind == ExprKind::Ident) {
      const auto &id = static_cast<IdentExpr &>(*inner);
      auto t = std::make_unique<TypeExpr>(TypeExprKind::Named, loc);
      t->package = id.package;
      t->name = id.name;
      return t;
    }
    fail(tok, "parenthesised type must be a type or an identity type '(a = b)'");
  }
  default:
    fail(tok, "expected a type, found " + std::string(tokenKindName(tok.kind)));
  }
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

ExprPtr Parser::parseExpr() { return parseOr(); }

ExprPtr Parser::parseOr() {
  ExprPtr lhs = parseAnd();
  while (at(TokenKind::KwOr)) {
    SourceLocation loc = advance().loc;
    lhs = std::make_unique<BinaryExpr>(loc, BinaryOp::Or, std::move(lhs), parseAnd());
  }
  return lhs;
}

ExprPtr Parser::parseAnd() {
  ExprPtr lhs = parseEquality();
  while (at(TokenKind::KwAnd)) {
    SourceLocation loc = advance().loc;
    lhs = std::make_unique<BinaryExpr>(loc, BinaryOp::And, std::move(lhs), parseEquality());
  }
  return lhs;
}

ExprPtr Parser::parseEquality() {
  ExprPtr lhs = parseComparison();
  for (;;) {
    BinaryOp op;
    if (at(TokenKind::EqEq))
      op = BinaryOp::Eq;
    else if (at(TokenKind::BangEq))
      op = BinaryOp::Ne;
    else
      return lhs;
    SourceLocation loc = advance().loc;
    lhs = std::make_unique<BinaryExpr>(loc, op, std::move(lhs), parseComparison());
  }
}

ExprPtr Parser::parseComparison() {
  ExprPtr lhs = parseAdditive();
  for (;;) {
    BinaryOp op;
    if (at(TokenKind::Less))
      op = BinaryOp::Lt;
    else if (at(TokenKind::LessEq))
      op = BinaryOp::Le;
    else if (at(TokenKind::Greater))
      op = BinaryOp::Gt;
    else if (at(TokenKind::GreaterEq))
      op = BinaryOp::Ge;
    else
      return lhs;
    SourceLocation loc = advance().loc;
    lhs = std::make_unique<BinaryExpr>(loc, op, std::move(lhs), parseAdditive());
  }
}

ExprPtr Parser::parseAdditive() {
  ExprPtr lhs = parseMultiplicative();
  for (;;) {
    BinaryOp op;
    if (at(TokenKind::Plus))
      op = BinaryOp::Add;
    else if (at(TokenKind::Minus))
      op = BinaryOp::Sub;
    else
      return lhs;
    SourceLocation loc = advance().loc;
    lhs = std::make_unique<BinaryExpr>(loc, op, std::move(lhs), parseMultiplicative());
  }
}

ExprPtr Parser::parseMultiplicative() {
  ExprPtr lhs = parseInterval();
  for (;;) {
    BinaryOp op;
    if (at(TokenKind::Star))
      op = BinaryOp::Mul;
    else if (at(TokenKind::Slash))
      op = BinaryOp::Div;
    else if (at(TokenKind::Percent))
      op = BinaryOp::Rem;
    else
      return lhs;
    SourceLocation loc = advance().loc;
    lhs = std::make_unique<BinaryExpr>(loc, op, std::move(lhs), parseInterval());
  }
}

ExprPtr Parser::parseInterval() {
  ExprPtr lhs = parseIntervalMeet();
  while (at(TokenKind::Join)) {
    SourceLocation loc = advance().loc;
    lhs = std::make_unique<BinaryExpr>(loc, BinaryOp::Join, std::move(lhs), parseIntervalMeet());
  }
  return lhs;
}

ExprPtr Parser::parseIntervalMeet() {
  ExprPtr lhs = parseUnary();
  while (at(TokenKind::Meet)) {
    SourceLocation loc = advance().loc;
    lhs = std::make_unique<BinaryExpr>(loc, BinaryOp::Meet, std::move(lhs), parseUnary());
  }
  return lhs;
}

ExprPtr Parser::parseUnary() {
  const Token &tok = peek();
  if (tok.kind == TokenKind::Minus) {
    advance();
    return std::make_unique<UnaryExpr>(tok.loc, UnaryOp::Neg, parseUnary());
  }
  if (tok.kind == TokenKind::KwNot) {
    advance();
    return std::make_unique<UnaryExpr>(tok.loc, UnaryOp::Not, parseUnary());
  }
  if (tok.kind == TokenKind::Tilde) {
    advance();
    return std::make_unique<UnaryExpr>(tok.loc, UnaryOp::IntervalRev, parseUnary());
  }
  // Path abstraction `<i> body`.
  if (tok.kind == TokenKind::Less && peek(1).kind == TokenKind::Identifier &&
      peek(2).kind == TokenKind::Greater) {
    advance();
    std::string var = advance().text;
    advance();
    return std::make_unique<PathAbsExpr>(tok.loc, std::move(var), parseExpr());
  }
  return parsePostfix();
}

ExprPtr Parser::parsePostfix() {
  ExprPtr e = parsePrimary();
  for (;;) {
    if (at(TokenKind::At)) {
      SourceLocation loc = advance().loc;
      // The argument of a path application is always an interval term, so we
      // parse it at interval precedence to allow `p @ i /\ j`.
      e = std::make_unique<PathAppExpr>(loc, std::move(e), parseInterval());
    } else if (at(TokenKind::Dot)) {
      SourceLocation loc = advance().loc;
      std::string name = expectIdentifier("after '.'");
      bool invoked = false;
      std::vector<ExprPtr> args;
      if (at(TokenKind::LParen)) {
        args = parseArgs();
        invoked = true;
      }
      e = std::make_unique<MethodCallExpr>(loc, std::move(e), std::move(name), std::move(args),
                                           invoked);
    } else if (at(TokenKind::LParen)) {
      SourceLocation loc = peek().loc;
      e = std::make_unique<CallExpr>(loc, std::move(e), parseArgs());
    } else {
      return e;
    }
  }
}

std::vector<ExprPtr> Parser::parseArgs() {
  std::vector<ExprPtr> args;
  expect(TokenKind::LParen, "to open argument list");
  if (!at(TokenKind::RParen)) {
    do {
      args.push_back(parseExpr());
    } while (accept(TokenKind::Comma));
  }
  expect(TokenKind::RParen, "to close argument list");
  return args;
}

ExprPtr Parser::parsePrimary() {
  const Token &tok = peek();
  switch (tok.kind) {
  case TokenKind::IntLiteral: {
    advance();
    std::int64_t v = 0;
    auto [ptr, ec] = std::from_chars(tok.text.data(), tok.text.data() + tok.text.size(), v);
    if (ec != std::errc{})
      fail(tok, "integer literal out of range: " + tok.text);
    return std::make_unique<IntLitExpr>(tok.loc, v);
  }
  case TokenKind::FloatLiteral: {
    advance();
    return std::make_unique<FloatLitExpr>(tok.loc, std::strtod(tok.text.c_str(), nullptr));
  }
  case TokenKind::KwTrue:
    advance();
    return std::make_unique<BoolLitExpr>(tok.loc, true);
  case TokenKind::KwFalse:
    advance();
    return std::make_unique<BoolLitExpr>(tok.loc, false);
  case TokenKind::KwI0:
    advance();
    return std::make_unique<IntervalLitExpr>(tok.loc, false);
  case TokenKind::KwI1:
    advance();
    return std::make_unique<IntervalLitExpr>(tok.loc, true);
  case TokenKind::LParen: {
    advance();
    ExprPtr inner = parseExpr();
    expect(TokenKind::RParen, "to close parenthesised expression");
    return inner;
  }
  case TokenKind::KwMatch:
    return parseMatch();
  case TokenKind::StringLiteral:
    advance();
    return std::make_unique<StringLitExpr>(tok.loc, tok.text);
  case TokenKind::LBracket: {
    SourceLocation loc = advance().loc;
    std::vector<ExprPtr> elems;
    if (!at(TokenKind::RBracket)) {
      do {
        elems.push_back(parseExpr());
      } while (accept(TokenKind::Comma));
    }
    expect(TokenKind::RBracket, "to close array literal");
    return std::make_unique<ArrayLitExpr>(loc, std::move(elems));
  }
  case TokenKind::KwFn:
    if (aheadIsLambda())
      return parseLambda();
    return std::make_unique<TypeLitExpr>(tok.loc, parseType());
  case TokenKind::Identifier: {
    if (tok.text == "hcomp" && peek(1).kind == TokenKind::LParen) {
      advance();
      return parseHcomp(tok.loc);
    }
    advance();
    return std::make_unique<IdentExpr>(tok.loc, "", tok.text);
  }
  default:
    if (atTypeStart())
      return std::make_unique<TypeLitExpr>(tok.loc, parseType());
    fail(tok, "expected an expression, found " + std::string(tokenKindName(tok.kind)) +
                  (tok.text.empty() ? "" : " '" + tok.text + "'"));
  }
}

bool Parser::aheadIsLambda() const {
  if (!at(TokenKind::KwFn) || peek(1).kind != TokenKind::LParen)
    return false;
  int depth = 0;
  for (size_t i = pos_ + 1; i < tokens_.size(); ++i) {
    TokenKind k = tokens_[i].kind;
    if (k == TokenKind::LParen)
      ++depth;
    else if (k == TokenKind::RParen) {
      if (--depth == 0)
        return i + 1 < tokens_.size() && tokens_[i + 1].kind == TokenKind::FatArrow;
    } else if (k == TokenKind::EndOfFile)
      break;
  }
  return false;
}

std::vector<Param> Parser::parseLambdaParams() {
  std::vector<Param> params;
  expect(TokenKind::LParen, "to open lambda parameters");
  if (!at(TokenKind::RParen)) {
    do {
      Param p;
      p.loc = peek().loc;
      p.name = expectIdentifier("as lambda parameter");
      if (accept(TokenKind::Colon))
        p.type = parseType();
      params.push_back(std::move(p));
    } while (accept(TokenKind::Comma));
  }
  expect(TokenKind::RParen, "to close lambda parameters");
  return params;
}

ExprPtr Parser::parseLambda() {
  SourceLocation loc = advance().loc;
  std::vector<Param> params = parseLambdaParams();
  expect(TokenKind::FatArrow, "after lambda parameters");
  return std::make_unique<LambdaExpr>(loc, std::move(params), parseExpr());
}

ExprPtr Parser::parseMatch() {
  SourceLocation loc = expect(TokenKind::KwMatch, "").loc;
  ExprPtr scrutinee = parseExpr();
  std::vector<MatchCase> cases;
  while (at(TokenKind::KwCase)) {
    MatchCase c;
    c.loc = advance().loc;
    c.ctor = expectIdentifier("as constructor name in 'case'");
    if (accept(TokenKind::LParen)) {
      if (!at(TokenKind::RParen)) {
        do {
          c.binders.push_back(expectIdentifier("as pattern variable"));
        } while (accept(TokenKind::Comma));
      }
      expect(TokenKind::RParen, "to close pattern");
    }
    expect(TokenKind::KwThen, "after match pattern");
    c.body = parseExpr();
    cases.push_back(std::move(c));
  }
  expect(TokenKind::KwEnd, "to close 'match'");
  return std::make_unique<MatchExpr>(loc, std::move(scrutinee), std::move(cases));
}

ExprPtr Parser::parseHcomp(SourceLocation loc) {
  expect(TokenKind::LParen, "after 'hcomp'");
  TypeExprPtr type = parseType();
  expect(TokenKind::Comma, "after hcomp type");
  ExprPtr base = parseExpr();
  std::vector<Face> faces;
  if (accept(TokenKind::Comma)) {
    expect(TokenKind::LBracket, "to open hcomp face system");
    if (!at(TokenKind::RBracket)) {
      do {
        Face f;
        f.loc = peek().loc;
        do {
          Face::Atom a;
          a.var = expectIdentifier("as interval variable in face constraint");
          expect(TokenKind::Assign, "in face constraint");
          const Token &v = peek();
          if (v.kind == TokenKind::IntLiteral && (v.text == "0" || v.text == "1"))
            a.one = v.text == "1";
          else if (v.kind == TokenKind::KwI0)
            a.one = false;
          else if (v.kind == TokenKind::KwI1)
            a.one = true;
          else
            fail(v, "face constraint must compare against 0/1 (or i0/i1)");
          advance();
          f.constraints.push_back(a);
        } while (accept(TokenKind::Meet));
        expect(TokenKind::Arrow, "between face constraint and partial element");
        f.side = parseExpr();
        faces.push_back(std::move(f));
      } while (accept(TokenKind::Comma));
    }
    expect(TokenKind::RBracket, "to close hcomp face system");
  }
  expect(TokenKind::RParen, "to close 'hcomp(...)'");
  return std::make_unique<HcompExpr>(loc, std::move(type), std::move(base), std::move(faces));
}

} // namespace cruft
