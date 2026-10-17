// ProofErasure.h - middle-end pass that deletes cubical proof content.
//
// Runs after type checking and before LLVM lowering. Identity transport
// becomes the transported term, homogeneous composition over data becomes
// its base, path-constructor match arms are dropped, and bindings whose
// type is I, Path, Equiv, or Type are removed. What remains has no proof
// to emit, so the generated machine code does not mention those types.
#pragma once

#include "ast/AST.h"
#include "sema/Normalizer.h"

namespace cruft::mid {

class ProofErasure {
public:
  static void run(ast::Program &program);

private:
  static void eraseBlock(ast::Block &block, sema::Normalizer &norm);
  static void eraseExpr(ast::ExprPtr &expr, sema::Normalizer &norm);
  static void substBlock(ast::Block &block, const sema::Symbol *var, const ast::Expr &with,
                         sema::Normalizer &norm);
  static void substExpr(ast::ExprPtr &expr, const sema::Symbol *var, const ast::Expr &with,
                        sema::Normalizer &norm);
};

} // namespace cruft::mid
