// AST.h - abstract syntax tree for Cruft.
//
// The tree is owned through std::unique_ptr. Semantic phases annotate nodes
// in place (resolved types / symbols) so that CodeGen never has to re-derive
// information the TypeChecker already established.
#pragma once

#include "Diagnostics.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cruft::sema {
struct Type;
struct Symbol;
} // namespace cruft::sema

namespace cruft::ast {

using TypeRef = std::shared_ptr<const sema::Type>;

enum class Visibility { Private, Public };

inline const char *visibilityName(Visibility v) {
  return v == Visibility::Public ? "pub" : "pri";
}

// ---------------------------------------------------------------------------
// Syntactic types
// ---------------------------------------------------------------------------

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct TypeExpr;
using TypeExprPtr = std::unique_ptr<TypeExpr>;

enum class TypeExprKind {
  Named, ///< i32, bool, I, Type, or a (possibly package-qualified) user type
  Path,  ///< Path(T, a, b)
  Equiv, ///< Equiv(A, B)
  Fn     ///< fn(A, B): R
};

struct TypeExpr {
  TypeExprKind kind;
  SourceLocation loc;

  // Named
  std::string package; ///< empty when unqualified
  std::string name;

  // Path
  TypeExprPtr elem;
  ExprPtr lhs, rhs;

  // Equiv / Fn
  std::vector<TypeExprPtr> params; ///< Equiv: {A, B}; Fn: parameter types
  TypeExprPtr ret;                 ///< Fn only; null => void

  explicit TypeExpr(TypeExprKind k, SourceLocation l) : kind(k), loc(std::move(l)) {}
  [[nodiscard]] TypeExprPtr clone() const;
};

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

enum class ExprKind {
  IntLit,
  FloatLit,
  BoolLit,
  Ident,
  Binary,
  Unary,
  Call,
  TypeLit,     ///< a type used as a term (lives in universe `Type`)
  IntervalLit, ///< i0 / i1
  PathAbs,     ///< <i> e
  PathApp,     ///< p @ r
  Hcomp,       ///< hcomp(T, base, [faces])
  Match,       ///< match e case C(x) then e ... end
  StringLit,   ///< "..."
  ArrayLit,    ///< [e, e, ...]
  Lambda,      ///< fn(x) => e
  Method       ///< receiver.name(args)  — also package.name before resolution
};

enum class BinaryOp {
  Add, Sub, Mul, Div, Rem,
  Eq, Ne, Lt, Le, Gt, Ge,
  And, Or,
  Meet, ///< /\ on the interval
  Join  ///< \/ on the interval
};

enum class UnaryOp { Neg, Not, IntervalRev };

const char *binaryOpSpelling(BinaryOp op);
const char *unaryOpSpelling(UnaryOp op);

/// Built-in operations recognised by the type checker on CallExpr.
enum class Builtin {
  None,
  Print,     ///< print(x)
  Refl,      ///< refl(a)              : Path(T, a, a)
  Sym,       ///< sym(p)               : Path(T, b, a)
  Transport, ///< transport(p, a)      : B      where p : Path(Type, A, B)
  Ua,        ///< ua(e)                : Path(Type, A, B) where e : Equiv(A, B)
  Iso,       ///< iso(f, g, sec, retr) : Equiv(A, B)
  RawSlice,    ///< raw_slice(addr, len) : Slice(T)  (T taken from the expected type)
  NullPtr,     ///< null() : ptr
  AllocString  ///< alloc_string(nbytes) : String  (heap, NUL-terminated)
};

/// How a `transport` call computes once its type path has been normalised.
enum class TransportDir { Identity, Forward, Backward };

struct Expr {
  ExprKind kind;
  SourceLocation loc;
  TypeRef type; ///< filled in by the TypeChecker

  Expr(ExprKind k, SourceLocation l) : kind(k), loc(std::move(l)) {}
  virtual ~Expr() = default;
  [[nodiscard]] virtual ExprPtr clone() const = 0;
};

struct IntLitExpr final : Expr {
  std::int64_t value;
  IntLitExpr(SourceLocation l, std::int64_t v) : Expr(ExprKind::IntLit, std::move(l)), value(v) {}
  ExprPtr clone() const override;
};

struct FloatLitExpr final : Expr {
  double value;
  FloatLitExpr(SourceLocation l, double v) : Expr(ExprKind::FloatLit, std::move(l)), value(v) {}
  ExprPtr clone() const override;
};

struct BoolLitExpr final : Expr {
  bool value;
  BoolLitExpr(SourceLocation l, bool v) : Expr(ExprKind::BoolLit, std::move(l)), value(v) {}
  ExprPtr clone() const override;
};

struct IdentExpr final : Expr {
  std::string package; ///< empty when unqualified
  std::string name;
  const sema::Symbol *symbol = nullptr; ///< resolved by the TypeChecker
  IdentExpr(SourceLocation l, std::string pkg, std::string n)
      : Expr(ExprKind::Ident, std::move(l)), package(std::move(pkg)), name(std::move(n)) {}
  ExprPtr clone() const override;
};

struct BinaryExpr final : Expr {
  BinaryOp op;
  ExprPtr lhs, rhs;
  BinaryExpr(SourceLocation l, BinaryOp o, ExprPtr a, ExprPtr b)
      : Expr(ExprKind::Binary, std::move(l)), op(o), lhs(std::move(a)), rhs(std::move(b)) {}
  ExprPtr clone() const override;
};

struct UnaryExpr final : Expr {
  UnaryOp op;
  ExprPtr operand;
  UnaryExpr(SourceLocation l, UnaryOp o, ExprPtr e)
      : Expr(ExprKind::Unary, std::move(l)), op(o), operand(std::move(e)) {}
  ExprPtr clone() const override;
};

struct CallExpr final : Expr {
  ExprPtr callee;
  std::vector<ExprPtr> args;

  // --- semantic annotations ---
  Builtin builtin = Builtin::None;
  const sema::Symbol *fn = nullptr;          ///< direct callee (function / ctor)
  TransportDir transportDir = TransportDir::Identity;
  const sema::Symbol *transportFn = nullptr; ///< fn implementing the transport

  CallExpr(SourceLocation l, ExprPtr c, std::vector<ExprPtr> a)
      : Expr(ExprKind::Call, std::move(l)), callee(std::move(c)), args(std::move(a)) {}
  ExprPtr clone() const override;
};

struct TypeLitExpr final : Expr {
  TypeExprPtr typeExpr;
  TypeRef denoted; ///< the type this term denotes (resolved by the TypeChecker)
  TypeLitExpr(SourceLocation l, TypeExprPtr t)
      : Expr(ExprKind::TypeLit, std::move(l)), typeExpr(std::move(t)) {}
  ExprPtr clone() const override;
};

struct IntervalLitExpr final : Expr {
  bool one; ///< false => i0, true => i1
  IntervalLitExpr(SourceLocation l, bool v) : Expr(ExprKind::IntervalLit, std::move(l)), one(v) {}
  ExprPtr clone() const override;
};

struct PathAbsExpr final : Expr {
  std::string var;
  ExprPtr body;
  const sema::Symbol *binder = nullptr; ///< interval variable symbol (TypeChecker)
  PathAbsExpr(SourceLocation l, std::string v, ExprPtr b)
      : Expr(ExprKind::PathAbs, std::move(l)), var(std::move(v)), body(std::move(b)) {}
  ExprPtr clone() const override;
};

struct PathAppExpr final : Expr {
  ExprPtr path;
  ExprPtr arg;
  PathAppExpr(SourceLocation l, ExprPtr p, ExprPtr r)
      : Expr(ExprKind::PathApp, std::move(l)), path(std::move(p)), arg(std::move(r)) {}
  ExprPtr clone() const override;
};

/// A face constraint `i = 0 /\ j = 1` together with its partial element.
struct Face {
  struct Atom {
    std::string var;
    bool one; ///< var = i1 if true, var = i0 otherwise
  };
  std::vector<Atom> constraints;
  ExprPtr side; ///< must be a path (in the composition direction)
  SourceLocation loc;
};

struct HcompExpr final : Expr {
  TypeExprPtr typeExpr;
  ExprPtr base;
  std::vector<Face> faces;
  HcompExpr(SourceLocation l, TypeExprPtr t, ExprPtr b, std::vector<Face> f)
      : Expr(ExprKind::Hcomp, std::move(l)), typeExpr(std::move(t)), base(std::move(b)),
        faces(std::move(f)) {}
  ExprPtr clone() const override;
};

struct MatchCase {
  std::string ctor;
  std::vector<std::string> binders;
  ExprPtr body;
  SourceLocation loc;
  const sema::Symbol *ctorSymbol = nullptr;             ///< resolved by the TypeChecker
  std::vector<const sema::Symbol *> binderSymbols;      ///< one PatternVar per binder
};

struct StringLitExpr final : Expr {
  std::string value;
  StringLitExpr(SourceLocation l, std::string v)
      : Expr(ExprKind::StringLit, std::move(l)), value(std::move(v)) {}
  ExprPtr clone() const override;
};

struct ArrayLitExpr final : Expr {
  std::vector<ExprPtr> elems;
  /// Byte offset into the enclosing framed `scope arena`, or -1 when the
  /// allocation is emitted as its own bump.
  int frameOffset = -1;
  ArrayLitExpr(SourceLocation l, std::vector<ExprPtr> es)
      : Expr(ExprKind::ArrayLit, std::move(l)), elems(std::move(es)) {}
  ExprPtr clone() const override;
};

/// `receiver.method(args)`. `invoked` is false for a bare `package.name` value.
struct MethodCallExpr final : Expr {
  ExprPtr receiver;
  std::string method;
  std::vector<ExprPtr> args;
  bool invoked = false;
  MethodCallExpr(SourceLocation l, ExprPtr recv, std::string m, std::vector<ExprPtr> a, bool call)
      : Expr(ExprKind::Method, std::move(l)), receiver(std::move(recv)), method(std::move(m)),
        args(std::move(a)), invoked(call) {}
  ExprPtr clone() const override;
};

struct MatchExpr final : Expr {
  ExprPtr scrutinee;
  std::vector<MatchCase> cases;
  MatchExpr(SourceLocation l, ExprPtr s, std::vector<MatchCase> c)
      : Expr(ExprKind::Match, std::move(l)), scrutinee(std::move(s)), cases(std::move(c)) {}
  ExprPtr clone() const override;
};

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

enum class StmtKind { VarDecl, Assign, Return, If, While, Expr, ScopeArena, Error };

struct Stmt {
  StmtKind kind;
  SourceLocation loc;
  Stmt(StmtKind k, SourceLocation l) : kind(k), loc(std::move(l)) {}
  virtual ~Stmt() = default;
};
using StmtPtr = std::unique_ptr<Stmt>;
using Block = std::vector<StmtPtr>;

struct VarDeclStmt final : Stmt {
  bool isConst;
  bool shared = false; ///< `shared var` — reference counted; otherwise owned (affine)
  std::string name;
  TypeExprPtr type; ///< null when the type is inferred from the initializer
  ExprPtr init;
  const sema::Symbol *symbol = nullptr;
  VarDeclStmt(SourceLocation l, bool c, std::string n, TypeExprPtr t, ExprPtr i)
      : Stmt(StmtKind::VarDecl, std::move(l)), isConst(c), name(std::move(n)),
        type(std::move(t)), init(std::move(i)) {}
};

struct AssignStmt final : Stmt {
  std::string name;
  ExprPtr value;
  const sema::Symbol *symbol = nullptr;
  AssignStmt(SourceLocation l, std::string n, ExprPtr v)
      : Stmt(StmtKind::Assign, std::move(l)), name(std::move(n)), value(std::move(v)) {}
};

struct ReturnStmt final : Stmt {
  ExprPtr value; ///< null for `ret` in a void function
  ReturnStmt(SourceLocation l, ExprPtr v) : Stmt(StmtKind::Return, std::move(l)), value(std::move(v)) {}
};

struct IfStmt final : Stmt {
  ExprPtr cond;
  Block thenBody, elseBody;
  IfStmt(SourceLocation l, ExprPtr c, Block t, Block e)
      : Stmt(StmtKind::If, std::move(l)), cond(std::move(c)), thenBody(std::move(t)),
        elseBody(std::move(e)) {}
};

struct WhileStmt final : Stmt {
  ExprPtr cond;
  Block body;
  WhileStmt(SourceLocation l, ExprPtr c, Block b)
      : Stmt(StmtKind::While, std::move(l)), cond(std::move(c)), body(std::move(b)) {}
};

struct ExprStmt final : Stmt {
  ExprPtr expr;
  ExprStmt(SourceLocation l, ExprPtr e) : Stmt(StmtKind::Expr, std::move(l)), expr(std::move(e)) {}
};

/// `scope arena` … `end`.
///
/// `body` is the statement list owned by this node (`Block` is
/// `std::vector<std::unique_ptr<Stmt>>`). Allocations inside are bump-allocated
/// from the runtime arena and the bump pointer is restored at `end`, including
/// on `ret`.
struct ScopeArenaStmt final : Stmt {
  Block body;
  /// No allocations: the push/pop pair is deleted.
  bool elide = false;
  /// Nested inside another arena; its watermark is the parent's.
  bool folded = false;
  /// Every bump in this scope has a compile-time size. One allocation of
  /// `frameBytes` covers them; each site reads `ArrayLitExpr::frameOffset`.
  bool framed = false;
  std::uint64_t frameBytes = 0;
  unsigned frameAlign = 1;
  ScopeArenaStmt(SourceLocation l, Block b)
      : Stmt(StmtKind::ScopeArena, std::move(l)), body(std::move(b)) {}
};

/// Placeholder left by panic-mode recovery. Never null; later phases skip it.
struct ErrorStmt final : Stmt {
  explicit ErrorStmt(SourceLocation l) : Stmt(StmtKind::Error, std::move(l)) {}
};

// ---------------------------------------------------------------------------
// Declarations
// ---------------------------------------------------------------------------

struct Param {
  std::string name;
  TypeExprPtr type; ///< null when a lambda parameter's type is inferred
  SourceLocation loc;

  [[nodiscard]] Param clone() const {
    Param p;
    p.name = name;
    p.loc = loc;
    if (type)
      p.type = type->clone();
    return p;
  }
};

struct LambdaExpr final : Expr {
  std::vector<Param> params;
  ExprPtr body;
  std::vector<const sema::Symbol *> paramSymbols;
  LambdaExpr(SourceLocation l, std::vector<Param> ps, ExprPtr b)
      : Expr(ExprKind::Lambda, std::move(l)), params(std::move(ps)), body(std::move(b)) {}
  ExprPtr clone() const override;
};

struct FnDecl {
  Visibility visibility = Visibility::Private;
  bool isExtern = false;
  std::string name;
  std::vector<Param> params;
  TypeExprPtr ret; ///< null => void
  Block body;      ///< empty for extern declarations
  SourceLocation loc;
  sema::Symbol *symbol = nullptr;
};

struct GlobalDecl {
  Visibility visibility = Visibility::Private;
  bool isConst = false;
  bool shared = false;
  std::string name;
  TypeExprPtr type;
  ExprPtr init;
  SourceLocation loc;
  sema::Symbol *symbol = nullptr;
};

/// Constructor of a (higher) inductive type. A constructor with `pathType`
/// set is a path constructor and carries no runtime payload.
struct CtorDecl {
  std::string name;
  std::vector<Param> params;
  TypeExprPtr pathType; ///< null => point constructor
  SourceLocation loc;
  sema::Symbol *symbol = nullptr;
};

struct TypeDecl {
  Visibility visibility = Visibility::Private;
  std::string name;
  std::vector<CtorDecl> ctors;
  SourceLocation loc;
  sema::Symbol *symbol = nullptr;
};

struct ImportDecl {
  std::string package;
  SourceLocation loc;
};

/// `use pkg.symbol` brings `symbol` into unqualified scope in this file.
/// `symbol` is empty when `use pkg` only imports the package.
struct UseBinding {
  std::string local;
  std::string package;
  std::string symbol;
  SourceLocation loc;
};

/// `export pkg.symbol` publishes `symbol` from `pkg` (or this package, when
/// `package` is empty) under `name` in the current package.
struct ExportDecl {
  std::string package;
  std::string name;
  SourceLocation loc;
};

/// One source file. Several files may share a package.
struct File {
  std::string filename;
  std::string package;
  /// File-level module name. Defaults to the source stem; `module <name>` overrides it.
  std::string moduleName;
  SourceLocation packageLoc;
  std::vector<ImportDecl> imports;
  std::vector<UseBinding> uses;
  std::vector<ExportDecl> exports;
  std::vector<std::unique_ptr<TypeDecl>> types;
  std::vector<std::unique_ptr<GlobalDecl>> globals;
  std::vector<std::unique_ptr<FnDecl>> functions;
};

struct Program {
  std::vector<std::unique_ptr<File>> files;
};

// ---------------------------------------------------------------------------
// Pretty printing (used by diagnostics and by the normaliser for keys)
// ---------------------------------------------------------------------------

std::string toString(const Expr &e);
std::string toString(const TypeExpr &t);

} // namespace cruft::ast
