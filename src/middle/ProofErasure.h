// ProofErasure.h - middle-end pass that deletes cubical proof content.
//
// Runs after type checking and before LLVM lowering. Identity transport
// becomes the transported term, homogeneous composition over data becomes
// its base, path-constructor match arms are dropped, and bindings whose
// type is I, Path, Equiv, or Type are removed. What remains has no proof
// to emit, so the generated machine code does not mention those types.
#pragma once

#include "ast/AST.h"

namespace cruft::mid {

class ProofErasure {
public:
  static void run(ast::Program &program);

private:
  static void eraseBlock(ast::Block &block);
  static void eraseExpr(ast::ExprPtr &expr);
};

} // namespace cruft::mid
