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

// Lower AST into linear IR
void CodeGen::lowerToIR(const cruft::Program& prog) {
    for (auto& stmt : prog.statements) {
        if (auto fn = dynamic_cast<cruft::FunctionDecl*>(stmt.get())) {
            // function label
            ir.emplace_back(std::make_unique<ir::IRLabel>(fn->name));
            // prologue: push rbp; mov rbp,rsp
            ir.emplace_back(std::make_unique<ir::IRCall>("__prologue", std::vector<std::string>{}, ""));
            // params to temps
            static const char* regs[] = {"rdi","rsi","rdx","rcx","r8","r9"};
            for (size_t i = 0; i < fn->parameters.size(); ++i) {
                std::string tmp = "t" + std::to_string(labelCount++);
                ir.emplace_back(std::make_unique<ir::IRBinary>(ir::IRBinary::Add, tmp, regs[i], "0"));
                regMap[tmp] = regs[i];
            }
            // body
            std::function<std::string(std::shared_ptr<cruft::Expr>)> genExpr;
            genExpr = [&](std::shared_ptr<cruft::Expr> e) -> std::string {
                if (auto b = dynamic_cast<cruft::BinaryExpr*>(e.get())) {
                    auto l = genExpr(b->left);
                    auto r = genExpr(b->right);
                    std::string dst = "t" + std::to_string(labelCount++);
                    ir.emplace_back(std::make_unique<ir::IRBinary>(
                        (ir::IRBinary::Op)([&]{
                            using Op=ir::IRBinary::Op;
                            if (b->op.value=="+") return Op::Add;
                            if (b->op.value=="-") return Op::Sub;
                            if (b->op.value=="*") return Op::Mul;
                            if (b->op.value=="/") return Op::Div;
                            throw std::runtime_error("unsupported op");
                        })(), dst, l, r));
                    return dst;
                }
                if (auto i = dynamic_cast<cruft::IntegerExpr*>(e.get())) {
                    std::string dst = "t" + std::to_string(labelCount++);
                    ir.emplace_back(std::make_unique<ir::IRBinary>(ir::IRBinary::Add, dst, std::to_string(i->value), "0"));
                    return dst;
                }
                if (auto v = dynamic_cast<cruft::VariableExpr*>(e.get())) {
                    return v->name;
                }
                if (auto c = dynamic_cast<cruft::CallExpr*>(e.get())) {
                    std::vector<std::string> args;
                    for (auto& a : c->args) args.push_back(genExpr(a));
                    std::string dst = "t" + std::to_string(labelCount++);
                    ir.emplace_back(std::make_unique<ir::IRCall>(c->callee, args, dst));
                    return dst;
                }
                throw std::runtime_error("unknown expr");
            };
            for (auto& s : fn->body) {
                if (auto pd = dynamic_cast<cruft::PrintStatement*>(s.get())) {
                    auto v = genExpr(pd->expression);
                    ir.emplace_back(std::make_unique<ir::IRCall>("printf", std::vector<std::string>{"fmt_int", v}, ""));
                }
                else if (auto rd = dynamic_cast<cruft::ReturnStatement*>(s.get())) {
                    auto v = genExpr(rd->value);
                    ir.emplace_back(std::make_unique<ir::IRReturn>(v));
                }
                // TODO: handle IfStatement, WhileStatement, Assignments
            }
            // epilogue
            ir.emplace_back(std::make_unique<ir::IRCall>("__epilogue", std::vector<std::string>{}, ""));
        }
    }
}

// Naive register allocation: map temps to RAX, RBX, RCX... in round-robin
void CodeGen::allocateRegisters() {
    const std::vector<std::string> regs = {"rax","rbx","rcx","rdx","rsi","rdi"};
    size_t idx = 0;
    for (auto& inst : ir) {
        if (auto bin = dynamic_cast<ir::IRBinary*>(inst.get())) {
            if (regMap.find(bin->dst)==regMap.end()) regMap[bin->dst] = regs[idx++ % regs.size()];
            if (regMap.find(bin->lhs)==regMap.end()) regMap[bin->lhs] = regs[idx++ % regs.size()];
            if (regMap.find(bin->rhs)==regMap.end()) regMap[bin->rhs] = regs[idx++ % regs.size()];
        }
        if (auto call = dynamic_cast<ir::IRCall*>(inst.get())) {
            for (auto& a: call->args) if (regMap.find(a)==regMap.end()) regMap[a] = regs[idx++ % regs.size()];
            if (!call->dst.empty() && regMap.find(call->dst)==regMap.end()) regMap[call->dst] = "rax";
        }
    }
}

// Simple peephole: remove add X, 0
void CodeGen::peepholeOptimize() {
    std::vector<std::unique_ptr<ir::IRInst>> filtered;
    for (auto& inst : ir) {
        if (auto bin = dynamic_cast<ir::IRBinary*>(inst.get())) {
            if (bin->op==ir::IRBinary::Add && bin->rhs=="0") continue;
        }
        filtered.push_back(std::move(inst));
    }
    ir.swap(filtered);
}

// Emit final assembly
void CodeGen::emitAsm() {
    // data
    emit("section .data");
    emit("fmt_int: db \"%d\",10,0");
    // text
    emit("section .text");
    emit("extern printf");
    emit("global main");

    for (auto& inst : ir) {
        if (auto lbl = dynamic_cast<ir::IRLabel*>(inst.get())) {
            emit(lbl->name + ":");
            continue;
        }
        if (auto bin = dynamic_cast<ir::IRBinary*>(inst.get())) {
            auto d = regMap[bin->dst];
            auto a = regMap[bin->lhs];
            auto b = regMap[bin->rhs];
            if (bin->op==ir::IRBinary::Add)      emit("    mov " + d + ", " + a), emit("    add " + d + ", " + b);
            else if (bin->op==ir::IRBinary::Sub) emit("    mov " + d + ", " + a), emit("    sub " + d + ", " + b);
            else if (bin->op==ir::IRBinary::Mul) emit("    mov " + d + ", " + a), emit("    imul " + d + ", " + b);
            else if (bin->op==ir::IRBinary::Div) {
                emit("    mov rax, " + a);
                emit("    cqo");
                emit("    idiv " + b);
                emit("    mov " + d + ", rax");
            }
            continue;
        }
        if (auto call = dynamic_cast<ir::IRCall*>(inst.get())) {
            for (size_t i=0;i<call->args.size()&&i<6;++i) {
                static const char* regs[] = {"rdi","rsi","rdx","rcx","r8","r9"};
                emit("    mov " + std::string(regs[i]) + ", " + regMap[call->args[i]]);
            }
            emit(std::string("    call ") + call->fn);
            if (!call->dst.empty() && call->fn!="__prologue" && call->fn!="__epilogue") {
                emit("    mov " + regMap[call->dst] + ", rax");
            }
            continue;
        }
        if (auto ret = dynamic_cast<ir::IRReturn*>(inst.get())) {
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