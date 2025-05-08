#pragma once

#include "Expr.h"
#include "../../lexer/Lexer.h"
#include <memory>

namespace cruft {
    class BinaryExpr : public Expr {
        public:
            std::shared_ptr<Expr> left;
            Token op;
            std::shared_ptr<Expr> right;
        
            BinaryExpr(std::shared_ptr<Expr> lhs, Token oper, std::shared_ptr<Expr> rhs)
                : left(lhs), op(oper), right(rhs) {}
        };        
}