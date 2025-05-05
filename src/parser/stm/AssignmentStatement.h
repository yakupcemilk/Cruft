#pragma once

#include "Statement.h"
#include "../stm/Expr.h"
#include <string>
#include <memory>

class AssignmentStatement : public Statement {
public:
    std::string name;
    std::shared_ptr<Expr> value;

    AssignmentStatement(const std::string& varName, std::shared_ptr<Expr> val)
        : name(varName), value(val) {}
};
