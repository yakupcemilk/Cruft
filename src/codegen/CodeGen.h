// CodeGen.h - LLVM IR generation for Cruft.
//
// ABI contract:
//   * every function (defined or declared) uses llvm::CallingConv::C;
//   * `pub` and `extern` functions keep their unmangled Cruft name, so they
//     link directly against C and against Rust `extern "C"`;
//   * `pri` symbols get internal linkage and a package-prefixed name;
//   * i32/i64/f32/f64 map to the identical C types, `bool` is an i1 with
//     `zeroext` at call boundaries (what clang emits for `_Bool`).
//
// Proof erasure runs as a middle-end pass before this lowering. Values of
// type I, Path(..), Equiv(..) and Type have no runtime representation.
// Parameters of such types vanish from signatures, locals of such types
// allocate nothing, and expressions of such types are never emitted.
// `transport` along refl is the identity; `hcomp` over data compiles to its
// base; path constructors of HITs are erased entirely.
//
// `pub` symbols keep an unmangled C name when it is unique. The stable
// spelling `_C_<package>_<module>_<symbol>` is that name under collision,
// and an external alias otherwise. `pri` symbols stay internal.
#pragma once

#include "ast/AST.h"
#include "Diagnostics.h"
#include "sema/TypeChecker.h"
#include "sema/Types.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Target/TargetMachine.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cruft {

struct CodeGenOptions {
  std::string targetTriple; ///< empty => host default
  unsigned optLevel = 0;    ///< 0..3
  std::vector<std::string> linkLibraries; ///< recorded as module metadata
};

class CodeGen {
public:
  CodeGen(ast::Program &program, sema::TypeChecker &checker, DiagnosticEngine &diags,
          std::string moduleName, CodeGenOptions options);
  ~CodeGen();

  /// Lowers the whole program. Returns false on failure (diagnostics issued).
  bool generate();

  /// Runs the default LLVM pipeline for the configured -O level.
  void optimize();

  bool writeIR(const std::string &path);
  bool writeObject(const std::string &path);
  bool writeAssembly(const std::string &path);

  llvm::Module &module() { return *module_; }

private:
  // --- types ---
  llvm::Type *lower(const sema::Type &t);              ///< nullptr for erased types
  llvm::FunctionType *lowerFnType(const sema::Type &t); ///< erased params dropped
  void lowerInductive(const sema::Symbol &typeSym);

  // --- declarations ---
  void declareFunction(const ast::FnDecl &fn);
  void defineGlobal(const ast::GlobalDecl &g);
  void defineFunction(const ast::FnDecl &fn);
  void applyBoolAttributes(llvm::Function &f, const sema::Type &fnType);

  // --- statements ---
  void genBlock(const ast::Block &block);
  void genStmt(const ast::Stmt &stmt);
  [[nodiscard]] bool blockTerminated() const;

  // --- expressions ---
  llvm::Value *gen(const ast::Expr &e); ///< nullptr for erased values
  llvm::Value *genIdent(const ast::IdentExpr &e);
  llvm::Value *genBinary(const ast::BinaryExpr &e);
  llvm::Value *genUnary(const ast::UnaryExpr &e);
  llvm::Value *genCall(const ast::CallExpr &e);
  llvm::Value *genBuiltin(const ast::CallExpr &e);
  llvm::Value *genCtorCall(const ast::CallExpr &e, const sema::Symbol &ctor);
  llvm::Value *genMatch(const ast::MatchExpr &e);
  llvm::Value *genPrint(const ast::Expr &arg);
  llvm::Value *genArray(const ast::ArrayLitExpr &e);
  llvm::Value *genString(const ast::StringLitExpr &e);
  llvm::Value *genMethod(const ast::MethodCallExpr &e);
  llvm::Function *lowerLambda(const ast::LambdaExpr &e);

  llvm::StructType *bufferType();
  llvm::StructType *sliceType();
  void emitAbiAlias(llvm::GlobalValue *gv, const sema::Symbol &sym);
  llvm::Value *allocBytes(llvm::Value *nbytes, unsigned align);
  llvm::Value *makeBuffer(llvm::Value *data, llvm::Value *len, llvm::Value *cap, llvm::Value *rc);
  /// Heap traffic goes through cruft_alloc / cruft_free / cruft_realloc (i64 addresses).
  llvm::FunctionCallee mallocCallee();
  llvm::FunctionCallee freeCallee();
  llvm::FunctionCallee reallocCallee();
  llvm::Value *heapAlloc(llvm::Value *nbytes);
  void heapFree(llvm::Value *ptr);
  llvm::Value *heapRealloc(llvm::Value *ptr, llvm::Value *nbytes);
  llvm::FunctionCallee boundsFailCallee();
  void emitBoundsCheck(llvm::Value *index, llvm::Value *len);
  /// Runtime bump allocator. `push` saves the watermark, `pop` restores it,
  /// `alloc` bumps. Deallocation of every object in the region is the pop.
  llvm::FunctionCallee arenaPushCallee();
  llvm::FunctionCallee arenaPopCallee();
  llvm::FunctionCallee arenaAllocCallee();
  void emitArenaPops();
  void emitDrops(const std::vector<const sema::Symbol *> &syms);
  void dropSlot(const sema::Symbol &sym);

  llvm::Constant *constantFor(const ast::Expr &init, const sema::Type &type);
  llvm::AllocaInst *createEntryAlloca(llvm::Type *ty, const std::string &name);
  llvm::FunctionCallee printfCallee();
  llvm::Constant *globalString(const std::string &s, const std::string &name);
  bool initTarget();

  ast::Program &program_;
  sema::TypeChecker &checker_;
  DiagnosticEngine &diags_;
  CodeGenOptions options_;

  std::unique_ptr<llvm::LLVMContext> ctx_;
  std::unique_ptr<llvm::Module> module_;
  std::unique_ptr<llvm::IRBuilder<>> builder_;
  std::unique_ptr<llvm::TargetMachine> targetMachine_;

  std::map<const sema::Symbol *, llvm::Function *> functions_;
  std::map<const sema::Symbol *, llvm::GlobalVariable *> globals_;
  std::map<const sema::Symbol *, llvm::Value *> values_;  ///< SSA values (params, pattern vars)
  std::map<const sema::Symbol *, llvm::AllocaInst *> locals_;
  std::map<const sema::Symbol *, llvm::StructType *> inductives_;
  std::map<const sema::Symbol *, llvm::StructType *> ctorPayloads_;
  std::map<std::string, llvm::Constant *> strings_;

  llvm::Function *currentFn_ = nullptr;
  const sema::Type *currentRet_ = nullptr;
  llvm::StructType *bufferTy_ = nullptr;
  llvm::StructType *sliceTy_ = nullptr;
  /// Base of the framed arena currently being emitted, if any.
  llvm::Value *frameBase_ = nullptr;
  /// How many `scope arena` blocks are open at the current IR insertion point.
  int arenaDepth_ = 0;
  std::vector<std::vector<const sema::Symbol *>> dropStacks_;
  bool consume_ = true;
  int lambdaSeq_ = 0;
};

} // namespace cruft
