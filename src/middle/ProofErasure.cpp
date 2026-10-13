#include "middle/ProofErasure.h"

#include "sema/Types.h"

namespace cruft::mid {

using namespace ast;

namespace {

bool erasedType(const Expr &e) { return e.type && e.type->isErased(); }

} // namespace

void ProofErasure::run(Program &program) {
  for (auto &file : program.files) {
    for (auto &fn : file->functions)
      if (!fn->isExtern)
        eraseBlock(fn->body);
    for (auto &g : file->globals)
      if (g->init)
        eraseExpr(g->init);
  }
}

void ProofErasure::eraseBlock(Block &block) {
  Block kept;
  kept.reserve(block.size());
  for (auto &stmt : block) {
    if (!stmt)
      continue;
    switch (stmt->kind) {
    case StmtKind::VarDecl: {
      auto &d = static_cast<VarDeclStmt &>(*stmt);
      if (d.init)
        eraseExpr(d.init);
      if (d.symbol && d.symbol->type && d.symbol->type->isErased())
        continue;
      kept.push_back(std::move(stmt));
      break;
    }
    case StmtKind::Assign: {
      auto &a = static_cast<AssignStmt &>(*stmt);
      if (a.value)
        eraseExpr(a.value);
      if (a.symbol && a.symbol->type && a.symbol->type->isErased())
        continue;
      kept.push_back(std::move(stmt));
      break;
    }
    case StmtKind::Return: {
      auto &r = static_cast<ReturnStmt &>(*stmt);
      if (r.value)
        eraseExpr(r.value);
      kept.push_back(std::move(stmt));
      break;
    }
    case StmtKind::If: {
      auto &i = static_cast<IfStmt &>(*stmt);
      eraseExpr(i.cond);
      eraseBlock(i.thenBody);
      eraseBlock(i.elseBody);
      kept.push_back(std::move(stmt));
      break;
    }
    case StmtKind::While: {
      auto &w = static_cast<WhileStmt &>(*stmt);
      eraseExpr(w.cond);
      eraseBlock(w.body);
      kept.push_back(std::move(stmt));
      break;
    }
    case StmtKind::ScopeArena: {
      auto &a = static_cast<ScopeArenaStmt &>(*stmt);
      eraseBlock(a.body);
      kept.push_back(std::move(stmt));
      break;
    }
    case StmtKind::Expr: {
      auto &e = static_cast<ExprStmt &>(*stmt);
      eraseExpr(e.expr);
      if (e.expr && erasedType(*e.expr) && e.expr->kind != ExprKind::Call &&
          e.expr->kind != ExprKind::Method)
        continue;
      kept.push_back(std::move(stmt));
      break;
    }
    case StmtKind::Error:
      kept.push_back(std::move(stmt));
      break;
    }
  }
  block = std::move(kept);
}

void ProofErasure::eraseExpr(ExprPtr &expr) {
  if (!expr)
    return;

  if (expr->kind == ExprKind::Call) {
    auto &call = static_cast<CallExpr &>(*expr);
    if (call.builtin == Builtin::Transport && call.transportDir == TransportDir::Identity &&
        call.args.size() >= 2) {
      eraseExpr(call.args[1]);
      expr = std::move(call.args[1]);
      return;
    }
    eraseExpr(call.callee);
    for (auto &arg : call.args)
      eraseExpr(arg);
    return;
  }

  if (expr->kind == ExprKind::Hcomp) {
    auto &h = static_cast<HcompExpr &>(*expr);
    eraseExpr(h.base);
    for (Face &face : h.faces)
      if (face.side)
        eraseExpr(face.side);
    if (expr->type && !expr->type->isErased() && h.base)
      expr = std::move(h.base);
    return;
  }

  switch (expr->kind) {
  case ExprKind::Binary: {
    auto &b = static_cast<BinaryExpr &>(*expr);
    eraseExpr(b.lhs);
    eraseExpr(b.rhs);
    break;
  }
  case ExprKind::Unary: {
    eraseExpr(static_cast<UnaryExpr &>(*expr).operand);
    break;
  }
  case ExprKind::PathAbs:
    eraseExpr(static_cast<PathAbsExpr &>(*expr).body);
    break;
  case ExprKind::PathApp: {
    auto &p = static_cast<PathAppExpr &>(*expr);
    eraseExpr(p.path);
    eraseExpr(p.arg);
    break;
  }
  case ExprKind::ArrayLit:
    for (auto &el : static_cast<ArrayLitExpr &>(*expr).elems)
      eraseExpr(el);
    break;
  case ExprKind::Lambda:
    eraseExpr(static_cast<LambdaExpr &>(*expr).body);
    break;
  case ExprKind::Method: {
    auto &m = static_cast<MethodCallExpr &>(*expr);
    eraseExpr(m.receiver);
    for (auto &arg : m.args)
      eraseExpr(arg);
    break;
  }
  case ExprKind::Match: {
    auto &m = static_cast<MatchExpr &>(*expr);
    eraseExpr(m.scrutinee);
    std::vector<MatchCase> kept;
    kept.reserve(m.cases.size());
    for (auto &c : m.cases) {
      if (c.ctorSymbol && c.ctorSymbol->isPathCtor)
        continue;
      if (c.body)
        eraseExpr(c.body);
      kept.push_back(std::move(c));
    }
    m.cases = std::move(kept);
    break;
  }
  case ExprKind::TypeLit: {
    auto &t = static_cast<TypeLitExpr &>(*expr);
    if (t.typeExpr) {
      if (t.typeExpr->lhs)
        eraseExpr(t.typeExpr->lhs);
      if (t.typeExpr->rhs)
        eraseExpr(t.typeExpr->rhs);
    }
    break;
  }
  case ExprKind::IntLit:
  case ExprKind::FloatLit:
  case ExprKind::BoolLit:
  case ExprKind::Ident:
  case ExprKind::IntervalLit:
  case ExprKind::StringLit:
  case ExprKind::Call:
  case ExprKind::Hcomp:
    break;
  }
}

} // namespace cruft::mid
