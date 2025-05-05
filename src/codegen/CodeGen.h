#pragma once

#include <ostream>
#include <string>
#include "../parser/AST.h"
#include "../parser/stm/Statement.h"
#include "../parser/stm/VarDeclStatement.h"
#include "../parser/stm/FunctionDecl.h"
#include "../parser/stm/IfStatement.h"
#include "../parser/stm/WhileStatement.h"
#include "../parser/stm/ReturnStatement.h"
#include "../parser/stm/PrintStatement.h"
#include "../parser/stm/BinaryExpr.h"
#include "../parser/stm/IntegerExpr.h"
#include "../parser/stm/VariableExpr.h"
#include "../parser/stm/AssignmentStatement.h"
#include "../parser/stm/ConstDeclStatement.h"

class CodeGen {
public:
    explicit CodeGen(std::ostream& output);
    void generate(const Program& prog);

private:
    std::ostream& out;
    int labelCount;

    // core emission
    void emit(const std::string& code);
    std::string uniqueLabel(const std::string& base);

    // generation routines
    void genFunction(const FunctionDecl& f);
    void genStatement(const std::shared_ptr<Statement>& s);
    void genPrint(const PrintStatement& p);
    void genReturn(const ReturnStatement& r);
    void genIf(const IfStatement& iff);
    void genWhile(const WhileStatement& wh);
    void genVarDecl(const VarDeclStatement& v);
    void genConstDecl(const ConstDeclStatement& c);
    void genAssignment(const AssignmentStatement& a);

    void genExpr(const std::shared_ptr<Expr>& e);
    void genBinary(const BinaryExpr& b);
    void genInteger(const IntegerExpr& i);
    void genVariable(const VariableExpr& v);
};