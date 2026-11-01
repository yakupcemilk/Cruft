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
//   4. checkConstFunctions : `const fn` bodies, before any constant is folded.
//   5. checkGlobals        : initialisers are well-typed compile-time constants.
//   6. checkFunctionBodies : statements and expressions, including the
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
  TypePtr applyBinders(TypePtr type, const std::vector<const Symbol *> &binders,
                       const std::vector<TypePtr> &args) const {
    return instantiateBinders(type, binders, args);
  }

private:
  // ---- phases ----
  void collectDeclarations();
  void collectFile(ast::File &file);
  void finishLinkage();
  void processExports();
  void checkDependencyGraph();
  void resolveSignatures();
  void resolveFileSignatures(ast::File &file, bool functions);
  void checkGlobals();
  void checkConstFunctions();
  void checkFunctionBodies();
  void checkFunction(ast::FnDecl &fn);
  void checkMain(const ast::FnDecl &fn);
  void checkForIterator(ast::ForStmt &stmt);
  void checkFieldAssign(ast::FieldAssignStmt &stmt);
  void checkConstFn(const ast::FnDecl &fn);
  void checkConstBlock(const ast::Block &block);
  void checkConstExpr(const ast::Expr &e);
  [[nodiscard]] bool isConstType(const Type &type, std::set<const Symbol *> &seen) const;
  std::vector<const Symbol *> openTypeBinders(const std::vector<ast::FnDecl::TypeBinder> &binders);
  TypePtr instantiateBinders(TypePtr type, const std::vector<const Symbol *> &binders,
                             const std::vector<TypePtr> &args) const;
  TypePtr instantiateSymbol(const Symbol &sym, const std::vector<TypePtr> &args, int region);
  [[nodiscard]] TypePtr inferTypeArg(const Type &param, const TypePtr &arg, const Symbol *var) const;

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
  void popScope();
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

  struct EffectRow {
    ast::EffectSet bits = 0;
    std::string var;
    bool declared = false;
  };
  [[nodiscard]] ast::Usage resolveUsage(ast::Usage annotated, const Type &type, bool shared,
                                        bool isConst) const;
  void noteEffect(const Type &callee, const Symbol *sym, const SourceLocation &loc);
  void rejectBoundary(const ast::Expr &arg, bool boundary, const SourceLocation &loc,
                      const std::string &what);
  [[nodiscard]] bool effectsCompatible(const Type &actual, const Type &expected) const;
  TypePtr instantiateEffects(const TypePtr &type, const std::string &var, ast::EffectSet bits) const;
  void joinUses(const std::set<const Symbol *> &saved, const std::set<const Symbol *> &thenSet,
                const std::set<const Symbol *> &elseSet, bool thenDiv, bool elseDiv, bool hasElse,
                const SourceLocation &loc);
  void exhaustScope(size_t scopeIndex, const SourceLocation &loc);
  void exhaustInScope(const SourceLocation &loc);
  [[nodiscard]] std::set<const Symbol *> symbolsInScope() const;
  void checkIndexResidues(const ast::Block &block, const std::vector<const Symbol *> &indices);
  void verifyCofn(ast::FnDecl &fn);
  bool cofnTerminates(ast::Block &block, const Symbol *self);
  void rejectUntailCalls(const ast::Block &block, const Symbol *self);
  [[nodiscard]] bool isFixedSize(const Type &type, std::set<const Symbol *> &seen) const;
  void checkAlong(ast::CallExpr &call, const Type &fnType);

  // ---- expressions ----
  /// Checks `e` (possibly replacing the node) and returns its type.
  TypePtr checkExpr(ast::ExprPtr &e, const Type *expected = nullptr);
  TypePtr checkIdent(ast::ExprPtr &e, const Type *expected);
  TypePtr checkBinary(ast::BinaryExpr &e, const Type *expected);
  TypePtr checkUnary(ast::UnaryExpr &e, const Type *expected);
  TypePtr checkCast(ast::CastExpr &e);
  TypePtr checkBorrow(ast::BorrowExpr &e);
  void releaseScopeBorrows();
  void releaseStatementBorrows();
  void acquireBorrow(const Symbol *owner, const Symbol *binding, bool mut,
                     const SourceLocation &loc);
  void bindLoan(const ast::Expr &init, const Symbol *binding);
  void rejectIfBorrowed(const Symbol *owner, const SourceLocation &loc, bool mutating);
  void requireUnsafe(const SourceLocation &loc, const std::string &what);
  void noteSpawnShared(const ast::CallExpr &call);
  TypePtr checkTry(ast::TryExpr &e);
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
  void assignFunctionType(ast::FnDecl &fn, const std::vector<const Symbol *> *sharedBinders);
  void resolveTraits(ast::File &file);
  void resolveImpls(ast::File &file);
  struct TraitMethod {
    std::string name;
    TypePtr type;
    SourceLocation loc;
  };
  struct ImplInfo {
    const Symbol *trait = nullptr;
    TypePtr target;
    std::vector<const Symbol *> binders;
    std::map<std::string, Symbol *> methods;
    ast::Visibility visibility = ast::Visibility::Private;
    std::string package;
    SourceLocation loc;
  };
  struct TraitDispatch {
    Symbol *fn = nullptr;
    std::vector<TypePtr> typeArgs;
  };
  enum class TraitLookup { None, Found, Failed };
  [[nodiscard]] bool typeMentions(const Type &type, const Symbol *var) const;
  [[nodiscard]] bool traitTypesEqual(const Type &a, const Type &b);
  [[nodiscard]] bool patternEqual(const Type &a, const Type &b,
                                  const std::vector<const Symbol *> &bindersA,
                                  const std::vector<const Symbol *> &bindersB);
  [[nodiscard]] bool bindPattern(const Type &pattern, const TypePtr &actual,
                                 const std::vector<const Symbol *> &binders,
                                 std::vector<TypePtr> &args);
  [[nodiscard]] bool matchImpl(const ImplInfo &impl, const TypePtr &recv,
                               std::vector<TypePtr> &args);
  [[nodiscard]] bool sameImplTarget(const ImplInfo &a, const ImplInfo &b);
  TraitLookup lookupTrait(const std::string &name, const TypePtr &recv, const SourceLocation &loc,
                          TraitDispatch &out);
  TypePtr tryTraitMethod(ast::ExprPtr &e, const TypePtr &recv, const Type *expected);
  bool tryTraitFunction(ast::CallExpr &call, const Type *expected, TypePtr &out);
  TypePtr checkStructCall(ast::CallExpr &call, const Type &structType);
  TypePtr substTypeVar(const TypePtr &type, const Symbol *var, const TypePtr &replacement) const;
  [[nodiscard]] int universeOf(const Type &type) const;
  [[nodiscard]] TypePtr typeOfDenoted(const Type &denoted) const;

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
  std::vector<EffectRow> effectStack_;
  int loopDepth_ = 0;
  struct LiveBorrow {
    const Symbol *owner = nullptr;
    const Symbol *refBinding = nullptr; ///< null until a `var` binds the loan
    bool mut = false;
    int scopeDepth = 0;
  };
  std::vector<LiveBorrow> borrows_;
  int unsafeDepth_ = 0;
  bool inBorrowPlace_ = false; ///< the place of `&` / `&mut` is not a use of the owner
  TypePtr currentSelf_; ///< impl target while resolving an impl method signature
  int implSerial_ = 0;
  std::map<const Symbol *, std::vector<TraitMethod>> traitMethods_;
  std::map<const Symbol *, const Symbol *> traitSelf_;
  std::map<const Symbol *, std::vector<const Symbol *>> traitBinders_;
  std::vector<ImplInfo> impls_;
};

} // namespace cruft::sema
