#pragma once

#include <vector>
#include <memory>
#include "stm/Statement.h"
#include <string> 

namespace cruft {
    struct Program {
        std::string packageName;
        std::vector<std::shared_ptr<Statement>> statements;
    };
}