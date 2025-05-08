// src/codegen/CodegenC.h
#pragma once
#include "../lexer/Lexer.h"
#include "../lexer/Token.h"
#include "../parser/AST.h"
#include "../parser/Parser.h"
#include "../parser/stm/AssignmentStatement.h"
#include "../parser/stm/BinaryExpr.h"
#include "../parser/stm/CallExpr.h"
#include "../parser/stm/ConstDeclStatement.h"
#include "../parser/stm/Expr.h"
#include "../parser/stm/FunctionDecl.h"
#include "../parser/stm/IfStatement.h"
#include "../parser/stm/IntegerExpr.h"
#include "../parser/stm/PrintStatement.h"
#include "../parser/stm/ReturnStatement.h"
#include "../parser/stm/VarDeclStatement.h"
#include "../parser/stm/VariableExpr.h"
#include "../parser/stm/WhileStatement.h"
#include "ir/IR.h"
#include <functional>
#include <memory>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace cruft {
namespace codegen {
namespace cpp {

class CodeGenC {
public:
    explicit CodeGenC(std::ostream &out);
    void generate(const cruft::Program &prog);

private:
    std::ostream &out;
    int labelCount = 0;

    // pipeline stages
    void lowerToIR(const cruft::Program &prog);
    void allocateRegisters(); // no-op for C
    void peepholeOptimize();
    void emitC();

    // expression-gen helper
    std::function<std::string(std::shared_ptr<cruft::Expr>)> genExpr;

    // helpers
    std::string uniqueLabel(const std::string &base);
    void emitLine(const std::string &line);
    std::string joinArgs(const std::vector<std::string> &args);

    // IR storage
    std::vector<std::unique_ptr<::codegen::ir::IRInst>> ir;

    // map function names to their parameter lists
    std::unordered_map<std::string, std::vector<std::string>> fnParams;
};

} // namespace cpp
} // namespace codegen
} // namespace cruft
