#pragma once
#include "Expr.h"
#include <memory>
#include <string>

class BinaryExpr : public Expr {
public:
    BinaryExpr(std::shared_ptr<Expr> left, std::string op, std::shared_ptr<Expr> right)
        : left(std::move(left)), op(std::move(op)), right(std::move(right)) {}

private:
    std::shared_ptr<Expr> left;
    std::string op;
    std::shared_ptr<Expr> right;
};
