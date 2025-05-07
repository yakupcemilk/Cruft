// src/parser/stm/ConstDeclStatement.h
#pragma once
#include "Statement.h"
#include "../stm/IntegerExpr.h"
#include <string>
#include <memory>

namespace cruft {
class ConstDeclStatement : public Statement {
public:
    std::string name;
    std::shared_ptr<IntegerExpr> initializer;

    ConstDeclStatement(const std::string& varName,
                       std::shared_ptr<IntegerExpr> init)
      : name(varName), initializer(std::move(init)) {}
};
}
