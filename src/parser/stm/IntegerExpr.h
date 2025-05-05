#pragma once

#include "Expr.h"

class IntegerExpr : public Expr {
public:
    int value;

    IntegerExpr(int val)
        : value(val) {}
};
