// CodeGen.cpp - LLVM IR generation. See CodeGen.h for the ABI/erasure contract.
#include "codegen/CodeGen.h"
#include "Target.h"

#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/GlobalAlias.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/PassManager.h>
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

/// Buffers are dropped because they own memory. A struct with a user `drop`
/// is dropped even when every field is a scalar.
bool slotNeedsDrop(const sema::Type &type) {
  if (type.isStorage())
    return true;
  return type.kind == TypeKind::Struct && type.inductive && type.inductive->dropFn;
}

const Symbol *nullaryCtor(const Expr &e) {
  if (e.kind != ExprKind::Call)
    return nullptr;
  const auto &c = as<CallExpr>(e);
  if (!c.fn || c.fn->kind != SymbolKind::Ctor || !c.args.empty())
    return nullptr;
  return c.fn;
}

llvm::AtomicOrdering atomicOrder(const Expr &e) {
  const Symbol *ctor = nullaryCtor(e);
  if (!ctor)
    return llvm::AtomicOrdering::SequentiallyConsistent;
  if (ctor->name == "Relaxed")
    return llvm::AtomicOrdering::Monotonic;
  if (ctor->name == "Acquire")
    return llvm::AtomicOrdering::Acquire;
  if (ctor->name == "Release")
    return llvm::AtomicOrdering::Release;
  if (ctor->name == "AcqRel")
    return llvm::AtomicOrdering::AcquireRelease;
  return llvm::AtomicOrdering::SequentiallyConsistent;
}

llvm::AtomicRMWInst::BinOp rmwOp(const Expr &e) {
  const Symbol *ctor = nullaryCtor(e);
  if (!ctor)
    return llvm::AtomicRMWInst::Xchg;
  if (ctor->name == "Add")
    return llvm::AtomicRMWInst::Add;
  if (ctor->name == "Sub")
    return llvm::AtomicRMWInst::Sub;
  if (ctor->name == "And")
    return llvm::AtomicRMWInst::And;
  if (ctor->name == "Or")
    return llvm::AtomicRMWInst::Or;
  if (ctor->name == "Xor")
    return llvm::AtomicRMWInst::Xor;
  return llvm::AtomicRMWInst::Xchg;
}
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

  // Same canonical triple `cfg` already selected on.
  std::string triple = TargetFacts::from(options_.targetTriple).triple;
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

std::string CodeGen::specKey(const Type &t) const {
  std::string key = t.inductive ? t.inductive->qualifiedName() : std::string("?");
  for (const auto &arg : t.typeArgs) {
    const Type *use = arg.get();
    if (use && use->kind == TypeKind::TyVar && use->tyVar) {
      auto it = tySubst_.find(use->tyVar);
      if (it != tySubst_.end() && it->second)
        use = it->second.get();
    }
    key += "/" + (use ? use->str() : std::string("?"));
  }
  return key;
}

llvm::StructType *CodeGen::specializedInductive(const Type &t) {
  std::string key = specKey(t);
  if (auto it = specStructs_.find(key); it != specStructs_.end())
    return it->second;
  auto *self = llvm::StructType::create(*ctx_, key);
  specStructs_[key] = self;
  if (!t.inductive)
    return self;
  const llvm::DataLayout &dl = module_->getDataLayout();
  std::uint64_t maxSize = 0;
  std::uint64_t maxAlign = 1;
  for (const Symbol *ctor : t.inductive->ctors) {
    if (!ctor || ctor->isPathCtor || !ctor->type)
      continue;
    std::vector<llvm::Type *> fields;
    if (ctor->type->kind == TypeKind::Fn) {
      for (const auto &p : ctor->type->params) {
        sema::TypePtr concrete =
            checker_.applyBinders(p, t.inductive->typeBinders, t.typeArgs);
        if (concrete)
          if (llvm::Type *ft = lower(*concrete))
            fields.push_back(ft);
      }
    }
    if (fields.empty())
      continue;
    auto *payload = llvm::StructType::create(*ctx_, fields, key + "." + ctor->name);
    std::uint64_t sz = dl.getTypeAllocSize(payload);
    std::uint64_t al = dl.getABITypeAlign(payload).value();
    maxSize = std::max(maxSize, sz);
    maxAlign = std::max(maxAlign, al);
  }
  std::vector<llvm::Type *> body{llvm::Type::getInt32Ty(*ctx_)};
  if (maxSize > 0) {
    llvm::Type *unit = llvm::Type::getIntNTy(*ctx_, static_cast<unsigned>(maxAlign * 8));
    std::uint64_t count = (maxSize + maxAlign - 1) / maxAlign;
    body.push_back(llvm::ArrayType::get(unit, count));
  }
  self->setBody(body);
  return self;
}

llvm::StructType *CodeGen::ctorPayload(const Symbol &ctor, const Type &inductive) {
  std::string key = specKey(inductive) + "#" + ctor.name;
  if (auto it = specStructs_.find(key); it != specStructs_.end())
    return it->second;
  std::vector<llvm::Type *> fields;
  if (ctor.type && ctor.type->kind == TypeKind::Fn && inductive.inductive) {
    for (const auto &p : ctor.type->params) {
      sema::TypePtr concrete = checker_.applyBinders(p, inductive.inductive->typeBinders, inductive.typeArgs);
      if (concrete)
        if (llvm::Type *ft = lower(*concrete))
          fields.push_back(ft);
    }
  }
  auto *payload = llvm::StructType::create(*ctx_, fields, key);
  specStructs_[key] = payload;
  return payload;
}

llvm::Type *CodeGen::lower(const Type &t) {
  switch (t.kind) {
  case TypeKind::I8:
  case TypeKind::U8: return llvm::Type::getInt8Ty(*ctx_);
  case TypeKind::I16:
  case TypeKind::U16: return llvm::Type::getInt16Ty(*ctx_);
  case TypeKind::I32:
  case TypeKind::U32: return llvm::Type::getInt32Ty(*ctx_);
  case TypeKind::I64:
  case TypeKind::U64: return llvm::Type::getInt64Ty(*ctx_);
  case TypeKind::USize:
  case TypeKind::ISize:
    return Type::pointerBits() <= 32 ? llvm::Type::getInt32Ty(*ctx_) : llvm::Type::getInt64Ty(*ctx_);
  case TypeKind::F32: return llvm::Type::getFloatTy(*ctx_);
  case TypeKind::F64: return llvm::Type::getDoubleTy(*ctx_);
  case TypeKind::Bool: return llvm::Type::getInt1Ty(*ctx_);
  case TypeKind::Void: return llvm::Type::getVoidTy(*ctx_);
  case TypeKind::Fn:
  case TypeKind::Ptr:
  case TypeKind::Ref: return llvm::PointerType::getUnqual(*ctx_);
  case TypeKind::Vector:
  case TypeKind::Map:
  case TypeKind::String: return bufferType();
  case TypeKind::Slice:
  case TypeKind::Text: return sliceType();
  case TypeKind::TyVar: {
    auto it = tySubst_.find(t.tyVar);
    if (it != tySubst_.end() && it->second)
      return lower(*it->second);
    return nullptr;
  }
  case TypeKind::Struct: {
    if (!t.typeArgs.empty()) {
      std::string key = specKey(t);
      if (auto it = specStructs_.find(key); it != specStructs_.end())
        return it->second;
      auto *sty = llvm::StructType::create(*ctx_, key);
      specStructs_[key] = sty;
      std::vector<llvm::Type *> fields;
      for (const auto &field : t.params)
        if (field)
          if (llvm::Type *ft = lower(*field))
            fields.push_back(ft);
      sty->setBody(fields);
      return sty;
    }
    if (!t.inductive)
      return nullptr;
    auto found = inductives_.find(t.inductive);
    if (found != inductives_.end())
      return found->second;
    auto *sty = llvm::StructType::create(*ctx_, t.inductive->qualifiedName());
    inductives_[t.inductive] = sty;
    std::vector<llvm::Type *> fields;
    for (const auto &field : t.params)
      if (field)
        if (llvm::Type *ft = lower(*field))
          fields.push_back(ft);
    sty->setBody(fields);
    return sty;
  }
  case TypeKind::Inductive: {
    if (!t.typeArgs.empty())
      return specializedInductive(t);
    auto it = inductives_.find(t.inductive);
    if (it == inductives_.end()) {
      lowerInductive(*t.inductive);
      it = inductives_.find(t.inductive);
    }
    return it->second;
  }
  case TypeKind::Simd: {
    llvm::Type *elt = t.elem ? lower(*t.elem) : nullptr;
    if (!elt || t.lanes <= 0)
      return nullptr;
    return llvm::FixedVectorType::get(elt, static_cast<unsigned>(t.lanes));
  }
  case TypeKind::Labeled:
  case TypeKind::Quotient:
    return t.elem ? lower(*t.elem) : nullptr;
  case TypeKind::Interval:
  case TypeKind::Universe:
  case TypeKind::Path:
  case TypeKind::Equiv:
  case TypeKind::Leak:
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
  return llvm::FunctionType::get(ret, params, t.varArg);
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

  dbg_ = std::make_unique<llvm::DIBuilder>(*module_);
  std::string fileName = "cruft.cruft";
  std::string dir = ".";
  if (!program_.files.empty() && !program_.files[0]->filename.empty()) {
    fileName = program_.files[0]->filename;
    auto slash = fileName.find_last_of("/\\");
    if (slash != std::string::npos) {
      dir = fileName.substr(0, slash);
      fileName = fileName.substr(slash + 1);
    }
  }
  dbgFile_ = dbg_->createFile(fileName, dir);
  dbg_->createCompileUnit(llvm::dwarf::DW_LANG_C, dbgFile_, "cruftc", false, "", 0);
  module_->addModuleFlag(llvm::Module::Warning, "Debug Info Version", llvm::DEBUG_METADATA_VERSION);
  module_->addModuleFlag(llvm::Module::Warning, "Dwarf Version", 4);
  dbgScope_ = dbgFile_;

  if (!options_.linkLibraries.empty()) {
    llvm::NamedMDNode *link = module_->getOrInsertNamedMetadata("cruft.link");
    for (const std::string &lib : options_.linkLibraries)
      link->addOperand(llvm::MDNode::get(*ctx_, llvm::MDString::get(*ctx_, lib)));
  }

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

  applySanitizer();
  if (dbg_)
    dbg_->finalize();
  dbgScope_ = nullptr;

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
  if (sym.type && (sym.type->typeParam || !sym.type->typeParams.empty()))
    return; // specialized at each call
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
  emitAbiAlias(f, sym);
}

void CodeGen::emitAbiAlias(llvm::GlobalValue *gv, const Symbol &sym) {
  if (!gv || sym.isExtern || sym.visibility != Visibility::Public)
    return;
  if (sym.abiName.empty() || sym.abiName == sym.linkName || sym.abiName == gv->getName())
    return;
  if (module_->getNamedValue(sym.abiName))
    return;
  llvm::GlobalAlias::create(gv->getType(), gv->getAddressSpace(), llvm::GlobalValue::ExternalLinkage,
                             sym.abiName, gv, module_.get());
}

llvm::Constant *CodeGen::constantValue(const Expr &value, const Type &type) {
  llvm::Type *lt = lower(type);
  if (!lt)
    return nullptr;
  switch (value.kind) {
  case ExprKind::IntLit:
    return llvm::ConstantInt::get(lt, static_cast<std::uint64_t>(as<IntLitExpr>(value).value), true);
  case ExprKind::FloatLit:
    return llvm::ConstantFP::get(lt, as<FloatLitExpr>(value).value);
  case ExprKind::BoolLit:
    return llvm::ConstantInt::get(lt, as<BoolLitExpr>(value).value ? 1 : 0);
  case ExprKind::Call: {
    const auto &call = as<CallExpr>(value);
    auto *sty = llvm::dyn_cast<llvm::StructType>(lt);
    if (!sty || !call.fn || !call.fn->type || call.fn->type->kind != sema::TypeKind::Struct)
      break;
    std::vector<llvm::Constant *> fields;
    size_t arg = 0;
    for (size_t i = 0; i < call.fn->type->params.size(); ++i) {
      const auto &field = call.fn->type->params[i];
      if (arg >= call.args.size() || !call.args[arg])
        return llvm::UndefValue::get(lt);
      if (field && lower(*field)) {
        llvm::Constant *piece = constantValue(*call.args[arg], *field);
        if (!piece)
          return llvm::UndefValue::get(lt);
        fields.push_back(piece);
      }
      ++arg;
    }
    if (fields.size() != sty->getNumElements())
      return llvm::UndefValue::get(lt);
    return llvm::ConstantStruct::get(sty, fields);
  }
  default:
    break;
  }
  return nullptr;
}

llvm::Constant *CodeGen::constantFor(const Expr &init, const Type &type) {
  ExprPtr nf = checker_.normalizer().normalize(init);
  llvm::Type *lt = lower(type);
  if (nf && sema::Normalizer::isValue(*nf))
    if (llvm::Constant *c = constantValue(*nf, type))
      return c;
  diags_.error(init.loc, "internal error: non-constant global initializer survived checking");
  return lt ? llvm::UndefValue::get(lt) : nullptr;
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
  emitAbiAlias(gv, sym);
}

llvm::AllocaInst *CodeGen::createEntryAlloca(llvm::Type *ty, const std::string &name) {
  llvm::BasicBlock &entry = currentFn_->getEntryBlock();
  llvm::IRBuilder<> tmp(&entry, entry.begin());
  return tmp.CreateAlloca(ty, nullptr, name);
}

void CodeGen::defineFunction(const FnDecl &fn) {
  const Symbol &sym = *fn.symbol;
  if (sym.type && (sym.type->typeParam || !sym.type->typeParams.empty()))
    return; // specialized at each call
  llvm::Function *f = functions_.at(&sym);
  currentFn_ = f;
  currentRet_ = sym.type->ret.get();
  locals_.clear();
  values_.clear();
  arenaDepth_ = 0;
  if (dbg_) {
    llvm::DISubroutineType *signature =
        sym.type ? debugSubroutine(*sym.type) : dbg_->createSubroutineType(dbg_->getOrCreateTypeArray({}));
    llvm::DISubprogram *sp = dbg_->createFunction(
        dbgFile_, fn.name, sym.linkName, dbgFile_, std::max(1u, fn.loc.line), signature,
        std::max(1u, fn.loc.line), llvm::DINode::FlagZero, llvm::DISubprogram::SPFlagDefinition);
    f->setSubprogram(sp);
    dbgScope_ = sp;
  }

  llvm::BasicBlock *entry = llvm::BasicBlock::Create(*ctx_, "entry", f);
  builder_->SetInsertPoint(entry);
  builder_->SetCurrentDebugLocation(llvm::DebugLoc());

  // Owned parameters are the callee's. Park them in allocas so a later
  // `ret` can move one out and drop whatever remains, including strings.
  dropStacks_.emplace_back();
  cofn_ = fn.isCofn;
  cofnParams_.clear();
  if (fn.isCofn)
    cofnHeader_ = llvm::BasicBlock::Create(*ctx_, "cofn.loop", f);
  unsigned argIndex = 0;
  for (size_t p = 0; p < fn.params.size(); ++p) {
    const Symbol *ps = sym.type->paramSymbols[p];
    if (!ps || !lower(*sym.type->params[p]))
      continue;
    llvm::Argument *arg = f->getArg(argIndex++);
    // Every parameter has an address so `&` / `&mut` can point at it.
    llvm::AllocaInst *slot = createEntryAlloca(lower(*ps->type), ps->name);
    builder_->CreateStore(arg, slot);
    locals_[ps] = slot;
    declareLocal(slot, ps->name, fn.params[p].loc, ps->type.get(), true, argIndex);
    if (ps->type && slotNeedsDrop(*ps->type))
      dropStacks_.back().push_back(ps);
    if (fn.isCofn)
      cofnParams_.push_back(ps);
  }

  if (fn.isCofn) {
    builder_->CreateBr(cofnHeader_);
    builder_->SetInsertPoint(cofnHeader_);
  }

  genBlock(fn.body);

  if (!blockTerminated()) {
    emitDrops(dropStacks_.back());
    if (fn.isCofn)
      builder_->CreateUnreachable();
    else if (currentRet_->kind == TypeKind::Void || !lower(*currentRet_))
      builder_->CreateRetVoid();
    else
      builder_->CreateUnreachable(); // checker proved all paths return
  }
  dropStacks_.pop_back();
  cofn_ = false;
  cofnHeader_ = nullptr;
  cofnParams_.clear();
  currentFn_ = nullptr;
  currentRet_ = nullptr;
  dbgScope_ = dbgFile_;
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
  setLoc(stmt.loc);
  switch (stmt.kind) {
  case StmtKind::VarDecl: {
    const auto &d = static_cast<const VarDeclStmt &>(stmt);
    llvm::Type *lt = lower(*d.symbol->type);
    if (!lt)
      return; // erased binding (proof / interval / type)
    llvm::AllocaInst *slot = createEntryAlloca(lt, d.name);
    locals_[d.symbol] = slot;
    declareLocal(slot, d.name, d.loc, d.symbol->type.get(), false, 0);
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
      llvm::Value *cell = heapAlloc(llvm::ConstantInt::get(i64, 8));
      llvm::StoreInst *rcInit = builder_->CreateStore(llvm::ConstantInt::get(i64, 1), cell);
      rcInit->setOrdering(llvm::AtomicOrdering::Monotonic);
      rcInit->setAlignment(llvm::Align(8));
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
    if (!dropStacks_.empty() && d.symbol->type && slotNeedsDrop(*d.symbol->type))
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
    // Evaluate first so a returned buffer is moved (its slot zeroed) before
    // the remaining locals are freed. Arena pops stay after that read.
    llvm::Value *result = nullptr;
    if (r.value && currentRet_ && lower(*currentRet_))
      result = gen(*r.value);
    else if (r.value)
      gen(*r.value);
    for (auto it = dropStacks_.rbegin(); it != dropStacks_.rend(); ++it)
      emitDrops(*it);
    emitArenaPops();
    if (!result)
      builder_->CreateRetVoid();
    else
      builder_->CreateRet(result);
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
    loops_.push_back(LoopTarget{condBB, endBB});
    builder_->SetInsertPoint(condBB);
    builder_->CreateCondBr(gen(*w.cond), bodyBB, endBB);
    builder_->SetInsertPoint(bodyBB);
    genBlock(w.body);
    if (!blockTerminated())
      builder_->CreateBr(condBB);
    loops_.pop_back();
    builder_->SetInsertPoint(endBB);
    return;
  }
  case StmtKind::For: {
    const auto &f = static_cast<const ForStmt &>(stmt);
    if (f.iter) {
      if (!(f.step && f.step->type && f.someCtor && f.binder && f.binder->type))
        return;
      llvm::Type *optTy = lower(*f.step->type);
      llvm::Type *itemTy = lower(*f.binder->type);
      auto *optStruct = llvm::dyn_cast_or_null<llvm::StructType>(optTy);
      if (!optStruct || optStruct->isOpaque() || !itemTy)
        return;
      llvm::AllocaInst *slot = createEntryAlloca(itemTy, f.name);
      locals_[f.binder] = slot;
      declareLocal(slot, f.name, f.loc, f.binder->type.get(), false, 0);
      llvm::BasicBlock *condBB = llvm::BasicBlock::Create(*ctx_, "iter.cond", currentFn_);
      llvm::BasicBlock *bodyBB = llvm::BasicBlock::Create(*ctx_, "iter.body", currentFn_);
      llvm::BasicBlock *endBB = llvm::BasicBlock::Create(*ctx_, "iter.end", currentFn_);
      builder_->CreateBr(condBB);
      loops_.push_back(LoopTarget{condBB, endBB});
      builder_->SetInsertPoint(condBB);
      llvm::Value *opt = gen(*f.step);
      llvm::AllocaInst *tmp = createEntryAlloca(optStruct, "iter.tmp");
      if (opt)
        builder_->CreateStore(opt, tmp);
      llvm::Value *tag = builder_->CreateLoad(llvm::Type::getInt32Ty(*ctx_),
                                              builder_->CreateStructGEP(optStruct, tmp, 0), "iter.tag");
      builder_->CreateCondBr(
          builder_->CreateICmpEQ(tag, llvm::ConstantInt::get(llvm::Type::getInt32Ty(*ctx_), f.noneTag)),
          endBB, bodyBB);
      builder_->SetInsertPoint(bodyBB);
      llvm::StructType *payload = nullptr;
      if (!f.step->type->typeArgs.empty())
        payload = ctorPayload(*f.someCtor, *f.step->type);
      else if (auto found = ctorPayloads_.find(f.someCtor); found != ctorPayloads_.end())
        payload = found->second;
      if (payload && payload->getNumElements() > 0 && optStruct->getNumElements() > 1) {
        llvm::Value *payloadPtr = builder_->CreateStructGEP(optStruct, tmp, 1);
        llvm::Value *castPayload =
            builder_->CreatePointerCast(payloadPtr, llvm::PointerType::getUnqual(*ctx_));
        llvm::Value *fieldPtr = builder_->CreateStructGEP(payload, castPayload, 0, "iter.field");
        builder_->CreateStore(builder_->CreateLoad(itemTy, fieldPtr, f.name), slot);
      }
      genBlock(f.body);
      if (!blockTerminated())
        builder_->CreateBr(condBB);
      loops_.pop_back();
      builder_->SetInsertPoint(endBB);
      return;
    }
    if (!f.low || !f.high)
      return;
    auto *i64 = llvm::Type::getInt64Ty(*ctx_);
    llvm::Value *low = gen(*f.low);
    llvm::Value *high = gen(*f.high);
    if (low && low->getType() != i64)
      low = builder_->CreateSExtOrTrunc(low, i64);
    if (high && high->getType() != i64)
      high = builder_->CreateSExtOrTrunc(high, i64);
    llvm::AllocaInst *slot = createEntryAlloca(i64, f.name);
    if (f.binder) {
      locals_[f.binder] = slot;
      declareLocal(slot, f.name, f.loc, f.binder->type.get(), false, 0);
    }
    builder_->CreateStore(low, slot);
    llvm::BasicBlock *condBB = llvm::BasicBlock::Create(*ctx_, "for.cond", currentFn_);
    llvm::BasicBlock *bodyBB = llvm::BasicBlock::Create(*ctx_, "for.body", currentFn_);
    llvm::BasicBlock *stepBB = llvm::BasicBlock::Create(*ctx_, "for.step", currentFn_);
    llvm::BasicBlock *endBB = llvm::BasicBlock::Create(*ctx_, "for.end", currentFn_);
    builder_->CreateBr(condBB);
    loops_.push_back(LoopTarget{stepBB, endBB});
    builder_->SetInsertPoint(condBB);
    builder_->CreateCondBr(builder_->CreateICmpSLT(builder_->CreateLoad(i64, slot), high), bodyBB, endBB);
    builder_->SetInsertPoint(bodyBB);
    genBlock(f.body);
    if (!blockTerminated())
      builder_->CreateBr(stepBB);
    builder_->SetInsertPoint(stepBB);
    builder_->CreateStore(builder_->CreateAdd(builder_->CreateLoad(i64, slot), llvm::ConstantInt::get(i64, 1)),
                          slot);
    builder_->CreateBr(condBB);
    loops_.pop_back();
    builder_->SetInsertPoint(endBB);
    return;
  }
  case StmtKind::Break:
    if (!loops_.empty())
      builder_->CreateBr(loops_.back().brk);
    return;
  case StmtKind::Continue:
    if (!loops_.empty())
      builder_->CreateBr(loops_.back().cont);
    return;
  case StmtKind::IndexAssign: {
    const auto &ix = static_cast<const IndexAssignStmt &>(stmt);
    const Type *placeTy = ix.place && ix.place->type ? ix.place->type.get() : nullptr;
    llvm::Value *slot = nullptr;
    if (placeTy && placeTy->kind == TypeKind::Ref) {
      slot = gen(*ix.place);
      placeTy = placeTy->elem.get();
    } else if (ix.place && ix.place->kind == ExprKind::Ident) {
      const Symbol *sym = as<IdentExpr>(*ix.place).symbol;
      if (auto it = locals_.find(sym); it != locals_.end())
        slot = it->second;
      else if (auto it = globals_.find(sym); it != globals_.end())
        slot = it->second;
    }
    if (!slot || !placeTy || !placeTy->elem)
      return;
    llvm::Value *buf = builder_->CreateLoad(
        placeTy->kind == TypeKind::Slice ? sliceType() : bufferType(), slot);
    llvm::Value *index = gen(*ix.index);
    llvm::Value *value = gen(*ix.value);
    llvm::Value *len = builder_->CreateExtractValue(buf, 1);
    llvm::Value *data = builder_->CreateExtractValue(buf, 0);
    emitBoundsCheck(index, len);
    llvm::Type *elt = lower(*placeTy->elem);
    builder_->CreateStore(value, builder_->CreateInBoundsGEP(elt, data, index));
    return;
  }
  case StmtKind::FieldAssign: {
    const auto &a = static_cast<const FieldAssignStmt &>(stmt);
    if (!a.place || !a.place->type || !a.value)
      return;
    const Type *sty = a.place->type.get();
    llvm::Value *addr = nullptr;
    if (sty->kind == TypeKind::Ref && sty->elem) {
      addr = gen(*a.place);
      sty = sty->elem.get();
    } else if (a.place->kind == ExprKind::Ident) {
      const Symbol *sym = as<IdentExpr>(*a.place).symbol;
      if (auto it = locals_.find(sym); it != locals_.end())
        addr = it->second;
      else if (auto it = globals_.find(sym); it != globals_.end())
        addr = it->second;
    }
    if (!addr || !sty || sty->kind != TypeKind::Struct)
      return;
    int field = -1;
    for (size_t i = 0; i < sty->params.size(); ++i) {
      const Symbol *name = i < sty->paramSymbols.size() ? sty->paramSymbols[i] : nullptr;
      if (name && name->name == a.field) {
        field = runtimeField(*sty, i);
        break;
      }
    }
    llvm::Type *lt = lower(*sty);
    llvm::Value *written = gen(*a.value);
    if (field < 0 || !lt || !written)
      return;
    builder_->CreateStore(written, builder_->CreateStructGEP(lt, addr, static_cast<unsigned>(field)));
    return;
  }
  case StmtKind::Foreach:
    genBlock(static_cast<const ForeachStmt &>(stmt).body);
    return;
  case StmtKind::Expr:
    gen(*static_cast<const ExprStmt &>(stmt).expr);
    return;
  case StmtKind::Error:
    return;
  case StmtKind::Unsafe:
    genBlock(static_cast<const UnsafeStmt &>(stmt).body);
    return;
  case StmtKind::ScopeArena: {
    const auto &a = static_cast<const ScopeArenaStmt &>(stmt);
    if (a.elide) {
      genBlock(a.body);
      return;
    }
    llvm::CallInst *push = builder_->CreateCall(arenaPushCallee());
    push->setCallingConv(llvm::CallingConv::C);
    ++arenaDepth_;
    llvm::Value *savedFrame = frameBase_;
    if (a.framed && a.frameBytes > 0) {
      auto *i64 = llvm::Type::getInt64Ty(*ctx_);
      frameBase_ = allocBytes(llvm::ConstantInt::get(i64, a.frameBytes), a.frameAlign);
    }
    genBlock(a.body);
    frameBase_ = savedFrame;
    if (!blockTerminated()) {
      llvm::CallInst *pop = builder_->CreateCall(arenaPopCallee());
      pop->setCallingConv(llvm::CallingConv::C);
    }
    --arenaDepth_;
    return;
  }
  }
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

llvm::Value *CodeGen::gen(const Expr &e) {
  setLoc(e.loc);
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
  case ExprKind::Cast:
    return genCast(as<CastExpr>(e));
  case ExprKind::Borrow:
    return genBorrow(as<BorrowExpr>(e));
  case ExprKind::Try:
    return genTry(as<TryExpr>(e));
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
        builder_->CreateAtomicRMW(llvm::AtomicRMWInst::Add, rc,
                                  llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx_), 1),
                                  llvm::MaybeAlign(), llvm::AtomicOrdering::Monotonic);
      } else {
        // Move: the caller now owns the buffer, including strings.
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
  const bool isUnsigned = operandTy.isUnsignedInteger();

  switch (e.op) {
  case BinaryOp::Add: return isFloat ? builder_->CreateFAdd(l, r, "add") : builder_->CreateAdd(l, r, "add");
  case BinaryOp::Sub: return isFloat ? builder_->CreateFSub(l, r, "sub") : builder_->CreateSub(l, r, "sub");
  case BinaryOp::Mul: return isFloat ? builder_->CreateFMul(l, r, "mul") : builder_->CreateMul(l, r, "mul");
  case BinaryOp::Div:
    return isFloat ? builder_->CreateFDiv(l, r, "div")
                   : (isUnsigned ? builder_->CreateUDiv(l, r, "div") : builder_->CreateSDiv(l, r, "div"));
  case BinaryOp::Rem:
    return isFloat ? builder_->CreateFRem(l, r, "rem")
                   : (isUnsigned ? builder_->CreateURem(l, r, "rem") : builder_->CreateSRem(l, r, "rem"));
  case BinaryOp::Eq: return isFloat ? builder_->CreateFCmpOEQ(l, r, "eq") : builder_->CreateICmpEQ(l, r, "eq");
  case BinaryOp::Ne: return isFloat ? builder_->CreateFCmpUNE(l, r, "ne") : builder_->CreateICmpNE(l, r, "ne");
  case BinaryOp::Lt:
    return isFloat ? builder_->CreateFCmpOLT(l, r, "lt")
                   : (isUnsigned ? builder_->CreateICmpULT(l, r, "lt") : builder_->CreateICmpSLT(l, r, "lt"));
  case BinaryOp::Le:
    return isFloat ? builder_->CreateFCmpOLE(l, r, "le")
                   : (isUnsigned ? builder_->CreateICmpULE(l, r, "le") : builder_->CreateICmpSLE(l, r, "le"));
  case BinaryOp::Gt:
    return isFloat ? builder_->CreateFCmpOGT(l, r, "gt")
                   : (isUnsigned ? builder_->CreateICmpUGT(l, r, "gt") : builder_->CreateICmpSGT(l, r, "gt"));
  case BinaryOp::Ge:
    return isFloat ? builder_->CreateFCmpOGE(l, r, "ge")
                   : (isUnsigned ? builder_->CreateICmpUGE(l, r, "ge") : builder_->CreateICmpSGE(l, r, "ge"));
  case BinaryOp::And: return builder_->CreateAnd(l, r, "and");
  case BinaryOp::Or: return builder_->CreateOr(l, r, "or");
  case BinaryOp::BitAnd: return builder_->CreateAnd(l, r, "band");
  case BinaryOp::BitOr: return builder_->CreateOr(l, r, "bor");
  case BinaryOp::BitXor: return builder_->CreateXor(l, r, "bxor");
  case BinaryOp::Shl: return builder_->CreateShl(l, r, "shl");
  case BinaryOp::Shr:
    return isUnsigned ? builder_->CreateLShr(l, r, "shr") : builder_->CreateAShr(l, r, "shr");
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
    if (e.operand->type && e.operand->type->isInteger())
      return builder_->CreateNot(x, "bnot");
    return nullptr;
  }
  return nullptr;
}

int CodeGen::runtimeField(const Type &structType, size_t field) {
  int index = 0;
  for (size_t i = 0; i < field && i < structType.params.size(); ++i)
    if (structType.params[i] && lower(*structType.params[i]))
      ++index;
  return index;
}

llvm::Function *CodeGen::specialize(const FnDecl &fn, const std::vector<sema::TypePtr> &args) {
  const Symbol &sym = *fn.symbol;
  std::string key = sym.linkName;
  for (const auto &arg : args)
    key += "/" + (arg ? arg->str() : "?");
  if (auto it = specs_.find(key); it != specs_.end())
    return it->second;
  auto savedSubst = tySubst_;
  if (sym.type) {
    for (size_t i = 0; i < sym.type->typeParams.size() && i < args.size(); ++i)
      tySubst_[sym.type->typeParams[i]] = args[i];
    if (sym.type->typeParam && !args.empty())
      tySubst_[sym.type->typeParam] = args.front();
  }
  llvm::FunctionType *fty = lowerFnType(*sym.type);
  llvm::Function *f = llvm::Function::Create(fty, llvm::Function::InternalLinkage,
                                             key, module_.get());
  f->setCallingConv(llvm::CallingConv::C);
  f->addFnAttr(llvm::Attribute::NoUnwind);
  specs_[key] = f;

  llvm::Function *savedFn = currentFn_;
  const Type *savedRet = currentRet_;
  llvm::BasicBlock *savedBB = builder_->GetInsertBlock();
  auto savedValues = values_;
  auto savedLocals = locals_;
  bool savedConsume = consume_;
  auto savedDrops = dropStacks_;
  bool savedCofn = cofn_;
  llvm::BasicBlock *savedHeader = cofnHeader_;
  auto savedCofnParams = cofnParams_;
  values_.clear();
  locals_.clear();
  dropStacks_.clear();
  cofn_ = false;
  cofnHeader_ = nullptr;
  cofnParams_.clear();
  currentFn_ = f;
  currentRet_ = sym.type->ret.get();
  llvm::DebugLoc savedLoc = builder_->getCurrentDebugLocation();
  llvm::DIScope *savedScope = dbgScope_;
  dbgScope_ = nullptr;
  builder_->SetCurrentDebugLocation(llvm::DebugLoc());
  llvm::BasicBlock *entry = llvm::BasicBlock::Create(*ctx_, "entry", f);
  builder_->SetInsertPoint(entry);
  if (dbg_ && dbgFile_ && sym.type) {
    llvm::DISubprogram *sp = dbg_->createFunction(
        dbgFile_, fn.name, key, dbgFile_, std::max(1u, fn.loc.line), debugSubroutine(*sym.type),
        std::max(1u, fn.loc.line), llvm::DINode::FlagZero, llvm::DISubprogram::SPFlagDefinition);
    f->setSubprogram(sp);
    dbgScope_ = sp;
  }
  dropStacks_.emplace_back();
  unsigned argIndex = 0;
  for (size_t p = 0; p < fn.params.size(); ++p) {
    const Symbol *ps = sym.type->paramSymbols[p];
    if (!ps || !sym.type->params[p] || !lower(*sym.type->params[p]))
      continue;
    llvm::Argument *argument = f->getArg(argIndex++);
    argument->setName(fn.params[p].name);
    llvm::AllocaInst *slot = createEntryAlloca(lower(*sym.type->params[p]), ps->name);
    builder_->CreateStore(argument, slot);
    locals_[ps] = slot;
    declareLocal(slot, ps->name, fn.params[p].loc, sym.type->params[p].get(), true, argIndex);
    if (ps->type && slotNeedsDrop(*ps->type))
      dropStacks_.back().push_back(ps);
  }
  genBlock(fn.body);
  if (!blockTerminated()) {
    emitDrops(dropStacks_.back());
    if (!sym.type->ret || sym.type->ret->kind == TypeKind::Void || !lower(*sym.type->ret))
      builder_->CreateRetVoid();
    else
      builder_->CreateUnreachable();
  }
  dropStacks_.pop_back();
  currentFn_ = savedFn;
  currentRet_ = savedRet;
  values_ = std::move(savedValues);
  locals_ = std::move(savedLocals);
  dropStacks_ = std::move(savedDrops);
  consume_ = savedConsume;
  cofn_ = savedCofn;
  cofnHeader_ = savedHeader;
  cofnParams_ = std::move(savedCofnParams);
  tySubst_ = std::move(savedSubst);
  dbgScope_ = savedScope;
  builder_->SetCurrentDebugLocation(savedLoc);
  if (savedBB)
    builder_->SetInsertPoint(savedBB);
  return f;
}

llvm::Value *CodeGen::genStruct(const CallExpr &e) {
  // The callee symbol still carries the unsubstituted binders. The call's own
  // type is the specialisation, with field types already substituted.
  const Type &st = (e.type && e.type->kind == TypeKind::Struct) ? *e.type : *e.fn->type;
  llvm::Type *lt = lower(st);
  if (!lt)
    return nullptr;
  llvm::Value *agg = llvm::UndefValue::get(lt);
  unsigned slot = 0;
  for (size_t i = 0; i < e.args.size() && i < st.params.size(); ++i) {
    if (!st.params[i] || !lower(*st.params[i]))
      continue;
    llvm::Value *field = e.args[i] ? gen(*e.args[i]) : nullptr;
    if (field)
      agg = builder_->CreateInsertValue(agg, field, slot);
    ++slot;
  }
  return agg;
}

llvm::Value *CodeGen::genCall(const CallExpr &e) {
  if (e.fn && e.fn->fnDecl && e.fn->fnDecl->isConstFn && e.type) {
    ExprPtr nf = checker_.normalizer().normalize(e);
    if (nf && sema::Normalizer::isValue(*nf))
      if (llvm::Constant *c = constantValue(*nf, *e.type))
        return c;
  }
  if (e.fn && e.fn->type && e.fn->type->kind == TypeKind::Struct)
    return genStruct(e);
  if (e.fn && e.fn->fnDecl && e.fn->type && !e.fn->type->typeParams.empty() && !e.typeArguments.empty()) {
    llvm::Function *spec = specialize(*e.fn->fnDecl, e.typeArguments);
    std::vector<llvm::Value *> args;
    for (size_t i = 0; i < e.args.size() && i < e.fn->type->params.size(); ++i) {
      auto saved = tySubst_;
      for (size_t b = 0; b < e.fn->type->typeParams.size() && b < e.typeArguments.size(); ++b)
        tySubst_[e.fn->type->typeParams[b]] = e.typeArguments[b];
      bool keep = e.fn->type->params[i] && lower(*e.fn->type->params[i]);
      llvm::Value *v = e.args[i] ? gen(*e.args[i]) : nullptr;
      tySubst_ = std::move(saved);
      if (keep && v)
        args.push_back(v);
    }
    llvm::CallInst *call = builder_->CreateCall(spec->getFunctionType(), spec, args);
    call->setCallingConv(llvm::CallingConv::C);
    if (spec->getReturnType()->isVoidTy())
      return nullptr;
    return call;
  }
  if (e.tailSelf && cofnHeader_) {
    const Type *fnType = e.fn && e.fn->type ? e.fn->type.get() : e.callee->type.get();
    std::vector<llvm::Value *> incoming;
    if (fnType) {
      for (size_t i = 0; i < e.args.size() && i < fnType->params.size(); ++i) {
        llvm::Value *v = e.args[i] ? gen(*e.args[i]) : nullptr;
        if (fnType->params[i] && lower(*fnType->params[i]))
          incoming.push_back(v);
      }
    }
    for (size_t i = 0; i < cofnParams_.size() && i < incoming.size(); ++i) {
      if (!incoming[i])
        continue;
      auto slot = locals_.find(cofnParams_[i]);
      if (slot != locals_.end())
        builder_->CreateStore(incoming[i], slot->second);
    }
    builder_->CreateBr(cofnHeader_);
    return nullptr;
  }

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
    if (i >= fnType->params.size() || (fnType->params[i] && lower(*fnType->params[i])))
      if (v)
        args.push_back(v); // erased arguments are dropped; varargs are kept
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
  case Builtin::NullPtr:
    return llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*ctx_));
  case Builtin::AllocString: {
    auto *i64 = llvm::Type::getInt64Ty(*ctx_);
    auto *i8 = llvm::Type::getInt8Ty(*ctx_);
    auto *ptrTy = llvm::PointerType::getUnqual(*ctx_);
    auto *nullPtr = llvm::ConstantPointerNull::get(ptrTy);
    llvm::Value *n = gen(*e.args[0]);
    llvm::Value *neg = builder_->CreateICmpSLT(n, llvm::ConstantInt::get(i64, 0));
    n = builder_->CreateSelect(neg, llvm::ConstantInt::get(i64, 0), n, "strlen");
    llvm::Value *bytes = builder_->CreateAdd(n, llvm::ConstantInt::get(i64, 1), "strbytes");
    llvm::Value *raw = heapAlloc(bytes);
    llvm::BasicBlock *ok = llvm::BasicBlock::Create(*ctx_, "str.ok", currentFn_);
    llvm::BasicBlock *fail = llvm::BasicBlock::Create(*ctx_, "str.oom", currentFn_);
    llvm::BasicBlock *done = llvm::BasicBlock::Create(*ctx_, "str.done", currentFn_);
    builder_->CreateCondBr(builder_->CreateICmpEQ(raw, nullPtr), fail, ok);
    builder_->SetInsertPoint(fail);
    llvm::Value *empty = makeBuffer(nullptr, llvm::ConstantInt::get(i64, 0), llvm::ConstantInt::get(i64, 0),
                                    nullptr);
    builder_->CreateBr(done);
    builder_->SetInsertPoint(ok);
    builder_->CreateMemSet(raw, llvm::ConstantInt::get(i8, 0), bytes, llvm::Align(1));
    llvm::Value *cap = builder_->CreateAdd(n, llvm::ConstantInt::get(i64, 1));
    llvm::Value *filled = makeBuffer(raw, n, cap, nullptr);
    builder_->CreateBr(done);
    builder_->SetInsertPoint(done);
    llvm::PHINode *phi = builder_->CreatePHI(bufferType(), 2, "str.buf");
    phi->addIncoming(empty, fail);
    phi->addIncoming(filled, ok);
    return phi;
  }
  case Builtin::RawSlice: {
    llvm::Value *addr = gen(*e.args[0]);
    llvm::Value *len = gen(*e.args[1]);
    llvm::Value *ptr = builder_->CreateIntToPtr(addr, llvm::PointerType::getUnqual(*ctx_), "slice.ptr");
    llvm::Value *slice = llvm::UndefValue::get(sliceType());
    slice = builder_->CreateInsertValue(slice, ptr, 0);
    slice = builder_->CreateInsertValue(slice, len, 1);
    return slice;
  }
  case Builtin::Seal:
  case Builtin::Reveal:
    return e.args.empty() ? nullptr : gen(*e.args[0]);
  case Builtin::Text: {
    if (e.args.empty() || e.args[0]->kind != ExprKind::StringLit)
      return nullptr;
    const auto &lit = as<StringLitExpr>(*e.args[0]);
    auto *i64 = llvm::Type::getInt64Ty(*ctx_);
    llvm::Value *data = globalString(lit.value, ".text.lit");
    llvm::Value *n = llvm::ConstantInt::get(i64, lit.value.size());
    llvm::Value *text = llvm::UndefValue::get(sliceType());
    text = builder_->CreateInsertValue(text, data, 0);
    text = builder_->CreateInsertValue(text, n, 1);
    return text;
  }
  case Builtin::Quot: {
    llvm::Value *payload = e.args.empty() ? nullptr : gen(*e.args[0]);
    if (!e.quotCanon)
      return payload;
    auto it = functions_.find(e.quotCanon);
    if (it == functions_.end() || !payload)
      return payload;
    llvm::CallInst *call =
        builder_->CreateCall(it->second->getFunctionType(), it->second, {payload}, "canon");
    call->setCallingConv(llvm::CallingConv::C);
    return call;
  }
  case Builtin::Assert: {
    llvm::Value *cond = gen(*e.args[0]);
    llvm::BasicBlock *fail = llvm::BasicBlock::Create(*ctx_, "assert.fail", currentFn_);
    llvm::BasicBlock *ok = llvm::BasicBlock::Create(*ctx_, "assert.ok", currentFn_);
    builder_->CreateCondBr(cond, ok, fail);
    builder_->SetInsertPoint(fail);
    llvm::FunctionType *fty = llvm::FunctionType::get(
        llvm::Type::getVoidTy(*ctx_), {llvm::PointerType::getUnqual(*ctx_)}, false);
    llvm::FunctionCallee trap = module_->getOrInsertFunction("cruft_trap", fty);
    llvm::CallInst *call =
        builder_->CreateCall(trap, {globalString("assertion failed\n", ".trap.assert")});
    call->setCallingConv(llvm::CallingConv::C);
    call->setDoesNotReturn();
    builder_->CreateUnreachable();
    builder_->SetInsertPoint(ok);
    return nullptr;
  }
  case Builtin::CmpXchg:
  case Builtin::AtomicRmw:
  case Builtin::AtomicLoad:
  case Builtin::AtomicStore: {
    const size_t placeAt = e.builtin == Builtin::AtomicRmw ? 1 : 0;
    const Type *elem = e.args[placeAt]->type && e.args[placeAt]->type->elem
                           ? e.args[placeAt]->type->elem.get()
                           : nullptr;
    llvm::Value *addr = gen(*e.args[placeAt]);
    llvm::Type *ty = elem ? lower(*elem) : nullptr;
    if (!addr || !ty)
      return nullptr;
    if (e.builtin == Builtin::AtomicLoad) {
      llvm::LoadInst *load = builder_->CreateLoad(ty, addr, "atomic.load");
      load->setOrdering(atomicOrder(*e.args[1]));
      load->setAlignment(module_->getDataLayout().getABITypeAlign(ty));
      return load;
    }
    if (e.builtin == Builtin::AtomicStore) {
      llvm::Value *val = gen(*e.args[1]);
      if (!val)
        return nullptr;
      llvm::StoreInst *store = builder_->CreateStore(val, addr);
      store->setOrdering(atomicOrder(*e.args[2]));
      store->setAlignment(module_->getDataLayout().getABITypeAlign(ty));
      return nullptr;
    }
    if (e.builtin == Builtin::AtomicRmw) {
      llvm::Value *val = gen(*e.args[2]);
      if (!val)
        return nullptr;
      return builder_->CreateAtomicRMW(rmwOp(*e.args[0]), addr, val, llvm::MaybeAlign(),
                                      atomicOrder(*e.args[3]));
    }
    llvm::Value *expected = gen(*e.args[1]);
    llvm::Value *desired = gen(*e.args[2]);
    if (!expected || !desired)
      return nullptr;
    llvm::AtomicCmpXchgInst *cx = builder_->CreateAtomicCmpXchg(
        addr, expected, desired, llvm::MaybeAlign(), atomicOrder(*e.args[3]), atomicOrder(*e.args[4]));
    return builder_->CreateExtractValue(cx, 1, "cmpxchg.ok");
  }
  case Builtin::Simd: {
    if (!e.type || e.type->kind != TypeKind::Simd)
      return nullptr;
    llvm::Type *vecTy = lower(*e.type);
    if (!vecTy)
      return nullptr;
    llvm::Value *acc = llvm::UndefValue::get(vecTy);
    for (size_t i = 0; i < e.args.size(); ++i) {
      llvm::Value *lane = e.args[i] ? gen(*e.args[i]) : nullptr;
      if (!lane)
        continue;
      acc = builder_->CreateInsertElement(acc, lane, llvm::ConstantInt::get(llvm::Type::getInt32Ty(*ctx_), i));
    }
    return acc;
  }
  case Builtin::SimdAdd: {
    llvm::Value *lhs = gen(*e.args[0]);
    llvm::Value *rhs = gen(*e.args[1]);
    if (!lhs || !rhs)
      return nullptr;
    const Type *elem = e.type && e.type->elem ? e.type->elem.get() : nullptr;
    if (elem && elem->isFloat())
      return builder_->CreateFAdd(lhs, rhs, "simd.add");
    return builder_->CreateAdd(lhs, rhs, "simd.add");
  }
  case Builtin::SimdExtract: {
    llvm::Value *vec = gen(*e.args[0]);
    if (!vec || e.args[1]->kind != ExprKind::IntLit)
      return nullptr;
    auto lane = static_cast<uint64_t>(as<IntLitExpr>(*e.args[1]).value);
    return builder_->CreateExtractElement(vec, llvm::ConstantInt::get(llvm::Type::getInt32Ty(*ctx_), lane),
                                          "simd.lane");
  }
  case Builtin::Asm: {
    std::string text = as<StringLitExpr>(*e.args[0]).value;
    llvm::FunctionType *fty = llvm::FunctionType::get(llvm::Type::getVoidTy(*ctx_), false);
    llvm::InlineAsm *ia = llvm::InlineAsm::get(fty, text, "", /*hasSideEffects=*/true);
    builder_->CreateCall(fty, ia);
    return nullptr;
  }
  case Builtin::Trap: {
    std::string msg = "trap\n";
    if (!e.args.empty() && e.args[0]->kind == ExprKind::StringLit)
      msg = as<StringLitExpr>(*e.args[0]).value;
    llvm::FunctionType *fty = llvm::FunctionType::get(
        llvm::Type::getVoidTy(*ctx_), {llvm::PointerType::getUnqual(*ctx_)}, false);
    llvm::FunctionCallee trap = module_->getOrInsertFunction("cruft_trap", fty);
    llvm::CallInst *call = builder_->CreateCall(trap, {globalString(msg, ".trap.msg")});
    call->setCallingConv(llvm::CallingConv::C);
    call->setDoesNotReturn();
    builder_->CreateUnreachable();
    return nullptr;
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
  const Type *indTy = e.type && e.type->kind == TypeKind::Inductive ? e.type.get() : nullptr;
  llvm::StructType *self = indTy && !indTy->typeArgs.empty() ? specializedInductive(*indTy)
                                                             : inductives_.at(ctor.owner);
  llvm::StructType *payload = indTy && !indTy->typeArgs.empty() ? ctorPayload(ctor, *indTy)
                                                                : ctorPayloads_.at(&ctor);

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
  const Type &scrutTy = *e.scrutinee->type;
  llvm::StructType *self =
      scrutTy.typeArgs.empty() ? inductives_.at(&typeSym) : specializedInductive(scrutTy);
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

    llvm::StructType *payload = scrutTy.typeArgs.empty() ? ctorPayloads_.at(c.ctorSymbol)
                                                        : ctorPayload(*c.ctorSymbol, scrutTy);
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
  case TypeKind::I8:
  case TypeKind::I16:
    args = {globalString("%d\n", ".fmt.i32"),
            builder_->CreateSExt(v, llvm::Type::getInt32Ty(*ctx_))};
    break;
  case TypeKind::U8:
  case TypeKind::U16:
    args = {globalString("%u\n", ".fmt.u32"),
            builder_->CreateZExt(v, llvm::Type::getInt32Ty(*ctx_))};
    break;
  case TypeKind::U32:
    args = {globalString("%u\n", ".fmt.u32"), v};
    break;
  case TypeKind::I32:
    args = {globalString("%d\n", ".fmt.i32"), v};
    break;
  case TypeKind::I64:
    args = {globalString("%lld\n", ".fmt.i64"), v};
    break;
  case TypeKind::U64:
    args = {globalString("%llu\n", ".fmt.u64"), v};
    break;
  case TypeKind::ISize:
    if (Type::pointerBits() <= 32)
      args = {globalString("%d\n", ".fmt.i32"), v};
    else
      args = {globalString("%lld\n", ".fmt.i64"), v};
    break;
  case TypeKind::USize:
    if (Type::pointerBits() <= 32)
      args = {globalString("%u\n", ".fmt.u32"), v};
    else
      args = {globalString("%llu\n", ".fmt.u64"), v};
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

llvm::StructType *CodeGen::sliceType() {
  if (!sliceTy_) {
    sliceTy_ = llvm::StructType::create(
        *ctx_, {llvm::PointerType::getUnqual(*ctx_), llvm::Type::getInt64Ty(*ctx_)}, "cruft.slice");
  }
  return sliceTy_;
}

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
  auto *i64 = llvm::Type::getInt64Ty(*ctx_);
  llvm::FunctionType *fty = llvm::FunctionType::get(i64, {i64}, false);
  llvm::FunctionCallee c = module_->getOrInsertFunction("cruft_alloc", fty);
  if (auto *f = llvm::dyn_cast<llvm::Function>(c.getCallee()))
    f->setCallingConv(llvm::CallingConv::C);
  return c;
}

llvm::FunctionCallee CodeGen::freeCallee() {
  auto *i64 = llvm::Type::getInt64Ty(*ctx_);
  llvm::FunctionType *fty = llvm::FunctionType::get(llvm::Type::getVoidTy(*ctx_), {i64}, false);
  llvm::FunctionCallee c = module_->getOrInsertFunction("cruft_free", fty);
  if (auto *f = llvm::dyn_cast<llvm::Function>(c.getCallee()))
    f->setCallingConv(llvm::CallingConv::C);
  return c;
}

llvm::FunctionCallee CodeGen::reallocCallee() {
  auto *i64 = llvm::Type::getInt64Ty(*ctx_);
  llvm::FunctionType *fty = llvm::FunctionType::get(i64, {i64, i64}, false);
  llvm::FunctionCallee c = module_->getOrInsertFunction("cruft_realloc", fty);
  if (auto *f = llvm::dyn_cast<llvm::Function>(c.getCallee()))
    f->setCallingConv(llvm::CallingConv::C);
  return c;
}

llvm::Value *CodeGen::heapAlloc(llvm::Value *nbytes) {
  llvm::CallInst *call = builder_->CreateCall(mallocCallee(), {nbytes}, "heap.addr");
  call->setCallingConv(llvm::CallingConv::C);
  return builder_->CreateIntToPtr(call, llvm::PointerType::getUnqual(*ctx_), "heap");
}

void CodeGen::heapFree(llvm::Value *ptr) {
  auto *i64 = llvm::Type::getInt64Ty(*ctx_);
  llvm::Value *addr = builder_->CreatePtrToInt(ptr, i64, "heap.free");
  llvm::CallInst *call = builder_->CreateCall(freeCallee(), {addr});
  call->setCallingConv(llvm::CallingConv::C);
}

llvm::Value *CodeGen::heapRealloc(llvm::Value *ptr, llvm::Value *nbytes) {
  auto *i64 = llvm::Type::getInt64Ty(*ctx_);
  llvm::Value *addr = builder_->CreatePtrToInt(ptr, i64, "heap.old");
  llvm::CallInst *call = builder_->CreateCall(reallocCallee(), {addr, nbytes}, "heap.resize");
  call->setCallingConv(llvm::CallingConv::C);
  return builder_->CreateIntToPtr(call, llvm::PointerType::getUnqual(*ctx_), "heap.moved");
}

llvm::FunctionCallee CodeGen::boundsFailCallee() {
  llvm::FunctionType *fty = llvm::FunctionType::get(llvm::Type::getVoidTy(*ctx_), false);
  llvm::FunctionCallee c = module_->getOrInsertFunction("cruft_bounds_fail", fty);
  if (auto *f = llvm::dyn_cast<llvm::Function>(c.getCallee())) {
    f->setCallingConv(llvm::CallingConv::C);
    f->setDoesNotReturn();
  }
  return c;
}

void CodeGen::emitBoundsCheck(llvm::Value *index, llvm::Value *len) {
  auto *i64 = llvm::Type::getInt64Ty(*ctx_);
  llvm::Value *negative = builder_->CreateICmpSLT(index, llvm::ConstantInt::get(i64, 0));
  llvm::Value *tooBig = builder_->CreateICmpSGE(index, len);
  llvm::Value *bad = builder_->CreateOr(negative, tooBig, "oob");
  llvm::BasicBlock *fail = llvm::BasicBlock::Create(*ctx_, "oob.fail", currentFn_);
  llvm::BasicBlock *ok = llvm::BasicBlock::Create(*ctx_, "oob.ok", currentFn_);
  builder_->CreateCondBr(bad, fail, ok);
  builder_->SetInsertPoint(fail);
  llvm::CallInst *call = builder_->CreateCall(boundsFailCallee());
  call->setCallingConv(llvm::CallingConv::C);
  builder_->CreateUnreachable();
  builder_->SetInsertPoint(ok);
}

llvm::FunctionCallee CodeGen::arenaPushCallee() {
  llvm::FunctionType *fty = llvm::FunctionType::get(llvm::Type::getVoidTy(*ctx_), false);
  llvm::FunctionCallee c = module_->getOrInsertFunction("cruft_arena_push", fty);
  if (auto *f = llvm::dyn_cast<llvm::Function>(c.getCallee())) {
    f->setCallingConv(llvm::CallingConv::C);
    f->addFnAttr(llvm::Attribute::NoUnwind);
  }
  return c;
}

llvm::FunctionCallee CodeGen::arenaPopCallee() {
  llvm::FunctionType *fty = llvm::FunctionType::get(llvm::Type::getVoidTy(*ctx_), false);
  llvm::FunctionCallee c = module_->getOrInsertFunction("cruft_arena_pop", fty);
  if (auto *f = llvm::dyn_cast<llvm::Function>(c.getCallee())) {
    f->setCallingConv(llvm::CallingConv::C);
    f->addFnAttr(llvm::Attribute::NoUnwind);
  }
  return c;
}

llvm::FunctionCallee CodeGen::arenaAllocCallee() {
  auto *i64 = llvm::Type::getInt64Ty(*ctx_);
  llvm::FunctionType *fty =
      llvm::FunctionType::get(llvm::PointerType::getUnqual(*ctx_), {i64, i64}, false);
  llvm::FunctionCallee c = module_->getOrInsertFunction("cruft_arena_alloc", fty);
  if (auto *f = llvm::dyn_cast<llvm::Function>(c.getCallee())) {
    f->setCallingConv(llvm::CallingConv::C);
    f->addFnAttr(llvm::Attribute::NoUnwind);
  }
  return c;
}

void CodeGen::emitArenaPops() {
  if (arenaDepth_ <= 0)
    return;
  llvm::FunctionCallee pop = arenaPopCallee();
  for (int i = 0; i < arenaDepth_; ++i) {
    llvm::CallInst *call = builder_->CreateCall(pop);
    call->setCallingConv(llvm::CallingConv::C);
  }
}

llvm::Value *CodeGen::allocBytes(llvm::Value *nbytes, unsigned align) {
  if (align == 0)
    align = 1;
  if (arenaDepth_ > 0) {
    auto *i64 = llvm::Type::getInt64Ty(*ctx_);
    llvm::CallInst *call = builder_->CreateCall(
        arenaAllocCallee(), {nbytes, llvm::ConstantInt::get(i64, align)}, "arena.ptr");
    call->setCallingConv(llvm::CallingConv::C);
    return call;
  }
  return heapAlloc(nbytes);
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
  if (!sym.type || !slotNeedsDrop(*sym.type))
    return;
  auto found = locals_.find(&sym);
  if (found == locals_.end())
    return;
  if (sym.type->kind == TypeKind::Struct) {
    if (sym.type->inductive && sym.type->inductive->dropFn && sym.type->inductive->dropFn->symbol) {
      const FnDecl *dfn = sym.type->inductive->dropFn;
      llvm::Function *df = nullptr;
      if (dfn->symbol->type && !dfn->symbol->type->typeParams.empty())
        df = specialize(*dfn, sym.type->typeArgs);
      else if (auto it = functions_.find(dfn->symbol); it != functions_.end())
        df = it->second;
      if (df) {
        llvm::CallInst *call = builder_->CreateCall(df, {found->second});
        call->setCallingConv(llvm::CallingConv::C);
      }
    }
    llvm::Type *sty = lower(*sym.type);
    if (!sty)
      return;
    llvm::Value *aggregate = builder_->CreateLoad(sty, found->second, "drop.struct");
    auto *i64 = llvm::Type::getInt64Ty(*ctx_);
    auto *nullPtr = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*ctx_));
    unsigned slot = 0;
    for (const auto &field : sym.type->params) {
      if (!field || !lower(*field))
        continue;
      if (field->kind == TypeKind::Vector || field->kind == TypeKind::String ||
          field->kind == TypeKind::Map) {
        llvm::Value *buf = builder_->CreateExtractValue(aggregate, slot, "drop.field");
        llvm::Value *data = builder_->CreateExtractValue(buf, 0, "drop.data");
        llvm::Value *cap = builder_->CreateExtractValue(buf, 2, "drop.cap");
        llvm::Value *owned = builder_->CreateAnd(
            builder_->CreateICmpNE(data, nullPtr),
            builder_->CreateICmpSGE(cap, llvm::ConstantInt::get(i64, 0)));
        llvm::BasicBlock *freeBB = llvm::BasicBlock::Create(*ctx_, "drop.struct.free", currentFn_);
        llvm::BasicBlock *nextBB = llvm::BasicBlock::Create(*ctx_, "drop.struct.next", currentFn_);
        builder_->CreateCondBr(owned, freeBB, nextBB);
        builder_->SetInsertPoint(freeBB);
        if (field->region == 0)
          heapFree(data);
        builder_->CreateBr(nextBB);
        builder_->SetInsertPoint(nextBB);
      }
      ++slot;
    }
    builder_->CreateStore(llvm::ConstantAggregateZero::get(sty), found->second);
    return;
  }
  auto *i64 = llvm::Type::getInt64Ty(*ctx_);
  auto *ptrTy = llvm::PointerType::getUnqual(*ctx_);
  auto *nullPtr = llvm::ConstantPointerNull::get(ptrTy);
  llvm::Value *buf = builder_->CreateLoad(bufferType(), found->second, "drop.buf");
  llvm::Value *data = builder_->CreateExtractValue(buf, 0, "drop.data");
  llvm::Value *cap = builder_->CreateExtractValue(buf, 2, "drop.cap");
  llvm::Value *rc = builder_->CreateExtractValue(buf, 3, "drop.rc");
  // String literals use a negative capacity. The bytes live in static storage
  // and must not be passed to cruft_free, even after the last shared owner.
  llvm::Value *immortal = builder_->CreateICmpSLT(cap, llvm::ConstantInt::get(i64, 0), "drop.static");
  llvm::BasicBlock *done = llvm::BasicBlock::Create(*ctx_, "drop.done", currentFn_);
  llvm::BasicBlock *live = llvm::BasicBlock::Create(*ctx_, "drop.live", currentFn_);
  builder_->CreateCondBr(builder_->CreateICmpEQ(data, nullPtr), done, live);
  builder_->SetInsertPoint(live);
  llvm::BasicBlock *freeData = llvm::BasicBlock::Create(*ctx_, "drop.free", currentFn_);
  llvm::BasicBlock *decRc = llvm::BasicBlock::Create(*ctx_, "drop.dec", currentFn_);
  builder_->CreateCondBr(builder_->CreateICmpEQ(rc, nullPtr), freeData, decRc);
  builder_->SetInsertPoint(decRc);
  // AcqRel so the last owner synchronises with every earlier share, including
  // one published to another thread through FFI or `thread.spawn`.
  llvm::Value *old = builder_->CreateAtomicRMW(llvm::AtomicRMWInst::Sub, rc,
                                               llvm::ConstantInt::get(i64, 1), llvm::MaybeAlign(),
                                               llvm::AtomicOrdering::AcquireRelease);
  llvm::BasicBlock *last = llvm::BasicBlock::Create(*ctx_, "drop.last", currentFn_);
  builder_->CreateCondBr(builder_->CreateICmpEQ(old, llvm::ConstantInt::get(i64, 1)), last, done);
  builder_->SetInsertPoint(last);
  heapFree(rc);
  builder_->CreateBr(freeData);
  builder_->SetInsertPoint(freeData);
  // Arena storage is reclaimed by cruft_arena_pop. Static string bytes are not
  // heap memory either.
  if (sym.type->region != 0) {
    builder_->CreateBr(done);
  } else {
    llvm::BasicBlock *doFree = llvm::BasicBlock::Create(*ctx_, "drop.heap", currentFn_);
    builder_->CreateCondBr(immortal, done, doFree);
    builder_->SetInsertPoint(doFree);
    heapFree(data);
    builder_->CreateBr(done);
  }
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
  int savedArenaDepth = arenaDepth_;
  llvm::Value *savedFrame = frameBase_;
  values_.clear();
  consume_ = true;
  // A lambda is a separate function. It must not bump the caller's arena,
  // even when the lambda expression itself appears inside `scope arena`.
  arenaDepth_ = 0;
  frameBase_ = nullptr;
  currentFn_ = fn;
  llvm::DebugLoc savedLoc = builder_->getCurrentDebugLocation();
  llvm::DIScope *savedScope = dbgScope_;
  dbgScope_ = nullptr;
  builder_->SetCurrentDebugLocation(llvm::DebugLoc());
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
  arenaDepth_ = savedArenaDepth;
  frameBase_ = savedFrame;
  currentFn_ = savedFn;
  dbgScope_ = savedScope;
  builder_->SetCurrentDebugLocation(savedLoc);
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
  llvm::Value *data = nullptr;
  if (e.frameOffset >= 0 && frameBase_) {
    llvm::Value *raw = builder_->CreateInBoundsGEP(
        llvm::Type::getInt8Ty(*ctx_), frameBase_, llvm::ConstantInt::get(i64, e.frameOffset),
        "arena.off");
    data = raw;
  } else {
    data = allocBytes(bytes, align);
  }
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
  // Negative capacity marks static bytes. rc stays null so `shared var` can
  // still attach a real counter without freeing the literal.
  std::int64_t capN = e.value.empty() ? static_cast<std::int64_t>(-1)
                                      : -static_cast<std::int64_t>(e.value.size());
  llvm::Value *cap = llvm::ConstantInt::getSigned(i64, capN);
  return makeBuffer(data, n, cap, nullptr);
}

llvm::Value *CodeGen::genMethod(const MethodCallExpr &m) {
  bool saved = consume_;
  consume_ = false;
  const Type *surface = m.receiver->type.get();
  llvm::Value *ownerSlot = nullptr;
  llvm::Value *recv = nullptr;
  if (surface && surface->kind == TypeKind::Ref) {
    ownerSlot = gen(*m.receiver);
    surface = surface->elem.get();
  } else if (m.receiver->kind == ExprKind::Ident) {
    const Symbol *sym = as<IdentExpr>(*m.receiver).symbol;
    if (auto it = locals_.find(sym); it != locals_.end())
      ownerSlot = it->second;
    else if (auto it = globals_.find(sym); it != globals_.end())
      ownerSlot = it->second;
  }
  if (surface && ownerSlot &&
      (surface->kind == TypeKind::Vector || surface->kind == TypeKind::String ||
       surface->kind == TypeKind::Map))
    recv = builder_->CreateLoad(bufferType(), ownerSlot, "recv.buf");
  else if (surface && ownerSlot && surface->kind == TypeKind::Slice)
    recv = builder_->CreateLoad(sliceType(), ownerSlot, "recv.slice");
  else if (surface && ownerSlot && surface->kind == TypeKind::Struct && m.receiver->type &&
           m.receiver->type->kind == TypeKind::Ref)
    recv = builder_->CreateLoad(lower(*surface), ownerSlot, "recv.struct");
  else
    recv = gen(*m.receiver);
  consume_ = saved;
  if (!surface)
    return nullptr;
  const Type &recvTy = *surface;
  auto *i64 = llvm::Type::getInt64Ty(*ctx_);

  // Lengths are i64, matching the buffer header. Do not truncate.
  auto lengthOf = [&](llvm::Value *buf) { return builder_->CreateExtractValue(buf, 1, "len"); };

  if (recvTy.kind == TypeKind::Struct && !m.invoked) {
    for (size_t i = 0; i < recvTy.params.size(); ++i) {
      const Symbol *field = i < recvTy.paramSymbols.size() ? recvTy.paramSymbols[i] : nullptr;
      if (!field || field->name != m.method || !recvTy.params[i])
        continue;
      bool storage = recvTy.params[i]->isStorage();
      if (storage)
        consume_ = true;
      // A reference's value is the pointer. `recv` is already the struct.
      llvm::Value *aggregate =
          m.receiver->type && m.receiver->type->kind == TypeKind::Ref ? recv : gen(*m.receiver);
      consume_ = saved;
      if (!aggregate)
        return nullptr;
      return builder_->CreateExtractValue(aggregate, static_cast<unsigned>(runtimeField(recvTy, i)),
                                          m.method);
    }
  }

  if (recvTy.kind == TypeKind::Text) {
    if (!recv)
      return nullptr;
    if (m.method == "data")
      return builder_->CreateExtractValue(recv, 0, "text.data");
    if (m.method == "length")
      return builder_->CreateExtractValue(recv, 1, "text.len");
  }

  if (recvTy.kind == TypeKind::Ptr && m.method == "addr")
    return builder_->CreatePtrToInt(recv, i64, "addr");

  if (recvTy.kind == TypeKind::Slice && m.method == "length")
    return builder_->CreateExtractValue(recv, 1, "slice.len");
  if (recvTy.kind == TypeKind::Slice && (m.method == "at" || m.method == "at_unchecked")) {
    llvm::Value *index = gen(*m.args[0]);
    llvm::Value *len = builder_->CreateExtractValue(recv, 1, "slice.len");
    llvm::Value *data = builder_->CreateExtractValue(recv, 0, "slice.data");
    if (m.method == "at")
      emitBoundsCheck(index, len);
    llvm::Type *elt = lower(*recvTy.elem);
    llvm::Value *slot = builder_->CreateInBoundsGEP(elt, data, index);
    return builder_->CreateLoad(elt, slot, "slice.at");
  }
  if (recvTy.kind == TypeKind::String && m.method == "length")
    return lengthOf(recv);
  if (recvTy.kind == TypeKind::String && m.method == "byte_len")
    return builder_->CreateExtractValue(recv, 1, "str.len");
  if (recvTy.kind == TypeKind::String && m.method == "data")
    return builder_->CreateExtractValue(recv, 0, "str.data");
  if (recvTy.kind == TypeKind::String && m.method == "set_len") {
    llvm::Value *slot = ownerSlot;
    if (!slot)
      return nullptr;
    llvm::Value *n = gen(*m.args[0]);
    llvm::Value *buf = builder_->CreateLoad(bufferType(), slot, "str.buf");
    llvm::Value *data = builder_->CreateExtractValue(buf, 0, "str.data");
    llvm::Value *cap = builder_->CreateExtractValue(buf, 2, "str.cap");
    auto *nullPtr = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*ctx_));
    llvm::Value *zero = llvm::ConstantInt::get(i64, 0);
    n = builder_->CreateSelect(builder_->CreateICmpSLT(n, zero), zero, n);
    llvm::Value *staticBytes = builder_->CreateICmpSLT(cap, zero);
    llvm::Value *room = builder_->CreateSub(cap, llvm::ConstantInt::get(i64, 1));
    llvm::Value *clamped = builder_->CreateSelect(builder_->CreateICmpSGT(n, room), room, n);
    // Static literals (negative cap) only adjust the view. Heap buffers keep a NUL.
    llvm::Value *chosen = builder_->CreateSelect(staticBytes, n, clamped, "str.newlen");
    llvm::BasicBlock *write = llvm::BasicBlock::Create(*ctx_, "str.set", currentFn_);
    llvm::BasicBlock *skip = llvm::BasicBlock::Create(*ctx_, "str.skip", currentFn_);
    llvm::BasicBlock *store = llvm::BasicBlock::Create(*ctx_, "str.store", currentFn_);
    llvm::Value *canWrite = builder_->CreateAnd(builder_->CreateNot(staticBytes),
                                                builder_->CreateICmpNE(data, nullPtr));
    builder_->CreateCondBr(canWrite, write, skip);
    builder_->SetInsertPoint(write);
    llvm::Value *nulAt = builder_->CreateInBoundsGEP(llvm::Type::getInt8Ty(*ctx_), data, chosen);
    builder_->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*ctx_), 0), nulAt);
    builder_->CreateBr(store);
    builder_->SetInsertPoint(skip);
    llvm::Value *skipped = builder_->CreateSelect(staticBytes, chosen, zero, "str.keeplen");
    builder_->CreateBr(store);
    builder_->SetInsertPoint(store);
    llvm::PHINode *lenPhi = builder_->CreatePHI(i64, 2, "str.len");
    lenPhi->addIncoming(chosen, write);
    lenPhi->addIncoming(skipped, skip);
    llvm::Value *updated = builder_->CreateInsertValue(buf, lenPhi, 1);
    builder_->CreateStore(updated, slot);
    return nullptr;
  }
  if (recvTy.kind == TypeKind::String && m.method == "bytes") {
    llvm::Value *slice = llvm::UndefValue::get(sliceType());
    slice = builder_->CreateInsertValue(slice, builder_->CreateExtractValue(recv, 0, "str.data"), 0);
    slice = builder_->CreateInsertValue(slice, builder_->CreateExtractValue(recv, 1, "str.len"), 1);
    return slice;
  }
  if (recvTy.kind == TypeKind::String && m.method == "append") {
    if (!ownerSlot)
      return nullptr;
    llvm::Value *extra = gen(*m.args[0]);
    llvm::Value *buf = builder_->CreateLoad(bufferType(), ownerSlot, "str.buf");
    llvm::Value *data = builder_->CreateExtractValue(buf, 0);
    llvm::Value *len = builder_->CreateExtractValue(buf, 1);
    llvm::Value *cap = builder_->CreateExtractValue(buf, 2);
    llvm::Value *add = builder_->CreateExtractValue(extra, 1, "app.len");
    llvm::Value *src = builder_->CreateExtractValue(extra, 0, "app.data");
    llvm::Value *need = builder_->CreateAdd(builder_->CreateAdd(len, add), llvm::ConstantInt::get(i64, 1));
    llvm::Value *room = builder_->CreateSelect(builder_->CreateICmpSLT(cap, llvm::ConstantInt::get(i64, 0)),
                                               llvm::ConstantInt::get(i64, 0), cap);
    llvm::BasicBlock *pre = builder_->GetInsertBlock();
    llvm::BasicBlock *grow = llvm::BasicBlock::Create(*ctx_, "str.grow", currentFn_);
    llvm::BasicBlock *put = llvm::BasicBlock::Create(*ctx_, "str.app", currentFn_);
    builder_->CreateCondBr(builder_->CreateICmpUGT(need, room), grow, put);
    builder_->SetInsertPoint(grow);
    llvm::Value *newCap = builder_->CreateSelect(
        builder_->CreateICmpULT(room, llvm::ConstantInt::get(i64, 8)), llvm::ConstantInt::get(i64, 8),
        builder_->CreateMul(room, llvm::ConstantInt::get(i64, 2)));
    llvm::Value *big = builder_->CreateICmpULT(newCap, need);
    newCap = builder_->CreateSelect(big, need, newCap);
    llvm::Value *newData = allocBytes(newCap, 1);
    builder_->CreateMemCpy(newData, llvm::Align(1), data, llvm::Align(1), len);
    builder_->CreateBr(put);
    builder_->SetInsertPoint(put);
    llvm::PHINode *curData = builder_->CreatePHI(llvm::PointerType::getUnqual(*ctx_), 2);
    llvm::PHINode *curCap = builder_->CreatePHI(i64, 2);
    curData->addIncoming(data, pre);
    curData->addIncoming(newData, grow);
    curCap->addIncoming(room, pre);
    curCap->addIncoming(newCap, grow);
    llvm::Value *dest = builder_->CreateInBoundsGEP(llvm::Type::getInt8Ty(*ctx_), curData, len);
    builder_->CreateMemCpy(dest, llvm::Align(1), src, llvm::Align(1), add);
    llvm::Value *newLen = builder_->CreateAdd(len, add);
    llvm::Value *nulAt = builder_->CreateInBoundsGEP(llvm::Type::getInt8Ty(*ctx_), curData, newLen);
    builder_->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*ctx_), 0), nulAt);
    builder_->CreateStore(makeBuffer(curData, newLen, curCap, builder_->CreateExtractValue(buf, 3)), ownerSlot);
    return nullptr;
  }
  if ((recvTy.kind == TypeKind::Vector || recvTy.kind == TypeKind::Map) && m.method == "length")
    return lengthOf(recv);
  if (recvTy.kind == TypeKind::Vector && m.method == "data")
    return builder_->CreateExtractValue(recv, 0, "vec.data");
  if (recvTy.kind == TypeKind::Vector && m.method == "slice") {
    if (!recv)
      return nullptr;
    llvm::Value *slice = llvm::UndefValue::get(sliceType());
    slice = builder_->CreateInsertValue(slice, builder_->CreateExtractValue(recv, 0, "vec.data"), 0);
    slice = builder_->CreateInsertValue(slice, builder_->CreateExtractValue(recv, 1, "vec.len"), 1);
    return slice;
  }
  if (recvTy.kind == TypeKind::Vector && (m.method == "at" || m.method == "at_unchecked")) {
    llvm::Value *index = gen(*m.args[0]);
    llvm::Value *len = builder_->CreateExtractValue(recv, 1, "vec.len");
    llvm::Value *data = builder_->CreateExtractValue(recv, 0, "vec.data");
    if (m.method == "at")
      emitBoundsCheck(index, len);
    llvm::Type *elt = lower(*recvTy.elem);
    llvm::Value *slot = builder_->CreateInBoundsGEP(elt, data, index);
    return builder_->CreateLoad(elt, slot, "vec.at");
  }

  if (recvTy.kind == TypeKind::Vector &&
      (m.method == "push" || m.method == "pop" || m.method == "set" || m.method == "reserve" ||
       m.method == "clear")) {
    if (!ownerSlot)
      return nullptr;
    llvm::Type *elt = lower(*recvTy.elem);
    const llvm::DataLayout &dl = module_->getDataLayout();
    std::uint64_t esize = dl.getTypeAllocSize(elt);
    unsigned align = static_cast<unsigned>(dl.getABITypeAlign(elt).value());
    auto loadBuf = [&]() { return builder_->CreateLoad(bufferType(), ownerSlot, "vec.buf"); };
    auto growTo = [&](llvm::Value *buf, llvm::Value *newCap) {
      llvm::Value *data = builder_->CreateExtractValue(buf, 0);
      llvm::Value *len = builder_->CreateExtractValue(buf, 1);
      llvm::Value *bytes = builder_->CreateMul(newCap, llvm::ConstantInt::get(i64, esize));
      llvm::Value *fresh = allocBytes(bytes, align);
      llvm::Value *nbytes = builder_->CreateMul(len, llvm::ConstantInt::get(i64, esize));
      builder_->CreateMemCpy(fresh, llvm::Align(align), data, llvm::Align(align), nbytes);
      if (arenaDepth_ == 0) {
        auto *nullPtr = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*ctx_));
        llvm::Value *cap = builder_->CreateExtractValue(buf, 2);
        llvm::Value *owned = builder_->CreateAnd(builder_->CreateICmpNE(data, nullPtr),
                                                 builder_->CreateICmpSGE(cap, llvm::ConstantInt::get(i64, 0)));
        llvm::BasicBlock *freeBB = llvm::BasicBlock::Create(*ctx_, "vec.freeold", currentFn_);
        llvm::BasicBlock *keepBB = llvm::BasicBlock::Create(*ctx_, "vec.keepold", currentFn_);
        builder_->CreateCondBr(owned, freeBB, keepBB);
        builder_->SetInsertPoint(freeBB);
        heapFree(data);
        builder_->CreateBr(keepBB);
        builder_->SetInsertPoint(keepBB);
      }
      llvm::Value *updated = makeBuffer(fresh, len, newCap, builder_->CreateExtractValue(buf, 3));
      builder_->CreateStore(updated, ownerSlot);
      return updated;
    };
    if (m.method == "clear") {
      llvm::Value *buf = loadBuf();
      builder_->CreateStore(makeBuffer(builder_->CreateExtractValue(buf, 0), llvm::ConstantInt::get(i64, 0),
                                       builder_->CreateExtractValue(buf, 2), builder_->CreateExtractValue(buf, 3)),
                            ownerSlot);
      return nullptr;
    }
    if (m.method == "reserve") {
      llvm::Value *n = gen(*m.args[0]);
      llvm::Value *buf = loadBuf();
      llvm::Value *cap = builder_->CreateExtractValue(buf, 2);
      llvm::BasicBlock *grow = llvm::BasicBlock::Create(*ctx_, "vec.reserve", currentFn_);
      llvm::BasicBlock *done = llvm::BasicBlock::Create(*ctx_, "vec.reserve.done", currentFn_);
      builder_->CreateCondBr(builder_->CreateICmpSGT(n, cap), grow, done);
      builder_->SetInsertPoint(grow);
      growTo(buf, n);
      builder_->CreateBr(done);
      builder_->SetInsertPoint(done);
      return nullptr;
    }
    if (m.method == "set") {
      llvm::Value *index = gen(*m.args[0]);
      llvm::Value *value = gen(*m.args[1]);
      llvm::Value *buf = loadBuf();
      emitBoundsCheck(index, builder_->CreateExtractValue(buf, 1));
      llvm::Value *data = builder_->CreateExtractValue(buf, 0);
      builder_->CreateStore(value, builder_->CreateInBoundsGEP(elt, data, index));
      return nullptr;
    }
    if (m.method == "pop") {
      llvm::Value *buf = loadBuf();
      llvm::Value *len = builder_->CreateExtractValue(buf, 1);
      emitBoundsCheck(llvm::ConstantInt::get(i64, 0), len);
      llvm::Value *last = builder_->CreateSub(len, llvm::ConstantInt::get(i64, 1));
      llvm::Value *data = builder_->CreateExtractValue(buf, 0);
      llvm::Value *el = builder_->CreateLoad(elt, builder_->CreateInBoundsGEP(elt, data, last), "vec.pop");
      builder_->CreateStore(makeBuffer(data, last, builder_->CreateExtractValue(buf, 2),
                                       builder_->CreateExtractValue(buf, 3)),
                            ownerSlot);
      return el;
    }
    llvm::Value *value = gen(*m.args[0]);
    llvm::Value *buf = loadBuf();
    llvm::Value *len = builder_->CreateExtractValue(buf, 1);
    llvm::Value *cap = builder_->CreateExtractValue(buf, 2);
    llvm::BasicBlock *pre = builder_->GetInsertBlock();
    llvm::BasicBlock *grow = llvm::BasicBlock::Create(*ctx_, "vec.grow", currentFn_);
    llvm::BasicBlock *put = llvm::BasicBlock::Create(*ctx_, "vec.push", currentFn_);
    builder_->CreateCondBr(builder_->CreateICmpEQ(len, cap), grow, put);
    builder_->SetInsertPoint(grow);
    llvm::Value *newCap = builder_->CreateSelect(builder_->CreateICmpEQ(cap, llvm::ConstantInt::get(i64, 0)),
                                                 llvm::ConstantInt::get(i64, 4),
                                                 builder_->CreateMul(cap, llvm::ConstantInt::get(i64, 2)));
    llvm::Value *grown = growTo(buf, newCap);
    // growTo may split the grow block to free the old heap buffer, so the
    // edge into the write is whichever block it left the builder in.
    llvm::BasicBlock *grownBB = builder_->GetInsertBlock();
    builder_->CreateBr(put);
    builder_->SetInsertPoint(put);
    llvm::PHINode *cur = builder_->CreatePHI(bufferType(), 2, "vec.cur");
    cur->addIncoming(buf, pre);
    cur->addIncoming(grown, grownBB);
    llvm::Value *data = builder_->CreateExtractValue(cur, 0);
    llvm::Value *n = builder_->CreateExtractValue(cur, 1);
    builder_->CreateStore(value, builder_->CreateInBoundsGEP(elt, data, n));
    builder_->CreateStore(makeBuffer(data, builder_->CreateAdd(n, llvm::ConstantInt::get(i64, 1)),
                                     builder_->CreateExtractValue(cur, 2), builder_->CreateExtractValue(cur, 3)),
                          ownerSlot);
    return nullptr;
  }

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

  // Open-addressed hash map. Capacity is a power of two. Each entry holds a
  // key, a value, and a state byte: 0 empty, 1 full, 2 tombstone. insert
  // replaces an equal key. Keys are hashed by their bits. This is not an
  // append-only pair list.
  if (recvTy.kind == TypeKind::Map &&
      (m.method == "insert" || m.method == "contains" || m.method == "remove" ||
       m.method == "get")) {
    if (!ownerSlot)
      return nullptr;
    const Type &keyTy = *recvTy.a;
    const Type &valTy = *recvTy.b;
    llvm::Type *kll = lower(keyTy);
    llvm::Type *vll = lower(valTy);
    const llvm::DataLayout &dl = module_->getDataLayout();
    std::uint64_t keySize = dl.getTypeAllocSize(kll);
    std::uint64_t valAlign = dl.getABITypeAlign(vll).value();
    std::uint64_t valSize = dl.getTypeAllocSize(vll);
    std::uint64_t valOff = (keySize + valAlign - 1) & ~(valAlign - 1);
    std::uint64_t stateOff = valOff + valSize;
    std::uint64_t strideAlign = std::max(dl.getABITypeAlign(kll).value(), valAlign);
    std::uint64_t stride = (stateOff + 1 + strideAlign - 1) & ~(strideAlign - 1);
    auto *i8 = llvm::Type::getInt8Ty(*ctx_);
    auto *ptrTy = llvm::PointerType::getUnqual(*ctx_);
    auto *i1 = llvm::Type::getInt1Ty(*ctx_);

    auto hashOf = [&](llvm::Value *key) -> llvm::Value * {
      llvm::Value *bits = nullptr;
      if (keyTy.kind == TypeKind::F32)
        bits = builder_->CreateZExt(builder_->CreateBitCast(key, llvm::Type::getInt32Ty(*ctx_)), i64);
      else if (keyTy.kind == TypeKind::F64)
        bits = builder_->CreateBitCast(key, i64);
      else if (keyTy.kind == TypeKind::Ptr)
        bits = builder_->CreatePtrToInt(key, i64);
      else if (keyTy.kind == TypeKind::Bool)
        bits = builder_->CreateZExt(key, i64);
      else if (key->getType()->isIntegerTy() && key->getType()->getIntegerBitWidth() < 64)
        bits = keyTy.isSignedInteger() ? builder_->CreateSExt(key, i64) : builder_->CreateZExt(key, i64);
      else
        bits = key;
      return builder_->CreateMul(bits, llvm::ConstantInt::get(i64, 0x9E3779B185EBCA87ULL), "map.hash");
    };
    auto keyEq = [&](llvm::Value *a, llvm::Value *b) -> llvm::Value * {
      if (keyTy.kind == TypeKind::F32) {
        auto *i32 = llvm::Type::getInt32Ty(*ctx_);
        return builder_->CreateICmpEQ(builder_->CreateBitCast(a, i32), builder_->CreateBitCast(b, i32));
      }
      if (keyTy.kind == TypeKind::F64)
        return builder_->CreateICmpEQ(builder_->CreateBitCast(a, i64), builder_->CreateBitCast(b, i64));
      return builder_->CreateICmpEQ(a, b);
    };
    auto entryAt = [&](llvm::Value *data, llvm::Value *index) {
      return builder_->CreateInBoundsGEP(i8, data,
                                         builder_->CreateMul(index, llvm::ConstantInt::get(i64, stride)));
    };
    auto statePtr = [&](llvm::Value *entry) {
      return builder_->CreateInBoundsGEP(i8, entry, llvm::ConstantInt::get(i64, stateOff));
    };

    llvm::Value *key = gen(*m.args[0]);
    llvm::Value *val = m.method == "insert" ? gen(*m.args[1]) : nullptr;
    llvm::Value *buf0 = builder_->CreateLoad(bufferType(), ownerSlot, "map.buf");
    llvm::Value *len0 = builder_->CreateExtractValue(buf0, 1);
    llvm::Value *cap0 = builder_->CreateExtractValue(buf0, 2);
    llvm::BasicBlock *grow = llvm::BasicBlock::Create(*ctx_, "map.grow", currentFn_);
    llvm::BasicBlock *probe = llvm::BasicBlock::Create(*ctx_, "map.probe", currentFn_);
    if (m.method == "insert") {
      llvm::Value *len1 = builder_->CreateAdd(len0, llvm::ConstantInt::get(i64, 1));
      llvm::Value *tooFull = builder_->CreateICmpUGT(
          builder_->CreateMul(len1, llvm::ConstantInt::get(i64, 10)),
          builder_->CreateMul(cap0, llvm::ConstantInt::get(i64, 7)));
      llvm::Value *emptyCap = builder_->CreateICmpEQ(cap0, llvm::ConstantInt::get(i64, 0));
      builder_->CreateCondBr(builder_->CreateOr(emptyCap, tooFull), grow, probe);
    } else {
      builder_->CreateBr(probe);
    }

    builder_->SetInsertPoint(grow);
    {
      llvm::Value *oldData = builder_->CreateExtractValue(buf0, 0);
      llvm::Value *newCap = builder_->CreateSelect(builder_->CreateICmpEQ(cap0, llvm::ConstantInt::get(i64, 0)),
                                                   llvm::ConstantInt::get(i64, 8),
                                                   builder_->CreateMul(cap0, llvm::ConstantInt::get(i64, 2)));
      llvm::Value *bytes = builder_->CreateMul(newCap, llvm::ConstantInt::get(i64, stride));
      llvm::Value *fresh = allocBytes(bytes, static_cast<unsigned>(std::max<std::uint64_t>(strideAlign, 1)));
      builder_->CreateMemSet(fresh, llvm::ConstantInt::get(i8, 0), bytes, llvm::MaybeAlign(1));
      llvm::AllocaInst *ri = createEntryAlloca(i64, "re.i");
      builder_->CreateStore(llvm::ConstantInt::get(i64, 0), ri);
      llvm::BasicBlock *rcond = llvm::BasicBlock::Create(*ctx_, "re.cond", currentFn_);
      llvm::BasicBlock *rbody = llvm::BasicBlock::Create(*ctx_, "re.body", currentFn_);
      llvm::BasicBlock *rdone = llvm::BasicBlock::Create(*ctx_, "re.done", currentFn_);
      builder_->CreateBr(rcond);
      builder_->SetInsertPoint(rcond);
      llvm::Value *i = builder_->CreateLoad(i64, ri);
      builder_->CreateCondBr(builder_->CreateICmpULT(i, cap0), rbody, rdone);
      builder_->SetInsertPoint(rbody);
      llvm::Value *oldEntry = entryAt(oldData, i);
      llvm::Value *st = builder_->CreateLoad(i8, statePtr(oldEntry));
      llvm::BasicBlock *rmove = llvm::BasicBlock::Create(*ctx_, "re.move", currentFn_);
      llvm::BasicBlock *rnext = llvm::BasicBlock::Create(*ctx_, "re.next", currentFn_);
      builder_->CreateCondBr(builder_->CreateICmpEQ(st, llvm::ConstantInt::get(i8, 1)), rmove, rnext);
      builder_->SetInsertPoint(rmove);
      llvm::Value *okey = builder_->CreateLoad(kll, oldEntry);
      llvm::Value *oval =
          builder_->CreateLoad(vll, builder_->CreateInBoundsGEP(i8, oldEntry, llvm::ConstantInt::get(i64, valOff)));
      llvm::Value *mask = builder_->CreateSub(newCap, llvm::ConstantInt::get(i64, 1));
      llvm::AllocaInst *rj = createEntryAlloca(i64, "re.j");
      builder_->CreateStore(builder_->CreateAnd(hashOf(okey), mask), rj);
      llvm::BasicBlock *pcond = llvm::BasicBlock::Create(*ctx_, "re.place", currentFn_);
      llvm::BasicBlock *pstep = llvm::BasicBlock::Create(*ctx_, "re.step", currentFn_);
      llvm::BasicBlock *pput = llvm::BasicBlock::Create(*ctx_, "re.put", currentFn_);
      builder_->CreateBr(pcond);
      builder_->SetInsertPoint(pcond);
      llvm::Value *j = builder_->CreateLoad(i64, rj);
      llvm::Value *slotE = entryAt(fresh, j);
      llvm::Value *pst = builder_->CreateLoad(i8, statePtr(slotE));
      builder_->CreateCondBr(builder_->CreateICmpEQ(pst, llvm::ConstantInt::get(i8, 0)), pput, pstep);
      builder_->SetInsertPoint(pstep);
      builder_->CreateStore(builder_->CreateAnd(builder_->CreateAdd(j, llvm::ConstantInt::get(i64, 1)), mask), rj);
      builder_->CreateBr(pcond);
      builder_->SetInsertPoint(pput);
      builder_->CreateStore(okey, slotE);
      builder_->CreateStore(oval, builder_->CreateInBoundsGEP(i8, slotE, llvm::ConstantInt::get(i64, valOff)));
      builder_->CreateStore(llvm::ConstantInt::get(i8, 1), statePtr(slotE));
      builder_->CreateBr(rnext);
      builder_->SetInsertPoint(rnext);
      builder_->CreateStore(builder_->CreateAdd(i, llvm::ConstantInt::get(i64, 1)), ri);
      builder_->CreateBr(rcond);
      builder_->SetInsertPoint(rdone);
      if (arenaDepth_ == 0) {
        auto *nullPtr = llvm::ConstantPointerNull::get(ptrTy);
        llvm::Value *owned = builder_->CreateAnd(builder_->CreateICmpNE(oldData, nullPtr),
                                                 builder_->CreateICmpNE(cap0, llvm::ConstantInt::get(i64, 0)));
        llvm::BasicBlock *freeBB = llvm::BasicBlock::Create(*ctx_, "map.freeold", currentFn_);
        llvm::BasicBlock *keepBB = llvm::BasicBlock::Create(*ctx_, "map.kept", currentFn_);
        builder_->CreateCondBr(owned, freeBB, keepBB);
        builder_->SetInsertPoint(freeBB);
        heapFree(oldData);
        builder_->CreateBr(keepBB);
        builder_->SetInsertPoint(keepBB);
      }
      builder_->CreateStore(makeBuffer(fresh, len0, newCap, builder_->CreateExtractValue(buf0, 3)), ownerSlot);
      builder_->CreateBr(probe);
    }

    builder_->SetInsertPoint(probe);
    llvm::Value *buf = builder_->CreateLoad(bufferType(), ownerSlot, "map.cur");
    llvm::Value *data = builder_->CreateExtractValue(buf, 0);
    llvm::Value *len = builder_->CreateExtractValue(buf, 1);
    llvm::Value *cap = builder_->CreateExtractValue(buf, 2);
    llvm::AllocaInst *foundA = createEntryAlloca(i1, "map.found");
    llvm::AllocaInst *placeA = createEntryAlloca(i64, "map.place");
    llvm::AllocaInst *canA = createEntryAlloca(i1, "map.can");
    builder_->CreateStore(llvm::ConstantInt::getFalse(*ctx_), foundA);
    builder_->CreateStore(llvm::ConstantInt::get(i64, 0), placeA);
    builder_->CreateStore(llvm::ConstantInt::getFalse(*ctx_), canA);
    llvm::BasicBlock *loop = llvm::BasicBlock::Create(*ctx_, "map.loop", currentFn_);
    llvm::BasicBlock *after = llvm::BasicBlock::Create(*ctx_, "map.after", currentFn_);
    builder_->CreateCondBr(builder_->CreateICmpEQ(cap, llvm::ConstantInt::get(i64, 0)), after, loop);

    builder_->SetInsertPoint(loop);
    llvm::Value *mask = builder_->CreateSub(cap, llvm::ConstantInt::get(i64, 1));
    llvm::AllocaInst *idxA = createEntryAlloca(i64, "map.idx");
    llvm::AllocaInst *tripsA = createEntryAlloca(i64, "map.trips");
    llvm::AllocaInst *tombA = createEntryAlloca(i1, "map.tomb");
    builder_->CreateStore(builder_->CreateAnd(hashOf(key), mask), idxA);
    builder_->CreateStore(llvm::ConstantInt::get(i64, 0), tripsA);
    builder_->CreateStore(llvm::ConstantInt::getFalse(*ctx_), tombA);
    llvm::BasicBlock *lcond = llvm::BasicBlock::Create(*ctx_, "map.lcond", currentFn_);
    llvm::BasicBlock *lbody = llvm::BasicBlock::Create(*ctx_, "map.lbody", currentFn_);
    builder_->CreateBr(lcond);
    builder_->SetInsertPoint(lcond);
    builder_->CreateCondBr(builder_->CreateICmpULT(builder_->CreateLoad(i64, tripsA), cap), lbody, after);
    builder_->SetInsertPoint(lbody);
    llvm::Value *idx = builder_->CreateLoad(i64, idxA);
    llvm::Value *entry = entryAt(data, idx);
    llvm::Value *st = builder_->CreateLoad(i8, statePtr(entry));
    llvm::BasicBlock *fullBB = llvm::BasicBlock::Create(*ctx_, "map.full", currentFn_);
    llvm::BasicBlock *notFull = llvm::BasicBlock::Create(*ctx_, "map.notfull", currentFn_);
    llvm::BasicBlock *emptyBB = llvm::BasicBlock::Create(*ctx_, "map.empty", currentFn_);
    llvm::BasicBlock *tombBB = llvm::BasicBlock::Create(*ctx_, "map.tombstone", currentFn_);
    llvm::BasicBlock *stepBB = llvm::BasicBlock::Create(*ctx_, "map.step", currentFn_);
    llvm::BasicBlock *hitBB = llvm::BasicBlock::Create(*ctx_, "map.hit", currentFn_);
    builder_->CreateCondBr(builder_->CreateICmpEQ(st, llvm::ConstantInt::get(i8, 1)), fullBB, notFull);
    builder_->SetInsertPoint(fullBB);
    llvm::Value *have = builder_->CreateLoad(kll, entry);
    builder_->CreateCondBr(keyEq(have, key), hitBB, stepBB);
    builder_->SetInsertPoint(hitBB);
    builder_->CreateStore(llvm::ConstantInt::getTrue(*ctx_), foundA);
    builder_->CreateStore(idx, placeA);
    builder_->CreateStore(llvm::ConstantInt::getTrue(*ctx_), canA);
    builder_->CreateBr(after);
    builder_->SetInsertPoint(notFull);
    builder_->CreateCondBr(builder_->CreateICmpEQ(st, llvm::ConstantInt::get(i8, 0)), emptyBB, tombBB);
    builder_->SetInsertPoint(emptyBB);
    llvm::BasicBlock *takeEmpty = llvm::BasicBlock::Create(*ctx_, "map.takeempty", currentFn_);
    builder_->CreateCondBr(builder_->CreateLoad(i1, tombA), after, takeEmpty);
    builder_->SetInsertPoint(takeEmpty);
    builder_->CreateStore(idx, placeA);
    builder_->CreateStore(llvm::ConstantInt::getTrue(*ctx_), canA);
    builder_->CreateBr(after);
    builder_->SetInsertPoint(tombBB);
    llvm::BasicBlock *takeTomb = llvm::BasicBlock::Create(*ctx_, "map.taketomb", currentFn_);
    builder_->CreateCondBr(builder_->CreateLoad(i1, tombA), stepBB, takeTomb);
    builder_->SetInsertPoint(takeTomb);
    builder_->CreateStore(llvm::ConstantInt::getTrue(*ctx_), tombA);
    builder_->CreateStore(idx, placeA);
    builder_->CreateStore(llvm::ConstantInt::getTrue(*ctx_), canA);
    builder_->CreateBr(stepBB);
    builder_->SetInsertPoint(stepBB);
    builder_->CreateStore(builder_->CreateAnd(builder_->CreateAdd(idx, llvm::ConstantInt::get(i64, 1)), mask), idxA);
    builder_->CreateStore(builder_->CreateAdd(builder_->CreateLoad(i64, tripsA), llvm::ConstantInt::get(i64, 1)),
                          tripsA);
    builder_->CreateBr(lcond);

    builder_->SetInsertPoint(after);
    llvm::Value *found = builder_->CreateLoad(i1, foundA);
    llvm::Value *place = builder_->CreateLoad(i64, placeA);
    llvm::Value *can = builder_->CreateLoad(i1, canA);
    llvm::Value *dest = entryAt(data, place);
    if (m.method == "contains")
      return found;
    if (m.method == "remove") {
      llvm::BasicBlock *yes = llvm::BasicBlock::Create(*ctx_, "map.rm", currentFn_);
      llvm::BasicBlock *no = llvm::BasicBlock::Create(*ctx_, "map.rm.no", currentFn_);
      llvm::BasicBlock *done = llvm::BasicBlock::Create(*ctx_, "map.rm.done", currentFn_);
      builder_->CreateCondBr(found, yes, no);
      builder_->SetInsertPoint(yes);
      builder_->CreateStore(llvm::ConstantInt::get(i8, 2), statePtr(dest));
      llvm::Value *newLen = builder_->CreateSub(len, llvm::ConstantInt::get(i64, 1));
      builder_->CreateStore(makeBuffer(data, newLen, cap, builder_->CreateExtractValue(buf, 3)), ownerSlot);
      builder_->CreateBr(done);
      builder_->SetInsertPoint(no);
      builder_->CreateBr(done);
      builder_->SetInsertPoint(done);
      llvm::PHINode *phi = builder_->CreatePHI(i1, 2, "removed");
      phi->addIncoming(llvm::ConstantInt::getTrue(*ctx_), yes);
      phi->addIncoming(llvm::ConstantInt::getFalse(*ctx_), no);
      return phi;
    }
    if (m.method == "get" && m.type && m.type->kind == TypeKind::Inductive) {
      llvm::StructType *optTy = llvm::cast<llvm::StructType>(lower(*m.type));
      const Symbol *some = nullptr;
      if (m.type->inductive)
        for (const Symbol *c : m.type->inductive->ctors)
          if (c && c->ctorIndex == 1)
            some = c;
      llvm::BasicBlock *yes = llvm::BasicBlock::Create(*ctx_, "map.some", currentFn_);
      llvm::BasicBlock *no = llvm::BasicBlock::Create(*ctx_, "map.none", currentFn_);
      llvm::BasicBlock *done = llvm::BasicBlock::Create(*ctx_, "map.get.done", currentFn_);
      builder_->CreateCondBr(found, yes, no);
      builder_->SetInsertPoint(no);
      llvm::Value *noneV = llvm::ConstantAggregateZero::get(optTy);
      builder_->CreateBr(done);
      builder_->SetInsertPoint(yes);
      llvm::AllocaInst *box = createEntryAlloca(optTy, "some");
      builder_->CreateStore(llvm::ConstantAggregateZero::get(optTy), box);
      builder_->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt32Ty(*ctx_), 1),
                            builder_->CreateStructGEP(optTy, box, 0));
      if (some && ctorPayload(*some, *m.type)->getNumElements() > 0) {
        llvm::StructType *pay = ctorPayload(*some, *m.type);
        llvm::Value *payPtr = builder_->CreateStructGEP(optTy, box, 1);
        llvm::Value *got =
            builder_->CreateLoad(vll, builder_->CreateInBoundsGEP(i8, dest, llvm::ConstantInt::get(i64, valOff)));
        builder_->CreateStore(got, builder_->CreateStructGEP(pay, payPtr, 0));
      }
      llvm::Value *someV = builder_->CreateLoad(optTy, box);
      builder_->CreateBr(done);
      builder_->SetInsertPoint(done);
      llvm::PHINode *phi = builder_->CreatePHI(optTy, 2, "map.get");
      phi->addIncoming(noneV, no);
      phi->addIncoming(someV, yes);
      return phi;
    }
    llvm::BasicBlock *hit = llvm::BasicBlock::Create(*ctx_, "map.write", currentFn_);
    llvm::BasicBlock *miss = llvm::BasicBlock::Create(*ctx_, "map.miss", currentFn_);
    llvm::BasicBlock *done = llvm::BasicBlock::Create(*ctx_, "map.insert.done", currentFn_);
    builder_->CreateCondBr(found, hit, miss);
    builder_->SetInsertPoint(hit);
    builder_->CreateStore(val, builder_->CreateInBoundsGEP(i8, dest, llvm::ConstantInt::get(i64, valOff)));
    builder_->CreateBr(done);
    builder_->SetInsertPoint(miss);
    llvm::BasicBlock *put = llvm::BasicBlock::Create(*ctx_, "map.put", currentFn_);
    llvm::BasicBlock *full = llvm::BasicBlock::Create(*ctx_, "map.fulltab", currentFn_);
    builder_->CreateCondBr(can, put, full);
    builder_->SetInsertPoint(full);
    builder_->CreateCall(boundsFailCallee());
    builder_->CreateUnreachable();
    builder_->SetInsertPoint(put);
    builder_->CreateStore(key, dest);
    builder_->CreateStore(val, builder_->CreateInBoundsGEP(i8, dest, llvm::ConstantInt::get(i64, valOff)));
    builder_->CreateStore(llvm::ConstantInt::get(i8, 1), statePtr(dest));
    llvm::Value *newLen = builder_->CreateAdd(len, llvm::ConstantInt::get(i64, 1));
    builder_->CreateStore(makeBuffer(data, newLen, cap, builder_->CreateExtractValue(buf, 3)), ownerSlot);
    builder_->CreateBr(done);
    builder_->SetInsertPoint(done);
    return nullptr;
  }
  return nullptr;
}

llvm::DIFile *CodeGen::debugFile(const SourceLocation &loc) {
  if (!dbg_)
    return nullptr;
  if (!loc.file || loc.file->empty())
    return dbgFile_;
  const std::string &full = *loc.file;
  if (auto it = dbgFiles_.find(full); it != dbgFiles_.end())
    return it->second;
  std::string name = full;
  std::string dir = ".";
  auto slash = full.find_last_of("/\\");
  if (slash != std::string::npos) {
    dir = full.substr(0, slash);
    name = full.substr(slash + 1);
  }
  llvm::DIFile *file = dbg_->createFile(name, dir);
  dbgFiles_[full] = file;
  return file;
}

const Type &CodeGen::debugSubst(const Type &type) const {
  if (type.kind == TypeKind::TyVar && type.tyVar) {
    auto it = tySubst_.find(type.tyVar);
    if (it != tySubst_.end() && it->second)
      return *it->second;
  }
  return type;
}

llvm::DIType *CodeGen::debugType(const Type &typeIn) {
  if (!dbg_)
    return nullptr;
  const Type &type = debugSubst(typeIn);
  if (type.isErased() || type.kind == TypeKind::Void || type.kind == TypeKind::Error)
    return nullptr;
  if (auto it = dbgTypes_.find(&type); it != dbgTypes_.end())
    return it->second;
  auto basic = [&](const char *name, unsigned size, unsigned encoding) -> llvm::DIBasicType * {
    std::string key = std::string(name) + "/" + std::to_string(size);
    if (auto found = dbgBasics_.find(key); found != dbgBasics_.end())
      return found->second;
    llvm::DIBasicType *ty = dbg_->createBasicType(name, size, encoding);
    dbgBasics_[key] = ty;
    return ty;
  };
  llvm::DIType *built = nullptr;
  using namespace llvm::dwarf;
  switch (type.kind) {
  case TypeKind::I8: built = basic("i8", 8, DW_ATE_signed); break;
  case TypeKind::I16: built = basic("i16", 16, DW_ATE_signed); break;
  case TypeKind::I32: built = basic("i32", 32, DW_ATE_signed); break;
  case TypeKind::I64: built = basic("i64", 64, DW_ATE_signed); break;
  case TypeKind::ISize: built = basic("isize", Type::pointerBits(), DW_ATE_signed); break;
  case TypeKind::U8: built = basic("u8", 8, DW_ATE_unsigned); break;
  case TypeKind::U16: built = basic("u16", 16, DW_ATE_unsigned); break;
  case TypeKind::U32: built = basic("u32", 32, DW_ATE_unsigned); break;
  case TypeKind::U64: built = basic("u64", 64, DW_ATE_unsigned); break;
  case TypeKind::USize: built = basic("usize", Type::pointerBits(), DW_ATE_unsigned); break;
  case TypeKind::F32: built = basic("f32", 32, DW_ATE_float); break;
  case TypeKind::F64: built = basic("f64", 64, DW_ATE_float); break;
  case TypeKind::Bool: built = basic("bool", 8, DW_ATE_boolean); break;
  case TypeKind::Ptr:
  case TypeKind::Fn:
    built = dbg_->createPointerType(basic("u8", 8, DW_ATE_unsigned), Type::pointerBits());
    break;
  case TypeKind::Ref: {
    llvm::DIType *pointee = type.elem ? debugType(*type.elem) : nullptr;
    if (!pointee)
      pointee = basic("u8", 8, DW_ATE_unsigned);
    built = dbg_->createPointerType(pointee, Type::pointerBits());
    break;
  }
  default:
    built = dbg_->createUnspecifiedType("value");
    break;
  }
  if (built)
    dbgTypes_[&type] = built;
  return built;
}

llvm::DISubroutineType *CodeGen::debugSubroutine(const Type &fnType) {
  std::vector<llvm::Metadata *> parts;
  const Type *ret = fnType.ret ? &debugSubst(*fnType.ret) : nullptr;
  if (!ret || ret->kind == TypeKind::Void || ret->isErased() || !lower(*fnType.ret))
    parts.push_back(nullptr);
  else
    parts.push_back(debugType(*ret));
  for (const auto &param : fnType.params) {
    if (!param || !lower(*param))
      continue;
    parts.push_back(debugType(*param));
  }
  return dbg_->createSubroutineType(dbg_->getOrCreateTypeArray(parts));
}

void CodeGen::declareLocal(llvm::Value *slot, const std::string &name, const SourceLocation &loc,
                           const Type *type, bool parameter, unsigned argNo) {
  if (!dbg_ || !slot || name.empty() || !type || !dbgScope_)
    return;
  auto *scope = llvm::dyn_cast<llvm::DILocalScope>(dbgScope_);
  if (!scope)
    return;
  llvm::DIType *dty = debugType(*type);
  if (!dty)
    return;
  unsigned line = loc.line == 0 ? 1 : loc.line;
  unsigned column = loc.column == 0 ? 1 : loc.column;
  llvm::DIFile *file = debugFile(loc);
  if (!file)
    file = dbgFile_;
  llvm::DILocalVariable *var =
      parameter ? dbg_->createParameterVariable(scope, name, argNo == 0 ? 1 : argNo, file, line, dty,
                                                true)
                : dbg_->createAutoVariable(scope, name, file, line, dty, true);
  auto *dl = llvm::DILocation::get(*ctx_, line, column, scope);
  llvm::DIExpression *expr = dbg_->createExpression();
  llvm::BasicBlock *bb = builder_->GetInsertBlock();
  if (!bb)
    return;
  auto ip = builder_->GetInsertPoint();
  if (ip != bb->end())
    dbg_->insertDeclare(slot, var, expr, dl, &*ip);
  else
    dbg_->insertDeclare(slot, var, expr, dl, bb);
}

void CodeGen::applySanitizer() {
  if (!module_ || options_.sanitize.empty())
    return;
  llvm::Attribute::AttrKind kind = llvm::Attribute::None;
  unsigned flag = 0;
  if (options_.sanitize == "address") {
    kind = llvm::Attribute::SanitizeAddress;
    flag = 1;
  } else if (options_.sanitize == "thread") {
    kind = llvm::Attribute::SanitizeThread;
    flag = 2;
  } else if (options_.sanitize == "memory") {
    kind = llvm::Attribute::SanitizeMemory;
    flag = 3;
  } else {
    return;
  }
  for (llvm::Function &fn : *module_) {
    if (!fn.isDeclaration())
      fn.addFnAttr(kind);
  }
  module_->addModuleFlag(llvm::Module::Warning, "Cruft Sanitizer", flag);
}

void CodeGen::setLoc(const SourceLocation &loc) {
  if (!dbgScope_ || loc.line == 0)
    return;
  builder_->SetCurrentDebugLocation(llvm::DILocation::get(
      *ctx_, loc.line, loc.column == 0 ? 1 : loc.column, dbgScope_));
}

llvm::Value *CodeGen::genCast(const CastExpr &e) {
  llvm::Value *v = e.operand ? gen(*e.operand) : nullptr;
  llvm::Type *dst = e.type ? lower(*e.type) : nullptr;
  if (!v || !dst || !e.operand || !e.operand->type || !e.type)
    return v;
  const Type &src = *e.operand->type;
  const Type &to = *e.type;
  if (v->getType() == dst)
    return v;
  if (src.isInteger() && to.isInteger())
    return builder_->CreateIntCast(v, dst, src.isSignedInteger(), "cast");
  if (src.isInteger() && to.isFloat())
    return src.isSignedInteger() ? builder_->CreateSIToFP(v, dst, "cast")
                                 : builder_->CreateUIToFP(v, dst, "cast");
  if (src.isFloat() && to.isInteger())
    return to.isSignedInteger() ? builder_->CreateFPToSI(v, dst, "cast")
                                : builder_->CreateFPToUI(v, dst, "cast");
  if (src.isFloat() && to.isFloat())
    return builder_->CreateFPCast(v, dst, "cast");
  if (src.kind == TypeKind::Bool && to.isInteger())
    return builder_->CreateZExt(v, dst, "cast");
  if (src.kind == TypeKind::Ptr && to.isInteger())
    return builder_->CreatePtrToInt(v, dst, "cast");
  if (src.isInteger() && to.kind == TypeKind::Ptr)
    return builder_->CreateIntToPtr(v, dst, "cast");
  return v;
}

llvm::Value *CodeGen::genBorrow(const BorrowExpr &e) {
  if (e.place && e.place->kind == ExprKind::Ident) {
    const Symbol *sym = as<IdentExpr>(*e.place).symbol;
    if (auto it = locals_.find(sym); it != locals_.end())
      return it->second;
    if (auto it = globals_.find(sym); it != globals_.end())
      return it->second;
  }
  llvm::Value *v = e.place ? gen(*e.place) : nullptr;
  if (!e.place || !e.place->type || !lower(*e.place->type))
    return v;
  llvm::AllocaInst *tmp = createEntryAlloca(lower(*e.place->type), "borrow");
  if (v)
    builder_->CreateStore(v, tmp);
  return tmp;
}

llvm::Value *CodeGen::genTry(const TryExpr &e) {
  llvm::Value *v = e.operand ? gen(*e.operand) : nullptr;
  if (!v || !e.operand || !e.operand->type || e.operand->type->kind != TypeKind::Inductive ||
      !currentRet_)
    return nullptr;
  const Type &opTy = *e.operand->type;
  bool option = opTy.inductive && opTy.inductive->name == "Option";
  llvm::StructType *self = llvm::cast<llvm::StructType>(lower(opTy));
  llvm::AllocaInst *tmp = createEntryAlloca(self, "try.tmp");
  builder_->CreateStore(v, tmp);
  llvm::Value *tag = builder_->CreateLoad(llvm::Type::getInt32Ty(*ctx_),
                                          builder_->CreateStructGEP(self, tmp, 0), "try.tag");
  llvm::Value *failed = option
                            ? builder_->CreateICmpEQ(tag, llvm::ConstantInt::get(llvm::Type::getInt32Ty(*ctx_), 0))
                            : builder_->CreateICmpNE(tag, llvm::ConstantInt::get(llvm::Type::getInt32Ty(*ctx_), 0));
  llvm::BasicBlock *failBB = llvm::BasicBlock::Create(*ctx_, "try.fail", currentFn_);
  llvm::BasicBlock *okBB = llvm::BasicBlock::Create(*ctx_, "try.ok", currentFn_);
  builder_->CreateCondBr(failed, failBB, okBB);
  builder_->SetInsertPoint(failBB);
  if (lower(*currentRet_)) {
    llvm::StructType *retTy = llvm::cast<llvm::StructType>(lower(*currentRet_));
    llvm::AllocaInst *retSlot = createEntryAlloca(retTy, "try.ret");
    builder_->CreateStore(llvm::ConstantAggregateZero::get(retTy), retSlot);
    if (!option) {
      builder_->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt32Ty(*ctx_), 1),
                            builder_->CreateStructGEP(retTy, retSlot, 0));
      const Symbol *srcErr = nullptr;
      const Symbol *dstErr = nullptr;
      if (opTy.inductive)
        for (const Symbol *c : opTy.inductive->ctors)
          if (c && c->ctorIndex == 1)
            srcErr = c;
      if (currentRet_->inductive)
        for (const Symbol *c : currentRet_->inductive->ctors)
          if (c && c->ctorIndex == 1)
            dstErr = c;
      if (srcErr && dstErr && retTy->getNumElements() > 1 && self->getNumElements() > 1) {
        llvm::StructType *srcP = ctorPayload(*srcErr, opTy);
        llvm::StructType *dstP = ctorPayload(*dstErr, *currentRet_);
        if (srcP->getNumElements() > 0 && dstP->getNumElements() > 0) {
          llvm::Value *srcPtr = builder_->CreateStructGEP(self, tmp, 1);
          llvm::Value *dstPtr = builder_->CreateStructGEP(retTy, retSlot, 1);
          llvm::Value *err = builder_->CreateLoad(srcP->getElementType(0),
                                                  builder_->CreateStructGEP(srcP, srcPtr, 0));
          builder_->CreateStore(err, builder_->CreateStructGEP(dstP, dstPtr, 0));
        }
      }
    }
    for (auto it = dropStacks_.rbegin(); it != dropStacks_.rend(); ++it)
      emitDrops(*it);
    emitArenaPops();
    builder_->CreateRet(builder_->CreateLoad(retTy, retSlot));
  } else {
    builder_->CreateUnreachable();
  }
  builder_->SetInsertPoint(okBB);
  const Symbol *okCtor = nullptr;
  unsigned okTag = option ? 1u : 0u;
  if (opTy.inductive)
    for (const Symbol *c : opTy.inductive->ctors)
      if (c && c->ctorIndex == static_cast<int>(okTag))
        okCtor = c;
  if (!okCtor || self->getNumElements() < 2)
    return nullptr;
  llvm::StructType *pay = ctorPayload(*okCtor, opTy);
  if (pay->getNumElements() == 0)
    return nullptr;
  llvm::Value *payPtr = builder_->CreateStructGEP(self, tmp, 1);
  return builder_->CreateLoad(pay->getElementType(0), builder_->CreateStructGEP(pay, payPtr, 0), "try.okv");
}

} // namespace cruft
