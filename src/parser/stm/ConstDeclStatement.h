#pragma once

#include "Statement.h"
#include "IntegerExpr.h"
#include <string>
#include <memory>

class ConstDeclStatement : public Statement {
public:
    std::string name;
    std::shared_ptr<IntegerExpr> initializer;

    ConstDeclStatement(const std::string& varName, std::shared_ptr<IntegerExpr> init)
        : name(varName), initializer(init) {}
};
