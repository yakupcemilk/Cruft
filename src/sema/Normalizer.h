// Normalizer.h - definitional equality for Cruft terms.
//
// Cruft's path types are dependent (`Path(T, a, b)` mentions the terms `a`
// and `b`), so type checking needs a notion of *definitional equality* of
// terms. The Normalizer rewrites a (type-annotated) expression into a normal
// form and compares normal forms up to alpha-equivalence.
//
// Supported computation rules:
//   * constant folding of literals (wrapping integer semantics);
//   * integer arithmetic is normalised to a linear polynomial
//     `c + k1*atom1 + ... + kn*atomn`, so `(b - 273) + 273 ≡ b`;
//   * unfolding of transparent functions (body is a single `ret e`) and of
//     `const` bindings with immutable initialisers;
//   * the De Morgan algebra of the interval I (`~`, `/\`, `\/`);
//   * cubical beta/eta rules: `(<i> e) @ r ≡ e[i := r]`, `<i> p @ i ≡ p`,
//     `refl(a) @ r ≡ a`, `p @ i0 ≡ lhs(p)`, `p @ i1 ≡ rhs(p)`,
//     `sym(sym(p)) ≡ p`, `sym(refl(a)) ≡ refl(a)`;
//   * computational univalence: `transport(ua(iso(f, g, _, _)), a) ≡ f(a)`,
//     `transport(sym(ua(iso(f, g, _, _))), a) ≡ g(a)`, `transport(refl, a) ≡ a`;
//   * `hcomp` over a 0-truncated type (primitive or point-only inductive)
//     reduces to its base;
//   * `match` on a constructor application selects the branch.
#pragma once

#include "ast/AST.h"
#include "sema/Types.h"

#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace cruft::sema {

class Normalizer {
public:
  Normalizer() = default;

  /// Full normal form (fuel-limited; returns a stuck term if fuel runs out).
  ast::ExprPtr normalize(const ast::Expr &e);

  /// Definitional equality of two terms.
  bool equal(const ast::Expr &a, const ast::Expr &b);

  /// Equality of types (recursing into path endpoints via `equal`).
  bool typeEqual(const Type &a, const Type &b);

  /// Capture-free substitution of `var` by `with` (identifiers are resolved
  /// to symbols, so binders never capture by name).
  ast::ExprPtr substitute(const ast::Expr &e, const Symbol *var, const ast::Expr &with);
  TypePtr substituteType(const TypePtr &t, const Symbol *var, const ast::Expr &with);

  /// Does `e` mention `var` free?
  static bool mentions(const ast::Expr &e, const Symbol *var);
  /// Does `e` reference any mutable (`var`) binding? Used to decide which
  /// `const` initialisers may be unfolded.
  static bool mentionsMutable(const ast::Expr &e);

  /// A structural key that distinguishes symbols by identity, used to order
  /// atoms canonically.
  static std::string key(const ast::Expr &e);

  /// Builds an identifier expression referring to `sym`.
  static ast::ExprPtr identOf(const Symbol *sym, SourceLocation loc);

private:
  ast::ExprPtr norm(const ast::Expr &e);
  ast::ExprPtr normBinary(const ast::BinaryExpr &e);
  ast::ExprPtr normUnary(const ast::UnaryExpr &e);
  ast::ExprPtr normCall(const ast::CallExpr &e);
  ast::ExprPtr normPathAbs(const ast::PathAbsExpr &e);
  ast::ExprPtr normPathApp(const ast::PathAppExpr &e);
  ast::ExprPtr normMatch(const ast::MatchExpr &e);
  ast::ExprPtr normHcomp(const ast::HcompExpr &e);

  ast::ExprPtr normalizeIntegerArith(const ast::Expr &e);

  using BinderMap = std::vector<std::pair<const Symbol *, const Symbol *>>;
  bool alphaEq(const ast::Expr &a, const ast::Expr &b, BinderMap &binders);
  bool typeEqualImpl(const Type &a, const Type &b, BinderMap &binders);

  const Symbol *freshIntervalVar(const std::string &hint);

  int fuel_ = 0;
  std::deque<Symbol> fresh_;
};

} // namespace cruft::sema
