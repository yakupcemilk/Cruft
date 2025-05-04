#pragma once
#include "Expr.h"
#include <string>

class VariableExpr : public Expr {
public:
    explicit VariableExpr(const std::string& name) : name(name) {}

private:
    std::string name;
};
