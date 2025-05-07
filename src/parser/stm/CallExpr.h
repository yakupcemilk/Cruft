// src/parser/stm/CallExpr.h
#pragma once
#include "Expr.h"
#include <string>
#include <vector>
#include <memory>

namespace cruft {
class CallExpr : public Expr {
public:
    std::string callee;
    std::vector<std::shared_ptr<Expr>> args;

    CallExpr(const std::string& fn, std::vector<std::shared_ptr<Expr>> a)
      : callee(fn), args(std::move(a)) {}
};
}
