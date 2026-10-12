// CodeGen.cpp - LLVM IR generation. See CodeGen.h for the ABI/erasure contract.
#include "CodeGen.h"

#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include <algorithm>
#include <cstdint>

namespace cruft {

using namespace ast;
using sema::Symbol;
using sema::SymbolKind;
using sema::Type;
using sema::TypeKind;

namespace {
template <class T> const T &as(const Expr &e) { return static_cast<const T &>(e); }
} // namespace

CodeGen::CodeGen(Program &program, sema::TypeChecker &checker, DiagnosticEngine &diags,
                 std::string moduleName, CodeGenOptions options)
    : program_(program), checker_(checker), diags_(diags), options_(std::move(options)),
      ctx_(std::make_unique<llvm::LLVMContext>()),
      module_(std::make_unique<llvm::Module>(moduleName, *ctx_)),
      builder_(std::make_unique<llvm::IRBuilder<>>(*ctx_)) {}

CodeGen::~CodeGen() = default;

// ---------------------------------------------------------------------------
// Target setup
// ---------------------------------------------------------------------------

bool CodeGen::initTarget() {
  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmParsers();
  llvm::InitializeAllAsmPrinters();

  std::string triple = options_.targetTriple.empty() ? llvm::sys::getDefaultTargetTriple()
                                                     : options_.targetTriple;
  // LLVM's legacy `arch-w64-mingw32` spelling is parsed as ELF in some 16.x
  // builds. Canonicalise it to the Windows-GNU form so we emit PE/COFF.
  if (triple.find("mingw32") != std::string::npos ||
      (llvm::Triple(triple).isOSWindows() &&
       llvm::Triple(triple).getObjectFormat() == llvm::Triple::ELF)) {
    llvm::Triple t(llvm::Triple::normalize(triple));
    t.setOS(llvm::Triple::Win32);
    t.setEnvironment(llvm::Triple::GNU);
    t.setObjectFormat(llvm::Triple::COFF);
    triple = t.str();
  }
  std::string error;
  const llvm::Target *target = llvm::TargetRegistry::lookupTarget(triple, error);
  if (!target) {
    diags_.error(SourceLocation{}, "unknown target '" + triple + "': " + error);
    return false;
  }

  llvm::TargetOptions opts;
#if LLVM_VERSION_MAJOR < 18
  using OptEnum = llvm::CodeGenOpt::Level;
#else
  using OptEnum = llvm::CodeGenOptLevel;
#endif
  OptEnum level = OptEnum::None;
  switch (options_.optLevel) {
  case 0: level = OptEnum::None; break;
  case 1: level = OptEnum::Less; break;
  case 2: level = OptEnum::Default; break;
  default: level = OptEnum::Aggressive; break;
  }
#if LLVM_VERSION_MAJOR >= 21
  targetMachine_.reset(target->createTargetMachine(llvm::Triple(triple), "generic", "", opts,
                                                   llvm::Reloc::PIC_, std::nullopt, level));
#else
  targetMachine_.reset(target->createTargetMachine(triple, "generic", "", opts,
                                                   llvm::Reloc::PIC_, std::nullopt, level));
#endif
  if (!targetMachine_) {
    diags_.error(SourceLocation{}, "could not create a target machine for '" + triple + "'");
    return false;
  }
#if LLVM_VERSION_MAJOR >= 21
  module_->setTargetTriple(llvm::Triple(triple));
#else
  module_->setTargetTriple(triple);
#endif
  module_->setDataLayout(targetMachine_->createDataLayout());
  return true;
}

// ---------------------------------------------------------------------------
// Type lowering
// ---------------------------------------------------------------------------

llvm::Type *CodeGen::lower(const Type &t) {
  switch (t.kind) {
  case TypeKind::I32: return llvm::Type::getInt32Ty(*ctx_);
  case TypeKind::I64: return llvm::Type::getInt64Ty(*ctx_);
  case TypeKind::F32: return llvm::Type::getFloatTy(*ctx_);
  case TypeKind::F64: return llvm::Type::getDoubleTy(*ctx_);
  case TypeKind::Bool: return llvm::Type::getInt1Ty(*ctx_);
  case TypeKind::Void: return llvm::Type::getVoidTy(*ctx_);
  case TypeKind::Fn: return llvm::PointerType::getUnqual(*ctx_);
  case TypeKind::Vector:
  case TypeKind::Map:
  case TypeKind::String: return bufferType();
  case TypeKind::Inductive: {
    auto it = inductives_.find(t.inductive);
    if (it == inductives_.end()) {
      lowerInductive(*t.inductive);
      it = inductives_.find(t.inductive);
    }
    return it->second;
  }
  case TypeKind::Interval:
  case TypeKind::Universe:
  case TypeKind::Path:
  case TypeKind::Equiv:
    return nullptr; // erased
  case TypeKind::Error:
    return nullptr;
  }
  return nullptr;
}

llvm::FunctionType *CodeGen::lowerFnType(const Type &t) {
  std::vector<llvm::Type *> params;
  for (const auto &p : t.params)
    if (llvm::Type *lt = lower(*p))
      params.push_back(lt);
  llvm::Type *ret = lower(*t.ret);
  if (!ret)
    ret = llvm::Type::getVoidTy(*ctx_); // erased result
  return llvm::FunctionType::get(ret, params, /*isVarArg=*/false);
}

/// An inductive type is a C-style tagged union:
///   struct { i32 tag; <payload storage aligned to the widest field> }
/// Point constructors get consecutive tags; path constructors vanish.
void CodeGen::lowerInductive(const Symbol &typeSym) {
  auto *self = llvm::StructType::create(*ctx_, typeSym.qualifiedName());
  inductives_[&typeSym] = self; // register first: constructors may be recursive

  const llvm::DataLayout &dl = module_->getDataLayout();
  std::uint64_t maxSize = 0;
  std::uint64_t maxAlign = 1;
  for (const Symbol *ctor : typeSym.ctors) {
    if (ctor->isPathCtor)
      continue;
    std::vector<llvm::Type *> fields;
    if (ctor->type->kind == TypeKind::Fn)
      for (const auto &p : ctor->type->params)
        if (llvm::Type *ft = lower(*p))
          fields.push_back(ft);
    auto *payload = llvm::StructType::create(*ctx_, fields, typeSym.qualifiedName() + "." + ctor->name);
    ctorPayloads_[ctor] = payload;
    if (!fields.empty()) {
      maxSize = std::max<std::uint64_t>(maxSize, dl.getTypeAllocSize(payload));
      maxAlign = std::max<std::uint64_t>(maxAlign, dl.getABITypeAlign(payload).value());
    }
  }

  std::vector<llvm::Type *> body{llvm::Type::getInt32Ty(*ctx_)};
  if (maxSize > 0) {
    llvm::Type *unit = llvm::Type::getIntNTy(*ctx_, static_cast<unsigned>(maxAlign * 8));
    std::uint64_t count = (maxSize + maxAlign - 1) / maxAlign;
    body.push_back(llvm::ArrayType::get(unit, count));
  }
  self->setBody(body);
}

// ---------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------

bool CodeGen::generate() {
  if (!initTarget())
    return false;

  for (auto &file : program_.files)
    for (auto &td : file->types)
      if (td->symbol)
        lower(*td->symbol->type); // lazily lowers (and memoises) the layout
  for (auto &file : program_.files)
    for (auto &fn : file->functions)
      if (fn->symbol)
        declareFunction(*fn);
  for (auto &file : program_.files)
    for (auto &g : file->globals)
      if (g->symbol)
        defineGlobal(*g);
  for (auto &file : program_.files)
    for (auto &fn : file->functions)
      if (fn->symbol && !fn->isExtern)
        defineFunction(*fn);

  std::string verifyErrors;
  llvm::raw_string_ostream os(verifyErrors);
  if (llvm::verifyModule(*module_, &os)) {
    diags_.error(SourceLocation{}, "internal error: generated LLVM module is invalid:\n" + os.str());
    return false;
  }
  return !diags_.hasErrors();
}

void CodeGen::optimize() {
  if (options_.optLevel == 0)
    return;
  llvm::LoopAnalysisManager lam;
  llvm::FunctionAnalysisManager fam;
  llvm::CGSCCAnalysisManager cgam;
  llvm::ModuleAnalysisManager mam;
  llvm::PassBuilder pb(targetMachine_.get());
  pb.registerModuleAnalyses(mam);
  pb.registerCGSCCAnalyses(cgam);
  pb.registerFunctionAnalyses(fam);
  pb.registerLoopAnalyses(lam);
  pb.crossRegisterProxies(lam, fam, cgam, mam);
  llvm::OptimizationLevel level = options_.optLevel == 1   ? llvm::OptimizationLevel::O1
                                  : options_.optLevel == 2 ? llvm::OptimizationLevel::O2
                                                           : llvm::OptimizationLevel::O3;
  llvm::ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(level);
  mpm.run(*module_, mam);
}

bool CodeGen::writeIR(const std::string &path) {
  std::error_code ec;
  llvm::raw_fd_ostream out(path == "-" ? "-" : path, ec, llvm::sys::fs::OF_Text);
  if (ec) {
    diags_.error(SourceLocation{}, "cannot open '" + path + "': " + ec.message());
    return false;
  }
  module_->print(out, nullptr);
  return true;
}

namespace {
bool emitFile(llvm::TargetMachine &tm, llvm::Module &m, const std::string &path,
              llvm::CodeGenFileType type, DiagnosticEngine &diags) {
  std::error_code ec;
  llvm::raw_fd_ostream out(path, ec, llvm::sys::fs::OF_None);
  if (ec) {
    diags.error(SourceLocation{}, "cannot open '" + path + "': " + ec.message());
    return false;
  }
  llvm::legacy::PassManager pm;
  if (tm.addPassesToEmitFile(pm, out, nullptr, type)) {
    diags.error(SourceLocation{}, "target machine cannot emit this file type");
    return false;
  }
  pm.run(m);
  out.flush();
  return true;
}
} // namespace

bool CodeGen::writeObject(const std::string &path) {
#if LLVM_VERSION_MAJOR < 18
  return emitFile(*targetMachine_, *module_, path, llvm::CGFT_ObjectFile, diags_);
#else
  return emitFile(*targetMachine_, *module_, path, llvm::CodeGenFileType::ObjectFile, diags_);
#endif
}

bool CodeGen::writeAssembly(const std::string &path) {
#if LLVM_VERSION_MAJOR < 18
  return emitFile(*targetMachine_, *module_, path, llvm::CGFT_AssemblyFile, diags_);
#else
  return emitFile(*targetMachine_, *module_, path, llvm::CodeGenFileType::AssemblyFile, diags_);
#endif
}

// ---------------------------------------------------------------------------
// Declarations
// ---------------------------------------------------------------------------

void CodeGen::applyBoolAttributes(llvm::Function &f, const Type &fnType) {
  // clang passes/returns `_Bool` as `zeroext i1`; mirror that so Cruft bools
  // are bit-compatible with C and Rust `bool` at the ABI boundary.
  unsigned argIndex = 0;
  for (const auto &p : fnType.params) {
    if (!lower(*p))
      continue;
    if (p->kind == TypeKind::Bool)
      f.addParamAttr(argIndex, llvm::Attribute::ZExt);
    ++argIndex;
  }
  if (fnType.ret->kind == TypeKind::Bool)
    f.addRetAttr(llvm::Attribute::ZExt);
}

void CodeGen::declareFunction(const FnDecl &fn) {
  const Symbol &sym = *fn.symbol;
  llvm::FunctionType *fty = lowerFnType(*sym.type);
  llvm::GlobalValue::LinkageTypes linkage =
      (sym.isExtern || sym.visibility == Visibility::Public) ? llvm::Function::ExternalLinkage
                                                              : llvm::Function::InternalLinkage;
  llvm::Function *f = llvm::Function::Create(fty, linkage, sym.linkName, module_.get());
  f->setCallingConv(llvm::CallingConv::C);
  if (!sym.isExtern)
    f->addFnAttr(llvm::Attribute::NoUnwind);
  applyBoolAttributes(*f, *sym.type);

  unsigned i = 0;
  for (size_t p = 0; p < fn.params.size(); ++p) {
    if (!lower(*sym.type->params[p]))
      continue; // erased parameter: no argument
    f->getArg(i++)->setName(fn.params[p].name);
  }
  functions_[&sym] = f;
}

llvm::Constant *CodeGen::constantFor(const Expr &init, const Type &type) {
  ExprPtr nf = checker_.normalizer().normalize(init);
  llvm::Type *lt = lower(type);
  switch (nf->kind) {
  case ExprKind::IntLit:
    return llvm::ConstantInt::get(lt, static_cast<std::uint64_t>(as<IntLitExpr>(*nf).value), true);
  case ExprKind::FloatLit:
    return llvm::ConstantFP::get(lt, as<FloatLitExpr>(*nf).value);
  case ExprKind::BoolLit:
    return llvm::ConstantInt::get(lt, as<BoolLitExpr>(*nf).value ? 1 : 0);
  default:
    diags_.error(init.loc, "internal error: non-constant global initializer survived checking");
    return llvm::UndefValue::get(lt);
  }
}

void CodeGen::defineGlobal(const GlobalDecl &g) {
  const Symbol &sym = *g.symbol;
  llvm::Type *lt = lower(*sym.type);
  if (!lt)
    return; // erased: proof / type-level constant
  llvm::Constant *init = constantFor(*g.init, *sym.type);
  auto linkage = sym.visibility == Visibility::Public ? llvm::GlobalValue::ExternalLinkage
                                                      : llvm::GlobalValue::InternalLinkage;
  auto *gv = new llvm::GlobalVariable(*module_, lt, sym.isConst, linkage, init, sym.linkName);
  gv->setAlignment(module_->getDataLayout().getABITypeAlign(lt));
  globals_[&sym] = gv;
}

llvm::AllocaInst *CodeGen::createEntryAlloca(llvm::Type *ty, const std::string &name) {
  llvm::BasicBlock &entry = currentFn_->getEntryBlock();
  llvm::IRBuilder<> tmp(&entry, entry.begin());
  return tmp.CreateAlloca(ty, nullptr, name);
}

void CodeGen::defineFunction(const FnDecl &fn) {
  const Symbol &sym = *fn.symbol;
  llvm::Function *f = functions_.at(&sym);
  currentFn_ = f;
  currentRet_ = sym.type->ret.get();
  locals_.clear();
  values_.clear();

  llvm::BasicBlock *entry = llvm::BasicBlock::Create(*ctx_, "entry", f);
  builder_->SetInsertPoint(entry);

  unsigned argIndex = 0;
  for (size_t p = 0; p < fn.params.size(); ++p) {
    const Symbol *ps = sym.type->paramSymbols[p];
    if (!lower(*sym.type->params[p]))
      continue;
    values_[ps] = f->getArg(argIndex++);
  }

  genBlock(fn.body);

  if (!blockTerminated()) {
    if (currentRet_->kind == TypeKind::Void || !lower(*currentRet_))
      builder_->CreateRetVoid();
    else
      builder_->CreateUnreachable(); // checker proved all paths return
  }
  currentFn_ = nullptr;
  currentRet_ = nullptr;
}

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

bool CodeGen::blockTerminated() const {
  llvm::BasicBlock *bb = builder_->GetInsertBlock();
  return bb && bb->getTerminator() != nullptr;
}

void CodeGen::genBlock(const Block &block) {
  dropStacks_.emplace_back();
  for (const auto &s : block) {
    if (blockTerminated())
      break;
    genStmt(*s);
  }
  if (!blockTerminated())
    emitDrops(dropStacks_.back());
  dropStacks_.pop_back();
}

void CodeGen::genStmt(const Stmt &stmt) {
  consume_ = true;
  switch (stmt.kind) {
  case StmtKind::VarDecl: {
    const auto &d = static_cast<const VarDeclStmt &>(stmt);
    llvm::Type *lt = lower(*d.symbol->type);
    if (!lt)
      return; // erased binding (proof / interval / type)
    llvm::AllocaInst *slot = createEntryAlloca(lt, d.name);
    locals_[d.symbol] = slot;
    llvm::Value *v = gen(*d.init);
    if (v && d.symbol->shared && d.symbol->type->isStorage()) {
      auto *i64 = llvm::Type::getInt64Ty(*ctx_);
      auto *nullPtr = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*ctx_));
      llvm::Value *rc = builder_->CreateExtractValue(v, 3, "rc");
      llvm::BasicBlock *fresh = llvm::BasicBlock::Create(*ctx_, "share.fresh", currentFn_);
      llvm::BasicBlock *have = llvm::BasicBlock::Create(*ctx_, "share.have", currentFn_);
      llvm::BasicBlock *done = llvm::BasicBlock::Create(*ctx_, "share.done", currentFn_);
      builder_->CreateCondBr(builder_->CreateICmpEQ(rc, nullPtr), fresh, have);
      builder_->SetInsertPoint(fresh);
      llvm::Value *cell = builder_->CreateCall(mallocCallee(), {llvm::ConstantInt::get(i64, 8)}, "rc.cell");
      builder_->CreateStore(llvm::ConstantInt::get(i64, 1), cell);
      llvm::Value *wrapped = builder_->CreateInsertValue(v, cell, 3);
      builder_->CreateBr(done);
      builder_->SetInsertPoint(have);
      builder_->CreateBr(done);
      builder_->SetInsertPoint(done);
      llvm::PHINode *phi = builder_->CreatePHI(bufferType(), 2, "shared.buf");
      phi->addIncoming(wrapped, fresh);
      phi->addIncoming(v, have);
      v = phi;
    }
    if (v)
      builder_->CreateStore(v, slot);
    if (!dropStacks_.empty() && d.symbol->type->isStorage())
      dropStacks_.back().push_back(d.symbol);
    return;
  }
  case StmtKind::Assign: {
    const auto &a = static_cast<const AssignStmt &>(stmt);
    if (!lower(*a.symbol->type))
      return;
    llvm::Value *v = gen(*a.value);
    if (auto it = locals_.find(a.symbol); it != locals_.end())
      builder_->CreateStore(v, it->second);
    else if (auto g = globals_.find(a.symbol); g != globals_.end())
      builder_->CreateStore(v, g->second);
    return;
  }
  case StmtKind::Return: {
    const auto &r = static_cast<const ReturnStmt &>(stmt);
    for (auto it = dropStacks_.rbegin(); it != dropStacks_.rend(); ++it)
      emitDrops(*it);
    if (!r.value || !lower(*currentRet_)) {
      if (r.value)
        gen(*r.value); // erased result: evaluate for effects, return nothing
      builder_->CreateRetVoid();
      return;
    }
    builder_->CreateRet(gen(*r.value));
    return;
  }
  case StmtKind::If: {
    const auto &i = static_cast<const IfStmt &>(stmt);
    llvm::Value *cond = gen(*i.cond);
    llvm::BasicBlock *thenBB = llvm::BasicBlock::Create(*ctx_, "if.then", currentFn_);
    llvm::BasicBlock *elseBB = llvm::BasicBlock::Create(*ctx_, "if.else", currentFn_);
    llvm::BasicBlock *endBB = llvm::BasicBlock::Create(*ctx_, "if.end", currentFn_);
    builder_->CreateCondBr(cond, thenBB, elseBB);

    builder_->SetInsertPoint(thenBB);
    genBlock(i.thenBody);
    if (!blockTerminated())
      builder_->CreateBr(endBB);

    builder_->SetInsertPoint(elseBB);
    genBlock(i.elseBody);
    if (!blockTerminated())
      builder_->CreateBr(endBB);

    if (endBB->hasNPredecessors(0)) {
      endBB->eraseFromParent();
      // Both branches returned; continue in a fresh unreachable block so
      // that later statements (dead code) still have an insertion point.
      llvm::BasicBlock *dead = llvm::BasicBlock::Create(*ctx_, "if.dead", currentFn_);
      builder_->SetInsertPoint(dead);
      builder_->CreateUnreachable();
      return;
    }
    builder_->SetInsertPoint(endBB);
    return;
  }
  case StmtKind::While: {
    const auto &w = static_cast<const WhileStmt &>(stmt);
    llvm::BasicBlock *condBB = llvm::BasicBlock::Create(*ctx_, "while.cond", currentFn_);
    llvm::BasicBlock *bodyBB = llvm::BasicBlock::Create(*ctx_, "while.body", currentFn_);
    llvm::BasicBlock *endBB = llvm::BasicBlock::Create(*ctx_, "while.end", currentFn_);
    builder_->CreateBr(condBB);
    builder_->SetInsertPoint(condBB);
    builder_->CreateCondBr(gen(*w.cond), bodyBB, endBB);
    builder_->SetInsertPoint(bodyBB);
    genBlock(w.body);
    if (!blockTerminated())
      builder_->CreateBr(condBB);
    builder_->SetInsertPoint(endBB);
    return;
  }
  case StmtKind::Expr:
    gen(*static_cast<const ExprStmt &>(stmt).expr);
    return;
  case StmtKind::Arena: {
    const auto &a = static_cast<const ArenaStmt &>(stmt);
    auto *bufTy = llvm::ArrayType::get(llvm::Type::getInt8Ty(*ctx_), kArenaBytes);
    llvm::AllocaInst *buf = createEntryAlloca(bufTy, "arena.buf");
    llvm::AllocaInst *off = createEntryAlloca(llvm::Type::getInt64Ty(*ctx_), "arena.off");
    builder_->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx_), 0), off);
    llvm::Value *base = builder_->CreateInBoundsGEP(
        bufTy, buf, {llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx_), 0),
                     llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx_), 0)},
        "arena.base");
    arenas_.push_back(ArenaFrame{base, off, kArenaBytes});
    genBlock(a.body);
    arenas_.pop_back();
    return;
  }
  }
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

llvm::Value *CodeGen::gen(const Expr &e) {
  if (!e.type || !lower(*e.type)) {
    // Erased value. Only calls may have side effects worth keeping.
    if (e.kind == ExprKind::Call)
      return genCall(as<CallExpr>(e));
    return nullptr;
  }
  switch (e.kind) {
  case ExprKind::IntLit:
    return llvm::ConstantInt::get(lower(*e.type),
                                  static_cast<std::uint64_t>(as<IntLitExpr>(e).value), true);
  case ExprKind::FloatLit:
    return llvm::ConstantFP::get(lower(*e.type), as<FloatLitExpr>(e).value);
  case ExprKind::BoolLit:
    return llvm::ConstantInt::get(llvm::Type::getInt1Ty(*ctx_), as<BoolLitExpr>(e).value);
  case ExprKind::Ident:
    return genIdent(as<IdentExpr>(e));
  case ExprKind::Binary:
    return genBinary(as<BinaryExpr>(e));
  case ExprKind::Unary:
    return genUnary(as<UnaryExpr>(e));
  case ExprKind::Call:
    return genCall(as<CallExpr>(e));
  case ExprKind::Match:
    return genMatch(as<MatchExpr>(e));
  case ExprKind::StringLit:
    return genString(as<StringLitExpr>(e));
  case ExprKind::ArrayLit:
    return genArray(as<ArrayLitExpr>(e));
  case ExprKind::Lambda:
    return lowerLambda(as<LambdaExpr>(e));
  case ExprKind::Method:
    return genMethod(as<MethodCallExpr>(e));
  case ExprKind::Hcomp:
    // Over data, homogeneous composition computes to its base: the faces
    // are proof content and are erased.
    return gen(*as<HcompExpr>(e).base);
  case ExprKind::PathApp: {
    // `p @ r` has the element type, but its value is determined by the
    // path's boundary; the checker only lets this through when the type
    // checker could normalise it. Fall back to the normal form.
    ExprPtr nf = checker_.normalizer().normalize(e);
    if (nf->kind == ExprKind::PathApp) {
      diags_.error(e.loc, "cannot compile path application with a neutral path; paths are erased");
      return llvm::UndefValue::get(lower(*e.type));
    }
    return gen(*nf);
  }
  case ExprKind::TypeLit:
  case ExprKind::IntervalLit:
  case ExprKind::PathAbs:
    return nullptr;
  }
  return nullptr;
}

llvm::Value *CodeGen::genIdent(const IdentExpr &e) {
  const Symbol *sym = e.symbol;
  if (auto it = values_.find(sym); it != values_.end())
    return it->second;
  if (auto it = locals_.find(sym); it != locals_.end()) {
    llvm::Value *v = builder_->CreateLoad(it->second->getAllocatedType(), it->second, sym->name);
    if (sym->type && sym->type->isStorage() && consume_) {
      if (sym->shared) {
        llvm::Value *rc = builder_->CreateExtractValue(v, 3, "rc");
        llvm::Value *n = builder_->CreateLoad(llvm::Type::getInt64Ty(*ctx_), rc, "rc.n");
        builder_->CreateStore(
            builder_->CreateAdd(n, llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx_), 1)), rc);
      } else if (sym->type->kind != TypeKind::String) {
        builder_->CreateStore(llvm::ConstantAggregateZero::get(it->second->getAllocatedType()),
                              it->second);
      }
    }
    return v;
  }
  if (auto it = globals_.find(sym); it != globals_.end())
    return builder_->CreateLoad(it->second->getValueType(), it->second, sym->name);
  if (auto it = functions_.find(sym); it != functions_.end())
    return it->second; // function pointer
  diags_.error(e.loc, "internal error: unresolved symbol '" + sym->name + "' during codegen");
  return llvm::UndefValue::get(lower(*e.type));
}

llvm::Value *CodeGen::genBinary(const BinaryExpr &e) {
  llvm::Value *l = gen(*e.lhs);
  llvm::Value *r = gen(*e.rhs);
  const Type &operandTy = *e.lhs->type;
  const bool isFloat = operandTy.isFloat();

  switch (e.op) {
  case BinaryOp::Add: return isFloat ? builder_->CreateFAdd(l, r, "add") : builder_->CreateAdd(l, r, "add");
  case BinaryOp::Sub: return isFloat ? builder_->CreateFSub(l, r, "sub") : builder_->CreateSub(l, r, "sub");
  case BinaryOp::Mul: return isFloat ? builder_->CreateFMul(l, r, "mul") : builder_->CreateMul(l, r, "mul");
  case BinaryOp::Div: return isFloat ? builder_->CreateFDiv(l, r, "div") : builder_->CreateSDiv(l, r, "div");
  case BinaryOp::Rem: return isFloat ? builder_->CreateFRem(l, r, "rem") : builder_->CreateSRem(l, r, "rem");
  case BinaryOp::Eq: return isFloat ? builder_->CreateFCmpOEQ(l, r, "eq") : builder_->CreateICmpEQ(l, r, "eq");
  case BinaryOp::Ne: return isFloat ? builder_->CreateFCmpUNE(l, r, "ne") : builder_->CreateICmpNE(l, r, "ne");
  case BinaryOp::Lt: return isFloat ? builder_->CreateFCmpOLT(l, r, "lt") : builder_->CreateICmpSLT(l, r, "lt");
  case BinaryOp::Le: return isFloat ? builder_->CreateFCmpOLE(l, r, "le") : builder_->CreateICmpSLE(l, r, "le");
  case BinaryOp::Gt: return isFloat ? builder_->CreateFCmpOGT(l, r, "gt") : builder_->CreateICmpSGT(l, r, "gt");
  case BinaryOp::Ge: return isFloat ? builder_->CreateFCmpOGE(l, r, "ge") : builder_->CreateICmpSGE(l, r, "ge");
  case BinaryOp::And: return builder_->CreateAnd(l, r, "and");
  case BinaryOp::Or: return builder_->CreateOr(l, r, "or");
  case BinaryOp::Meet:
  case BinaryOp::Join:
    return nullptr; // interval algebra is erased
  }
  return nullptr;
}

llvm::Value *CodeGen::genUnary(const UnaryExpr &e) {
  llvm::Value *x = gen(*e.operand);
  switch (e.op) {
  case UnaryOp::Neg:
    return e.type->isFloat() ? builder_->CreateFNeg(x, "neg") : builder_->CreateNeg(x, "neg");
  case UnaryOp::Not:
    return builder_->CreateNot(x, "not");
  case UnaryOp::IntervalRev:
    return nullptr;
  }
  return nullptr;
}

llvm::Value *CodeGen::genCall(const CallExpr &e) {
  if (e.builtin != Builtin::None)
    return genBuiltin(e);

  if (e.fn && e.fn->kind == SymbolKind::Ctor)
    return genCtorCall(e, *e.fn);

  // Resolve the callee: direct function or a function-pointer value.
  const Type *fnType = e.callee->type.get();
  llvm::Value *calleeV = nullptr;
  if (e.fn) {
    calleeV = functions_.at(e.fn);
    fnType = e.fn->type.get();
  } else {
    calleeV = gen(*e.callee);
  }
  llvm::FunctionType *fty = lowerFnType(*fnType);

  std::vector<llvm::Value *> args;
  for (size_t i = 0; i < e.args.size(); ++i) {
    llvm::Value *v = gen(*e.args[i]);
    if (lower(*fnType->params[i]))
      args.push_back(v); // erased arguments are dropped
  }
  llvm::CallInst *call = builder_->CreateCall(fty, calleeV, args);
  call->setCallingConv(llvm::CallingConv::C);
  if (fty->getReturnType()->isVoidTy())
    return nullptr;
  return call;
}

llvm::Value *CodeGen::genBuiltin(const CallExpr &e) {
  switch (e.builtin) {
  case Builtin::Print:
    return genPrint(*e.args[0]);
  case Builtin::Transport: {
    // Computational univalence, erased to a plain call.
    llvm::Value *payload = gen(*e.args[1]);
    if (e.transportDir == TransportDir::Identity || !e.transportFn)
      return payload;
    llvm::Function *f = functions_.at(e.transportFn);
    llvm::CallInst *call = builder_->CreateCall(f->getFunctionType(), f, {payload}, "transport");
    call->setCallingConv(llvm::CallingConv::C);
    return call;
  }
  case Builtin::Refl:
  case Builtin::Sym:
  case Builtin::Ua:
  case Builtin::Iso:
  case Builtin::None:
    return nullptr; // pure proof content
  }
  return nullptr;
}

llvm::Value *CodeGen::genCtorCall(const CallExpr &e, const Symbol &ctor) {
  if (ctor.isPathCtor)
    return nullptr; // path constructors are erased
  llvm::StructType *self = inductives_.at(ctor.owner);
  llvm::StructType *payload = ctorPayloads_.at(&ctor);

  llvm::AllocaInst *tmp = createEntryAlloca(self, ctor.owner->name + ".tmp");
  llvm::Value *tagPtr = builder_->CreateStructGEP(self, tmp, 0, "tag.ptr");
  builder_->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt32Ty(*ctx_), ctor.ctorIndex), tagPtr);

  if (payload->getNumElements() > 0) {
    llvm::Value *payloadPtr = builder_->CreateStructGEP(self, tmp, 1, "payload.ptr");
    unsigned field = 0;
    for (size_t i = 0; i < e.args.size(); ++i) {
      llvm::Value *v = gen(*e.args[i]);
      if (!v)
        continue;
      llvm::Value *fieldPtr = builder_->CreateStructGEP(payload, payloadPtr, field++, "field.ptr");
      builder_->CreateStore(v, fieldPtr);
    }
  }
  return builder_->CreateLoad(self, tmp, ctor.name);
}

llvm::Value *CodeGen::genMatch(const MatchExpr &e) {
  const Symbol &typeSym = *e.scrutinee->type->inductive;
  llvm::StructType *self = inductives_.at(&typeSym);
  llvm::Value *scrut = gen(*e.scrutinee);
  llvm::AllocaInst *tmp = createEntryAlloca(self, "match.tmp");
  builder_->CreateStore(scrut, tmp);
  llvm::Value *tag = builder_->CreateLoad(llvm::Type::getInt32Ty(*ctx_),
                                          builder_->CreateStructGEP(self, tmp, 0), "tag");

  llvm::Type *resultTy = e.type ? lower(*e.type) : nullptr;
  const bool produceValue = resultTy && !resultTy->isVoidTy();
  llvm::BasicBlock *endBB = llvm::BasicBlock::Create(*ctx_, "match.end", currentFn_);
  llvm::BasicBlock *deadBB = llvm::BasicBlock::Create(*ctx_, "match.unreachable", currentFn_);
  llvm::SwitchInst *sw = builder_->CreateSwitch(tag, deadBB, static_cast<unsigned>(e.cases.size()));

  std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> incoming;
  for (const MatchCase &c : e.cases) {
    llvm::BasicBlock *caseBB = llvm::BasicBlock::Create(*ctx_, "case." + c.ctor, currentFn_);
    sw->addCase(llvm::ConstantInt::get(llvm::Type::getInt32Ty(*ctx_), c.ctorSymbol->ctorIndex), caseBB);
    builder_->SetInsertPoint(caseBB);

    llvm::StructType *payload = ctorPayloads_.at(c.ctorSymbol);
    if (payload->getNumElements() > 0) {
      llvm::Value *payloadPtr = builder_->CreateStructGEP(self, tmp, 1);
      unsigned field = 0;
      for (size_t i = 0; i < c.binderSymbols.size(); ++i) {
        const Symbol *b = c.binderSymbols[i];
        llvm::Type *bt = lower(*b->type);
        if (!bt)
          continue;
        llvm::Value *castPayload = builder_->CreatePointerCast(payloadPtr, llvm::PointerType::getUnqual(*ctx_));
        llvm::Value *fieldPtr = builder_->CreateStructGEP(payload, castPayload, field++);
        values_[b] = builder_->CreateLoad(bt, fieldPtr, b->name);
      }
    }
    llvm::Value *v = gen(*c.body);
    if (!blockTerminated()) {
      incoming.emplace_back(produceValue ? v : nullptr, builder_->GetInsertBlock());
      builder_->CreateBr(endBB);
    }
  }

  builder_->SetInsertPoint(deadBB);
  builder_->CreateUnreachable();

  builder_->SetInsertPoint(endBB);
  if (!produceValue)
    return nullptr;
  llvm::PHINode *phi = builder_->CreatePHI(resultTy, static_cast<unsigned>(incoming.size()), "match");
  for (auto &[v, bb] : incoming)
    phi->addIncoming(v, bb);
  return phi;
}

llvm::FunctionCallee CodeGen::printfCallee() {
  llvm::FunctionType *fty = llvm::FunctionType::get(
      llvm::Type::getInt32Ty(*ctx_), {llvm::PointerType::getUnqual(*ctx_)}, /*isVarArg=*/true);
  llvm::FunctionCallee callee = module_->getOrInsertFunction("printf", fty);
  if (auto *f = llvm::dyn_cast<llvm::Function>(callee.getCallee()))
    f->setCallingConv(llvm::CallingConv::C);
  return callee;
}

llvm::Constant *CodeGen::globalString(const std::string &s, const std::string &name) {
  auto it = strings_.find(s);
  if (it != strings_.end())
    return it->second;
  // With opaque pointers the global itself is the `ptr` we pass to printf.
  llvm::Constant *c = builder_->CreateGlobalString(s, name, 0, module_.get());
  strings_[s] = c;
  return c;
}

llvm::Value *CodeGen::genPrint(const Expr &arg) {
  llvm::Value *v = gen(arg);
  const Type &t = *arg.type;
  llvm::FunctionCallee pf = printfCallee();
  std::vector<llvm::Value *> args;
  switch (t.kind) {
  case TypeKind::I32:
    args = {globalString("%d\n", ".fmt.i32"), v};
    break;
  case TypeKind::I64:
    args = {globalString("%lld\n", ".fmt.i64"), v};
    break;
  case TypeKind::F32:
    args = {globalString("%f\n", ".fmt.f"), builder_->CreateFPExt(v, llvm::Type::getDoubleTy(*ctx_))};
    break;
  case TypeKind::F64:
    args = {globalString("%f\n", ".fmt.f"), v};
    break;
  case TypeKind::Bool: {
    llvm::Value *s = builder_->CreateSelect(v, globalString("true", ".str.true"),
                                            globalString("false", ".str.false"));
    args = {globalString("%s\n", ".fmt.s"), s};
    break;
  }
  default:
    return nullptr;
  }
  llvm::CallInst *call = builder_->CreateCall(pf, args);
  call->setCallingConv(llvm::CallingConv::C);
  return nullptr;
}

// ---------------------------------------------------------------------------
// Arenas, buffers, lambdas, collections
// ---------------------------------------------------------------------------

llvm::StructType *CodeGen::bufferType() {
  if (!bufferTy_) {
    bufferTy_ = llvm::StructType::create(
        *ctx_,
        {llvm::PointerType::getUnqual(*ctx_), llvm::Type::getInt64Ty(*ctx_),
         llvm::Type::getInt64Ty(*ctx_), llvm::PointerType::getUnqual(*ctx_)},
        "cruft.buf");
  }
  return bufferTy_;
}

llvm::FunctionCallee CodeGen::mallocCallee() {
  llvm::FunctionType *fty = llvm::FunctionType::get(
      llvm::PointerType::getUnqual(*ctx_), {llvm::Type::getInt64Ty(*ctx_)}, false);
  llvm::FunctionCallee c = module_->getOrInsertFunction("malloc", fty);
  if (auto *f = llvm::dyn_cast<llvm::Function>(c.getCallee()))
    f->setCallingConv(llvm::CallingConv::C);
  return c;
}

llvm::FunctionCallee CodeGen::freeCallee() {
  llvm::FunctionType *fty = llvm::FunctionType::get(
      llvm::Type::getVoidTy(*ctx_), {llvm::PointerType::getUnqual(*ctx_)}, false);
  llvm::FunctionCallee c = module_->getOrInsertFunction("free", fty);
  if (auto *f = llvm::dyn_cast<llvm::Function>(c.getCallee()))
    f->setCallingConv(llvm::CallingConv::C);
  return c;
}

llvm::Value *CodeGen::allocBytes(llvm::Value *nbytes, unsigned align) {
  if (align == 0)
    align = 1;
  auto *i64 = llvm::Type::getInt64Ty(*ctx_);
  if (!arenas_.empty()) {
    ArenaFrame &frame = arenas_.back();
    llvm::Value *off = builder_->CreateLoad(i64, frame.offset, "arena.off");
    llvm::Value *mask = llvm::ConstantInt::get(i64, align - 1);
    llvm::Value *aligned =
        builder_->CreateAnd(builder_->CreateAdd(off, mask), builder_->CreateNot(mask), "arena.aligned");
    llvm::Value *end = builder_->CreateAdd(aligned, nbytes, "arena.end");
    llvm::Value *overflow =
        builder_->CreateICmpUGT(end, llvm::ConstantInt::get(i64, frame.capacity), "arena.overflow");
    llvm::BasicBlock *ok = llvm::BasicBlock::Create(*ctx_, "arena.ok", currentFn_);
    llvm::BasicBlock *bad = llvm::BasicBlock::Create(*ctx_, "arena.bad", currentFn_);
    builder_->CreateCondBr(overflow, bad, ok);
    builder_->SetInsertPoint(bad);
    llvm::Function *trap = llvm::Intrinsic::getDeclaration(module_.get(), llvm::Intrinsic::trap);
    builder_->CreateCall(trap);
    builder_->CreateUnreachable();
    builder_->SetInsertPoint(ok);
    builder_->CreateStore(end, frame.offset);
    return builder_->CreateInBoundsGEP(llvm::Type::getInt8Ty(*ctx_), frame.base, aligned, "arena.ptr");
  }
  return builder_->CreateCall(mallocCallee(), {nbytes}, "heap");
}

llvm::Value *CodeGen::makeBuffer(llvm::Value *data, llvm::Value *len, llvm::Value *cap, llvm::Value *rc) {
  auto *i64 = llvm::Type::getInt64Ty(*ctx_);
  if (!data)
    data = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*ctx_));
  if (!len)
    len = llvm::ConstantInt::get(i64, 0);
  if (!cap)
    cap = len;
  if (!rc)
    rc = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*ctx_));
  llvm::Value *buf = llvm::UndefValue::get(bufferType());
  buf = builder_->CreateInsertValue(buf, data, 0);
  buf = builder_->CreateInsertValue(buf, len, 1);
  buf = builder_->CreateInsertValue(buf, cap, 2);
  buf = builder_->CreateInsertValue(buf, rc, 3);
  return buf;
}

void CodeGen::emitDrops(const std::vector<const Symbol *> &syms) {
  for (auto it = syms.rbegin(); it != syms.rend(); ++it)
    dropSlot(**it);
}

void CodeGen::dropSlot(const Symbol &sym) {
  if (!sym.type || !sym.type->isStorage() || sym.type->kind == TypeKind::String)
    return;
  auto found = locals_.find(&sym);
  if (found == locals_.end())
    return;
  auto *i64 = llvm::Type::getInt64Ty(*ctx_);
  auto *i1 = llvm::Type::getInt1Ty(*ctx_);
  llvm::Value *buf = builder_->CreateLoad(bufferType(), found->second, "drop.buf");
  llvm::Value *data = builder_->CreateExtractValue(buf, 0, "drop.data");
  llvm::Value *rc = builder_->CreateExtractValue(buf, 3, "drop.rc");
  llvm::BasicBlock *done = llvm::BasicBlock::Create(*ctx_, "drop.done", currentFn_);
  llvm::BasicBlock *live = llvm::BasicBlock::Create(*ctx_, "drop.live", currentFn_);
  builder_->CreateCondBr(
      builder_->CreateICmpEQ(data, llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*ctx_))),
      done, live);
  builder_->SetInsertPoint(live);
  llvm::BasicBlock *freeData = llvm::BasicBlock::Create(*ctx_, "drop.free", currentFn_);
  llvm::BasicBlock *decRc = llvm::BasicBlock::Create(*ctx_, "drop.dec", currentFn_);
  builder_->CreateCondBr(
      builder_->CreateICmpEQ(rc, llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*ctx_))),
      freeData, decRc);
  builder_->SetInsertPoint(decRc);
  llvm::Value *count = builder_->CreateLoad(i64, rc, "rc.n");
  llvm::Value *next = builder_->CreateSub(count, llvm::ConstantInt::get(i64, 1));
  builder_->CreateStore(next, rc);
  llvm::BasicBlock *last = llvm::BasicBlock::Create(*ctx_, "drop.last", currentFn_);
  builder_->CreateCondBr(builder_->CreateICmpEQ(next, llvm::ConstantInt::get(i64, 0)), last, done);
  builder_->SetInsertPoint(last);
  builder_->CreateCall(freeCallee(), {rc});
  builder_->CreateBr(freeData);
  builder_->SetInsertPoint(freeData);
  llvm::Value *inArena = llvm::ConstantInt::get(i1, 0);
  for (const ArenaFrame &frame : arenas_) {
    llvm::Value *limit = builder_->CreateInBoundsGEP(
        llvm::Type::getInt8Ty(*ctx_), frame.base, llvm::ConstantInt::get(i64, frame.capacity));
    llvm::Value *ge = builder_->CreateICmpUGE(data, frame.base);
    llvm::Value *lt = builder_->CreateICmpULT(data, limit);
    inArena = builder_->CreateOr(inArena, builder_->CreateAnd(ge, lt));
  }
  llvm::BasicBlock *doFree = llvm::BasicBlock::Create(*ctx_, "drop.heap", currentFn_);
  builder_->CreateCondBr(inArena, done, doFree);
  builder_->SetInsertPoint(doFree);
  builder_->CreateCall(freeCallee(), {data});
  builder_->CreateBr(done);
  builder_->SetInsertPoint(done);
}

llvm::Function *CodeGen::lowerLambda(const LambdaExpr &lam) {
  llvm::FunctionType *fty = lowerFnType(*lam.type);
  llvm::Function *fn = llvm::Function::Create(fty, llvm::Function::InternalLinkage,
                                              "lambda." + std::to_string(++lambdaSeq_), module_.get());
  fn->setCallingConv(llvm::CallingConv::C);
  fn->addFnAttr(llvm::Attribute::NoUnwind);

  llvm::Function *savedFn = currentFn_;
  llvm::BasicBlock *savedBB = builder_->GetInsertBlock();
  auto savedValues = std::move(values_);
  bool savedConsume = consume_;
  values_.clear();
  consume_ = true;
  currentFn_ = fn;
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(*ctx_, "entry", fn);
  builder_->SetInsertPoint(entry);
  unsigned arg = 0;
  for (size_t i = 0; i < lam.paramSymbols.size(); ++i) {
    const Symbol *ps = lam.paramSymbols[i];
    if (!ps || !ps->type || !lower(*ps->type))
      continue;
    fn->getArg(arg)->setName(lam.params[i].name);
    values_[ps] = fn->getArg(arg++);
  }
  llvm::Value *result = gen(*lam.body);
  if (!blockTerminated()) {
    if (!lam.type->ret || !lower(*lam.type->ret) || lam.type->ret->kind == TypeKind::Void)
      builder_->CreateRetVoid();
    else
      builder_->CreateRet(result);
  }
  values_ = std::move(savedValues);
  consume_ = savedConsume;
  currentFn_ = savedFn;
  if (savedBB)
    builder_->SetInsertPoint(savedBB);
  return fn;
}

llvm::Value *CodeGen::genArray(const ArrayLitExpr &e) {
  auto *i64 = llvm::Type::getInt64Ty(*ctx_);
  if (e.type->kind == TypeKind::Map || e.elems.empty())
    return makeBuffer(nullptr, llvm::ConstantInt::get(i64, 0), llvm::ConstantInt::get(i64, 0), nullptr);
  const Type &elem = *e.type->elem;
  llvm::Type *elt = lower(elem);
  const llvm::DataLayout &dl = module_->getDataLayout();
  std::uint64_t esize = dl.getTypeAllocSize(elt);
  unsigned align = static_cast<unsigned>(dl.getABITypeAlign(elt).value());
  llvm::Value *bytes = llvm::ConstantInt::get(i64, esize * e.elems.size());
  llvm::Value *data = allocBytes(bytes, align);
  for (size_t i = 0; i < e.elems.size(); ++i) {
    llvm::Value *slot = builder_->CreateInBoundsGEP(elt, data, llvm::ConstantInt::get(i64, i));
    builder_->CreateStore(gen(*e.elems[i]), slot);
  }
  llvm::Value *n = llvm::ConstantInt::get(i64, e.elems.size());
  return makeBuffer(data, n, n, nullptr);
}

llvm::Value *CodeGen::genString(const StringLitExpr &e) {
  auto *i64 = llvm::Type::getInt64Ty(*ctx_);
  llvm::Value *data = globalString(e.value, ".str.lit");
  llvm::Value *n = llvm::ConstantInt::get(i64, e.value.size());
  return makeBuffer(data, n, n, nullptr);
}

llvm::Value *CodeGen::genMethod(const MethodCallExpr &m) {
  bool saved = consume_;
  consume_ = false;
  llvm::Value *recv = gen(*m.receiver);
  consume_ = saved;
  const Type &recvTy = *m.receiver->type;
  auto *i64 = llvm::Type::getInt64Ty(*ctx_);
  auto *i32 = llvm::Type::getInt32Ty(*ctx_);

  auto lengthOf = [&](llvm::Value *buf) {
    return builder_->CreateTrunc(builder_->CreateExtractValue(buf, 1, "len64"), i32, "len");
  };

  if (recvTy.kind == TypeKind::String && m.method == "length")
    return lengthOf(recv);
  if ((recvTy.kind == TypeKind::Vector || recvTy.kind == TypeKind::Map) && m.method == "length")
    return lengthOf(recv);

  if (recvTy.kind == TypeKind::Vector && m.method == "sum") {
    llvm::Type *elt = lower(*recvTy.elem);
    llvm::AllocaInst *acc = createEntryAlloca(elt, "sum");
    builder_->CreateStore(llvm::Constant::getNullValue(elt), acc);
    llvm::Value *data = builder_->CreateExtractValue(recv, 0);
    llvm::Value *len = builder_->CreateExtractValue(recv, 1);
    llvm::AllocaInst *idx = createEntryAlloca(i64, "i");
    builder_->CreateStore(llvm::ConstantInt::get(i64, 0), idx);
    llvm::BasicBlock *cond = llvm::BasicBlock::Create(*ctx_, "sum.cond", currentFn_);
    llvm::BasicBlock *body = llvm::BasicBlock::Create(*ctx_, "sum.body", currentFn_);
    llvm::BasicBlock *end = llvm::BasicBlock::Create(*ctx_, "sum.end", currentFn_);
    builder_->CreateBr(cond);
    builder_->SetInsertPoint(cond);
    llvm::Value *i = builder_->CreateLoad(i64, idx);
    builder_->CreateCondBr(builder_->CreateICmpULT(i, len), body, end);
    builder_->SetInsertPoint(body);
    llvm::Value *slot = builder_->CreateInBoundsGEP(elt, data, i);
    llvm::Value *el = builder_->CreateLoad(elt, slot);
    llvm::Value *cur = builder_->CreateLoad(elt, acc);
    llvm::Value *next = recvTy.elem->isFloat() ? builder_->CreateFAdd(cur, el) : builder_->CreateAdd(cur, el);
    builder_->CreateStore(next, acc);
    builder_->CreateStore(builder_->CreateAdd(i, llvm::ConstantInt::get(i64, 1)), idx);
    builder_->CreateBr(cond);
    builder_->SetInsertPoint(end);
    return builder_->CreateLoad(elt, acc, "sum");
  }

  if (recvTy.kind == TypeKind::Vector && (m.method == "filter" || m.method == "map")) {
    bool filtering = m.method == "filter";
    llvm::Function *pred = lowerLambda(as<LambdaExpr>(*m.args[0]));
    const Type &srcElem = *recvTy.elem;
    const Type &dstElem = filtering ? srcElem : *m.type->elem;
    llvm::Type *srcTy = lower(srcElem);
    llvm::Type *dstTy = lower(dstElem);
    const llvm::DataLayout &dl = module_->getDataLayout();
    unsigned align = static_cast<unsigned>(dl.getABITypeAlign(dstTy).value());
    std::uint64_t dsize = dl.getTypeAllocSize(dstTy);
    llvm::Value *data = builder_->CreateExtractValue(recv, 0);
    llvm::Value *len = builder_->CreateExtractValue(recv, 1);
    llvm::Value *bytes = builder_->CreateMul(len, llvm::ConstantInt::get(i64, dsize));
    llvm::Value *out = allocBytes(bytes, align);
    llvm::AllocaInst *written = createEntryAlloca(i64, "written");
    builder_->CreateStore(llvm::ConstantInt::get(i64, 0), written);
    llvm::AllocaInst *idx = createEntryAlloca(i64, "i");
    builder_->CreateStore(llvm::ConstantInt::get(i64, 0), idx);
    llvm::BasicBlock *cond = llvm::BasicBlock::Create(*ctx_, "vec.cond", currentFn_);
    llvm::BasicBlock *body = llvm::BasicBlock::Create(*ctx_, "vec.body", currentFn_);
    llvm::BasicBlock *end = llvm::BasicBlock::Create(*ctx_, "vec.end", currentFn_);
    builder_->CreateBr(cond);
    builder_->SetInsertPoint(cond);
    llvm::Value *i = builder_->CreateLoad(i64, idx);
    builder_->CreateCondBr(builder_->CreateICmpULT(i, len), body, end);
    builder_->SetInsertPoint(body);
    llvm::Value *srcSlot = builder_->CreateInBoundsGEP(srcTy, data, i);
    llvm::Value *el = builder_->CreateLoad(srcTy, srcSlot);
    llvm::CallInst *called = builder_->CreateCall(pred->getFunctionType(), pred, {el});
    called->setCallingConv(llvm::CallingConv::C);
    if (filtering) {
      llvm::BasicBlock *keep = llvm::BasicBlock::Create(*ctx_, "vec.keep", currentFn_);
      llvm::BasicBlock *next = llvm::BasicBlock::Create(*ctx_, "vec.next", currentFn_);
      builder_->CreateCondBr(called, keep, next);
      builder_->SetInsertPoint(keep);
      llvm::Value *n = builder_->CreateLoad(i64, written);
      builder_->CreateStore(el, builder_->CreateInBoundsGEP(dstTy, out, n));
      builder_->CreateStore(builder_->CreateAdd(n, llvm::ConstantInt::get(i64, 1)), written);
      builder_->CreateBr(next);
      builder_->SetInsertPoint(next);
    } else {
      builder_->CreateStore(called, builder_->CreateInBoundsGEP(dstTy, out, i));
    }
    builder_->CreateStore(builder_->CreateAdd(builder_->CreateLoad(i64, idx), llvm::ConstantInt::get(i64, 1)),
                          idx);
    builder_->CreateBr(cond);
    builder_->SetInsertPoint(end);
    llvm::Value *n = filtering ? builder_->CreateLoad(i64, written) : len;
    return makeBuffer(out, n, len, nullptr);
  }

  if (recvTy.kind == TypeKind::Map && (m.method == "insert" || m.method == "contains")) {
    const Type &keyTy = *recvTy.a;
    const Type &valTy = *recvTy.b;
    llvm::Type *kll = lower(keyTy);
    llvm::Type *vll = lower(valTy);
    const llvm::DataLayout &dl = module_->getDataLayout();
    std::uint64_t keySize = dl.getTypeAllocSize(kll);
    std::uint64_t valAlign = dl.getABITypeAlign(vll).value();
    std::uint64_t valSize = dl.getTypeAllocSize(vll);
    std::uint64_t valOff = (keySize + valAlign - 1) & ~(valAlign - 1);
    std::uint64_t strideAlign = std::max(dl.getABITypeAlign(kll).value(), valAlign);
    std::uint64_t stride = (valOff + valSize + strideAlign - 1) & ~(strideAlign - 1);
    auto *i8 = llvm::Type::getInt8Ty(*ctx_);

    const auto &id = as<IdentExpr>(*m.receiver);
    llvm::AllocaInst *slot = locals_.at(id.symbol);
    auto loadField = [&](llvm::Value *base, std::uint64_t offset, llvm::Type *ty) {
      llvm::Value *p = builder_->CreateInBoundsGEP(i8, base, llvm::ConstantInt::get(i64, offset));
      return builder_->CreateLoad(ty, p);
    };

    if (m.method == "contains") {
      llvm::Value *key = gen(*m.args[0]);
      llvm::Value *buf = builder_->CreateLoad(bufferType(), slot);
      llvm::Value *data = builder_->CreateExtractValue(buf, 0);
      llvm::Value *len = builder_->CreateExtractValue(buf, 1);
      llvm::AllocaInst *idx = createEntryAlloca(i64, "mi");
      builder_->CreateStore(llvm::ConstantInt::get(i64, 0), idx);
      llvm::BasicBlock *cond = llvm::BasicBlock::Create(*ctx_, "has.cond", currentFn_);
      llvm::BasicBlock *body = llvm::BasicBlock::Create(*ctx_, "has.body", currentFn_);
      llvm::BasicBlock *end = llvm::BasicBlock::Create(*ctx_, "has.end", currentFn_);
      builder_->CreateBr(cond);
      builder_->SetInsertPoint(cond);
      llvm::Value *i = builder_->CreateLoad(i64, idx);
      builder_->CreateCondBr(builder_->CreateICmpULT(i, len), body, end);
      builder_->SetInsertPoint(body);
      llvm::Value *entry = builder_->CreateInBoundsGEP(i8, data, builder_->CreateMul(i, llvm::ConstantInt::get(i64, stride)));
      llvm::Value *have = loadField(entry, 0, kll);
      llvm::Value *eq = keyTy.isFloat() ? builder_->CreateFCmpOEQ(have, key) : builder_->CreateICmpEQ(have, key);
      llvm::BasicBlock *yes = llvm::BasicBlock::Create(*ctx_, "has.yes", currentFn_);
      llvm::BasicBlock *no = llvm::BasicBlock::Create(*ctx_, "has.no", currentFn_);
      builder_->CreateCondBr(eq, yes, no);
      builder_->SetInsertPoint(no);
      builder_->CreateStore(builder_->CreateAdd(i, llvm::ConstantInt::get(i64, 1)), idx);
      builder_->CreateBr(cond);
      builder_->SetInsertPoint(yes);
      builder_->CreateBr(end);
      builder_->SetInsertPoint(end);
      llvm::PHINode *phi = builder_->CreatePHI(llvm::Type::getInt1Ty(*ctx_), 2, "contains");
      phi->addIncoming(llvm::ConstantInt::getTrue(*ctx_), yes);
      phi->addIncoming(llvm::ConstantInt::getFalse(*ctx_), cond);
      return phi;
    }

    llvm::Value *key = gen(*m.args[0]);
    llvm::Value *val = gen(*m.args[1]);
    llvm::Value *buf = builder_->CreateLoad(bufferType(), slot);
    llvm::Value *data = builder_->CreateExtractValue(buf, 0);
    llvm::Value *len = builder_->CreateExtractValue(buf, 1);
    llvm::Value *cap = builder_->CreateExtractValue(buf, 2);
    llvm::Value *full = builder_->CreateICmpEQ(len, cap);
    llvm::BasicBlock *pre = builder_->GetInsertBlock();
    llvm::BasicBlock *grow = llvm::BasicBlock::Create(*ctx_, "map.grow", currentFn_);
    llvm::BasicBlock *put = llvm::BasicBlock::Create(*ctx_, "map.put", currentFn_);
    builder_->CreateCondBr(full, grow, put);
    builder_->SetInsertPoint(grow);
    llvm::Value *newCap = builder_->CreateSelect(
        builder_->CreateICmpEQ(cap, llvm::ConstantInt::get(i64, 0)), llvm::ConstantInt::get(i64, 8),
        builder_->CreateMul(cap, llvm::ConstantInt::get(i64, 2)));
    llvm::Value *newData = allocBytes(builder_->CreateMul(newCap, llvm::ConstantInt::get(i64, stride)),
                                      static_cast<unsigned>(strideAlign));
    llvm::AllocaInst *ci = createEntryAlloca(i64, "copy.i");
    builder_->CreateStore(llvm::ConstantInt::get(i64, 0), ci);
    llvm::Value *nbytes = builder_->CreateMul(len, llvm::ConstantInt::get(i64, stride));
    llvm::BasicBlock *ccond = llvm::BasicBlock::Create(*ctx_, "copy.cond", currentFn_);
    llvm::BasicBlock *cbody = llvm::BasicBlock::Create(*ctx_, "copy.body", currentFn_);
    llvm::BasicBlock *cend = llvm::BasicBlock::Create(*ctx_, "copy.end", currentFn_);
    builder_->CreateBr(ccond);
    builder_->SetInsertPoint(ccond);
    llvm::Value *ciV = builder_->CreateLoad(i64, ci);
    builder_->CreateCondBr(builder_->CreateICmpULT(ciV, nbytes), cbody, cend);
    builder_->SetInsertPoint(cbody);
    builder_->CreateStore(builder_->CreateLoad(i8, builder_->CreateInBoundsGEP(i8, data, ciV)),
                          builder_->CreateInBoundsGEP(i8, newData, ciV));
    builder_->CreateStore(builder_->CreateAdd(ciV, llvm::ConstantInt::get(i64, 1)), ci);
    builder_->CreateBr(ccond);
    builder_->SetInsertPoint(cend);
    llvm::Value *grown = makeBuffer(newData, len, newCap, builder_->CreateExtractValue(buf, 3));
    builder_->CreateStore(grown, slot);
    builder_->CreateBr(put);
    builder_->SetInsertPoint(put);
    llvm::PHINode *curData = builder_->CreatePHI(llvm::PointerType::getUnqual(*ctx_), 2, "map.data");
    llvm::PHINode *curCap = builder_->CreatePHI(i64, 2, "map.cap");
    curData->addIncoming(data, pre);
    curData->addIncoming(newData, cend);
    curCap->addIncoming(cap, pre);
    curCap->addIncoming(newCap, cend);
    llvm::Value *dest = builder_->CreateInBoundsGEP(
        i8, curData, builder_->CreateMul(len, llvm::ConstantInt::get(i64, stride)));
    builder_->CreateStore(key, dest);
    builder_->CreateStore(val, builder_->CreateInBoundsGEP(i8, dest, llvm::ConstantInt::get(i64, valOff)));
    llvm::Value *newLen = builder_->CreateAdd(len, llvm::ConstantInt::get(i64, 1));
    llvm::Value *stored = makeBuffer(curData, newLen, curCap, builder_->CreateExtractValue(buf, 3));
    builder_->CreateStore(stored, slot);
    (void)curCap;
    return nullptr;
  }
  return nullptr;
}

} // namespace cruft
