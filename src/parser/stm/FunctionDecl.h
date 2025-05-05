#pragma once

#include "Statement.h"
#include <string>
#include <vector>
#include <memory>

class FunctionDecl : public Statement {
public:
    std::string name;
    std::vector<std::string> parameters;
    std::vector<std::shared_ptr<Statement>> body;

    FunctionDecl(const std::string& funcName,
                 std::vector<std::string> params,
                 std::vector<std::shared_ptr<Statement>> bodyStmts)
        : name(funcName), parameters(params), body(bodyStmts) {}
};
