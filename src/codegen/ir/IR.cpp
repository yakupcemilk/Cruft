// src/codegen/ir/IR.cpp
#include "IR.h"
#include <stdexcept>

using namespace codegen::ir;

std::string IRBinary::toString() const {
    static const char* names[] = {
        "add", "sub", "imul", "idiv", "mod",
        "eq", "ne", "lt", "le", "gt", "ge"
    };
    if (op < 0 || op > Ge) throw std::runtime_error("invalid IRBinary op");
    return dst + " = " + names[op] + " " + lhs + ", " + rhs;
}