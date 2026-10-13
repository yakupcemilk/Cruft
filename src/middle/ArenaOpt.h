// ArenaOpt.h - fold, delete, and constant-offset `scope arena` regions.
//
// A region that allocates nothing loses its push/pop. A region nested in
// another arena shares the parent's watermark. When every bump in a region
// is a fixed-size array of a primitive, the pass assigns a byte offset and
// the code generator emits one allocation for the whole frame.
#pragma once

#include "ast/AST.h"

namespace cruft::mid {

class ArenaOpt {
public:
  static void run(ast::Program &program);
};

} // namespace cruft::mid
