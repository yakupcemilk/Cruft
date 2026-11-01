// Parser.cpp - recursive-descent parser for Cruft.
//
// Grammar (informal):
//
//   File       := 'package' IDENT ('module' IDENT)? TopLevel*
//   TopLevel   := 'import' IDENT
//               | 'use' IDENT ('.' IDENT)?
//               | 'export' IDENT ('.' IDENT)?
//               | Vis? 'extern' 'fn' IDENT ('[' IDENT ']')? '(' Params ')' (':' Type)? Effect?
//               | Vis? 'fn' IDENT ('[' IDENT ']')? '(' Params ')' (':' Type)? Effect? Block 'end'
//               | Vis? 'cofn' IDENT ('[' IDENT ']')? '(' Params ')' (':' Type)? Effect? Block 'end'
//               | Vis? 'axiom' IDENT ':' Type
//               | Vis? 'trait' IDENT MethodSig* 'end'
//               | Vis? 'impl' TraitName ('[' Binders ']')? 'for' Type Method* 'end'
//               | Vis? 'const' 'fn' IDENT '(' Params ')' (':' Type)? Block 'end'
//               | 'cfg' CfgPred Item* 'end'
//   Effect     := '!' '{' EffectName* '}' | '!' IDENT
//   EffectName := 'io' | 'alloc' | 'net'
//               | Vis? ('var' | 'const') IDENT ':' Type '=' Expr
//               | Vis? 'type' IDENT Ctor* 'end'
//   Ctor       := IDENT ('(' Params ')')? (':' Type)?
//   Stmt       := ('var' | 'const') IDENT ':' Type '=' Expr
//               | 'ret' Expr?
//               | 'if' Expr 'then' Block ('else' Block)? 'end'
//               | 'while' Expr 'do' Block 'end'
//               | 'foreach' IDENT 'in' 'I' 'do' Block 'end'
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
#include "parser/Parser.h"

#include <charconv>
#include <cstdlib>

namespace cruft {

using namespace ast;

Parser::Parser(std::vector<Token> tokens, std::string filename, DiagnosticEngine &diags,
               TargetFacts target)
    : tokens_(std::move(tokens)), filename_(std::move(filename)), diags_(diags),
      target_(std::move(target)) {}

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
  diags_.beginPanic();
  throw ParseError{};
}

namespace {

std::string fileStem(const std::string &path) {
  size_t slash = path.find_last_of("/\\");
  std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
  size_t dot = base.find_last_of('.');
  if (dot != std::string::npos && dot > 0)
    base = base.substr(0, dot);
  return base.empty() ? "mod" : base;
}

} // namespace

bool Parser::isRecoveryBoundary(TokenKind k, bool topLevel) const {
  switch (k) {
  case TokenKind::EndOfFile:
  case TokenKind::Semicolon:
  case TokenKind::RBrace:
  case TokenKind::KwEnd:
  case TokenKind::KwFn:
  case TokenKind::KwPackage:
  case TokenKind::KwImport:
  case TokenKind::KwExport:
  case TokenKind::KwUse:
  case TokenKind::KwPub:
  case TokenKind::KwPri:
  case TokenKind::KwExtern:
  case TokenKind::KwType:
  case TokenKind::KwVar:
  case TokenKind::KwConst:
  case TokenKind::KwShared:
  case TokenKind::KwOwned:
  case TokenKind::KwLin:
  case TokenKind::KwAff:
  case TokenKind::KwCofn:
  case TokenKind::KwAxiom:
  case TokenKind::KwStruct:
  case TokenKind::KwEnum:
  case TokenKind::KwTrait:
  case TokenKind::KwImpl:
  case TokenKind::KwCfg:
    return true;
  case TokenKind::KwRet:
  case TokenKind::KwIf:
  case TokenKind::KwWhile:
  case TokenKind::KwForeach:
  case TokenKind::KwScope:
  case TokenKind::KwUnsafe:
    return !topLevel;
  default:
    return false;
  }
}

bool Parser::isTopLevelStart(TokenKind k) const {
  switch (k) {
  case TokenKind::KwImport:
  case TokenKind::KwExport:
  case TokenKind::KwUse:
  case TokenKind::KwPub:
  case TokenKind::KwPri:
  case TokenKind::KwExtern:
  case TokenKind::KwFn:
  case TokenKind::KwCofn:
  case TokenKind::KwAxiom:
  case TokenKind::KwStruct:
  case TokenKind::KwEnum:
  case TokenKind::KwTrait:
  case TokenKind::KwImpl:
  case TokenKind::KwCfg:
  case TokenKind::KwType:
  case TokenKind::KwDrop:
  case TokenKind::KwVar:
  case TokenKind::KwConst:
  case TokenKind::KwShared:
  case TokenKind::KwOwned:
  case TokenKind::KwLin:
  case TokenKind::KwAff:
  case TokenKind::EndOfFile:
    return true;
  default:
    return false;
  }
}

void Parser::synchronize(bool topLevel) {
  diags_.endPanic();
  if (!isRecoveryBoundary(peek().kind, topLevel)) {
    advance();
    while (!at(TokenKind::EndOfFile) && !isRecoveryBoundary(peek().kind, topLevel))
      advance();
  }
  if (at(TokenKind::Semicolon))
    advance();
  // A boundary that cannot start a declaration would otherwise be retried
  // forever (`end`, `}`, a statement keyword at file scope).
  if (topLevel && !at(TokenKind::EndOfFile) && !isTopLevelStart(peek().kind))
    advance();
}

void Parser::noteImport(File &file, std::string package, SourceLocation loc) {
  for (const ImportDecl &imp : file.imports)
    if (imp.package == package)
      return;
  file.imports.push_back(ImportDecl{std::move(package), std::move(loc)});
}

// ---------------------------------------------------------------------------
// File / declarations
// ---------------------------------------------------------------------------

std::unique_ptr<File> Parser::parseFile() {
  auto file = std::make_unique<File>();
  file->filename = filename_;
  file->moduleName = fileStem(filename_);
  try {
    const Token &pkgTok = expect(TokenKind::KwPackage, "at start of file");
    file->packageLoc = pkgTok.loc;
    file->package = expectIdentifier("after 'package'");
    if (accept(TokenKind::KwModule))
      file->moduleName = expectIdentifier("after 'module'");
  } catch (ParseError &) {
    synchronize(/*topLevel=*/true);
  }

  while (!at(TokenKind::EndOfFile)) {
    if (accept(TokenKind::Semicolon))
      continue;
    try {
      parseTopLevel(*file);
    } catch (ParseError &) {
      synchronize(/*topLevel=*/true);
    }
  }
  diags_.endPanic();
  return file;
}

void Parser::parseTopLevel(File &file) {
  if (at(TokenKind::KwImport)) {
    SourceLocation loc = advance().loc;
    noteImport(file, expectIdentifier("after 'import'"), loc);
    return;
  }
  if (at(TokenKind::KwUse)) {
    SourceLocation loc = advance().loc;
    std::string package = expectIdentifier("after 'use'");
    noteImport(file, package, loc);
    if (accept(TokenKind::Dot)) {
      std::string symbol = expectIdentifier("after '.'");
      file.uses.push_back(UseBinding{symbol, std::move(package), symbol, loc});
    }
    return;
  }
  if (at(TokenKind::KwExport)) {
    SourceLocation loc = advance().loc;
    std::string first = expectIdentifier("after 'export'");
    if (accept(TokenKind::Dot)) {
      std::string symbol = expectIdentifier("after '.'");
      noteImport(file, first, loc);
      file.exports.push_back(ExportDecl{std::move(first), std::move(symbol), loc});
    } else {
      file.exports.push_back(ExportDecl{"", std::move(first), loc});
    }
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
    file.functions.push_back(parseFunction(vis, /*isExtern=*/true, /*isCofn=*/false));
    return;
  case TokenKind::KwFn:
    advance();
    file.functions.push_back(parseFunction(vis, /*isExtern=*/false, /*isCofn=*/false));
    return;
  case TokenKind::KwCofn:
    advance();
    file.functions.push_back(parseFunction(vis, /*isExtern=*/false, /*isCofn=*/true));
    return;
  case TokenKind::KwAxiom:
    advance();
    file.axioms.push_back(parseAxiom(vis));
    return;
  case TokenKind::KwConst:
    if (peek(1).kind == TokenKind::KwFn) {
      if (shared)
        fail(peek(), "'shared' applies to a value, not a function");
      advance();
      advance();
      auto fn = parseFunction(vis, /*isExtern=*/false, /*isCofn=*/false);
      fn->isConstFn = true;
      file.functions.push_back(std::move(fn));
      return;
    }
    [[fallthrough]];
  case TokenKind::KwVar: {
    auto g = parseGlobal(vis);
    g->shared = shared;
    file.globals.push_back(std::move(g));
    return;
  }
  case TokenKind::KwCfg:
    if (shared)
      fail(peek(), "'shared' applies to a value, not 'cfg'");
    advance();
    parseCfg(file);
    return;
  case TokenKind::KwType:
    advance();
    file.types.push_back(parseTypeDecl(vis, /*isEnum=*/false));
    return;
  case TokenKind::KwEnum:
    advance();
    file.types.push_back(parseTypeDecl(vis, /*isEnum=*/true));
    return;
  case TokenKind::KwStruct:
    advance();
    file.structs.push_back(parseStruct(vis));
    return;
  case TokenKind::KwTrait:
    advance();
    parseTrait(file, vis);
    return;
  case TokenKind::KwImpl:
    advance();
    parseImpl(file, vis);
    return;
  case TokenKind::KwDrop: {
    advance();
    auto fn = std::make_unique<FnDecl>();
    fn->visibility = vis;
    fn->isDrop = true;
    fn->loc = peek().loc;
    std::string typeName = expectIdentifier("as the type cleaned up by 'drop'");
    fn->dropTypeName = typeName;
    fn->name = "__drop_" + typeName;
    fn->params = parseParamList();
    fn->body = parseBlock({TokenKind::KwEnd});
    expect(TokenKind::KwEnd, "to close 'drop'");
    file.functions.push_back(std::move(fn));
    return;
  }
  default:
    if (shared)
      fail(peek(), "expected 'var' or 'const' after 'shared'");
    fail(peek(), "expected a declaration ('fn', 'const fn', 'cofn', 'axiom', 'extern', 'var', 'const', 'type', 'trait', 'impl', 'cfg' or 'import'), found " +
                     std::string(tokenKindName(peek().kind)));
  }
}

std::vector<Param> Parser::parseParamList(bool *varArg) {
  std::vector<Param> params;
  expect(TokenKind::LParen, "to open parameter list");
  if (!at(TokenKind::RParen)) {
    do {
      if (at(TokenKind::Ellipsis)) {
        if (!varArg)
          fail(peek(), "'...' is only allowed on an 'extern fn'");
        advance();
        *varArg = true;
        if (!at(TokenKind::RParen))
          fail(peek(), "'...' must be the last parameter");
        break;
      }
      Param p;
      p.loc = peek().loc;
      if (accept(TokenKind::KwLin))
        p.usage = Usage::Linear;
      else if (accept(TokenKind::KwAff))
        p.usage = Usage::Affine;
      p.name = expectIdentifier("as parameter name");
      expect(TokenKind::Colon, "after parameter name");
      p.type = parseType();
      params.push_back(std::move(p));
    } while (accept(TokenKind::Comma));
  }
  expect(TokenKind::RParen, "to close parameter list");
  return params;
}

std::vector<FnDecl::TypeBinder> Parser::parseTypeBinders() {
  std::vector<FnDecl::TypeBinder> binders;
  expect(TokenKind::LBracket, "to open type binders");
  do {
    FnDecl::TypeBinder b;
    b.loc = peek().loc;
    b.name = expectIdentifier("as a type binder");
    expect(TokenKind::Colon, "after type binder");
    b.bound = parseType();
    binders.push_back(std::move(b));
  } while (accept(TokenKind::Comma));
  expect(TokenKind::RBracket, "to close type binders");
  return binders;
}

Parser::EffectClause Parser::parseEffectClause() {
  EffectClause clause;
  clause.declared = true;
  if (accept(TokenKind::LBrace)) {
    if (!at(TokenKind::RBrace)) {
      do {
        std::string name = expectIdentifier("as an effect ('io', 'alloc' or 'net')");
        EffectSet bit = 0;
        if (!namedEffect(name, bit))
          fail(peek(), "unknown effect '" + name + "' (expected io, alloc or net)");
        clause.effects = static_cast<EffectSet>(clause.effects | bit);
      } while (accept(TokenKind::Comma));
    }
    expect(TokenKind::RBrace, "to close effect row");
    return clause;
  }
  std::string name = expectIdentifier("as an effect variable");
  if (effectBinder_.empty() || name != effectBinder_)
    fail(peek(), "expected '{" + std::string("io, alloc, net}' or the effect variable '") +
                     (effectBinder_.empty() ? std::string("...") : effectBinder_) + "'");
  clause.var = std::move(name);
  return clause;
}

std::unique_ptr<FnDecl> Parser::parseFunction(Visibility vis, bool isExtern, bool isCofn,
                                             bool signatureOnly) {
  auto fn = std::make_unique<FnDecl>();
  fn->visibility = vis;
  fn->isExtern = isExtern;
  fn->isCofn = isCofn;
  fn->loc = peek().loc;
  fn->name = expectIdentifier("as function name");
  if (accept(TokenKind::LBracket)) {
    std::string binder = expectIdentifier("as type or effect variable");
    if (accept(TokenKind::Colon)) {
      FnDecl::TypeBinder first;
      first.loc = fn->loc;
      first.name = std::move(binder);
      first.bound = parseType();
      fn->typeBinders.push_back(std::move(first));
      while (accept(TokenKind::Comma)) {
        FnDecl::TypeBinder b;
        b.loc = peek().loc;
        b.name = expectIdentifier("as a type binder");
        expect(TokenKind::Colon, "after type binder");
        b.bound = parseType();
        fn->typeBinders.push_back(std::move(b));
      }
      expect(TokenKind::RBracket, "after type binders");
      fn->typeParam = fn->typeBinders.front().name;
      if (fn->typeBinders.front().bound)
        fn->typeParamBound = fn->typeBinders.front().bound->clone();
    } else {
      fn->effectVar = std::move(binder);
      expect(TokenKind::RBracket, "after effect variable");
    }
  }
  std::string savedBinder = effectBinder_;
  if (!fn->effectVar.empty())
    effectBinder_ = fn->effectVar;
  bool varArg = false;
  fn->params = parseParamList(isExtern ? &varArg : nullptr);
  fn->isVarArg = varArg;
  if (accept(TokenKind::Colon))
    fn->ret = parseType();
  if (accept(TokenKind::Bang)) {
    EffectClause clause = parseEffectClause();
    fn->effectsDeclared = true;
    if (clause.var.empty()) {
      fn->effects = clause.effects;
    } else {
      fn->effects = 0;
      fn->effectVar = clause.var;
    }
  }
  effectBinder_ = std::move(savedBinder);
  if (isExtern || signatureOnly)
    return fn; // no body: foreign prototype or trait signature

  const FnDecl *saved = currentFn_;
  currentFn_ = fn.get();
  fn->body = parseBlock({TokenKind::KwEnd});
  currentFn_ = saved;
  expect(TokenKind::KwEnd, "to close function body");
  return fn;
}

namespace {

FnDecl::TypeBinder copyBinder(const FnDecl::TypeBinder &b) {
  FnDecl::TypeBinder out;
  out.name = b.name;
  out.loc = b.loc;
  if (b.bound)
    out.bound = b.bound->clone();
  return out;
}

} // namespace

void Parser::parseTrait(File &file, Visibility vis) {
  auto tr = std::make_unique<TraitDecl>();
  tr->visibility = vis;
  tr->loc = peek().loc;
  tr->name = expectIdentifier("as trait name");
  if (at(TokenKind::LBracket))
    tr->typeBinders = parseTypeBinders();
  while (!at(TokenKind::KwEnd) && !at(TokenKind::EndOfFile)) {
    expect(TokenKind::KwFn, "to start a trait method");
    tr->methods.push_back(parseFunction(vis, /*isExtern=*/false, /*isCofn=*/false,
                                        /*signatureOnly=*/true));
  }
  expect(TokenKind::KwEnd, "to close trait");
  file.traits.push_back(std::move(tr));
}

void Parser::parseImpl(File &file, Visibility vis) {
  auto impl = std::make_unique<ImplDecl>();
  impl->visibility = vis;
  impl->loc = peek().loc;
  impl->traitName = expectIdentifier("as trait name");
  if (accept(TokenKind::Dot)) {
    impl->traitPackage = std::move(impl->traitName);
    impl->traitName = expectIdentifier("as trait name");
  }
  if (at(TokenKind::LBracket)) {
    // `impl Trait[T: Type(0)]` binds T. `impl Trait[i64]` supplies a type.
    const bool binders = peek(1).kind == TokenKind::Identifier && peek(2).kind == TokenKind::Colon;
    if (binders) {
      impl->binders = parseTypeBinders();
    } else {
      expect(TokenKind::LBracket, "to open trait arguments");
      if (!at(TokenKind::RBracket)) {
        do {
          impl->traitArgs.push_back(parseType());
        } while (accept(TokenKind::Comma));
      }
      expect(TokenKind::RBracket, "after trait arguments");
    }
  }
  expect(TokenKind::KwFor, "before the implemented type");
  impl->target = parseType();
  const int index = static_cast<int>(file.impls.size());
  while (!at(TokenKind::KwEnd) && !at(TokenKind::EndOfFile)) {
    accept(TokenKind::KwPub);
    accept(TokenKind::KwPri);
    expect(TokenKind::KwFn, "to start an impl method");
    auto fn = parseFunction(Visibility::Private, /*isExtern=*/false, /*isCofn=*/false);
    if (!fn->typeBinders.empty()) {
      diags_.error(fn->loc, "put type binders on the impl, as in 'impl Trait[T: Type(0)] for ...'");
      diags_.beginPanic();
      throw ParseError{};
    }
    for (const auto &b : impl->binders)
      fn->typeBinders.push_back(copyBinder(b));
    fn->implIndex = index;
    file.functions.push_back(std::move(fn));
  }
  expect(TokenKind::KwEnd, "to close impl");
  file.impls.push_back(std::move(impl));
}

bool Parser::parseCfgPredicate() {
  const bool neg = accept(TokenKind::KwNot);
  bool matches = false;
  auto named = [&](const std::string &have) {
    const std::string want = expectIdentifier("as a cfg name");
    matches = have == want;
  };
  if (at(TokenKind::Identifier) && peek().text == "os") {
    advance();
    named(target_.os);
  } else if (at(TokenKind::Identifier) && peek().text == "arch") {
    advance();
    named(target_.arch);
  } else if (at(TokenKind::Identifier) && peek().text == "env") {
    advance();
    named(target_.env);
  } else if (at(TokenKind::Identifier) && peek().text == "triple") {
    advance();
    const Token &lit = expect(TokenKind::StringLiteral, "as a target triple");
    matches = TargetFacts::from(lit.text).triple == target_.triple;
  } else {
    const std::string name = expectIdentifier("as a cfg predicate ('os', 'arch', 'env', or a name)");
    matches = name == target_.arch || name == target_.os || name == target_.env;
  }
  return neg ? !matches : matches;
}

void Parser::parseCfg(File &file) {
  const bool active = parseCfgPredicate();
  File dropped;
  dropped.package = file.package;
  File &dest = active ? file : dropped;
  while (!at(TokenKind::KwEnd) && !at(TokenKind::EndOfFile)) {
    if (accept(TokenKind::Semicolon))
      continue;
    parseTopLevel(dest);
  }
  expect(TokenKind::KwEnd, "to close 'cfg'");
}

std::unique_ptr<AxiomDecl> Parser::parseAxiom(Visibility vis) {
  auto ax = std::make_unique<AxiomDecl>();
  ax->visibility = vis;
  ax->loc = peek().loc;
  ax->name = expectIdentifier("as axiom name");
  expect(TokenKind::Colon, "after axiom name");
  ax->type = parseType();
  return ax;
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

std::unique_ptr<StructDecl> Parser::parseStruct(Visibility vis) {
  auto sd = std::make_unique<StructDecl>();
  sd->visibility = vis;
  sd->loc = peek().loc;
  sd->name = expectIdentifier("as struct name");
  if (at(TokenKind::LBracket))
    sd->typeBinders = parseTypeBinders();
  while (!at(TokenKind::KwEnd) && !at(TokenKind::EndOfFile)) {
    Param field;
    field.loc = peek().loc;
    field.name = expectIdentifier("as field name");
    expect(TokenKind::Colon, "after field name");
    field.type = parseType();
    sd->fields.push_back(std::move(field));
  }
  expect(TokenKind::KwEnd, "to close struct");
  return sd;
}

std::unique_ptr<TypeDecl> Parser::parseTypeDecl(Visibility vis, bool isEnum) {
  auto td = std::make_unique<TypeDecl>();
  td->visibility = vis;
  td->isEnum = isEnum;
  td->loc = peek().loc;
  td->name = expectIdentifier("as type name");
  if (at(TokenKind::LBracket))
    td->typeBinders = parseTypeBinders();
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
    if (at(TokenKind::EndOfFile) || at(TokenKind::RBrace))
      return true;
    // A new declaration means this block was never closed. Stop so the
    // outer parser can synchronize instead of retrying the same token.
    if (at(TokenKind::KwPackage) || at(TokenKind::KwImport) || at(TokenKind::KwExport) ||
        at(TokenKind::KwUse) || at(TokenKind::KwPub) || at(TokenKind::KwPri) ||
        at(TokenKind::KwExtern) || at(TokenKind::KwType) || at(TokenKind::KwCofn) ||
        at(TokenKind::KwAxiom) || at(TokenKind::KwStruct) || at(TokenKind::KwEnum) ||
        at(TokenKind::KwTrait) || at(TokenKind::KwImpl))
      return true;
    if ((at(TokenKind::KwFn) || at(TokenKind::KwCofn)) && !aheadIsLambda())
      return true;
    for (TokenKind k : terminators)
      if (at(k))
        return true;
    return false;
  };
  while (!isTerminator()) {
    if (accept(TokenKind::Semicolon))
      continue;
    if (at(TokenKind::KwCfg)) {
      advance();
      const bool active = parseCfgPredicate();
      Block inner = parseBlock({TokenKind::KwEnd});
      expect(TokenKind::KwEnd, "to close 'cfg'");
      if (active)
        for (auto &stmt : inner)
          block.push_back(std::move(stmt));
      continue;
    }
    block.push_back(parseStatement());
  }
  return block;
}

StmtPtr Parser::parseStatement() {
  SourceLocation loc = peek().loc;
  try {
    return parseStatementBody();
  } catch (ParseError &) {
    synchronize(/*topLevel=*/false);
    return std::make_unique<ErrorStmt>(loc);
  }
}

StmtPtr Parser::parseStatementBody() {
  bool shared = false;
  Usage usage = Usage::Many;
  if (accept(TokenKind::KwShared))
    shared = true;
  else if (accept(TokenKind::KwLin))
    usage = Usage::Linear;
  else if (accept(TokenKind::KwAff))
    usage = Usage::Affine;
  else
    accept(TokenKind::KwOwned);

  const Token &tok = peek();
  if (shared && tok.kind != TokenKind::KwVar && tok.kind != TokenKind::KwConst)
    fail(tok, "'shared' applies to 'var' or 'const'");
  if (usage != Usage::Many && tok.kind != TokenKind::KwVar && tok.kind != TokenKind::KwConst)
    fail(tok, "'lin' and 'aff' apply to 'var', 'const', or a parameter");
  switch (tok.kind) {
  case TokenKind::KwScope: {
    SourceLocation loc = advance().loc;
    expect(TokenKind::KwArena, "after 'scope' (the only region form is 'scope arena')");
    Block body = parseBlock({TokenKind::KwEnd});
    expect(TokenKind::KwEnd, "to close 'scope arena'");
    return std::make_unique<ScopeArenaStmt>(loc, std::move(body));
  }
  case TokenKind::KwUnsafe: {
    SourceLocation loc = advance().loc;
    Block body = parseBlock({TokenKind::KwEnd});
    expect(TokenKind::KwEnd, "to close 'unsafe'");
    return std::make_unique<UnsafeStmt>(loc, std::move(body));
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
    decl->usage = usage;
    return decl;
  }
  case TokenKind::KwForeach: {
    SourceLocation loc = advance().loc;
    std::string var = expectIdentifier("as interval binder");
    std::string in = expectIdentifier("after foreach binder ('in')");
    if (in != "in")
      fail(peek(), "expected 'in' after foreach binder, found '" + in + "'");
    expect(TokenKind::KwInterval, "after 'in' (the only foreach domain is 'I')");
    expect(TokenKind::KwDo, "after 'foreach i in I'");
    Block body = parseBlock({TokenKind::KwEnd});
    expect(TokenKind::KwEnd, "to close 'foreach'");
    return std::make_unique<ForeachStmt>(loc, std::move(var), std::move(body));
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
  case TokenKind::KwFor: {
    SourceLocation loc = advance().loc;
    std::string name = expectIdentifier("as loop binder");
    std::string in = expectIdentifier("after for binder ('in')");
    if (in != "in")
      fail(peek(), "expected 'in' after for binder, found '" + in + "'");
    ExprPtr first = parseExpr();
    if (accept(TokenKind::DotDot)) {
      ExprPtr high = parseExpr();
      expect(TokenKind::KwDo, "after 'for' bounds");
      Block body = parseBlock({TokenKind::KwEnd});
      expect(TokenKind::KwEnd, "to close 'for'");
      return std::make_unique<ForStmt>(loc, std::move(name), std::move(first), std::move(high),
                                       std::move(body));
    }
    expect(TokenKind::KwDo, "after the 'for' iterator");
    Block body = parseBlock({TokenKind::KwEnd});
    expect(TokenKind::KwEnd, "to close 'for'");
    auto stmt = std::make_unique<ForStmt>(loc, std::move(name), nullptr, nullptr, std::move(body));
    stmt->iter = std::move(first);
    return stmt;
  }
  case TokenKind::KwBreak:
    return std::make_unique<BreakStmt>(advance().loc);
  case TokenKind::KwContinue:
    return std::make_unique<ContinueStmt>(advance().loc);
  default: {
    SourceLocation loc = tok.loc;
    ExprPtr e = parseExpr();
    if (at(TokenKind::Assign)) {
      if (e->kind == ExprKind::Method) {
        auto &m = static_cast<MethodCallExpr &>(*e);
        if (!m.invoked && m.args.empty()) {
          advance();
          ExprPtr value = parseExpr();
          return std::make_unique<FieldAssignStmt>(loc, std::move(m.receiver), m.method,
                                                   std::move(value));
        }
        if (m.invoked && m.method == "at" && m.args.size() == 1) {
          advance();
          ExprPtr value = parseExpr();
          ExprPtr index = std::move(m.args[0]);
          ExprPtr place = std::move(m.receiver);
          return std::make_unique<IndexAssignStmt>(loc, std::move(place), std::move(index),
                                                   std::move(value));
        }
      }
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
  case TokenKind::KwI8:
  case TokenKind::KwI16:
  case TokenKind::KwI32:
  case TokenKind::KwI64:
  case TokenKind::KwU8:
  case TokenKind::KwU16:
  case TokenKind::KwU32:
  case TokenKind::KwU64:
  case TokenKind::KwUSize:
  case TokenKind::KwISize:
  case TokenKind::KwF32:
  case TokenKind::KwF64:
  case TokenKind::KwBool:
  case TokenKind::KwPtr:
  case TokenKind::KwInterval:
  case TokenKind::KwUniverse:
  case TokenKind::KwPath:
  case TokenKind::KwEquiv:
  case TokenKind::Amp:
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
  case TokenKind::Amp: {
    auto t = std::make_unique<TypeExpr>(TypeExprKind::Ref, tok.loc);
    advance();
    t->refMut = accept(TokenKind::KwMut);
    t->elem = parseType();
    return t;
  }
  case TokenKind::KwI8:
  case TokenKind::KwI16:
  case TokenKind::KwI32:
  case TokenKind::KwI64:
  case TokenKind::KwU8:
  case TokenKind::KwU16:
  case TokenKind::KwU32:
  case TokenKind::KwU64:
  case TokenKind::KwUSize:
  case TokenKind::KwISize:
  case TokenKind::KwF32:
  case TokenKind::KwF64:
  case TokenKind::KwBool:
  case TokenKind::KwPtr:
  case TokenKind::KwInterval: {
    auto t = std::make_unique<TypeExpr>(TypeExprKind::Named, tok.loc);
    t->name = tok.text;
    advance();
    return t;
  }
  case TokenKind::KwUniverse: {
    auto t = std::make_unique<TypeExpr>(TypeExprKind::Named, tok.loc);
    t->name = "Type";
    advance();
    if (accept(TokenKind::LParen)) {
      if (!at(TokenKind::IntLiteral))
        fail(peek(), "expected a universe level, as in Type(1)");
      int level = 0;
      auto [ptr, ec] = std::from_chars(peek().text.data(), peek().text.data() + peek().text.size(), level);
      if (ec != std::errc{} || level < 0)
        fail(peek(), "universe level must be a non-negative integer");
      advance();
      t->level = level;
      expect(TokenKind::RParen, "to close Type(n)");
    }
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
    if (accept(TokenKind::Bang)) {
      EffectClause clause = parseEffectClause();
      t->effectsDeclared = true;
      t->effects = clause.effects;
      t->effectVar = clause.var;
    }
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
    if (t->package.empty() && t->name == "Simd" && at(TokenKind::LParen)) {
      advance();
      t->params.push_back(parseType());
      expect(TokenKind::Comma, "after Simd element type");
      t->lhs = parseExpr();
      expect(TokenKind::RParen, "to close 'Simd(T, n)'");
      return t;
    }
    if (t->package.empty() && t->name == "Vec" && at(TokenKind::LParen)) {
      advance();
      t->params.push_back(parseType());
      expect(TokenKind::Comma, "after Vec element type");
      t->lhs = parseExpr();
      expect(TokenKind::RParen, "to close 'Vec(T, n)'");
      return t;
    }
    if (t->package.empty() && t->name == "Quotient" && at(TokenKind::LParen)) {
      t->kind = TypeExprKind::Quotient;
      advance();
      t->elem = parseType();
      expect(TokenKind::Comma, "after Quotient carrier");
      t->lhs = parseExpr();
      expect(TokenKind::Comma, "after Quotient relation");
      t->rhs = parseExpr();
      expect(TokenKind::RParen, "to close 'Quotient(...)'");
      return t;
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
  ExprPtr lhs = parseBitOr();
  while (at(TokenKind::KwAnd)) {
    SourceLocation loc = advance().loc;
    lhs = std::make_unique<BinaryExpr>(loc, BinaryOp::And, std::move(lhs), parseBitOr());
  }
  return lhs;
}

ExprPtr Parser::parseBitOr() {
  ExprPtr lhs = parseBitXor();
  while (at(TokenKind::Pipe)) {
    SourceLocation loc = advance().loc;
    lhs = std::make_unique<BinaryExpr>(loc, BinaryOp::BitOr, std::move(lhs), parseBitXor());
  }
  return lhs;
}

ExprPtr Parser::parseBitXor() {
  ExprPtr lhs = parseBitAnd();
  while (at(TokenKind::Caret)) {
    SourceLocation loc = advance().loc;
    lhs = std::make_unique<BinaryExpr>(loc, BinaryOp::BitXor, std::move(lhs), parseBitAnd());
  }
  return lhs;
}

ExprPtr Parser::parseBitAnd() {
  ExprPtr lhs = parseEquality();
  while (at(TokenKind::Amp)) {
    SourceLocation loc = advance().loc;
    lhs = std::make_unique<BinaryExpr>(loc, BinaryOp::BitAnd, std::move(lhs), parseEquality());
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
  ExprPtr lhs = parseShift();
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
    lhs = std::make_unique<BinaryExpr>(loc, op, std::move(lhs), parseShift());
  }
}

ExprPtr Parser::parseShift() {
  ExprPtr lhs = parseAdditive();
  for (;;) {
    BinaryOp op;
    if (at(TokenKind::ShiftL))
      op = BinaryOp::Shl;
    else if (at(TokenKind::ShiftR))
      op = BinaryOp::Shr;
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
  ExprPtr lhs = parseCast();
  while (at(TokenKind::Meet)) {
    SourceLocation loc = advance().loc;
    lhs = std::make_unique<BinaryExpr>(loc, BinaryOp::Meet, std::move(lhs), parseCast());
  }
  return lhs;
}

ExprPtr Parser::parseCast() {
  ExprPtr e = parseUnary();
  while (accept(TokenKind::KwAs)) {
    TypeExprPtr ty = parseType();
    e = std::make_unique<CastExpr>(e->loc, std::move(e), std::move(ty));
  }
  return e;
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
  if (tok.kind == TokenKind::Amp) {
    advance();
    bool mut = accept(TokenKind::KwMut);
    return std::make_unique<BorrowExpr>(tok.loc, mut, parseUnary());
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
    } else if (at(TokenKind::LBracket)) {
      SourceLocation loc = peek().loc;
      advance();
      if (at(TokenKind::LBrace)) {
        advance();
        EffectSet bits = 0;
        if (!at(TokenKind::RBrace)) {
          do {
            std::string name = expectIdentifier("as an instantiated effect");
            EffectSet bit = 0;
            if (!namedEffect(name, bit))
              fail(peek(), "unknown effect '" + name + "' in instantiation");
            bits = static_cast<EffectSet>(bits | bit);
          } while (accept(TokenKind::Comma));
        }
        expect(TokenKind::RBrace, "to close instantiated effect row");
        expect(TokenKind::RBracket, "to close effect instantiation");
        auto call = std::make_unique<CallExpr>(loc, std::move(e), parseArgs());
        call->hasEffectInst = true;
        call->effectInst = bits;
        e = std::move(call);
      } else {
        std::vector<TypeExprPtr> targs;
        do {
          targs.push_back(parseType());
        } while (accept(TokenKind::Comma));
        expect(TokenKind::RBracket, "to close type instantiation");
        auto call = std::make_unique<CallExpr>(loc, std::move(e), parseArgs());
        call->hasTypeArg = true;
        call->typeArgs = std::move(targs);
        if (!call->typeArgs.empty())
          call->typeArg = call->typeArgs.front()->clone();
        e = std::move(call);
      }
    } else if (at(TokenKind::Question)) {
      SourceLocation loc = advance().loc;
      e = std::make_unique<TryExpr>(loc, std::move(e));
    } else if (at(TokenKind::KwAlong)) {
      if (!e || e->kind != ExprKind::Call)
        fail(peek(), "'along' follows a cofn self-call");
      advance();
      static_cast<CallExpr &>(*e).along = parseExpr();
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
