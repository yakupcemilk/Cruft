#pragma once

#include "Expr.h"

namespace cruft {
    class IntegerExpr : public Expr {
    public:
        int value;

        IntegerExpr(int val)
            : value(val) {}
    };
}

