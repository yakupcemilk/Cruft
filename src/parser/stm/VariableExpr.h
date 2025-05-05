#pragma once

#include "Expr.h"
#include <string>

class VariableExpr : public Expr {
public:
    std::string name;

    VariableExpr(const std::string& varName)
        : name(varName) {}
};
