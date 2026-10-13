// Types.h - semantic types, symbols and the package/module registry.
//
// Types are immutable once built and shared through std::shared_ptr. Because
// Cruft has dependent path types (`Path(T, a, b)` mentions *terms*), a Type
// may own expression trees; equality of such types is decided by the
// Normalizer, not by structural comparison alone.
#pragma once

#include "ast/AST.h"
#include "Diagnostics.h"

#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace cruft::sema {

struct Symbol;
struct Type;
using TypePtr = std::shared_ptr<const Type>;

enum class TypeKind {
  I32,
  I64,
  F32,
  F64,
  Bool,
  Void,
  Interval,  ///< I  - the cubical interval (erased)
  Universe,  ///< Type (erased)
  Path,      ///< Path(T, a, b) (erased: proof content only)
  Equiv,     ///< Equiv(A, B) (erased: compile-time only)
  Fn,        ///< fn(A, B): R  (first-class as a C function pointer)
  Ptr,       ///< ptr — untyped C pointer (FILE*, void*, char*)
  Inductive, ///< user (higher) inductive type
  Vector,    ///< Vector(T) — affine buffer, arena- or heap-backed
  Map,       ///< Map(K, V)
  String,    ///< String
  Slice,     ///< Slice(T) — non-owning pointer + length
  Error      ///< poisoned type after a diagnostic
};

struct Type {
  TypeKind kind = TypeKind::Error;

  // Path(T, a, b)
  TypePtr elem;
  ast::ExprPtr lhs, rhs;

  // Equiv(A, B)
  TypePtr a, b;

  // Fn. `paramSymbols[i]` is non-null for declared functions so that the
  // return type may depend on the parameter (e.g. `fn sec(b: B): Path(B, f(g(b)), b)`).
  std::vector<TypePtr> params;
  std::vector<const Symbol *> paramSymbols;
  TypePtr ret;

  // Inductive
  const Symbol *inductive = nullptr;

  /// Allocation region. 0 is the heap / static region (a constant lifetime
  /// path, transportable anywhere). Non-zero ids are lexical scopes; an arena
  /// region is a non-constant path and cannot be transported outward.
  int region = 0;

  // --- factories -------------------------------------------------------
  static TypePtr primitive(TypeKind k);
  static TypePtr i32();
  static TypePtr i64();
  static TypePtr f32();
  static TypePtr f64();
  static TypePtr boolean();
  static TypePtr voidTy();
  static TypePtr interval();
  static TypePtr universe();
  static TypePtr error();
  static TypePtr path(TypePtr elem, ast::ExprPtr lhs, ast::ExprPtr rhs);
  static TypePtr equiv(TypePtr a, TypePtr b);
  static TypePtr fn(std::vector<TypePtr> params, std::vector<const Symbol *> paramSymbols,
                    TypePtr ret);
  static TypePtr inductiveOf(const Symbol *sym);
  static TypePtr vector(TypePtr elem, int region);
  static TypePtr map(TypePtr key, TypePtr val, int region);
  static TypePtr stringTy(int region);
  static TypePtr slice(TypePtr elem, int region);
  static TypePtr ptr();

  // --- queries ---------------------------------------------------------
  [[nodiscard]] bool isInteger() const { return kind == TypeKind::I32 || kind == TypeKind::I64; }
  [[nodiscard]] bool isFloat() const { return kind == TypeKind::F32 || kind == TypeKind::F64; }
  [[nodiscard]] bool isNumeric() const { return isInteger() || isFloat(); }
  [[nodiscard]] bool isError() const { return kind == TypeKind::Error; }
  /// Types with no computational content; CodeGen erases every value of these.
  [[nodiscard]] bool isErased() const {
    return kind == TypeKind::Interval || kind == TypeKind::Universe || kind == TypeKind::Path ||
           kind == TypeKind::Equiv;
  }
  /// Types that may legally cross a C ABI boundary (extern fn signatures).
  [[nodiscard]] bool isCCompatible() const;
  /// Buffers with an owner. Primitives stay copyable so arithmetic code is unchanged.
  [[nodiscard]] bool isStorage() const {
    return kind == TypeKind::Vector || kind == TypeKind::Map || kind == TypeKind::String;
  }
  /// Storage, plus non-owning slices whose data still dies with a region.
  [[nodiscard]] bool isRegionBound() const { return isStorage() || kind == TypeKind::Slice; }

  [[nodiscard]] std::string str() const;
};

enum class SymbolKind {
  Function,    ///< fn / extern fn
  Global,      ///< module-level var / const
  Local,       ///< block-local var / const
  Param,       ///< function parameter
  Type,        ///< (higher) inductive type
  Ctor,        ///< constructor of an inductive type
  IntervalVar, ///< binder introduced by `<i>`
  PatternVar   ///< binder introduced by a `case` pattern
};

struct Symbol {
  SymbolKind kind;
  std::string name;
  std::string package; ///< owning package (empty for locals)
  ast::Visibility visibility = ast::Visibility::Private;
  TypePtr type;
  SourceLocation loc;

  bool isConst = false;
  bool isExtern = false;
  bool shared = false; ///< reference-counted binding (`shared var`)

  // Declaration back-links (whichever applies).
  const ast::FnDecl *fnDecl = nullptr;
  const ast::GlobalDecl *globalDecl = nullptr;
  const ast::TypeDecl *typeDecl = nullptr;
  const ast::CtorDecl *ctorDecl = nullptr;
  const ast::Expr *constInit = nullptr; ///< initializer of a `const` (for unfolding)

  // Ctor-specific
  const Symbol *owner = nullptr; ///< the inductive type symbol
  int ctorIndex = -1;            ///< index among *point* constructors (tag)
  bool isPathCtor = false;

  // Type-specific
  std::vector<Symbol *> ctors;

  /// Name used in the emitted object: unmangled for `pub`/`extern` and for
  /// `main`, package-prefixed for `pri`. A `pub` name that would collide is
  /// rewritten to `abiName`.
  std::string linkName;
  /// Deterministic C ABI spelling `_C_<package>_<module>_<symbol>`.
  std::string abiName;
  /// Module that declared the symbol (file stem, or `module` directive).
  std::string module;

  [[nodiscard]] std::string qualifiedName() const {
    return package.empty() ? name : package + "." + name;
  }
};

/// A package (module). Several files may contribute to the same package;
/// imports are tracked per file because visibility is a file-level notion
/// for `import`, but a package-level notion for `pub`/`pri`.
struct Module {
  std::string name;
  std::vector<const ast::File *> files;
  std::map<std::string, Symbol *> symbols;
  /// Public names re-exported from another package (`export pkg.name`).
  std::map<std::string, Symbol *> reexports;
  std::map<const ast::File *, std::set<std::string>> importsByFile;
  std::set<std::string> dependencies; ///< union of imports (edges of the DAG)
};

/// Owns all module-level symbols and implements the lookup rules:
///   * package names come from `package <name>`, never from file names;
///   * a `pri` symbol is invisible outside its package;
///   * a package may only reference another package it imports;
///   * the import graph must be acyclic (strict hierarchy of modules).
class ModuleRegistry {
public:
  Module &getOrCreate(const std::string &name);
  [[nodiscard]] Module *find(const std::string &name);
  [[nodiscard]] const Module *find(const std::string &name) const;
  [[nodiscard]] const std::map<std::string, Module> &modules() const { return modules_; }

  /// Allocates a symbol with a stable address.
  Symbol *create(Symbol sym);

  enum class LookupStatus { Found, NotFound, Private, NotImported, NoSuchPackage };
  struct LookupResult {
    LookupStatus status;
    Symbol *symbol = nullptr;
  };

  /// Resolves `pkg.name` (or `name` when `pkg` is empty) as seen from `file`.
  LookupResult lookup(const std::string &pkg, const std::string &name, const ast::File &from,
                      const std::string &fromPackage) const;

  /// Returns a cycle (as a list of package names) if the dependency graph
  /// has one, otherwise an empty vector.
  [[nodiscard]] std::vector<std::string> findDependencyCycle() const;

private:
  std::map<std::string, Module> modules_;
  std::deque<Symbol> storage_;
};

} // namespace cruft::sema
