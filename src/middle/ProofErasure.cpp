#include "middle/ProofErasure.h"

#include "sema/Types.h"

namespace cruft::mid {

using namespace ast;

namespace {

bool erasedType(const Expr &e) { return e.type && e.type->isErased(); }

} // namespace

void ProofErasure::substExpr(ExprPtr &expr, const sema::Symbol *var, const Expr &with,
                             sema::Normalizer &norm) {
  if (!expr || !var)
    return;
  expr = norm.substitute(*expr, var, with);
}

void ProofErasure::substBlock(Block &block, const sema::Symbol *var, const Expr &with,
                              sema::Normalizer &norm) {
  for (auto &stmt : block) {
    if (!stmt)
      continue;
    switch (stmt->kind) {
    case StmtKind::VarDecl:
      substExpr(static_cast<VarDeclStmt &>(*stmt).init, var, with, norm);
      break;
    case StmtKind::Assign:
      substExpr(static_cast<AssignStmt &>(*stmt).value, var, with, norm);
      break;
    case StmtKind::Return:
      substExpr(static_cast<ReturnStmt &>(*stmt).value, var, with, norm);
      break;
    case StmtKind::If: {
      auto &i = static_cast<IfStmt &>(*stmt);
      substExpr(i.cond, var, with, norm);
      substBlock(i.thenBody, var, with, norm);
      substBlock(i.elseBody, var, with, norm);
      break;
    }
    case StmtKind::While: {
      auto &w = static_cast<WhileStmt &>(*stmt);
      substExpr(w.cond, var, with, norm);
      substBlock(w.body, var, with, norm);
      break;
    }
    case StmtKind::Foreach:
      substBlock(static_cast<ForeachStmt &>(*stmt).body, var, with, norm);
      break;
    case StmtKind::Expr:
      substExpr(static_cast<ExprStmt &>(*stmt).expr, var, with, norm);
      break;
    case StmtKind::ScopeArena:
      substBlock(static_cast<ScopeArenaStmt &>(*stmt).body, var, with, norm);
      break;
    case StmtKind::Error:
      break;
    }
  }
}

void ProofErasure::run(Program &program) {
  sema::Normalizer norm;
  for (auto &file : program.files) {
    for (auto &fn : file->functions) {
      if (fn->isExtern)
        continue;
      if (fn->symbol && fn->symbol->type) {
        const sema::Type &ty = *fn->symbol->type;
        for (size_t i = 0; i < ty.params.size(); ++i) {
          if (!ty.params[i] || ty.params[i]->kind != sema::TypeKind::Interval)
            continue;
          if (i >= ty.paramSymbols.size() || !ty.paramSymbols[i])
            continue;
          auto face = std::make_unique<IntervalLitExpr>(fn->loc, false);
          face->type = sema::Type::interval();
          substBlock(fn->body, ty.paramSymbols[i], *face, norm);
        }
      }
      eraseBlock(fn->body, norm);
    }
    for (auto &g : file->globals)
      if (g->init)
        eraseExpr(g->init, norm);
  }
}

void ProofErasure::eraseBlock(Block &block, sema::Normalizer &norm) {
  Block kept;
  kept.reserve(block.size());
  for (auto &stmt : block) {
    if (!stmt)
      continue;
    switch (stmt->kind) {
    case StmtKind::VarDecl: {
      auto &d = static_cast<VarDeclStmt &>(*stmt);
      if (d.init)
        eraseExpr(d.init, norm);
      if (d.symbol && d.symbol->type && d.symbol->type->isErased())
        continue;
      kept.push_back(std::move(stmt));
      break;
    }
    case StmtKind::Assign: {
      auto &a = static_cast<AssignStmt &>(*stmt);
      if (a.value)
        eraseExpr(a.value, norm);
      if (a.symbol && a.symbol->type && a.symbol->type->isErased())
        continue;
      kept.push_back(std::move(stmt));
      break;
    }
    case StmtKind::Return: {
      auto &r = static_cast<ReturnStmt &>(*stmt);
      if (r.value)
        eraseExpr(r.value, norm);
      kept.push_back(std::move(stmt));
      break;
    }
    case StmtKind::If: {
      auto &i = static_cast<IfStmt &>(*stmt);
      eraseExpr(i.cond, norm);
      eraseBlock(i.thenBody, norm);
      eraseBlock(i.elseBody, norm);
      kept.push_back(std::move(stmt));
      break;
    }
    case StmtKind::While: {
      auto &w = static_cast<WhileStmt &>(*stmt);
      eraseExpr(w.cond, norm);
      eraseBlock(w.body, norm);
      kept.push_back(std::move(stmt));
      break;
    }
    case StmtKind::Foreach: {
      auto &f = static_cast<ForeachStmt &>(*stmt);
      if (f.binder) {
        auto face = std::make_unique<IntervalLitExpr>(f.loc, false);
        face->type = sema::Type::interval();
        substBlock(f.body, f.binder, *face, norm);
      }
      eraseBlock(f.body, norm);
      for (auto &inner : f.body)
        kept.push_back(std::move(inner));
      break;
    }
    case StmtKind::ScopeArena: {
      auto &a = static_cast<ScopeArenaStmt &>(*stmt);
      eraseBlock(a.body, norm);
      kept.push_back(std::move(stmt));
      break;
    }
    case StmtKind::Expr: {
      auto &e = static_cast<ExprStmt &>(*stmt);
      eraseExpr(e.expr, norm);
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

void ProofErasure::eraseExpr(ExprPtr &expr, sema::Normalizer &norm) {
  if (!expr)
    return;

  if (expr->kind == ExprKind::Call) {
    auto &call = static_cast<CallExpr &>(*expr);
    if (call.builtin == Builtin::Transport && call.transportDir == TransportDir::Identity &&
        call.args.size() >= 2) {
      eraseExpr(call.args[1], norm);
      expr = std::move(call.args[1]);
      return;
    }
    if ((call.builtin == Builtin::Reveal || call.builtin == Builtin::Seal) && !call.args.empty()) {
      eraseExpr(call.args[0], norm);
      expr = std::move(call.args[0]);
      return;
    }
    if (call.builtin == Builtin::Quot && call.quotCanon && !call.args.empty()) {
      eraseExpr(call.args[0], norm);
      auto callee = std::make_unique<IdentExpr>(call.loc, "", call.quotCanon->name);
      callee->symbol = call.quotCanon;
      callee->type = call.quotCanon->type;
      std::vector<ExprPtr> args;
      args.push_back(std::move(call.args[0]));
      auto rewritten = std::make_unique<CallExpr>(call.loc, std::move(callee), std::move(args));
      rewritten->fn = call.quotCanon;
      rewritten->type = call.quotCanon->type ? call.quotCanon->type->ret : call.type;
      expr = std::move(rewritten);
      return;
    }
    eraseExpr(call.callee, norm);
    for (auto &arg : call.args)
      eraseExpr(arg, norm);
    call.along.reset();
    return;
  }

  if (expr->kind == ExprKind::Hcomp) {
    auto &h = static_cast<HcompExpr &>(*expr);
    eraseExpr(h.base, norm);
    for (Face &face : h.faces)
      if (face.side)
        eraseExpr(face.side, norm);
    if (expr->type && !expr->type->isErased() && h.base)
      expr = std::move(h.base);
    return;
  }

  switch (expr->kind) {
  case ExprKind::Binary: {
    auto &b = static_cast<BinaryExpr &>(*expr);
    eraseExpr(b.lhs, norm);
    eraseExpr(b.rhs, norm);
    break;
  }
  case ExprKind::Unary: {
    eraseExpr(static_cast<UnaryExpr &>(*expr).operand, norm);
    break;
  }
  case ExprKind::PathAbs:
    eraseExpr(static_cast<PathAbsExpr &>(*expr).body, norm);
    break;
  case ExprKind::PathApp: {
    auto &p = static_cast<PathAppExpr &>(*expr);
    eraseExpr(p.path, norm);
    eraseExpr(p.arg, norm);
    break;
  }
  case ExprKind::ArrayLit:
    for (auto &el : static_cast<ArrayLitExpr &>(*expr).elems)
      eraseExpr(el, norm);
    break;
  case ExprKind::Lambda:
    eraseExpr(static_cast<LambdaExpr &>(*expr).body, norm);
    break;
  case ExprKind::Method: {
    auto &m = static_cast<MethodCallExpr &>(*expr);
    eraseExpr(m.receiver, norm);
    for (auto &arg : m.args)
      eraseExpr(arg, norm);
    break;
  }
  case ExprKind::Match: {
    auto &m = static_cast<MatchExpr &>(*expr);
    eraseExpr(m.scrutinee, norm);
    std::vector<MatchCase> kept;
    kept.reserve(m.cases.size());
    for (auto &c : m.cases) {
      if (c.ctorSymbol && c.ctorSymbol->isPathCtor)
        continue;
      if (c.body)
        eraseExpr(c.body, norm);
      kept.push_back(std::move(c));
    }
    m.cases = std::move(kept);
    break;
  }
  case ExprKind::TypeLit: {
    auto &t = static_cast<TypeLitExpr &>(*expr);
    if (t.typeExpr) {
      if (t.typeExpr->lhs)
        eraseExpr(t.typeExpr->lhs, norm);
      if (t.typeExpr->rhs)
        eraseExpr(t.typeExpr->rhs, norm);
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
