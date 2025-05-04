#pragma once
#include "Statement.h"
#include "Expr.h"
#include <memory>

class WhileStatement : public Statement {
public:
    explicit WhileStatement(std::shared_ptr<Expr> condition)
        : condition(std::move(condition)) {}
    
    int getId() const override { return 2; } //FIXME bi ara veri at buraya

private:
    std::shared_ptr<Expr> condition;
};
