#pragma once

#include "Statement.h"
#include "../stm/Expr.h"
#include <memory>

namespace cruft {
    class PrintStatement : public Statement {
    public:
        std::shared_ptr<Expr> expression;
    
        PrintStatement(std::shared_ptr<Expr> expr)
            : expression(expr) {}
    };
}