#pragma once
#include "Expr.h"

class IntegerExpr : public Expr {
public:
    explicit IntegerExpr(int value) : value(value) {}

private:
    int value;
};
