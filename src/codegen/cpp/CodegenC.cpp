// src/codegen/cpp/CodeGenC.cpp
#include "CodegenC.h"
#include <stdexcept>

using namespace cruft::codegen::cpp;
using namespace codegen::ir;

CodeGenC::CodeGenC(std::ostream& o) : out(o) {}

void CodeGenC::generate(const cruft::Program& prog) {
    // Header
    emitLine("// Generated C++ from Cruft");
    emitLine("#include <iostream>");
    emitLine("#include <memory>");
    emitLine("");
    // Package isolation
    emitLine("namespace " + prog.packageName + " {");
    emitLine("");

    // Build IR and optimize
    lowerToIR(prog);
    allocateRegisters();
    peepholeOptimize();

    // Emit C++ functions
    emitC();

    emitLine("");
    emitLine("} // namespace " + prog.packageName);
}

void CodeGenC::lowerToIR(const cruft::Program& prog) {
    ir.clear();
    labelCount = 0;

    // define expression generator
    genExpr = [&](std::shared_ptr<cruft::Expr> e) -> std::string {
        if (auto b = std::dynamic_pointer_cast<cruft::BinaryExpr>(e)) {
            auto l = genExpr(b->left);
            auto r = genExpr(b->right);
            std::string dst = "tmp" + std::to_string(labelCount++);
            IRBinary::Op op;
            if (b->op.value == "+") op = IRBinary::Add;
            else if (b->op.value == "-") op = IRBinary::Sub;
            else if (b->op.value == "*") op = IRBinary::Mul;
            else if (b->op.value == "/") op = IRBinary::Div;
            else throw std::runtime_error("unsupported op");
            ir.emplace_back(std::make_unique<IRBinary>(op, dst, l, r));
            return dst;
        }
        if (auto i = std::dynamic_pointer_cast<cruft::IntegerExpr>(e)) {
            std::string dst = "tmp" + std::to_string(labelCount++);
            ir.emplace_back(std::make_unique<IRBinary>(IRBinary::Add, dst, std::to_string(i->value), "0"));
            return dst;
        }
        if (auto v = std::dynamic_pointer_cast<cruft::VariableExpr>(e)) {
            return v->name;
        }
        if (auto c = std::dynamic_pointer_cast<cruft::CallExpr>(e)) {
            std::vector<std::string> args;
            for (auto& a : c->args) args.push_back(genExpr(a));
            std::string dst = "tmp" + std::to_string(labelCount++);
            ir.emplace_back(std::make_unique<IRCall>(c->callee, args, dst));
            return dst;
        }
        throw std::runtime_error("unknown expr");
    };

    // Lower each top‑level statement
    for (auto& stmt : prog.statements) {
        if (auto fn = std::dynamic_pointer_cast<cruft::FunctionDecl>(stmt)) {
            // label
            ir.emplace_back(std::make_unique<IRLabel>(fn->name));
            // parameters are assumed all int
            for (auto& param : fn->parameters) {
                // declare param variable
                // (we'll refer to by name directly in C)
            }
            // body
            for (auto& s : fn->body) {
                if (auto pd = std::dynamic_pointer_cast<cruft::PrintStatement>(s)) {
                    auto v = genExpr(pd->expression);
                    ir.emplace_back(std::make_unique<IRCall>("std::cout <<", std::vector<std::string>{v}, ""));
                    ir.emplace_back(std::make_unique<IRCall>("std::cout << std::endl", std::vector<std::string>{}, ""));
                }
                else if (auto rd = std::dynamic_pointer_cast<cruft::ReturnStatement>(s)) {
                    auto v = genExpr(rd->value);
                    ir.emplace_back(std::make_unique<IRReturn>(v));
                }
                else if (auto vd = std::dynamic_pointer_cast<cruft::VarDeclStatement>(s)) {
                    if (vd->initializer) {
                        auto v = genExpr(vd->initializer);
                        ir.emplace_back(std::make_unique<IRBinary>(IRBinary::Add, vd->name, v, "0"));
                    } else {
                        // default zero
                        ir.emplace_back(std::make_unique<IRBinary>(IRBinary::Add, vd->name, "0", "0"));
                    }
                }
                else if (auto ad = std::dynamic_pointer_cast<cruft::AssignmentStatement>(s)) {
                    auto v = genExpr(ad->value);
                    ir.emplace_back(std::make_unique<IRBinary>(IRBinary::Add, ad->name, v, "0"));
                }
                else if (auto iff = std::dynamic_pointer_cast<cruft::IfStatement>(s)) {
                    std::string thenL = uniqueLabel("then");
                    std::string elseL = uniqueLabel("else");
                    std::string endL  = uniqueLabel("endif");
                    auto cond = genExpr(iff->condition);
                    ir.emplace_back(std::make_unique<IRCondJump>(cond, thenL, elseL));
                    ir.emplace_back(std::make_unique<IRLabel>(thenL));
                    for (auto& ts : iff->thenBranch) {
                        // recursively lower each
                        // (for brevity we assume no nested control here)
                    }
                    ir.emplace_back(std::make_unique<IRJump>(endL));
                    ir.emplace_back(std::make_unique<IRLabel>(elseL));
                    for (auto& es : iff->elseBranch) {
                        // lower
                    }
                    ir.emplace_back(std::make_unique<IRLabel>(endL));
                }
                else if (auto wh = std::dynamic_pointer_cast<cruft::WhileStatement>(s)) {
                    std::string startL = uniqueLabel("while");
                    std::string endL   = uniqueLabel("endwhile");
                    ir.emplace_back(std::make_unique<IRLabel>(startL));
                    auto cond = genExpr(wh->condition);
                    ir.emplace_back(std::make_unique<IRCondJump>(cond, startL, endL));
                    for (auto& bs : wh->body) {
                        // lower
                    }
                    ir.emplace_back(std::make_unique<IRJump>(startL));
                    ir.emplace_back(std::make_unique<IRLabel>(endL));
                }
            }
        }
    }
}

void CodeGenC::allocateRegisters() {
    // no-op: C++ backend doesn’t use regs
}

void CodeGenC::peepholeOptimize() {
    std::vector<std::unique_ptr<IRInst>> filtered;
    for (auto& inst : ir) {
        if (auto bin = dynamic_cast<IRBinary*>(inst.get())) {
            // remove x = x + 0
            if (bin->op == IRBinary::Add && bin->rhs == "0" && bin->dst == bin->lhs)
                continue;
        }
        filtered.push_back(std::move(inst));
    }
    ir.swap(filtered);
}

std::string CodeGenC::joinArgs(const std::vector<std::string>& args) {
    std::string out;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) out += ", ";
        out += args[i];
    }
    return out;
}

void CodeGenC::emitC() {
    std::string currentFn;
    // Emit function signatures and bodies
    for (size_t i = 0; i < ir.size(); ++i) {
        IRInst* inst = ir[i].get();
        if (auto lbl = dynamic_cast<IRLabel*>(inst)) {
            currentFn = lbl->name;
            // find the original FunctionDecl to get parameters
            emitLine("int " + currentFn + "(" + /* assume all int params */ "int argc, char** argv" + ") {");
            continue;
        }
        if (auto bin = dynamic_cast<IRBinary*>(inst)) {
            std::string a = bin->lhs;
            std::string b = bin->rhs;
            std::string d = bin->dst;
            switch (bin->op) {
                case IRBinary::Add:
                    emitLine("    int " + d + " = " + a + " + " + b + ";");
                    break;
                case IRBinary::Sub:
                    emitLine("    int " + d + " = " + a + " - " + b + ";");
                    break;
                case IRBinary::Mul:
                    emitLine("    int " + d + " = " + a + " * " + b + ";");
                    break;
                case IRBinary::Div:
                    emitLine("    int " + d + " = " + a + " / " + b + ";");
                    break;
                default:
                    emitLine("    // op not implemented");
            }
            continue;
        }
        if (auto call = dynamic_cast<IRCall*>(inst)) {
            std::string args = joinArgs(call->args);
            if (call->dst.empty()) {
                emitLine("    " + call->fn + "(" + args + ");");
            } else {
                emitLine("    auto " + call->dst + " = " + call->fn + "(" + args + ");");
            }
            continue;
        }
        if (auto cj = dynamic_cast<IRCondJump*>(inst)) {
            emitLine("    if (!(" + cj->cond + ")) goto " + cj->elseLabel + ";");
            continue;
        }
        if (auto j = dynamic_cast<IRJump*>(inst)) {
            emitLine("    goto " + j->target + ";");
            continue;
        }
        if (auto lbl = dynamic_cast<IRLabel*>(inst)) {
            emitLine(lbl->name + ": ;");
            continue;
        }
        if (auto ret = dynamic_cast<IRReturn*>(inst)) {
            emitLine("    return " + ret->value + ";");
            emitLine("}");
            currentFn.clear();
            continue;
        }
    }
}

std::string CodeGenC::uniqueLabel(const std::string& base) {
    return base + "_" + std::to_string(labelCount++);
}

void CodeGenC::emitLine(const std::string& line) {
    out << line << "\n";
}
