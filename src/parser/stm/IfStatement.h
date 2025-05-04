#pragma once
#include "Statement.h"
#include "Expr.h"
#include <memory>

class IfStatement : public Statement {
public:
    explicit IfStatement(std::shared_ptr<Expr> condition)
        : condition(std::move(condition)) {}

    int getId() const override { 
        return 1; 
    } //FIXME bi ara inşallah buraya id vereceğim ya da sabit bir değer döndüreceğim

private:
    std::shared_ptr<Expr> condition;
};
