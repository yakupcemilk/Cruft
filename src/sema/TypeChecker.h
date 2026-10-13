// TypeChecker.h - semantic analysis: module system, visibility, dependent
// path types, FFI layout rules and HoTT proof obligations.
//
// Pipeline inside `check()`:
//   1. collectDeclarations : build packages from `package <name>`, register
//                            every top-level symbol with its visibility, and
//                            record per-file imports.
//   2. checkDependencyGraph: imports must name existing packages and the
//                            package graph must be acyclic (strict hierarchy).
//   3. resolveSignatures   : types of functions, globals and constructors;
//                            FFI layout checks for `extern`; no private type
//                            may leak through a `pub` signature.
//   4. checkGlobals        : initialisers are well-typed compile-time constants.
//   5. checkFunctionBodies : statements and expressions, including the
//                            cubical rules (path boundaries, face systems,
//                            path-respecting eliminators for HITs).
#pragma once

#include "ast/AST.h"
#include "Diagnostics.h"
#include "sema/Mangler.h"
#include "sema/Normalizer.h"
#include "sema/Types.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace cruft::sema {

class TypeChecker {
public:
  TypeChecker(ast::Program &program, DiagnosticEngine &diags);

  /// Runs every phase. Returns false if any error was reported.
  bool check();

  ModuleRegistry &registry() { return registry_; }
  Normalizer &normalizer() { return normalizer_; }

private:
  // ---- phases ----
  void collectDeclarations();
  void collectFile(ast::File &file);
  void finishLinkage();
  void processExports();
  void checkDependencyGraph();
  void resolveSignatures();
  void resolveFileSignatures(ast::File &file);
  void checkGlobals();
  void checkFunctionBodies();
  void checkFunction(ast::FnDecl &fn);

  // ---- scopes ----
  /// `Arena` is the lexical contour opened by `scope arena`. Names declared
  /// there die with the block, and storage born there cannot be transported
  /// to a binding declared in an outer scope.
  enum class ScopeKind { Lexical, Arena };
  struct Scope {
    std::map<std::string, Symbol *> names;
    ScopeKind kind = ScopeKind::Lexical;
    int region = 0;
  };
  void pushScope(ScopeKind kind = ScopeKind::Lexical) {
    Scope s;
    s.kind = kind;
    s.region = currentRegion_;
    scopes_.emplace_back(std::move(s));
  }
  void popScope() { scopes_.pop_back(); }
  [[nodiscard]] bool insideArena() const;
  Symbol *declareLocal(Symbol sym);
  [[nodiscard]] Symbol *lookupLocal(const std::string &name) const;

  // ---- resolution ----
  TypePtr resolveType(const ast::TypeExpr &t);
  Symbol *resolveName(const std::string &pkg, const std::string &name, const SourceLocation &loc,
                      bool reportErrors = true);
  [[nodiscard]] const ast::UseBinding *findUse(const std::string &name) const;
  void ensureNotLeakingPrivate(const Type &t, const SourceLocation &loc, const std::string &what);
  bool isStaticOnlyType(const Type &t) const;

  // ---- statements ----
  void checkBlock(ast::Block &block);
  void checkStmt(ast::Stmt &stmt);
  [[nodiscard]] static bool blockAlwaysReturns(const ast::Block &block);

  // ---- expressions ----
  /// Checks `e` (possibly replacing the node) and returns its type.
  TypePtr checkExpr(ast::ExprPtr &e, const Type *expected = nullptr);
  TypePtr checkIdent(ast::ExprPtr &e, const Type *expected);
  TypePtr checkBinary(ast::BinaryExpr &e, const Type *expected);
  TypePtr checkUnary(ast::UnaryExpr &e, const Type *expected);
  TypePtr checkCall(ast::ExprPtr &e, const Type *expected);
  TypePtr checkBuiltinCall(ast::CallExpr &call, ast::Builtin builtin, const Type *expected);
  TypePtr checkFnCall(ast::CallExpr &call, const Type &fnType, const Type *expected);
  TypePtr checkPathAbs(ast::PathAbsExpr &e, const Type *expected);
  TypePtr checkPathApp(ast::PathAppExpr &e);
  TypePtr checkHcomp(ast::HcompExpr &e);
  TypePtr checkMatch(ast::MatchExpr &e, const Type *expected);

  void checkMatchRespectsPaths(ast::MatchExpr &m, const Symbol &typeSym);
  TypePtr resolveTransport(ast::CallExpr &call);
  TypePtr checkArray(ast::ArrayLitExpr &e, const Type *expected);
  TypePtr checkLambda(ast::LambdaExpr &e, const Type *expected);
  TypePtr checkMethod(ast::ExprPtr &e, const Type *expected);

  /// Lexical regions. Region 0 is the heap: its lifetime witness is a constant
  /// path (normalises to `refl`) and transport along it is the identity.
  /// An arena region is a non-constant path, so a value born there cannot be
  /// transported to an enclosing scope. Programmers never write the witness.
  struct Region {
    int id = 0;
    int parent = -1;
    bool arena = false;
    SourceLocation loc;
    ast::ExprPtr witness;
  };
  void initRegions();
  int pushRegion(bool arena, SourceLocation loc);
  void popRegion();
  [[nodiscard]] bool regionOutlives(int valueRegion, int destRegion);
  void requireLives(const Type &value, int destRegion, const SourceLocation &loc,
                    const std::string &what);
  void noteUse(const Symbol *sym, const SourceLocation &loc);
  ast::ExprPtr lifetimeWitness(bool constant, SourceLocation loc);

  // ---- helpers ----
  bool unify(const Type &actual, const Type &expected, const SourceLocation &loc,
             const std::string &context);
  TypePtr fail(const SourceLocation &loc, const std::string &msg);
  ast::ExprPtr makeTypeLit(const TypePtr &t, const SourceLocation &loc) const;
  TypePtr typeOfTypeTerm(const ast::Expr &e);
  std::string linkNameFor(const Symbol &sym) const;

  ast::Program &program_;
  DiagnosticEngine &diags_;
  ModuleRegistry registry_;
  Normalizer normalizer_;

  std::vector<Scope> scopes_;
  const ast::File *file_ = nullptr;
  std::string package_;
  const ast::FnDecl *currentFn_ = nullptr;
  TypePtr currentRet_;

  std::vector<Region> regions_;
  int currentRegion_ = 0;
  std::set<const Symbol *> moved_;
  bool consume_ = true; ///< false while checking a borrowed method receiver
};

} // namespace cruft::sema
