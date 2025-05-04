// ===== AST.h =====

#pragma once
#include <string>
#include <vector>
#include <memory>

// ===== EXPRESSIONS =====

struct Expr {
    virtual ~Expr() = default;
};

struct IntegerExpr : public Expr {
    int value;
    explicit IntegerExpr(int v) : value(v) {}
};

struct VariableExpr : public Expr {
    std::string name;
    explicit VariableExpr(const std::string& n) : name(n) {}
};

struct BinaryExpr : public Expr {
    std::shared_ptr<Expr> left;
    std::string op;
    std::shared_ptr<Expr> right;
    BinaryExpr(std::shared_ptr<Expr> l, const std::string& o, std::shared_ptr<Expr> r)
        : left(std::move(l)), op(o), right(std::move(r)) {}
};

// ===== STATEMENTS =====

struct Statement {
    virtual ~Statement() = default;
};

struct PrintStatement : public Statement {
    std::shared_ptr<Expr> expr;
    explicit PrintStatement(std::shared_ptr<Expr> e) : expr(std::move(e)) {}
};

struct RetStatement : public Statement {
    std::shared_ptr<Expr> expr;
    explicit RetStatement(std::shared_ptr<Expr> e) : expr(std::move(e)) {}
};

struct IfStatement : public Statement {
    std::shared_ptr<Expr> condition;
    std::vector<std::shared_ptr<Statement>> thenBranch;
    std::vector<std::shared_ptr<Statement>> elseBranch;
    IfStatement(std::shared_ptr<Expr> cond) : condition(std::move(cond)) {}
};

struct WhileStatement : public Statement {
    std::shared_ptr<Expr> condition;
    std::vector<std::shared_ptr<Statement>> body;
    explicit WhileStatement(std::shared_ptr<Expr> cond) : condition(std::move(cond)) {}
};

struct PckgStatement : public Statement {
    std::string name;
    explicit PckgStatement(const std::string& n) : name(n) {}
};

struct VarDeclStatement : public Statement {
    std::string name;
    std::string type;
    std::shared_ptr<Expr> initializer; // opsiyonel
    VarDeclStatement(const std::string& n, const std::string& t, std::shared_ptr<Expr> init = nullptr)
        : name(n), type(t), initializer(std::move(init)) {}
};

struct ConstDeclStatement : public Statement {
    std::string name;
    std::string type;
    std::shared_ptr<Expr> initializer;
    ConstDeclStatement(const std::string& n, const std::string& t, std::shared_ptr<Expr> init = nullptr)
        : name(n), type(t), initializer(std::move(init)) {}
};

struct AssignmentStatement : public Statement {
    std::string name;
    std::shared_ptr<Expr> value;
    AssignmentStatement(const std::string& n, std::shared_ptr<Expr> v)
        : name(n), value(std::move(v)) {}
};

struct FunctionDecl : public Statement {
    std::string name;
    std::vector<std::pair<std::string, std::string>> parameters;
    std::string returnType;
    std::vector<std::shared_ptr<Statement>> body;

    FunctionDecl(const std::string& n,
                 const std::vector<std::pair<std::string, std::string>>& params,
                 const std::string& retType)
        : name(n), parameters(params), returnType(retType) {}
};

struct Program {
    std::string packageName;
    std::vector<std::shared_ptr<Statement>> statements;
};

struct EndStatement : public Statement {};