#pragma once
#include <string>
#include <vector>
#include <memory>

namespace codegen {
namespace ir {

// Base IR instruction
struct IRInst {
    virtual ~IRInst() = default;
    virtual std::string toString() const = 0;
};

// Label
struct IRLabel : IRInst {
    std::string name;
    explicit IRLabel(std::string n) : name(std::move(n)) {}
    std::string toString() const override { return name + ":"; }
};

// Unconditional jump
struct IRJump : IRInst {
    std::string target;
    explicit IRJump(std::string t) : target(std::move(t)) {}
    std::string toString() const override { return "goto " + target; }
};

// Conditional jump
struct IRCondJump : IRInst {
    std::string cond;
    std::string thenLabel, elseLabel;
    IRCondJump(std::string c, std::string t, std::string e)
        : cond(std::move(c)), thenLabel(std::move(t)), elseLabel(std::move(e)) {}
    std::string toString() const override {
        return "if " + cond + " goto " + thenLabel + " else goto " + elseLabel;
    }
};

// Return
struct IRReturn : IRInst {
    std::string value;
    explicit IRReturn(std::string v) : value(std::move(v)) {}
    std::string toString() const override { return "ret " + value; }
};

// Call
struct IRCall : IRInst {
    std::string fn;
    std::vector<std::string> args;
    std::string dst; // optional destination

    IRCall(std::string f, std::vector<std::string> a, std::string d)
        : fn(std::move(f)), args(std::move(a)), dst(std::move(d)) {}

    std::string toString() const override {
        std::string result = dst.empty() ? "" : dst + " = ";
        result += "call " + fn + "(";
        for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0) result += ", ";
            result += args[i];
        }
        result += ")";
        return result;
    }
};

// Binary operation
struct IRBinary : IRInst {
    enum Op {
        Add, Sub, Mul, Div,
        Mod, Eq, Ne, Lt, Le, Gt, Ge
    };

    Op op;
    std::string dst, lhs, rhs;

    IRBinary(Op o, std::string d, std::string l, std::string r)
        : op(o), dst(std::move(d)), lhs(std::move(l)), rhs(std::move(r)) {}

    std::string toString() const override;
};

} // namespace ir
} // namespace codegen