// Normalizer.cpp - see Normalizer.h for the list of computation rules.
#include "sema/Normalizer.h"

#include <cmath>
#include <cstdint>
#include <map>
#include <sstream>
#include <utility>

namespace cruft::sema {

using namespace ast;

namespace {

constexpr int kFuel = 20000;

template <class T> const T &as(const Expr &e) { return static_cast<const T &>(e); }

bool isBuiltinCall(const Expr &e, Builtin b) {
  return e.kind == ExprKind::Call && as<CallExpr>(e).builtin == b;
}

bool isCtorCall(const Expr &e) {
  return e.kind == ExprKind::Call && as<CallExpr>(e).fn &&
         as<CallExpr>(e).fn->kind == SymbolKind::Ctor;
}

/// Is `sym` a function whose body is a single `ret <expr>`?
const Expr *transparentBody(const Symbol *sym) {
  if (!sym || sym->kind != SymbolKind::Function || sym->isExtern || !sym->fnDecl)
    return nullptr;
  const FnDecl &fn = *sym->fnDecl;
  if (fn.body.size() != 1 || fn.body[0]->kind != StmtKind::Return)
    return nullptr;
  return static_cast<const ReturnStmt &>(*fn.body[0]).value.get();
}

bool isIntervalConst(const Expr &e, bool one) {
  return e.kind == ExprKind::IntervalLit && as<IntervalLitExpr>(e).one == one;
}

ExprPtr mkInt(SourceLocation loc, std::int64_t v, TypeRef type) {
  auto n = std::make_unique<IntLitExpr>(std::move(loc), v);
  n->type = std::move(type);
  return n;
}

ExprPtr mkFloat(SourceLocation loc, double v, TypeRef type) {
  auto n = std::make_unique<FloatLitExpr>(std::move(loc), v);
  n->type = std::move(type);
  return n;
}

ExprPtr mkBool(SourceLocation loc, bool v) {
  auto n = std::make_unique<BoolLitExpr>(std::move(loc), v);
  n->type = Type::boolean();
  return n;
}

ExprPtr mkInterval(SourceLocation loc, bool one) {
  auto n = std::make_unique<IntervalLitExpr>(std::move(loc), one);
  n->type = Type::interval();
  return n;
}

ExprPtr mkBinary(const Expr &orig, BinaryOp op, ExprPtr l, ExprPtr r) {
  auto n = std::make_unique<BinaryExpr>(orig.loc, op, std::move(l), std::move(r));
  n->type = orig.type;
  return n;
}

ExprPtr mkUnary(const Expr &orig, UnaryOp op, ExprPtr x) {
  auto n = std::make_unique<UnaryExpr>(orig.loc, op, std::move(x));
  n->type = orig.type;
  return n;
}

std::int64_t wrapInt(std::int64_t v, const Type *t) {
  if (t && t->kind == TypeKind::I32)
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::uint64_t>(v)));
  return v;
}

/// Linear polynomial over integer atoms.
struct Poly {
  std::uint64_t constant = 0; // modular arithmetic, reinterpreted at the end
  std::map<std::string, std::pair<std::uint64_t, ExprPtr>> terms;

  Poly() = default;
  Poly(Poly &&) = default;
  Poly &operator=(Poly &&) = default;
  Poly(const Poly &) = delete;
  Poly &operator=(const Poly &) = delete;

  void addTerm(ExprPtr atom, std::uint64_t coeff) {
    std::string k = Normalizer::key(*atom);
    auto it = terms.find(k);
    if (it == terms.end())
      terms.emplace(std::move(k), std::make_pair(coeff, std::move(atom)));
    else
      it->second.first += coeff;
  }
  void add(const Poly &o) {
    constant += o.constant;
    for (const auto &[k, t] : o.terms) {
      auto it = terms.find(k);
      if (it == terms.end())
        terms.emplace(k, std::make_pair(t.first, t.second->clone()));
      else
        it->second.first += t.first;
    }
  }
  void scale(std::uint64_t s) {
    constant *= s;
    for (auto &[k, t] : terms)
      t.first *= s;
  }
  Poly negated() const {
    Poly p;
    p.constant = 0 - constant;
    for (const auto &[k, t] : terms)
      p.terms.emplace(k, std::make_pair(0 - t.first, t.second->clone()));
    return p;
  }
  [[nodiscard]] bool isConstant() const { return terms.empty(); }
};

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

ExprPtr Normalizer::normalize(const Expr &e) {
  fuel_ = kFuel;
  return norm(e);
}

bool Normalizer::equal(const Expr &a, const Expr &b) {
  ExprPtr na = normalize(a);
  ExprPtr nb = normalize(b);
  BinderMap binders;
  return alphaEq(*na, *nb, binders);
}

bool Normalizer::typeEqual(const Type &a, const Type &b) {
  BinderMap binders;
  return typeEqualImpl(a, b, binders);
}

ExprPtr Normalizer::identOf(const Symbol *sym, SourceLocation loc) {
  auto id = std::make_unique<IdentExpr>(std::move(loc), "", sym->name);
  id->symbol = sym;
  id->type = sym->type;
  return id;
}

const Symbol *Normalizer::freshIntervalVar(const std::string &hint) {
  Symbol s;
  s.kind = SymbolKind::IntervalVar;
  s.name = hint + "'" + std::to_string(fresh_.size());
  s.type = Type::interval();
  fresh_.push_back(std::move(s));
  return &fresh_.back();
}

// ---------------------------------------------------------------------------
// Structural helpers
// ---------------------------------------------------------------------------

std::string Normalizer::key(const Expr &e) {
  std::ostringstream os;
  switch (e.kind) {
  case ExprKind::Ident: {
    const auto &id = as<IdentExpr>(e);
    os << "id:" << id.name << '#' << static_cast<const void *>(id.symbol);
    break;
  }
  case ExprKind::Binary: {
    const auto &b = as<BinaryExpr>(e);
    os << '(' << key(*b.lhs) << ' ' << binaryOpSpelling(b.op) << ' ' << key(*b.rhs) << ')';
    break;
  }
  case ExprKind::Unary: {
    const auto &u = as<UnaryExpr>(e);
    os << '(' << unaryOpSpelling(u.op) << key(*u.operand) << ')';
    break;
  }
  case ExprKind::Call: {
    const auto &c = as<CallExpr>(e);
    os << key(*c.callee) << '(';
    for (size_t i = 0; i < c.args.size(); ++i)
      os << (i ? "," : "") << key(*c.args[i]);
    os << ')';
    break;
  }
  case ExprKind::PathAbs: {
    const auto &p = as<PathAbsExpr>(e);
    os << "<" << p.var << "> " << key(*p.body);
    break;
  }
  case ExprKind::PathApp: {
    const auto &p = as<PathAppExpr>(e);
    os << '(' << key(*p.path) << " @ " << key(*p.arg) << ')';
    break;
  }
  case ExprKind::Match: {
    const auto &m = as<MatchExpr>(e);
    os << "match " << key(*m.scrutinee);
    for (const auto &c : m.cases)
      os << " case " << c.ctor << " then " << key(*c.body);
    break;
  }
  case ExprKind::StringLit:
  case ExprKind::ArrayLit:
  case ExprKind::Lambda:
  case ExprKind::Method:
  default:
    os << toString(e);
    break;
  }
  return os.str();
}

bool Normalizer::mentions(const Expr &e, const Symbol *var) {
  switch (e.kind) {
  case ExprKind::Ident:
    return as<IdentExpr>(e).symbol == var;
  case ExprKind::Binary:
    return mentions(*as<BinaryExpr>(e).lhs, var) || mentions(*as<BinaryExpr>(e).rhs, var);
  case ExprKind::Unary:
    return mentions(*as<UnaryExpr>(e).operand, var);
  case ExprKind::Call: {
    const auto &c = as<CallExpr>(e);
    if (mentions(*c.callee, var))
      return true;
    for (const auto &a : c.args)
      if (mentions(*a, var))
        return true;
    return false;
  }
  case ExprKind::PathAbs:
    return mentions(*as<PathAbsExpr>(e).body, var);
  case ExprKind::PathApp:
    return mentions(*as<PathAppExpr>(e).path, var) || mentions(*as<PathAppExpr>(e).arg, var);
  case ExprKind::Hcomp: {
    const auto &h = as<HcompExpr>(e);
    if (mentions(*h.base, var))
      return true;
    for (const auto &f : h.faces)
      if (mentions(*f.side, var))
        return true;
    return false;
  }
  case ExprKind::Match: {
    const auto &m = as<MatchExpr>(e);
    if (mentions(*m.scrutinee, var))
      return true;
    for (const auto &c : m.cases)
      if (mentions(*c.body, var))
        return true;
    return false;
  }
  case ExprKind::TypeLit: {
    const auto &t = as<TypeLitExpr>(e);
    if (!t.denoted)
      return false;
    // Only path types can mention terms.
    const Type *ty = t.denoted.get();
    if (ty->kind == TypeKind::Path)
      return mentions(*ty->lhs, var) || mentions(*ty->rhs, var);
    return false;
  }
  case ExprKind::StringLit:
    return false;
  case ExprKind::ArrayLit:
    for (const auto &x : as<ArrayLitExpr>(e).elems)
      if (mentions(*x, var))
        return true;
    return false;
  case ExprKind::Lambda:
    return mentions(*as<LambdaExpr>(e).body, var);
  case ExprKind::Method: {
    const auto &m = as<MethodCallExpr>(e);
    if (mentions(*m.receiver, var))
      return true;
    for (const auto &a : m.args)
      if (mentions(*a, var))
        return true;
    return false;
  }
  }
  return false;
}

bool Normalizer::mentionsMutable(const Expr &e) {
  switch (e.kind) {
  case ExprKind::Ident: {
    const Symbol *s = as<IdentExpr>(e).symbol;
    return s && (s->kind == SymbolKind::Global || s->kind == SymbolKind::Local) && !s->isConst;
  }
  case ExprKind::Binary:
    return mentionsMutable(*as<BinaryExpr>(e).lhs) || mentionsMutable(*as<BinaryExpr>(e).rhs);
  case ExprKind::Unary:
    return mentionsMutable(*as<UnaryExpr>(e).operand);
  case ExprKind::Call: {
    const auto &c = as<CallExpr>(e);
    for (const auto &a : c.args)
      if (mentionsMutable(*a))
        return true;
    return mentionsMutable(*c.callee);
  }
  case ExprKind::PathAbs:
    return mentionsMutable(*as<PathAbsExpr>(e).body);
  case ExprKind::PathApp:
    return mentionsMutable(*as<PathAppExpr>(e).path) || mentionsMutable(*as<PathAppExpr>(e).arg);
  case ExprKind::Hcomp: {
    const auto &h = as<HcompExpr>(e);
    if (mentionsMutable(*h.base))
      return true;
    for (const auto &f : h.faces)
      if (mentionsMutable(*f.side))
        return true;
    return false;
  }
  case ExprKind::Match: {
    const auto &m = as<MatchExpr>(e);
    if (mentionsMutable(*m.scrutinee))
      return true;
    for (const auto &c : m.cases)
      if (mentionsMutable(*c.body))
        return true;
    return false;
  }
  case ExprKind::StringLit:
    return false;
  case ExprKind::ArrayLit:
    for (const auto &x : as<ArrayLitExpr>(e).elems)
      if (mentionsMutable(*x))
        return true;
    return false;
  case ExprKind::Lambda:
    return mentionsMutable(*as<LambdaExpr>(e).body);
  case ExprKind::Method: {
    const auto &m = as<MethodCallExpr>(e);
    if (mentionsMutable(*m.receiver))
      return true;
    for (const auto &a : m.args)
      if (mentionsMutable(*a))
        return true;
    return false;
  }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Substitution
// ---------------------------------------------------------------------------

ExprPtr Normalizer::substitute(const Expr &e, const Symbol *var, const Expr &with) {
  switch (e.kind) {
  case ExprKind::Ident:
    if (as<IdentExpr>(e).symbol == var) {
      ExprPtr r = with.clone();
      r->loc = e.loc;
      return r;
    }
    return e.clone();
  case ExprKind::Binary: {
    const auto &b = as<BinaryExpr>(e);
    return mkBinary(e, b.op, substitute(*b.lhs, var, with), substitute(*b.rhs, var, with));
  }
  case ExprKind::Unary: {
    const auto &u = as<UnaryExpr>(e);
    return mkUnary(e, u.op, substitute(*u.operand, var, with));
  }
  case ExprKind::Call: {
    const auto &c = as<CallExpr>(e);
    std::vector<ExprPtr> args;
    for (const auto &a : c.args)
      args.push_back(substitute(*a, var, with));
    auto n = std::make_unique<CallExpr>(e.loc, substitute(*c.callee, var, with), std::move(args));
    n->builtin = c.builtin;
    n->fn = c.fn;
    n->transportDir = c.transportDir;
    n->transportFn = c.transportFn;
    n->type = substituteType(e.type, var, with);
    return n;
  }
  case ExprKind::PathAbs: {
    const auto &p = as<PathAbsExpr>(e);
    if (p.binder == var)
      return e.clone(); // shadowed
    auto n = std::make_unique<PathAbsExpr>(e.loc, p.var, substitute(*p.body, var, with));
    n->binder = p.binder;
    n->type = substituteType(e.type, var, with);
    return n;
  }
  case ExprKind::PathApp: {
    const auto &p = as<PathAppExpr>(e);
    auto n = std::make_unique<PathAppExpr>(e.loc, substitute(*p.path, var, with),
                                           substitute(*p.arg, var, with));
    n->type = substituteType(e.type, var, with);
    return n;
  }
  case ExprKind::Hcomp: {
    const auto &h = as<HcompExpr>(e);
    std::vector<Face> faces;
    for (const auto &f : h.faces) {
      Face c;
      c.constraints = f.constraints;
      c.loc = f.loc;
      c.side = substitute(*f.side, var, with);
      faces.push_back(std::move(c));
    }
    auto n = std::make_unique<HcompExpr>(e.loc, h.typeExpr->clone(), substitute(*h.base, var, with),
                                         std::move(faces));
    n->type = substituteType(e.type, var, with);
    return n;
  }
  case ExprKind::Match: {
    const auto &m = as<MatchExpr>(e);
    std::vector<MatchCase> cases;
    for (const auto &c : m.cases) {
      MatchCase mc;
      mc.ctor = c.ctor;
      mc.binders = c.binders;
      mc.loc = c.loc;
      mc.ctorSymbol = c.ctorSymbol;
      mc.binderSymbols = c.binderSymbols;
      mc.body = substitute(*c.body, var, with);
      cases.push_back(std::move(mc));
    }
    auto n = std::make_unique<MatchExpr>(e.loc, substitute(*m.scrutinee, var, with), std::move(cases));
    n->type = substituteType(e.type, var, with);
    return n;
  }
  case ExprKind::TypeLit: {
    const auto &t = as<TypeLitExpr>(e);
    auto n = std::make_unique<TypeLitExpr>(e.loc, t.typeExpr->clone());
    n->denoted = substituteType(t.denoted, var, with);
    n->type = e.type;
    return n;
  }
  case ExprKind::StringLit:
    return e.clone();
  case ExprKind::ArrayLit: {
    std::vector<ExprPtr> es;
    for (const auto &x : as<ArrayLitExpr>(e).elems)
      es.push_back(substitute(*x, var, with));
    auto n = std::make_unique<ArrayLitExpr>(e.loc, std::move(es));
    n->type = substituteType(e.type, var, with);
    return n;
  }
  case ExprKind::Lambda: {
    const auto &l = as<LambdaExpr>(e);
    for (const Symbol *ps : l.paramSymbols)
      if (ps == var)
        return e.clone();
    std::vector<Param> ps;
    for (const auto &p : l.params)
      ps.push_back(p.clone());
    auto n = std::make_unique<LambdaExpr>(e.loc, std::move(ps), substitute(*l.body, var, with));
    n->paramSymbols = l.paramSymbols;
    n->type = substituteType(e.type, var, with);
    return n;
  }
  case ExprKind::Method: {
    const auto &m = as<MethodCallExpr>(e);
    std::vector<ExprPtr> args;
    for (const auto &a : m.args)
      args.push_back(substitute(*a, var, with));
    auto n = std::make_unique<MethodCallExpr>(e.loc, substitute(*m.receiver, var, with), m.method,
                                              std::move(args), m.invoked);
    n->type = substituteType(e.type, var, with);
    return n;
  }
  }
  return e.clone();
}

TypePtr Normalizer::substituteType(const TypePtr &t, const Symbol *var, const Expr &with) {
  if (!t)
    return t;
  switch (t->kind) {
  case TypeKind::Path: {
    if (!mentions(*t->lhs, var) && !mentions(*t->rhs, var) &&
        substituteType(t->elem, var, with) == t->elem)
      return t;
    return Type::path(substituteType(t->elem, var, with), substitute(*t->lhs, var, with),
                      substitute(*t->rhs, var, with));
  }
  case TypeKind::Equiv: {
    TypePtr a = substituteType(t->a, var, with);
    TypePtr b = substituteType(t->b, var, with);
    if (a == t->a && b == t->b)
      return t;
    return Type::equiv(std::move(a), std::move(b));
  }
  case TypeKind::Fn: {
    bool changed = false;
    std::vector<TypePtr> params;
    for (const auto &p : t->params) {
      params.push_back(substituteType(p, var, with));
      changed |= params.back() != p;
    }
    TypePtr ret = substituteType(t->ret, var, with);
    changed |= ret != t->ret;
    if (!changed)
      return t;
    return Type::fn(std::move(params), t->paramSymbols, std::move(ret));
  }
  default:
    return t;
  }
}

// ---------------------------------------------------------------------------
// Alpha equivalence
// ---------------------------------------------------------------------------

bool Normalizer::alphaEq(const Expr &a, const Expr &b, BinderMap &binders) {
  if (a.kind != b.kind)
    return false;
  switch (a.kind) {
  case ExprKind::IntLit:
    return as<IntLitExpr>(a).value == as<IntLitExpr>(b).value;
  case ExprKind::FloatLit:
    return as<FloatLitExpr>(a).value == as<FloatLitExpr>(b).value;
  case ExprKind::BoolLit:
    return as<BoolLitExpr>(a).value == as<BoolLitExpr>(b).value;
  case ExprKind::IntervalLit:
    return as<IntervalLitExpr>(a).one == as<IntervalLitExpr>(b).one;
  case ExprKind::Ident: {
    const Symbol *sa = as<IdentExpr>(a).symbol;
    const Symbol *sb = as<IdentExpr>(b).symbol;
    for (auto it = binders.rbegin(); it != binders.rend(); ++it) {
      if (it->first == sa || it->second == sb)
        return it->first == sa && it->second == sb;
    }
    if (sa || sb)
      return sa == sb;
    return as<IdentExpr>(a).name == as<IdentExpr>(b).name;
  }
  case ExprKind::Binary: {
    const auto &x = as<BinaryExpr>(a), &y = as<BinaryExpr>(b);
    return x.op == y.op && alphaEq(*x.lhs, *y.lhs, binders) && alphaEq(*x.rhs, *y.rhs, binders);
  }
  case ExprKind::Unary: {
    const auto &x = as<UnaryExpr>(a), &y = as<UnaryExpr>(b);
    return x.op == y.op && alphaEq(*x.operand, *y.operand, binders);
  }
  case ExprKind::Call: {
    const auto &x = as<CallExpr>(a), &y = as<CallExpr>(b);
    if (x.builtin != y.builtin || x.args.size() != y.args.size())
      return false;
    if (x.builtin == Builtin::None && !alphaEq(*x.callee, *y.callee, binders))
      return false;
    for (size_t i = 0; i < x.args.size(); ++i)
      if (!alphaEq(*x.args[i], *y.args[i], binders))
        return false;
    return true;
  }
  case ExprKind::TypeLit: {
    const auto &x = as<TypeLitExpr>(a), &y = as<TypeLitExpr>(b);
    if (!x.denoted || !y.denoted)
      return toString(*x.typeExpr) == toString(*y.typeExpr);
    return typeEqualImpl(*x.denoted, *y.denoted, binders);
  }
  case ExprKind::PathAbs: {
    const auto &x = as<PathAbsExpr>(a), &y = as<PathAbsExpr>(b);
    binders.emplace_back(x.binder, y.binder);
    bool r = alphaEq(*x.body, *y.body, binders);
    binders.pop_back();
    return r;
  }
  case ExprKind::PathApp: {
    const auto &x = as<PathAppExpr>(a), &y = as<PathAppExpr>(b);
    return alphaEq(*x.path, *y.path, binders) && alphaEq(*x.arg, *y.arg, binders);
  }
  case ExprKind::Hcomp: {
    const auto &x = as<HcompExpr>(a), &y = as<HcompExpr>(b);
    if (x.faces.size() != y.faces.size() || !alphaEq(*x.base, *y.base, binders))
      return false;
    for (size_t i = 0; i < x.faces.size(); ++i) {
      if (x.faces[i].constraints.size() != y.faces[i].constraints.size())
        return false;
      for (size_t j = 0; j < x.faces[i].constraints.size(); ++j)
        if (x.faces[i].constraints[j].var != y.faces[i].constraints[j].var ||
            x.faces[i].constraints[j].one != y.faces[i].constraints[j].one)
          return false;
      if (!alphaEq(*x.faces[i].side, *y.faces[i].side, binders))
        return false;
    }
    return true;
  }
  case ExprKind::StringLit:
    return as<StringLitExpr>(a).value == as<StringLitExpr>(b).value;
  case ExprKind::ArrayLit: {
    const auto &x = as<ArrayLitExpr>(a), &y = as<ArrayLitExpr>(b);
    if (x.elems.size() != y.elems.size())
      return false;
    for (size_t i = 0; i < x.elems.size(); ++i)
      if (!alphaEq(*x.elems[i], *y.elems[i], binders))
        return false;
    return true;
  }
  case ExprKind::Lambda: {
    const auto &x = as<LambdaExpr>(a), &y = as<LambdaExpr>(b);
    if (x.paramSymbols.size() != y.paramSymbols.size())
      return false;
    for (size_t i = 0; i < x.paramSymbols.size(); ++i)
      binders.emplace_back(x.paramSymbols[i], y.paramSymbols[i]);
    bool r = alphaEq(*x.body, *y.body, binders);
    binders.resize(binders.size() - x.paramSymbols.size());
    return r;
  }
  case ExprKind::Method: {
    const auto &x = as<MethodCallExpr>(a), &y = as<MethodCallExpr>(b);
    if (x.method != y.method || x.invoked != y.invoked || x.args.size() != y.args.size())
      return false;
    if (!alphaEq(*x.receiver, *y.receiver, binders))
      return false;
    for (size_t i = 0; i < x.args.size(); ++i)
      if (!alphaEq(*x.args[i], *y.args[i], binders))
        return false;
    return true;
  }
  case ExprKind::Match: {
    const auto &x = as<MatchExpr>(a), &y = as<MatchExpr>(b);
    if (x.cases.size() != y.cases.size() || !alphaEq(*x.scrutinee, *y.scrutinee, binders))
      return false;
    for (size_t i = 0; i < x.cases.size(); ++i) {
      const MatchCase &cx = x.cases[i], &cy = y.cases[i];
      if (cx.ctorSymbol != cy.ctorSymbol || cx.binderSymbols.size() != cy.binderSymbols.size())
        return false;
      for (size_t j = 0; j < cx.binderSymbols.size(); ++j)
        binders.emplace_back(cx.binderSymbols[j], cy.binderSymbols[j]);
      bool r = alphaEq(*cx.body, *cy.body, binders);
      binders.resize(binders.size() - cx.binderSymbols.size());
      if (!r)
        return false;
    }
    return true;
  }
  }
  return false;
}

bool Normalizer::typeEqualImpl(const Type &a, const Type &b, BinderMap &binders) {
  if (&a == &b)
    return true;
  if (a.kind != b.kind)
    return false;
  switch (a.kind) {
  case TypeKind::Path: {
    if (!typeEqualImpl(*a.elem, *b.elem, binders))
      return false;
    int savedFuel = fuel_;
    ExprPtr al = normalize(*a.lhs), bl = normalize(*b.lhs);
    ExprPtr ar = normalize(*a.rhs), br = normalize(*b.rhs);
    fuel_ = savedFuel;
    return alphaEq(*al, *bl, binders) && alphaEq(*ar, *br, binders);
  }
  case TypeKind::Equiv:
    return typeEqualImpl(*a.a, *b.a, binders) && typeEqualImpl(*a.b, *b.b, binders);
  case TypeKind::Fn: {
    if (a.params.size() != b.params.size())
      return false;
    for (size_t i = 0; i < a.params.size(); ++i)
      if (!typeEqualImpl(*a.params[i], *b.params[i], binders))
        return false;
    // Dependent return types: identify b's parameters with a's.
    TypePtr bret = b.ret;
    for (size_t i = 0; i < a.params.size(); ++i) {
      if (a.paramSymbols[i] && b.paramSymbols[i] && a.paramSymbols[i] != b.paramSymbols[i]) {
        ExprPtr id = identOf(a.paramSymbols[i], a.paramSymbols[i]->loc);
        bret = substituteType(bret, b.paramSymbols[i], *id);
      }
    }
    return typeEqualImpl(*a.ret, *bret, binders);
  }
  case TypeKind::Inductive:
    return a.inductive == b.inductive;
  case TypeKind::Vector:
    return a.elem && b.elem && typeEqualImpl(*a.elem, *b.elem, binders);
  case TypeKind::Map:
    return a.a && b.a && a.b && b.b && typeEqualImpl(*a.a, *b.a, binders) &&
           typeEqualImpl(*a.b, *b.b, binders);
  case TypeKind::String:
    return true;
  case TypeKind::Slice:
    return a.elem && b.elem && typeEqualImpl(*a.elem, *b.elem, binders);
  default:
    return true; // primitives: same kind means same type
  }
}

// ---------------------------------------------------------------------------
// Normalisation
// ---------------------------------------------------------------------------

ExprPtr Normalizer::norm(const Expr &e) {
  if (fuel_-- <= 0)
    return e.clone();
  switch (e.kind) {
  case ExprKind::IntLit:
  case ExprKind::FloatLit:
  case ExprKind::BoolLit:
  case ExprKind::IntervalLit:
  case ExprKind::TypeLit:
    return e.clone();
  case ExprKind::Ident: {
    const Symbol *s = as<IdentExpr>(e).symbol;
    if (s && s->isConst && s->constInit &&
        (s->kind == SymbolKind::Global || s->kind == SymbolKind::Local) &&
        !mentionsMutable(*s->constInit))
      return norm(*s->constInit);
    return e.clone();
  }
  case ExprKind::Binary:
    return normBinary(as<BinaryExpr>(e));
  case ExprKind::Unary:
    return normUnary(as<UnaryExpr>(e));
  case ExprKind::Call:
    return normCall(as<CallExpr>(e));
  case ExprKind::PathAbs:
    return normPathAbs(as<PathAbsExpr>(e));
  case ExprKind::PathApp:
    return normPathApp(as<PathAppExpr>(e));
  case ExprKind::Hcomp:
    return normHcomp(as<HcompExpr>(e));
  case ExprKind::Match:
    return normMatch(as<MatchExpr>(e));
  case ExprKind::StringLit:
    return e.clone();
  case ExprKind::ArrayLit: {
    std::vector<ExprPtr> es;
    for (const auto &x : as<ArrayLitExpr>(e).elems)
      es.push_back(norm(*x));
    auto n = std::make_unique<ArrayLitExpr>(e.loc, std::move(es));
    n->type = e.type;
    return n;
  }
  case ExprKind::Lambda: {
    const auto &l = as<LambdaExpr>(e);
    std::vector<Param> ps;
    for (const auto &p : l.params)
      ps.push_back(p.clone());
    auto n = std::make_unique<LambdaExpr>(e.loc, std::move(ps), norm(*l.body));
    n->paramSymbols = l.paramSymbols;
    n->type = e.type;
    return n;
  }
  case ExprKind::Method: {
    const auto &m = as<MethodCallExpr>(e);
    std::vector<ExprPtr> args;
    for (const auto &a : m.args)
      args.push_back(norm(*a));
    auto n = std::make_unique<MethodCallExpr>(e.loc, norm(*m.receiver), m.method, std::move(args),
                                              m.invoked);
    n->type = e.type;
    return n;
  }
  }
  return e.clone();
}

namespace {

bool toPoly(const Expr &e, Poly &out) {
  switch (e.kind) {
  case ExprKind::IntLit:
    out.constant = static_cast<std::uint64_t>(as<IntLitExpr>(e).value);
    return true;
  case ExprKind::Unary:
    if (as<UnaryExpr>(e).op == UnaryOp::Neg) {
      Poly inner;
      toPoly(*as<UnaryExpr>(e).operand, inner);
      out = inner.negated();
      return true;
    }
    break;
  case ExprKind::Binary: {
    const auto &b = as<BinaryExpr>(e);
    if (b.op == BinaryOp::Add || b.op == BinaryOp::Sub) {
      Poly l, r;
      toPoly(*b.lhs, l);
      toPoly(*b.rhs, r);
      out = std::move(l);
      if (b.op == BinaryOp::Add)
        out.add(r);
      else
        out.add(r.negated());
      return true;
    }
    if (b.op == BinaryOp::Mul) {
      Poly l, r;
      toPoly(*b.lhs, l);
      toPoly(*b.rhs, r);
      if (l.isConstant()) {
        out = std::move(r);
        out.scale(l.constant);
        return true;
      }
      if (r.isConstant()) {
        out = std::move(l);
        out.scale(r.constant);
        return true;
      }
    }
    break;
  }
  default:
    break;
  }
  out = Poly{};
  out.addTerm(e.clone(), 1);
  return true;
}

ExprPtr fromPoly(Poly &p, const Expr &orig) {
  const Type *ty = orig.type.get();
  auto wrap = [&](std::uint64_t v) { return wrapInt(static_cast<std::int64_t>(v), ty); };

  ExprPtr acc;
  for (auto &[k, t] : p.terms) {
    std::int64_t coeff = wrap(t.first);
    if (coeff == 0)
      continue;
    ExprPtr term;
    bool negative = coeff < 0;
    std::int64_t mag = negative ? -coeff : coeff;
    if (mag == 1)
      term = t.second->clone();
    else
      term = mkBinary(orig, BinaryOp::Mul, mkInt(orig.loc, mag, orig.type), t.second->clone());
    if (!acc)
      acc = negative ? mkUnary(orig, UnaryOp::Neg, std::move(term)) : std::move(term);
    else
      acc = mkBinary(orig, negative ? BinaryOp::Sub : BinaryOp::Add, std::move(acc), std::move(term));
  }
  std::int64_t c = wrap(p.constant);
  if (!acc)
    return mkInt(orig.loc, c, orig.type);
  if (c > 0)
    return mkBinary(orig, BinaryOp::Add, std::move(acc), mkInt(orig.loc, c, orig.type));
  if (c < 0)
    return mkBinary(orig, BinaryOp::Sub, std::move(acc), mkInt(orig.loc, -c, orig.type));
  return acc;
}

} // namespace

ExprPtr Normalizer::normalizeIntegerArith(const Expr &e) {
  Poly p;
  toPoly(e, p);
  return fromPoly(p, e);
}

ExprPtr Normalizer::normUnary(const UnaryExpr &e) {
  ExprPtr x = norm(*e.operand);
  switch (e.op) {
  case UnaryOp::Neg:
    if (x->kind == ExprKind::IntLit)
      return mkInt(e.loc, wrapInt(-as<IntLitExpr>(*x).value, e.type.get()), e.type);
    if (x->kind == ExprKind::FloatLit)
      return mkFloat(e.loc, -as<FloatLitExpr>(*x).value, e.type);
    if (e.type && e.type->isInteger())
      return normalizeIntegerArith(*mkUnary(e, UnaryOp::Neg, std::move(x)));
    return mkUnary(e, UnaryOp::Neg, std::move(x));
  case UnaryOp::Not:
    if (x->kind == ExprKind::BoolLit)
      return mkBool(e.loc, !as<BoolLitExpr>(*x).value);
    if (x->kind == ExprKind::Unary && as<UnaryExpr>(*x).op == UnaryOp::Not)
      return as<UnaryExpr>(*x).operand->clone();
    return mkUnary(e, UnaryOp::Not, std::move(x));
  case UnaryOp::IntervalRev:
    if (x->kind == ExprKind::IntervalLit)
      return mkInterval(e.loc, !as<IntervalLitExpr>(*x).one);
    if (x->kind == ExprKind::Unary && as<UnaryExpr>(*x).op == UnaryOp::IntervalRev)
      return as<UnaryExpr>(*x).operand->clone();
    if (x->kind == ExprKind::Binary) {
      // De Morgan: push reversal to the atoms.
      const auto &b = as<BinaryExpr>(*x);
      if (b.op == BinaryOp::Meet || b.op == BinaryOp::Join) {
        auto rebuilt = mkBinary(e, b.op == BinaryOp::Meet ? BinaryOp::Join : BinaryOp::Meet,
                                mkUnary(e, UnaryOp::IntervalRev, b.lhs->clone()),
                                mkUnary(e, UnaryOp::IntervalRev, b.rhs->clone()));
        return norm(*rebuilt);
      }
    }
    return mkUnary(e, UnaryOp::IntervalRev, std::move(x));
  }
  return mkUnary(e, e.op, std::move(x));
}

ExprPtr Normalizer::normBinary(const BinaryExpr &e) {
  ExprPtr l = norm(*e.lhs);
  ExprPtr r = norm(*e.rhs);
  const bool bothInt = l->kind == ExprKind::IntLit && r->kind == ExprKind::IntLit;
  const bool bothFloat = l->kind == ExprKind::FloatLit && r->kind == ExprKind::FloatLit;
  const bool bothBool = l->kind == ExprKind::BoolLit && r->kind == ExprKind::BoolLit;

  auto iv = [&](const ExprPtr &x) { return as<IntLitExpr>(*x).value; };
  auto fv = [&](const ExprPtr &x) {
    double v = as<FloatLitExpr>(*x).value;
    return v;
  };
  auto roundFloat = [&](double v) {
    if (e.type && e.type->kind == TypeKind::F32)
      return static_cast<double>(static_cast<float>(v));
    return v;
  };

  switch (e.op) {
  case BinaryOp::Add:
  case BinaryOp::Sub:
  case BinaryOp::Mul:
    if (e.type && e.type->isInteger())
      return normalizeIntegerArith(*mkBinary(e, e.op, std::move(l), std::move(r)));
    if (bothFloat) {
      double a = fv(l), b = fv(r);
      double v = e.op == BinaryOp::Add ? a + b : e.op == BinaryOp::Sub ? a - b : a * b;
      return mkFloat(e.loc, roundFloat(v), e.type);
    }
    break;
  case BinaryOp::Div:
  case BinaryOp::Rem:
    if (bothInt && iv(r) != 0) {
      std::int64_t a = iv(l), b = iv(r);
      if (!(a == INT64_MIN && b == -1)) {
        std::int64_t v = e.op == BinaryOp::Div ? a / b : a % b;
        return mkInt(e.loc, wrapInt(v, e.type.get()), e.type);
      }
    }
    if (bothFloat && e.op == BinaryOp::Div)
      return mkFloat(e.loc, roundFloat(fv(l) / fv(r)), e.type);
    if (bothFloat && e.op == BinaryOp::Rem)
      return mkFloat(e.loc, roundFloat(std::fmod(fv(l), fv(r))), e.type);
    break;
  case BinaryOp::Eq:
  case BinaryOp::Ne:
  case BinaryOp::Lt:
  case BinaryOp::Le:
  case BinaryOp::Gt:
  case BinaryOp::Ge: {
    auto cmp = [&](auto a, auto b) {
      switch (e.op) {
      case BinaryOp::Eq: return a == b;
      case BinaryOp::Ne: return a != b;
      case BinaryOp::Lt: return a < b;
      case BinaryOp::Le: return a <= b;
      case BinaryOp::Gt: return a > b;
      default: return a >= b;
      }
    };
    if (bothInt)
      return mkBool(e.loc, cmp(iv(l), iv(r)));
    if (bothFloat)
      return mkBool(e.loc, cmp(fv(l), fv(r)));
    if (bothBool && (e.op == BinaryOp::Eq || e.op == BinaryOp::Ne))
      return mkBool(e.loc, cmp(as<BoolLitExpr>(*l).value, as<BoolLitExpr>(*r).value));
    break;
  }
  case BinaryOp::And:
  case BinaryOp::Or: {
    bool isAnd = e.op == BinaryOp::And;
    if (bothBool)
      return mkBool(e.loc, isAnd ? (as<BoolLitExpr>(*l).value && as<BoolLitExpr>(*r).value)
                                 : (as<BoolLitExpr>(*l).value || as<BoolLitExpr>(*r).value));
    auto unit = [&](const ExprPtr &x) {
      return x->kind == ExprKind::BoolLit && as<BoolLitExpr>(*x).value == isAnd;
    };
    auto absorb = [&](const ExprPtr &x) {
      return x->kind == ExprKind::BoolLit && as<BoolLitExpr>(*x).value != isAnd;
    };
    if (unit(l))
      return r;
    if (unit(r))
      return l;
    if (absorb(l))
      return l;
    if (absorb(r))
      return r;
    break;
  }
  case BinaryOp::Meet:
  case BinaryOp::Join: {
    bool isMeet = e.op == BinaryOp::Meet;
    // unit: i1 for /\, i0 for \/ ; absorbing: i0 for /\, i1 for \/
    if (isIntervalConst(*l, isMeet))
      return r;
    if (isIntervalConst(*r, isMeet))
      return l;
    if (isIntervalConst(*l, !isMeet))
      return l;
    if (isIntervalConst(*r, !isMeet))
      return r;
    BinderMap none;
    if (alphaEq(*l, *r, none))
      return l;
    if (key(*l) > key(*r))
      std::swap(l, r); // commutativity: canonical order
    break;
  }
  }
  return mkBinary(e, e.op, std::move(l), std::move(r));
}

ExprPtr Normalizer::normCall(const CallExpr &e) {
  std::vector<ExprPtr> args;
  for (const auto &a : e.args)
    args.push_back(norm(*a));

  auto rebuild = [&]() -> ExprPtr {
    auto n = std::make_unique<CallExpr>(e.loc, e.callee->clone(), std::move(args));
    n->builtin = e.builtin;
    n->fn = e.fn;
    n->transportDir = e.transportDir;
    n->transportFn = e.transportFn;
    n->type = e.type;
    return n;
  };

  switch (e.builtin) {
  case Builtin::Sym: {
    if (args.size() != 1)
      return rebuild();
    const Expr &p = *args[0];
    if (isBuiltinCall(p, Builtin::Refl))
      return std::move(args[0]);
    if (isBuiltinCall(p, Builtin::Sym) && as<CallExpr>(p).args.size() == 1)
      return as<CallExpr>(p).args[0]->clone();
    return rebuild();
  }
  case Builtin::Transport: {
    if (args.size() != 2)
      return rebuild();
    const Expr &p = *args[0];
    if (isBuiltinCall(p, Builtin::Refl))
      return std::move(args[1]);
    // transport(ua(iso(f, g, ..)), a) -> f(a) ; transport(sym(ua(iso(..))), a) -> g(a)
    const Expr *ua = nullptr;
    bool reversed = false;
    if (isBuiltinCall(p, Builtin::Ua)) {
      ua = &p;
    } else if (isBuiltinCall(p, Builtin::Sym) && as<CallExpr>(p).args.size() == 1 &&
               isBuiltinCall(*as<CallExpr>(p).args[0], Builtin::Ua)) {
      ua = as<CallExpr>(p).args[0].get();
      reversed = true;
    }
    if (ua && as<CallExpr>(*ua).args.size() == 1) {
      const Expr &eq = *as<CallExpr>(*ua).args[0];
      if (isBuiltinCall(eq, Builtin::Iso) && as<CallExpr>(eq).args.size() == 4) {
        const Expr &fnExpr = *as<CallExpr>(eq).args[reversed ? 1 : 0];
        std::vector<ExprPtr> callArgs;
        callArgs.push_back(std::move(args[1]));
        auto call = std::make_unique<CallExpr>(e.loc, fnExpr.clone(), std::move(callArgs));
        if (fnExpr.kind == ExprKind::Ident)
          call->fn = as<IdentExpr>(fnExpr).symbol;
        call->type = e.type;
        return norm(*call);
      }
    }
    return rebuild();
  }
  case Builtin::None: {
    const Symbol *callee =
        e.fn ? e.fn
             : (e.callee->kind == ExprKind::Ident ? as<IdentExpr>(*e.callee).symbol : nullptr);
    if (const Expr *body = transparentBody(callee)) {
      const FnDecl &fn = *callee->fnDecl;
      if (callee->type && callee->type->paramSymbols.size() == args.size() &&
          fn.params.size() == args.size()) {
        ExprPtr result = body->clone();
        for (size_t i = 0; i < args.size(); ++i)
          if (callee->type->paramSymbols[i])
            result = substitute(*result, callee->type->paramSymbols[i], *args[i]);
        return norm(*result);
      }
    }
    return rebuild();
  }
  default:
    return rebuild();
  }
}

ExprPtr Normalizer::normPathAbs(const PathAbsExpr &e) {
  ExprPtr body = norm(*e.body);
  if (!e.binder || !mentions(*body, e.binder)) {
    // Constant path: <i> a  ≡  refl(a)
    std::vector<ExprPtr> a;
    a.push_back(std::move(body));
    auto refl = std::make_unique<CallExpr>(e.loc, std::make_unique<IdentExpr>(e.loc, "", "refl"),
                                           std::move(a));
    refl->builtin = Builtin::Refl;
    refl->type = e.type;
    return refl;
  }
  // Eta: <i> p @ i  ≡  p   (when p does not mention i)
  if (body->kind == ExprKind::PathApp) {
    const auto &app = as<PathAppExpr>(*body);
    if (app.arg->kind == ExprKind::Ident && as<IdentExpr>(*app.arg).symbol == e.binder &&
        !mentions(*app.path, e.binder))
      return app.path->clone();
  }
  auto n = std::make_unique<PathAbsExpr>(e.loc, e.var, std::move(body));
  n->binder = e.binder;
  n->type = e.type;
  return n;
}

ExprPtr Normalizer::normPathApp(const PathAppExpr &e) {
  ExprPtr p = norm(*e.path);
  ExprPtr r = norm(*e.arg);

  if (p->kind == ExprKind::PathAbs) {
    const auto &abs = as<PathAbsExpr>(*p);
    if (!abs.binder)
      return abs.body->clone();
    return norm(*substitute(*abs.body, abs.binder, *r));
  }
  if (isBuiltinCall(*p, Builtin::Refl) && as<CallExpr>(*p).args.size() == 1)
    return as<CallExpr>(*p).args[0]->clone();
  if (isBuiltinCall(*p, Builtin::Sym) && as<CallExpr>(*p).args.size() == 1) {
    auto rev = std::make_unique<UnaryExpr>(e.loc, UnaryOp::IntervalRev, std::move(r));
    rev->type = Type::interval();
    auto app = std::make_unique<PathAppExpr>(e.loc, as<CallExpr>(*p).args[0]->clone(), std::move(rev));
    app->type = e.type;
    return norm(*app);
  }
  // Boundary rules: p @ i0 ≡ lhs, p @ i1 ≡ rhs  (read off the type of p).
  if (r->kind == ExprKind::IntervalLit && p->type && p->type->kind == TypeKind::Path) {
    const Expr &endpoint = as<IntervalLitExpr>(*r).one ? *p->type->rhs : *p->type->lhs;
    return norm(endpoint);
  }
  auto n = std::make_unique<PathAppExpr>(e.loc, std::move(p), std::move(r));
  n->type = e.type;
  return n;
}

ExprPtr Normalizer::normHcomp(const HcompExpr &e) {
  ExprPtr base = norm(*e.base);
  // Over a 0-truncated type every composition problem is solved by its
  // base: the primitive types and point-only inductive types are sets with
  // trivial higher structure, so hcomp computes to `base`.
  const Type *ty = e.type.get();
  bool zeroTruncated = false;
  if (ty) {
    if (ty->kind != TypeKind::Path && ty->kind != TypeKind::Inductive && !ty->isErased())
      zeroTruncated = true;
    if (ty->kind == TypeKind::Inductive && ty->inductive) {
      zeroTruncated = true;
      for (const Symbol *c : ty->inductive->ctors)
        if (c->isPathCtor)
          zeroTruncated = false;
    }
  }
  if (zeroTruncated)
    return base;
  std::vector<Face> faces;
  for (const auto &f : e.faces) {
    Face c;
    c.constraints = f.constraints;
    c.loc = f.loc;
    c.side = norm(*f.side);
    faces.push_back(std::move(c));
  }
  auto n = std::make_unique<HcompExpr>(e.loc, e.typeExpr->clone(), std::move(base), std::move(faces));
  n->type = e.type;
  return n;
}

ExprPtr Normalizer::normMatch(const MatchExpr &e) {
  ExprPtr s = norm(*e.scrutinee);
  if (isCtorCall(*s)) {
    const auto &call = as<CallExpr>(*s);
    for (const auto &c : e.cases) {
      if (c.ctorSymbol != call.fn)
        continue;
      ExprPtr body = c.body->clone();
      for (size_t i = 0; i < c.binderSymbols.size() && i < call.args.size(); ++i)
        if (c.binderSymbols[i])
          body = substitute(*body, c.binderSymbols[i], *call.args[i]);
      return norm(*body);
    }
  }
  std::vector<MatchCase> cases;
  for (const auto &c : e.cases) {
    MatchCase mc;
    mc.ctor = c.ctor;
    mc.binders = c.binders;
    mc.loc = c.loc;
    mc.ctorSymbol = c.ctorSymbol;
    mc.binderSymbols = c.binderSymbols;
    mc.body = norm(*c.body);
    cases.push_back(std::move(mc));
  }
  auto n = std::make_unique<MatchExpr>(e.loc, std::move(s), std::move(cases));
  n->type = e.type;
  return n;
}

} // namespace cruft::sema
