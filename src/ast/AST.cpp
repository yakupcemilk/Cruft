// AST.cpp - cloning and pretty printing of AST nodes.
#include "ast/AST.h"

#include <sstream>

namespace cruft::ast {

// ---------------------------------------------------------------------------
// Operator spellings
// ---------------------------------------------------------------------------

const char *binaryOpSpelling(BinaryOp op) {
  switch (op) {
  case BinaryOp::Add: return "+";
  case BinaryOp::Sub: return "-";
  case BinaryOp::Mul: return "*";
  case BinaryOp::Div: return "/";
  case BinaryOp::Rem: return "%";
  case BinaryOp::Eq: return "==";
  case BinaryOp::Ne: return "!=";
  case BinaryOp::Lt: return "<";
  case BinaryOp::Le: return "<=";
  case BinaryOp::Gt: return ">";
  case BinaryOp::Ge: return ">=";
  case BinaryOp::And: return "and";
  case BinaryOp::Or: return "or";
  case BinaryOp::Meet: return "/\\";
  case BinaryOp::Join: return "\\/";
  }
  return "?";
}

const char *unaryOpSpelling(UnaryOp op) {
  switch (op) {
  case UnaryOp::Neg: return "-";
  case UnaryOp::Not: return "not ";
  case UnaryOp::IntervalRev: return "~";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// Cloning
// ---------------------------------------------------------------------------

TypeExprPtr TypeExpr::clone() const {
  auto t = std::make_unique<TypeExpr>(kind, loc);
  t->package = package;
  t->name = name;
  if (elem)
    t->elem = elem->clone();
  if (lhs)
    t->lhs = lhs->clone();
  if (rhs)
    t->rhs = rhs->clone();
  for (const auto &p : params)
    t->params.push_back(p->clone());
  if (ret)
    t->ret = ret->clone();
  return t;
}

namespace {
template <class T> std::unique_ptr<T> withType(std::unique_ptr<T> n, const Expr &src) {
  n->type = src.type;
  return n;
}
} // namespace

ExprPtr IntLitExpr::clone() const { return withType(std::make_unique<IntLitExpr>(loc, value), *this); }
ExprPtr FloatLitExpr::clone() const { return withType(std::make_unique<FloatLitExpr>(loc, value), *this); }
ExprPtr BoolLitExpr::clone() const { return withType(std::make_unique<BoolLitExpr>(loc, value), *this); }

ExprPtr IdentExpr::clone() const {
  auto n = std::make_unique<IdentExpr>(loc, package, name);
  n->symbol = symbol;
  return withType(std::move(n), *this);
}

ExprPtr BinaryExpr::clone() const {
  return withType(std::make_unique<BinaryExpr>(loc, op, lhs->clone(), rhs->clone()), *this);
}

ExprPtr UnaryExpr::clone() const {
  return withType(std::make_unique<UnaryExpr>(loc, op, operand->clone()), *this);
}

ExprPtr CallExpr::clone() const {
  std::vector<ExprPtr> a;
  for (const auto &x : args)
    a.push_back(x->clone());
  auto n = std::make_unique<CallExpr>(loc, callee->clone(), std::move(a));
  n->builtin = builtin;
  n->fn = fn;
  n->transportDir = transportDir;
  n->transportFn = transportFn;
  return withType(std::move(n), *this);
}

ExprPtr TypeLitExpr::clone() const {
  auto n = std::make_unique<TypeLitExpr>(loc, typeExpr->clone());
  n->denoted = denoted;
  return withType(std::move(n), *this);
}

ExprPtr IntervalLitExpr::clone() const {
  return withType(std::make_unique<IntervalLitExpr>(loc, one), *this);
}

ExprPtr PathAbsExpr::clone() const {
  auto n = std::make_unique<PathAbsExpr>(loc, var, body->clone());
  n->binder = binder;
  return withType(std::move(n), *this);
}

ExprPtr PathAppExpr::clone() const {
  return withType(std::make_unique<PathAppExpr>(loc, path->clone(), arg->clone()), *this);
}

ExprPtr HcompExpr::clone() const {
  std::vector<Face> fs;
  for (const auto &f : faces) {
    Face c;
    c.constraints = f.constraints;
    c.side = f.side->clone();
    c.loc = f.loc;
    fs.push_back(std::move(c));
  }
  return withType(std::make_unique<HcompExpr>(loc, typeExpr->clone(), base->clone(), std::move(fs)),
                  *this);
}

ExprPtr StringLitExpr::clone() const {
  return withType(std::make_unique<StringLitExpr>(loc, value), *this);
}

ExprPtr ArrayLitExpr::clone() const {
  std::vector<ExprPtr> es;
  for (const auto &x : elems)
    es.push_back(x->clone());
  auto n = std::make_unique<ArrayLitExpr>(loc, std::move(es));
  n->frameOffset = frameOffset;
  return withType(std::move(n), *this);
}

ExprPtr LambdaExpr::clone() const {
  std::vector<Param> ps;
  for (const auto &p : params)
    ps.push_back(p.clone());
  auto n = std::make_unique<LambdaExpr>(loc, std::move(ps), body->clone());
  n->paramSymbols = paramSymbols;
  return withType(std::move(n), *this);
}

ExprPtr MethodCallExpr::clone() const {
  std::vector<ExprPtr> a;
  for (const auto &x : args)
    a.push_back(x->clone());
  auto n = std::make_unique<MethodCallExpr>(loc, receiver->clone(), method, std::move(a), invoked);
  return withType(std::move(n), *this);
}

ExprPtr MatchExpr::clone() const {
  std::vector<MatchCase> cs;
  for (const auto &c : cases) {
    MatchCase m;
    m.ctor = c.ctor;
    m.binders = c.binders;
    m.body = c.body->clone();
    m.loc = c.loc;
    m.ctorSymbol = c.ctorSymbol;
    m.binderSymbols = c.binderSymbols;
    cs.push_back(std::move(m));
  }
  return withType(std::make_unique<MatchExpr>(loc, scrutinee->clone(), std::move(cs)), *this);
}

// ---------------------------------------------------------------------------
// Printing
// ---------------------------------------------------------------------------

namespace {

void print(std::ostream &os, const TypeExpr &t);

void print(std::ostream &os, const Expr &e) {
  switch (e.kind) {
  case ExprKind::IntLit:
    os << static_cast<const IntLitExpr &>(e).value;
    break;
  case ExprKind::FloatLit: {
    std::ostringstream tmp;
    tmp.precision(17);
    tmp << static_cast<const FloatLitExpr &>(e).value;
    os << tmp.str();
    break;
  }
  case ExprKind::BoolLit:
    os << (static_cast<const BoolLitExpr &>(e).value ? "true" : "false");
    break;
  case ExprKind::Ident: {
    const auto &id = static_cast<const IdentExpr &>(e);
    if (!id.package.empty())
      os << id.package << '.';
    os << id.name;
    break;
  }
  case ExprKind::Binary: {
    const auto &b = static_cast<const BinaryExpr &>(e);
    os << '(';
    print(os, *b.lhs);
    os << ' ' << binaryOpSpelling(b.op) << ' ';
    print(os, *b.rhs);
    os << ')';
    break;
  }
  case ExprKind::Unary: {
    const auto &u = static_cast<const UnaryExpr &>(e);
    os << '(' << unaryOpSpelling(u.op);
    print(os, *u.operand);
    os << ')';
    break;
  }
  case ExprKind::Call: {
    const auto &c = static_cast<const CallExpr &>(e);
    print(os, *c.callee);
    os << '(';
    for (size_t i = 0; i < c.args.size(); ++i) {
      if (i)
        os << ", ";
      print(os, *c.args[i]);
    }
    os << ')';
    break;
  }
  case ExprKind::TypeLit:
    print(os, *static_cast<const TypeLitExpr &>(e).typeExpr);
    break;
  case ExprKind::IntervalLit:
    os << (static_cast<const IntervalLitExpr &>(e).one ? "i1" : "i0");
    break;
  case ExprKind::PathAbs: {
    const auto &p = static_cast<const PathAbsExpr &>(e);
    os << '<' << p.var << "> ";
    print(os, *p.body);
    break;
  }
  case ExprKind::PathApp: {
    const auto &p = static_cast<const PathAppExpr &>(e);
    os << '(';
    print(os, *p.path);
    os << " @ ";
    print(os, *p.arg);
    os << ')';
    break;
  }
  case ExprKind::Hcomp: {
    const auto &h = static_cast<const HcompExpr &>(e);
    os << "hcomp(";
    print(os, *h.typeExpr);
    os << ", ";
    print(os, *h.base);
    os << ", [";
    for (size_t i = 0; i < h.faces.size(); ++i) {
      if (i)
        os << ", ";
      const Face &f = h.faces[i];
      for (size_t j = 0; j < f.constraints.size(); ++j) {
        if (j)
          os << " /\\ ";
        os << f.constraints[j].var << " = " << (f.constraints[j].one ? '1' : '0');
      }
      os << " -> ";
      print(os, *f.side);
    }
    os << "])";
    break;
  }
  case ExprKind::StringLit:
    os << '"' << static_cast<const StringLitExpr &>(e).value << '"';
    break;
  case ExprKind::ArrayLit: {
    const auto &a = static_cast<const ArrayLitExpr &>(e);
    os << '[';
    for (size_t i = 0; i < a.elems.size(); ++i) {
      if (i)
        os << ", ";
      print(os, *a.elems[i]);
    }
    os << ']';
    break;
  }
  case ExprKind::Lambda: {
    const auto &l = static_cast<const LambdaExpr &>(e);
    os << "fn(";
    for (size_t i = 0; i < l.params.size(); ++i) {
      if (i)
        os << ", ";
      os << l.params[i].name;
    }
    os << ") => ";
    print(os, *l.body);
    break;
  }
  case ExprKind::Method: {
    const auto &m = static_cast<const MethodCallExpr &>(e);
    print(os, *m.receiver);
    os << '.' << m.method;
    if (m.invoked) {
      os << '(';
      for (size_t i = 0; i < m.args.size(); ++i) {
        if (i)
          os << ", ";
        print(os, *m.args[i]);
      }
      os << ')';
    }
    break;
  }
  case ExprKind::Match: {
    const auto &m = static_cast<const MatchExpr &>(e);
    os << "match ";
    print(os, *m.scrutinee);
    for (const auto &c : m.cases) {
      os << " case " << c.ctor;
      if (!c.binders.empty()) {
        os << '(';
        for (size_t i = 0; i < c.binders.size(); ++i) {
          if (i)
            os << ", ";
          os << c.binders[i];
        }
        os << ')';
      }
      os << " then ";
      print(os, *c.body);
    }
    os << " end";
    break;
  }
  }
}

void print(std::ostream &os, const TypeExpr &t) {
  switch (t.kind) {
  case TypeExprKind::Named:
    if (!t.package.empty())
      os << t.package << '.';
    os << t.name;
    if (!t.params.empty()) {
      os << '(';
      for (size_t i = 0; i < t.params.size(); ++i) {
        if (i)
          os << ", ";
        print(os, *t.params[i]);
      }
      os << ')';
    }
    break;
  case TypeExprKind::Path:
    if (t.elem) {
      os << "Path(";
      print(os, *t.elem);
      os << ", ";
      print(os, *t.lhs);
      os << ", ";
      print(os, *t.rhs);
      os << ')';
    } else {
      os << '(';
      print(os, *t.lhs);
      os << " = ";
      print(os, *t.rhs);
      os << ')';
    }
    break;
  case TypeExprKind::Equiv:
    os << "Equiv(";
    print(os, *t.params[0]);
    os << ", ";
    print(os, *t.params[1]);
    os << ')';
    break;
  case TypeExprKind::Fn:
    os << "fn(";
    for (size_t i = 0; i < t.params.size(); ++i) {
      if (i)
        os << ", ";
      print(os, *t.params[i]);
    }
    os << ')';
    if (t.ret) {
      os << ": ";
      print(os, *t.ret);
    }
    break;
  }
}

} // namespace

std::string toString(const Expr &e) {
  std::ostringstream os;
  print(os, e);
  return os.str();
}

std::string toString(const TypeExpr &t) {
  std::ostringstream os;
  print(os, t);
  return os.str();
}

} // namespace cruft::ast
