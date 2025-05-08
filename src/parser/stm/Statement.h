// src/parser/stm/Statement.h
#pragma once
#include "../../runtime/rc.h"

namespace cruft {

class Statement : public RCObject {
public:
    virtual ~Statement() = default;
};

} // namespace cruft