#pragma once
#include "Statement.h"
#include "Expr.h"
#include <memory>

class PrintStatement : public Statement {
    public:
        explicit PrintStatement(std::shared_ptr<Expr> expr)
          : value(std::move(expr)) {}
    
        int getId() const override { return 4; } //FIXME veri koy bi ara
    
    private:
        std::shared_ptr<Expr> value;
    };