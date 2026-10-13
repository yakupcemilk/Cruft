#include "middle/ArenaOpt.h"

#include "sema/Types.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace cruft::mid {

using namespace ast;

namespace {

struct Summary {
  bool anyAlloc = false;
  bool dynamic = false;
  std::vector<ArrayLitExpr *> arrays;
};

std::optional<std::uint64_t> primitiveSize(sema::TypeKind kind) {
  switch (kind) {
  case sema::TypeKind::I32:
  case sema::TypeKind::F32:
    return 4;
  case sema::TypeKind::I64:
  case sema::TypeKind::F64:
    return 8;
  default:
    return std::nullopt;
  }
}

std::uint64_t alignUp(std::uint64_t value, std::uint64_t align) {
  if (align <= 1)
    return value;
  return (value + align - 1) & ~(align - 1);
}

void absorb(Summary &dst, Summary &src) {
  dst.anyAlloc = dst.anyAlloc || src.anyAlloc;
  dst.dynamic = dst.dynamic || src.dynamic;
  dst.arrays.insert(dst.arrays.end(), src.arrays.begin(), src.arrays.end());
}

void clearOffsets(Summary &summary) {
  for (ArrayLitExpr *array : summary.arrays)
    array->frameOffset = -1;
}

void packFrame(ScopeArenaStmt &arena, Summary &summary) {
  std::uint64_t cursor = 0;
  std::uint64_t maxAlign = 1;
  for (ArrayLitExpr *array : summary.arrays) {
    if (!array->type || !array->type->elem)
      continue;
    std::optional<std::uint64_t> size = primitiveSize(array->type->elem->kind);
    if (!size)
      continue;
    std::uint64_t align = *size;
    cursor = alignUp(cursor, align);
    array->frameOffset = static_cast<int>(cursor);
    cursor += *size * array->elems.size();
    if (align > maxAlign)
      maxAlign = align;
  }
  arena.framed = cursor > 0;
  arena.frameBytes = alignUp(cursor, maxAlign);
  arena.frameAlign = static_cast<unsigned>(maxAlign);
  arena.elide = false;
  if (!arena.framed)
    clearOffsets(summary);
}

void visitExpr(Expr &expr, Summary &summary);

void visitExpr(Expr &expr, Summary &summary) {
  switch (expr.kind) {
  case ExprKind::ArrayLit: {
    auto &array = static_cast<ArrayLitExpr &>(expr);
    for (auto &el : array.elems)
      if (el)
        visitExpr(*el, summary);
    if (!array.elems.empty() && array.type && array.type->kind == sema::TypeKind::Vector &&
        array.type->elem) {
      summary.anyAlloc = true;
      if (!primitiveSize(array.type->elem->kind))
        summary.dynamic = true;
      else
        summary.arrays.push_back(&array);
    }
    break;
  }
  case ExprKind::Binary: {
    auto &b = static_cast<BinaryExpr &>(expr);
    if (b.lhs)
      visitExpr(*b.lhs, summary);
    if (b.rhs)
      visitExpr(*b.rhs, summary);
    break;
  }
  case ExprKind::Unary:
    if (static_cast<UnaryExpr &>(expr).operand)
      visitExpr(*static_cast<UnaryExpr &>(expr).operand, summary);
    break;
  case ExprKind::Call: {
    auto &c = static_cast<CallExpr &>(expr);
    if (c.callee)
      visitExpr(*c.callee, summary);
    for (auto &arg : c.args)
      if (arg)
        visitExpr(*arg, summary);
    break;
  }
  case ExprKind::Method: {
    auto &m = static_cast<MethodCallExpr &>(expr);
    if (m.receiver)
      visitExpr(*m.receiver, summary);
    if (m.method == "filter" || m.method == "map") {
      summary.anyAlloc = true;
      summary.dynamic = true;
    }
    for (auto &arg : m.args) {
      if (!arg)
        continue;
      // A lambda is its own function and does not bump this arena.
      if (arg->kind == ExprKind::Lambda)
        continue;
      visitExpr(*arg, summary);
    }
    break;
  }
  case ExprKind::Match: {
    auto &m = static_cast<MatchExpr &>(expr);
    if (m.scrutinee)
      visitExpr(*m.scrutinee, summary);
    for (auto &c : m.cases)
      if (c.body)
        visitExpr(*c.body, summary);
    break;
  }
  case ExprKind::Lambda:
    break;
  case ExprKind::PathAbs:
    if (static_cast<PathAbsExpr &>(expr).body)
      visitExpr(*static_cast<PathAbsExpr &>(expr).body, summary);
    break;
  case ExprKind::PathApp: {
    auto &p = static_cast<PathAppExpr &>(expr);
    if (p.path)
      visitExpr(*p.path, summary);
    if (p.arg)
      visitExpr(*p.arg, summary);
    break;
  }
  case ExprKind::Hcomp: {
    auto &h = static_cast<HcompExpr &>(expr);
    if (h.base)
      visitExpr(*h.base, summary);
    break;
  }
  case ExprKind::IntLit:
  case ExprKind::FloatLit:
  case ExprKind::BoolLit:
  case ExprKind::Ident:
  case ExprKind::TypeLit:
  case ExprKind::IntervalLit:
  case ExprKind::StringLit:
    break;
  }
}

void visitBlock(Block &block, Summary &summary, bool inArena);

void visitStmt(Stmt &stmt, Summary &summary, bool inArena) {
  switch (stmt.kind) {
  case StmtKind::VarDecl: {
    auto &d = static_cast<VarDeclStmt &>(stmt);
    if (d.init)
      visitExpr(*d.init, summary);
    break;
  }
  case StmtKind::Assign:
    if (static_cast<AssignStmt &>(stmt).value)
      visitExpr(*static_cast<AssignStmt &>(stmt).value, summary);
    break;
  case StmtKind::Return:
    if (static_cast<ReturnStmt &>(stmt).value)
      visitExpr(*static_cast<ReturnStmt &>(stmt).value, summary);
    break;
  case StmtKind::If: {
    auto &i = static_cast<IfStmt &>(stmt);
    if (i.cond)
      visitExpr(*i.cond, summary);
    visitBlock(i.thenBody, summary, inArena);
    visitBlock(i.elseBody, summary, inArena);
    break;
  }
  case StmtKind::While: {
    auto &w = static_cast<WhileStmt &>(stmt);
    if (w.cond)
      visitExpr(*w.cond, summary);
    visitBlock(w.body, summary, inArena);
    break;
  }
  case StmtKind::Expr:
    if (static_cast<ExprStmt &>(stmt).expr)
      visitExpr(*static_cast<ExprStmt &>(stmt).expr, summary);
    break;
  case StmtKind::Error:
    break;
  case StmtKind::ScopeArena: {
    auto &arena = static_cast<ScopeArenaStmt &>(stmt);
    Summary inner;
    visitBlock(arena.body, inner, /*inArena=*/true);
    if (inArena) {
      arena.elide = true;
      arena.folded = true;
      arena.framed = false;
      absorb(summary, inner);
      break;
    }
    if (!inner.anyAlloc) {
      arena.elide = true;
      arena.folded = false;
      arena.framed = false;
      clearOffsets(inner);
      break;
    }
    if (!inner.dynamic) {
      packFrame(arena, inner);
      break;
    }
    arena.elide = false;
    arena.folded = false;
    arena.framed = false;
    clearOffsets(inner);
    break;
  }
  }
}

void visitBlock(Block &block, Summary &summary, bool inArena) {
  for (auto &stmt : block)
    if (stmt)
      visitStmt(*stmt, summary, inArena);
}

} // namespace

void ArenaOpt::run(Program &program) {
  for (auto &file : program.files)
    for (auto &fn : file->functions) {
      if (fn->isExtern)
        continue;
      Summary ignored;
      visitBlock(fn->body, ignored, /*inArena=*/false);
    }
}

} // namespace cruft::mid
