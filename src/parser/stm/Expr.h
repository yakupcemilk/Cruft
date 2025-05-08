// src/parser/stm/Expr.h
#pragma once
#include "../../runtime/rc.h"

namespace cruft {

class Expr : public RCObject {
public:
    virtual ~Expr() = default;
};

} // namespace cruft