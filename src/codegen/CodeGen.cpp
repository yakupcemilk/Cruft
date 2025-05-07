// src/codegen/CodeGen.cpp
#include "CodeGen.h"
#include "ir/IR.h"
#include "../parser/AST.h"
#include "../parser/stm/FunctionDecl.h"
#include "../parser/stm/VarDeclStatement.h"
#include "../parser/stm/ConstDeclStatement.h"
#include "../parser/stm/PrintStatement.h"
#include "../parser/stm/ReturnStatement.h"
#include "../parser/stm/IfStatement.h"
#include "../parser/stm/WhileStatement.h"
#include "../parser/stm/AssignmentStatement.h"
#include "../parser/stm/BinaryExpr.h"
#include "../parser/stm/IntegerExpr.h"
#include "../parser/stm/VariableExpr.h"
#include "../parser/stm/CallExpr.h"
#include "../parser/stm/Expr.h"
#include <stdexcept>
#include <functional>

using namespace codegen;
using namespace codegen::ir;

CodeGen::CodeGen(std::ostream& o) : out(o) {}

void CodeGen::generate(const cruft::Program& prog) {
    lowerToIR(prog);
    allocateRegisters();
    peepholeOptimize();
    emitAsm();
}

void CodeGen::lowerToIR(const cruft::Program& prog) {
    for (auto& stmt : prog.statements) {
        if (auto fn = dynamic_cast<cruft::FunctionDecl*>(stmt.get())) {
            // function label
            ir.emplace_back(std::make_unique<IRLabel>(fn->name));
            // prologue
            ir.emplace_back(std::make_unique<IRCall>("__prologue", std::vector<std::string>{}, ""));
            // params -> temps
            static const char* regs[] = {"rdi","rsi","rdx","rcx","r8","r9"};
            for (size_t i = 0; i < fn->parameters.size(); ++i) {
                std::string tmp = "t" + std::to_string(labelCount++);
                ir.emplace_back(std::make_unique<IRBinary>(IRBinary::Add, tmp, regs[i], "0"));
                regMap[tmp] = regs[i];
                // retain incoming parameter
                ir.emplace_back(std::make_unique<IRCall>("retain", std::vector<std::string>{tmp}, ""));
            }

            // recursive expression gen
            std::function<std::string(std::shared_ptr<cruft::Expr>)> genExpr;
            genExpr = [&](std::shared_ptr<cruft::Expr> e)->std::string {
                if (auto b = dynamic_cast<cruft::BinaryExpr*>(e.get())) {
                    auto l = genExpr(b->left);
                    auto r = genExpr(b->right);
                    std::string dst = "t" + std::to_string(labelCount++);
                    IRBinary::Op op;
                    if (b->op.value == "+") op = IRBinary::Add;
                    else if (b->op.value == "-") op = IRBinary::Sub;
                    else if (b->op.value == "*") op = IRBinary::Mul;
                    else if (b->op.value == "/") op = IRBinary::Div;
                    else throw std::runtime_error("unsupported op");
                    ir.emplace_back(std::make_unique<IRBinary>(op, dst, l, r));
                    // release operands
                    ir.emplace_back(std::make_unique<IRCall>("release", std::vector<std::string>{l}, ""));
                    ir.emplace_back(std::make_unique<IRCall>("release", std::vector<std::string>{r}, ""));
                    // dst is newly produced, no retain needed
                    return dst;
                }
                if (auto i = dynamic_cast<cruft::IntegerExpr*>(e.get())) {
                    std::string dst = "t" + std::to_string(labelCount++);
                    ir.emplace_back(std::make_unique<IRBinary>(IRBinary::Add, dst, std::to_string(i->value), "0"));
                    return dst;
                }
                if (auto v = dynamic_cast<cruft::VariableExpr*>(e.get())) {
                    // retain on use
                    ir.emplace_back(std::make_unique<IRCall>("retain", std::vector<std::string>{v->name}, ""));
                    return v->name;
                }
                if (auto c = dynamic_cast<cruft::CallExpr*>(e.get())) {
                    std::vector<std::string> args;
                    for (auto& a : c->args) args.push_back(genExpr(a));
                    std::string dst = "t" + std::to_string(labelCount++);
                    ir.emplace_back(std::make_unique<IRCall>(c->callee, args, dst));
                    // release argument temps
                    for (auto& a : args) ir.emplace_back(std::make_unique<IRCall>("release", std::vector<std::string>{a}, ""));
                    // retain return
                    ir.emplace_back(std::make_unique<IRCall>("retain", std::vector<std::string>{dst}, ""));
                    return dst;
                }
                throw std::runtime_error("unknown expr");
            };

            // body
            for (auto& s : fn->body) {
                if (auto pd = dynamic_cast<cruft::PrintStatement*>(s.get())) {
                    auto v = genExpr(pd->expression);
                    ir.emplace_back(std::make_unique<IRCall>("printf", std::vector<std::string>{"fmt_int", v}, ""));
                    ir.emplace_back(std::make_unique<IRCall>("release", std::vector<std::string>{v}, ""));
                }
                else if (auto rd = dynamic_cast<cruft::ReturnStatement*>(s.get())) {
                    auto v = genExpr(rd->value);
                    ir.emplace_back(std::make_unique<IRReturn>(v));
                }
                else if (auto vd = dynamic_cast<cruft::VarDeclStatement*>(s.get())) {
                    if (vd->initializer) {
                        auto v = genExpr(vd->initializer);
                        ir.emplace_back(std::make_unique<IRCall>("release", std::vector<std::string>{vd->name}, ""));
                        ir.emplace_back(std::make_unique<IRCall>("retain", std::vector<std::string>{v}, ""));
                        // assign v to var
                        ir.emplace_back(std::make_unique<IRBinary>(IRBinary::Add, vd->name, v, "0"));
                    }
                }
                else if (auto ad = dynamic_cast<cruft::AssignmentStatement*>(s.get())) {
                    auto v = genExpr(ad->value);
                    // release old
                    ir.emplace_back(std::make_unique<IRCall>("release", std::vector<std::string>{ad->name}, ""));
                    // retain new
                    ir.emplace_back(std::make_unique<IRCall>("retain", std::vector<std::string>{v}, ""));
                    ir.emplace_back(std::make_unique<IRBinary>(IRBinary::Add, ad->name, v, "0"));
                }
                // TODO: IfStatement, WhileStatement handling with proper releases...
            }

            // epilogue
            ir.emplace_back(std::make_unique<IRCall>("__epilogue", std::vector<std::string>{}, ""));
        }
    }
}

void CodeGen::allocateRegisters() {
    const std::vector<std::string> regs = {"rax","rbx","rcx","rdx","rsi","rdi"};
    size_t idx = 0;
    for (auto& inst : ir) {
        if (auto bin = dynamic_cast<IRBinary*>(inst.get())) {
            if (!regMap.count(bin->dst)) regMap[bin->dst] = regs[idx++ % regs.size()];
            if (!regMap.count(bin->lhs)) regMap[bin->lhs] = regs[idx++ % regs.size()];
            if (!regMap.count(bin->rhs)) regMap[bin->rhs] = regs[idx++ % regs.size()];
        }
        if (auto call = dynamic_cast<IRCall*>(inst.get())) {
            for (auto& a : call->args)
                if (!regMap.count(a))
                    regMap[a] = regs[idx++ % regs.size()];
            if (!call->dst.empty() && !regMap.count(call->dst))
                regMap[call->dst] = "rax";
        }
        if (auto ret = dynamic_cast<IRReturn*>(inst.get())) {
            if (!regMap.count(ret->value)) regMap[ret->value] = regs[idx++ % regs.size()];
        }
    }
}

void CodeGen::peepholeOptimize() {
    std::vector<std::unique_ptr<IRInst>> filtered;
    for (auto& inst : ir) {
        if (auto bin = dynamic_cast<IRBinary*>(inst.get()))
            if (bin->op == IRBinary::Add && bin->rhs == "0")
                continue;
        filtered.push_back(std::move(inst));
    }
    ir.swap(filtered);
}

void CodeGen::emitAsm() {
    // data
    emit("section .data");
    emit("fmt_int: db \"%d\",10,0");

    // text
    emit("section .text");
    emit("extern printf");
    emit("extern retain");
    emit("extern release");
    emit("global main");

    for (auto& inst : ir) {
        if (auto lbl = dynamic_cast<IRLabel*>(inst.get())) {
            emit(lbl->name + ":");
            continue;
        }
        if (auto bin = dynamic_cast<IRBinary*>(inst.get())) {
            auto d = regMap[bin->dst], a = regMap[bin->lhs], b = regMap[bin->rhs];
            switch (bin->op) {
                case IRBinary::Add:
                    emit("    mov " + d + ", " + a);
                    emit("    add " + d + ", " + b);
                    break;
                case IRBinary::Sub:
                    emit("    mov " + d + ", " + a);
                    emit("    sub " + d + ", " + b);
                    break;
                case IRBinary::Mul:
                    emit("    mov " + d + ", " + a);
                    emit("    imul " + d + ", " + b);
                    break;
                case IRBinary::Div:
                    emit("    mov rax, " + a);
                    emit("    cqo");
                    emit("    idiv " + b);
                    emit("    mov " + d + ", rax");
                    break;
                default:
                    break;
            }
            continue;
        }
        if (auto call = dynamic_cast<IRCall*>(inst.get())) {
            static const char* argregs[] = {"rdi","rsi","rdx","rcx","r8","r9"};
            for (size_t i = 0; i < call->args.size() && i < 6; ++i)
                emit("    mov " + std::string(argregs[i]) + ", " + regMap[call->args[i]]);
            emit("    call " + call->fn);
            if (!call->dst.empty()) {
                // store in dst-reg (rax), already retained in IR
                emit("    mov " + regMap[call->dst] + ", rax");
            }
            continue;
        }
        if (auto ret = dynamic_cast<IRReturn*>(inst.get())) {
            // release return value inside RAX
            emit("    mov rdi, " + regMap[ret->value]);
            emit("    call release");
            emit("    mov rax, " + regMap[ret->value]);
            emit("    ret");
            continue;
        }
    }
}

std::string CodeGen::uniqueLabel(const std::string& base) {
    return base + "_" + std::to_string(labelCount++);
}

void CodeGen::emit(const std::string& line) {
    out << line << "\n";
}
