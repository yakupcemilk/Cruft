#pragma once

#include "Statement.h"
#include "../stm/Expr.h"
#include <memory>
#include <vector>

namespace cruft {
        class IfStatement : public Statement {
                public:
                    std::shared_ptr<Expr> condition;
                    std::vector<std::shared_ptr<Statement>> thenBranch;
                    std::vector<std::shared_ptr<Statement>> elseBranch;
                
                    IfStatement(std::shared_ptr<Expr> cond,
                                std::vector<std::shared_ptr<Statement>> thenStmts,
                                std::vector<std::shared_ptr<Statement>> elseStmts)
                        : condition(cond), thenBranch(thenStmts), elseBranch(elseStmts) {}
                };                
}
