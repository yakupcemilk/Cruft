#pragma once

#include "Statement.h"
#include "../stm/Expr.h"
#include <vector>
#include <memory>

class WhileStatement : public Statement {
public:
    std::shared_ptr<Expr> condition;
    std::vector<std::shared_ptr<Statement>> body;

    WhileStatement(std::shared_ptr<Expr> cond,
                   std::vector<std::shared_ptr<Statement>> bodyStmts)
        : condition(cond), body(bodyStmts) {}
};
