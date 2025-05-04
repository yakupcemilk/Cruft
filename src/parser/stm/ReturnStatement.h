#pragma once
#include "Statement.h"
#include "Expr.h"
#include <memory>

class ReturnStatement : public Statement {
    public:
        explicit ReturnStatement(std::shared_ptr<Expr> expr)
          : value(std::move(expr)) {}
    
        int getId() const override { return 3; }
    
    private:
        std::shared_ptr<Expr> value;
    };