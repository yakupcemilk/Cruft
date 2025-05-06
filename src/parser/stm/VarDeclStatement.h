#pragma once

#include "Statement.h"
#include "../stm/Expr.h"
#include <string>
#include <memory>

namespace cruft {
    class VarDeclStatement : public Statement {
        public:
            std::string name;
            std::shared_ptr<Expr> initializer;
        
            VarDeclStatement(const std::string& varName, std::shared_ptr<Expr> init)
                : name(varName), initializer(init) {}
        };
}
