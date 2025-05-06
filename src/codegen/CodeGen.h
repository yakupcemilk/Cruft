// src/codegen/CodeGen.h
#pragma once

#include <iostream>
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include "ir/IR.h"

namespace cruft { struct Program; }

namespace codegen {

class CodeGen {
public:
    explicit CodeGen(std::ostream& o);
    void generate(const cruft::Program& prog);

private:
    std::ostream& out;
    int labelCount = 0;

    // pipeline stages
    void lowerToIR(const cruft::Program& prog);
    void allocateRegisters();
    void peepholeOptimize();
    void emitAsm();

    // helpers
    std::string uniqueLabel(const std::string& base);
    void emit(const std::string& line);

    // IR and reg map
    std::vector<std::unique_ptr<ir::IRInst>> ir;
    std::unordered_map<std::string, std::string> regMap;
};

} // namespace codegen