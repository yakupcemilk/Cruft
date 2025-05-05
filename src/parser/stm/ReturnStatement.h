#pragma once

#include "Statement.h"
#include "../stm/Expr.h"
#include <memory>

class ReturnStatement : public Statement {
public:
    std::shared_ptr<Expr> value;

    ReturnStatement(std::shared_ptr<Expr> val)
        : value(val) {}
};
