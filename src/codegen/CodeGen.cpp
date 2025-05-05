#include "CodeGen.h"

CodeGen::CodeGen(std::ostream& output)
    : out(output), labelCount(0) {}

void CodeGen::emit(const std::string& code) {
    out << code << "\n";
}

std::string CodeGen::uniqueLabel(const std::string& base) {
    return base + std::to_string(labelCount++);
}

void CodeGen::generate(const Program& prog) {
    emit("section .data");
    emit("fmt_int: db \"%d\", 10, 0");

    // globals
    for (auto& stmt : prog.statements) {
        if (auto vd = dynamic_cast<VarDeclStatement*>(stmt.get())) {
            emit(vd->name + ": dq 0");
        }
        if (auto cd = dynamic_cast<ConstDeclStatement*>(stmt.get())) {
            emit(cd->name + ": dq " + std::to_string(cd->initializer->value));
        }
    }

    emit("section .text");
    emit("extern printf");
    emit("global main");

    for (auto& stmt : prog.statements) {
        if (auto fn = dynamic_cast<FunctionDecl*>(stmt.get())) {
            genFunction(*fn);
        }
    }
}

void CodeGen::genFunction(const FunctionDecl& f) {
    emit(f.name + ":");
    emit("    push rbp");
    emit("    mov rbp, rsp");
    emit("    sub rsp, 32"); // reserve stack

    for (auto& st : f.body) genStatement(st);

    emit("    mov rsp, rbp");
    emit("    pop rbp");
    emit("    ret");
}

void CodeGen::genStatement(const std::shared_ptr<Statement>& s) {
    if (auto p = dynamic_cast<PrintStatement*>(s.get())) genPrint(*p);
    else if (auto r = dynamic_cast<ReturnStatement*>(s.get())) genReturn(*r);
    else if (auto iff = dynamic_cast<IfStatement*>(s.get())) genIf(*iff);
    else if (auto wh = dynamic_cast<WhileStatement*>(s.get())) genWhile(*wh);
    else if (auto vd = dynamic_cast<VarDeclStatement*>(s.get())) genVarDecl(*vd);
    else if (auto cd = dynamic_cast<ConstDeclStatement*>(s.get())) genConstDecl(*cd);
    else if (auto asg = dynamic_cast<AssignmentStatement*>(s.get())) genAssignment(*asg);
}

void CodeGen::genPrint(const PrintStatement& p) {
    genExpr(p.expression);
    emit("    mov rdi, fmt_int");
    emit("    mov rsi, rax");
    emit("    xor rax, rax");
    emit("    call printf");
}

void CodeGen::genReturn(const ReturnStatement& r) {
    genExpr(r.value);
    emit("    mov rsp, rbp");
    emit("    pop rbp");
    emit("    ret");
}

void CodeGen::genIf(const IfStatement& iff) {
    std::string elseL = uniqueLabel("else");
    std::string endL  = uniqueLabel("endif");
    genExpr(iff.condition);
    emit("    cmp rax, 0");
    emit("    je " + elseL);
    for (auto& st : iff.thenBranch) genStatement(st);
    emit("    jmp " + endL);
    emit(elseL + ":");
    for (auto& st : iff.elseBranch) genStatement(st);
    emit(endL + ":");
}

void CodeGen::genWhile(const WhileStatement& wh) {
    std::string startL = uniqueLabel("while_start");
    std::string endL   = uniqueLabel("while_end");
    emit(startL + ":");
    genExpr(wh.condition);
    emit("    cmp rax, 0");
    emit("    je " + endL);
    for (auto& st : wh.body) genStatement(st);
    emit("    jmp " + startL);
    emit(endL + ":");
}

void CodeGen::genVarDecl(const VarDeclStatement& v) {
    // placeholder: locals handled in stack frame
}

void CodeGen::genConstDecl(const ConstDeclStatement& c) {
    // no runtime code, handled in data section
}

void CodeGen::genAssignment(const AssignmentStatement& a) {
    genExpr(a.value);
    emit("    mov [rbp-8], rax");
}

void CodeGen::genExpr(const std::shared_ptr<Expr>& e) {
    if (auto b = dynamic_cast<BinaryExpr*>(e.get())) genBinary(*b);
    else if (auto i = dynamic_cast<IntegerExpr*>(e.get())) genInteger(*i);
    else if (auto v = dynamic_cast<VariableExpr*>(e.get())) genVariable(*v);
}

void CodeGen::genBinary(const BinaryExpr& b) {
    genExpr(b.left);
    emit("    push rax");
    genExpr(b.right);
    emit("    pop rcx");
    if (b.op.value == "+") emit("    add rax, rcx");
    else if (b.op.value == "-") {
        emit("    mov rdx, rcx");
        emit("    sub rdx, rax");
        emit("    mov rax, rdx");
    }
    else if (b.op.value == "*") emit("    imul rax, rcx");
}

void CodeGen::genInteger(const IntegerExpr& i) {
    emit("    mov rax, " + std::to_string(i.value));
}

void CodeGen::genVariable(const VariableExpr& v) {
    emit("    mov rax, [" + v.name + "]");
}