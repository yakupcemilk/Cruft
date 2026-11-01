// TypeChecker.cpp - semantic analysis for Cruft. See TypeChecker.h.
#include "sema/TypeChecker.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <functional>
#include <set>
#include <sstream>

namespace cruft::sema {

using namespace ast;

namespace {

template <class T> T &as(Expr &e) { return static_cast<T &>(e); }
template <class T> const T &as(const Expr &e) { return static_cast<const T &>(e); }

const std::map<std::string, Builtin> &builtinTable() {
  static const std::map<std::string, Builtin> table = {
      {"print", Builtin::Print}, {"refl", Builtin::Refl},
      {"sym", Builtin::Sym},     {"transport", Builtin::Transport},
      {"ua", Builtin::Ua},       {"iso", Builtin::Iso},
      {"raw_slice", Builtin::RawSlice},
      {"null", Builtin::NullPtr},
      {"alloc_string", Builtin::AllocString},
      {"seal", Builtin::Seal},
      {"reveal", Builtin::Reveal},
      {"quot", Builtin::Quot},
      {"text", Builtin::Text},
      {"assert", Builtin::Assert},
      {"trap", Builtin::Trap},
      {"cmpxchg", Builtin::CmpXchg},
      {"atomicrmw", Builtin::AtomicRmw},
      {"atomic_load", Builtin::AtomicLoad},
      {"atomic_store", Builtin::AtomicStore},
      {"simd", Builtin::Simd},
      {"simd_add", Builtin::SimdAdd},
      {"simd_extract", Builtin::SimdExtract},
      {"asm", Builtin::Asm},
  };
  return table;
}

const Symbol *nullaryCtor(const Expr &e) {
  if (e.kind != ExprKind::Call)
    return nullptr;
  const auto &c = as<CallExpr>(e);
  if (!c.fn || c.fn->kind != SymbolKind::Ctor || !c.args.empty() || !c.fn->owner)
    return nullptr;
  return c.fn;
}

int orderRank(const std::string &name) {
  if (name == "Relaxed")
    return 1;
  if (name == "Acquire" || name == "Release")
    return 2;
  if (name == "AcqRel")
    return 3;
  if (name == "SeqCst")
    return 4;
  return 0;
}

bool atomicWidth(const Type &t) {
  if (!t.isInteger())
    return false;
  const unsigned bits = t.bitWidth();
  return bits == 8 || bits == 16 || bits == 32 || bits == 64;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / driver
// ---------------------------------------------------------------------------

TypeChecker::TypeChecker(Program &program, DiagnosticEngine &diags)
    : program_(program), diags_(diags) {}

bool TypeChecker::check() {
  initRegions();
  collectDeclarations();
  finishLinkage();
  processExports();
  checkDependencyGraph();
  if (diags_.hasErrors())
    return false;
  resolveSignatures();
  if (diags_.hasErrors())
    return false;
  checkConstFunctions();
  checkGlobals();
  checkFunctionBodies();
  return !diags_.hasErrors();
}

TypePtr TypeChecker::fail(const SourceLocation &loc, const std::string &msg) {
  diags_.error(loc, msg);
  return Type::error();
}

std::string TypeChecker::linkNameFor(const Symbol &sym) const {
  // `extern` and `pub` symbols keep their exact name so that C / Rust code
  // can link against them bit-for-bit. Private symbols are prefixed with
  // their package so that `pri fn helper` in two packages never collide.
  if (sym.isExtern || sym.visibility == Visibility::Public)
    return sym.name;
  return sym.package + "__" + sym.name;
}

// ---------------------------------------------------------------------------
// Phase 1: declarations
// ---------------------------------------------------------------------------

void TypeChecker::collectDeclarations() {
  for (auto &file : program_.files)
    collectFile(*file);

  // Validate imports now that every package is known.
  for (auto &file : program_.files) {
    Module &mod = registry_.getOrCreate(file->package);
    std::set<std::string> seen;
    for (const ImportDecl &imp : file->imports) {
      if (imp.package == file->package) {
        diags_.error(imp.loc, "package '" + imp.package + "' cannot import itself");
        continue;
      }
      if (!registry_.find(imp.package)) {
        diags_.error(imp.loc, "imported package '" + imp.package +
                                  "' is not part of this compilation (no file declares "
                                  "'package " +
                                  imp.package + "')");
        continue;
      }
      if (!seen.insert(imp.package).second)
        diags_.warning(imp.loc, "duplicate import of package '" + imp.package + "'");
      mod.importsByFile[file.get()].insert(imp.package);
      mod.dependencies.insert(imp.package);
    }
    mod.importsByFile[file.get()]; // ensure entry exists
  }
}

void TypeChecker::finishLinkage() {
  auto stamp = [](Symbol *sym, const std::string &module) {
    if (!sym)
      return;
    if (sym->module.empty())
      sym->module = module.empty() ? "mod" : module;
    sym->abiName = mangleAbi(sym->package, sym->module, sym->name);
  };

  for (auto &file : program_.files) {
    const std::string &module = file->moduleName;
    for (auto &td : file->types) {
      stamp(td->symbol, module);
      for (auto &ctor : td->ctors)
        stamp(ctor.symbol, module);
    }
    for (auto &g : file->globals)
      stamp(g->symbol, module);
    for (auto &fn : file->functions)
      stamp(fn->symbol, module);
  }

  // Two `pub fn add` declarations in different packages would otherwise both
  // claim the C symbol `add`. Extern declarations and `main` keep the
  // spelling C and the OS expect; every other colliding `pub` symbol takes
  // its `_C_<package>_<module>_<symbol>` name.
  std::map<std::string, std::vector<Symbol *>> groups;
  auto consider = [&](Symbol *sym) {
    if (!sym)
      return;
    if (sym->isExtern || sym->visibility == Visibility::Public)
      groups[sym->linkName].push_back(sym);
  };
  for (auto &file : program_.files) {
    for (auto &td : file->types) {
      consider(td->symbol);
      for (auto &ctor : td->ctors)
        consider(ctor.symbol);
    }
    for (auto &g : file->globals)
      consider(g->symbol);
    for (auto &fn : file->functions)
      consider(fn->symbol);
  }

  for (auto &[name, group] : groups) {
    if (group.size() < 2)
      continue;
    std::vector<Symbol *> flexible;
    std::vector<Symbol *> fixed;
    for (Symbol *sym : group) {
      if (sym->isExtern || sym->name == "main")
        fixed.push_back(sym);
      else
        flexible.push_back(sym);
    }
    if (fixed.size() > 1) {
      diags_.error(fixed[1]->loc, "duplicate C linkage name '" + name + "'");
      diags_.note(fixed[0]->loc, "previous declaration with this linkage name is here");
    }
    for (Symbol *sym : flexible)
      sym->linkName = sym->abiName;
  }

  std::map<std::string, Symbol *> seen;
  auto claim = [&](Symbol *sym) {
    if (!sym || sym->linkName.empty())
      return;
    if (sym->visibility != Visibility::Public && !sym->isExtern)
      return;
    auto [it, inserted] = seen.emplace(sym->linkName, sym);
    if (inserted)
      return;
    diags_.error(sym->loc, "duplicate C linkage name '" + sym->linkName + "'");
    diags_.note(it->second->loc, "previous declaration with this linkage name is here");
  };
  for (auto &file : program_.files) {
    for (auto &td : file->types) {
      claim(td->symbol);
      for (auto &ctor : td->ctors)
        claim(ctor.symbol);
    }
    for (auto &g : file->globals)
      claim(g->symbol);
    for (auto &fn : file->functions)
      claim(fn->symbol);
  }
}

void TypeChecker::processExports() {
  for (auto &file : program_.files) {
    if (file->package.empty())
      continue;
    file_ = file.get();
    package_ = file->package;
    Module &mod = registry_.getOrCreate(file->package);
    for (const ExportDecl &exp : file->exports) {
      const std::string pkg = exp.package.empty() ? file->package : exp.package;
      if (!exp.package.empty()) {
        auto imported = mod.importsByFile.find(file.get());
        if (imported == mod.importsByFile.end() || !imported->second.count(exp.package)) {
          diags_.error(exp.loc, "package '" + exp.package +
                                    "' is not imported in this file; add 'import " + exp.package +
                                    "'");
          continue;
        }
      }
      Module *src = registry_.find(pkg);
      if (!src) {
        diags_.error(exp.loc, "unknown package '" + pkg + "'");
        continue;
      }
      auto sit = src->symbols.find(exp.name);
      if (sit == src->symbols.end()) {
        diags_.error(exp.loc, "package '" + pkg + "' has no symbol named '" + exp.name + "'");
        continue;
      }
      Symbol *sym = sit->second;
      if (sym->visibility != Visibility::Public) {
        diags_.error(exp.loc, "'" + exp.name + "' is private to package '" + pkg +
                                  "' and cannot be accessed from package '" + package_ + "'");
        diags_.note(sym->loc, "'" + exp.name + "' is declared 'pri' here");
        continue;
      }
      if (mod.symbols.count(exp.name) || mod.reexports.count(exp.name)) {
        diags_.error(exp.loc, "redefinition of '" + exp.name + "' in package '" + file->package + "'");
        continue;
      }
      mod.reexports[exp.name] = sym;
    }
  }
}

void TypeChecker::collectFile(File &file) {
  if (file.package.empty())
    return; // parse error already reported
  Module &mod = registry_.getOrCreate(file.package);
  mod.files.push_back(&file);

  auto declare = [&](Symbol sym) -> Symbol * {
    sym.package = file.package;
    auto it = mod.symbols.find(sym.name);
    if (it != mod.symbols.end()) {
      diags_.error(sym.loc, "redefinition of '" + sym.name + "' in package '" + file.package + "'");
      diags_.note(it->second->loc, "previous definition is here");
      return nullptr;
    }
    Symbol *s = registry_.create(std::move(sym));
    s->linkName = linkNameFor(*s);
    mod.symbols[s->name] = s;
    return s;
  };

  for (auto &td : file.types) {
    Symbol ts;
    ts.kind = SymbolKind::Type;
    ts.name = td->name;
    ts.visibility = td->visibility;
    ts.loc = td->loc;
    ts.typeDecl = td.get();
    Symbol *typeSym = declare(std::move(ts));
    if (!typeSym)
      continue;
    typeSym->type = Type::inductiveOf(typeSym);
    td->symbol = typeSym;

    int pointIndex = 0;
    for (auto &ctor : td->ctors) {
      Symbol cs;
      cs.kind = SymbolKind::Ctor;
      cs.name = ctor.name;
      cs.visibility = td->visibility; // constructors inherit the type's visibility
      cs.loc = ctor.loc;
      cs.ctorDecl = &ctor;
      cs.owner = typeSym;
      cs.isPathCtor = ctor.pathType != nullptr;
      cs.ctorIndex = cs.isPathCtor ? -1 : pointIndex++;
      Symbol *ctorSym = declare(std::move(cs));
      if (!ctorSym)
        continue;
      ctor.symbol = ctorSym;
      typeSym->ctors.push_back(ctorSym);
    }
  }

  for (auto &sd : file.structs) {
    Symbol ss;
    ss.kind = SymbolKind::Type;
    ss.name = sd->name;
    ss.visibility = sd->visibility;
    ss.loc = sd->loc;
    sd->symbol = declare(std::move(ss));
  }

  for (auto &g : file.globals) {
    Symbol gs;
    gs.kind = SymbolKind::Global;
    gs.name = g->name;
    gs.visibility = g->visibility;
    gs.loc = g->loc;
    gs.isConst = g->isConst;
    gs.globalDecl = g.get();
    gs.constInit = g->isConst ? g->init.get() : nullptr;
    g->symbol = declare(std::move(gs));
  }

  for (auto &fn : file.functions) {
    Symbol fs;
    fs.kind = SymbolKind::Function;
    if (fn->implIndex >= 0) {
      // The method name stays on the declaration for diagnostics and trait
      // lookup. The symbol name is unique so two `len` witnesses do not collide.
      fs.name = "__impl_" + std::to_string(implSerial_++) + "_" + fn->name;
      fs.visibility = Visibility::Private;
    } else {
      fs.name = fn->name;
      fs.visibility = fn->visibility;
    }
    fs.loc = fn->loc;
    fs.isExtern = fn->isExtern;
    fs.fnDecl = fn.get();
    fn->symbol = declare(std::move(fs));
  }

  for (auto &tr : file.traits) {
    Symbol ts;
    ts.kind = SymbolKind::Trait;
    ts.name = tr->name;
    ts.visibility = tr->visibility;
    ts.loc = tr->loc;
    tr->symbol = declare(std::move(ts));
  }

  for (auto &ax : file.axioms) {
    Symbol as;
    as.kind = SymbolKind::Axiom;
    as.name = ax->name;
    as.visibility = ax->visibility;
    as.loc = ax->loc;
    ax->symbol = declare(std::move(as));
  }
}

// ---------------------------------------------------------------------------
// Phase 2: module hierarchy
// ---------------------------------------------------------------------------

void TypeChecker::checkDependencyGraph() {
  std::vector<std::string> cycle = registry_.findDependencyCycle();
  if (cycle.empty())
    return;
  std::string chain;
  for (size_t i = 0; i < cycle.size(); ++i)
    chain += (i ? " -> " : "") + cycle[i];
  SourceLocation loc;
  if (const Module *m = registry_.find(cycle.front()); m && !m->files.empty())
    loc = m->files.front()->packageLoc;
  diags_.error(loc, "cyclic package dependency violates the module hierarchy: " + chain);
}

// ---------------------------------------------------------------------------
// Phase 3: signatures
// ---------------------------------------------------------------------------

bool TypeChecker::isStaticOnlyType(const Type &t) const {
  // Values of these types carry no runtime representation *and* cannot be
  // recovered from an erased argument, so they may only live in `const`
  // bindings whose initialiser is statically known.
  if (t.kind == TypeKind::Universe || t.kind == TypeKind::Equiv || t.kind == TypeKind::Leak)
    return true;
  if (t.kind == TypeKind::Path)
    return t.elem->kind == TypeKind::Universe;
  return false;
}

void TypeChecker::ensureNotLeakingPrivate(const Type &t, const SourceLocation &loc,
                                          const std::string &what) {
  std::function<void(const Type &)> walk = [&](const Type &ty) {
    switch (ty.kind) {
    case TypeKind::Inductive:
      if (ty.inductive && ty.inductive->visibility == Visibility::Private)
        diags_.error(loc, what + " exposes private type '" + ty.inductive->qualifiedName() +
                              "' through a public interface; mark the type 'pub' or the "
                              "declaration 'pri'");
      break;
    case TypeKind::Path:
      walk(*ty.elem);
      break;
    case TypeKind::Equiv:
      walk(*ty.a);
      walk(*ty.b);
      break;
    case TypeKind::Fn:
      for (const auto &p : ty.params)
        walk(*p);
      walk(*ty.ret);
      break;
    case TypeKind::Struct:
      for (const auto &p : ty.params)
        if (p)
          walk(*p);
      break;
    case TypeKind::Vector:
    case TypeKind::Slice:
    case TypeKind::Labeled:
    case TypeKind::Quotient:
      if (ty.elem)
        walk(*ty.elem);
      break;
    case TypeKind::Map:
      if (ty.a)
        walk(*ty.a);
      if (ty.b)
        walk(*ty.b);
      break;
    default:
      break;
    }
  };
  walk(t);
}

void TypeChecker::resolveSignatures() {
  for (int pass = 0; pass < 2; ++pass) {
    if (pass == 1) {
      for (auto &file : program_.files) {
        if (file->package.empty())
          continue;
        file_ = file.get();
        package_ = file->package;
        resolveTraits(*file);
      }
    }
    for (auto &file : program_.files) {
      if (file->package.empty())
        continue;
      file_ = file.get();
      package_ = file->package;
      // Types and structs first, across every file, so a later package can
      // name Result(String, i32) after result's binders exist.
      resolveFileSignatures(*file, pass == 1);
    }
  }
}

void TypeChecker::resolveFileSignatures(File &file, bool functions) {
  if (functions)
    goto resolve_functions;
  // --- inductive types: point constructors first, then path constructors
  //     (path constructor boundaries mention point constructors).
  for (auto &td : file.types) {
    if (!td->symbol)
      continue;
    pushScope();
    td->symbol->typeBinders = openTypeBinders(td->typeBinders);
    if (td->symbol->type) {
      auto edited = std::const_pointer_cast<Type>(td->symbol->type);
      edited->typeParams = td->symbol->typeBinders;
      edited->typeParamLevels.clear();
      for (const Symbol *b : td->symbol->typeBinders)
        edited->typeParamLevels.push_back(b && b->type ? b->type->level : 0);
    }
    for (int pass = 0; pass < 2; ++pass) {
      for (auto &ctor : td->ctors) {
        if (!ctor.symbol || (ctor.pathType != nullptr) != (pass == 1))
          continue;
        pushScope();
        std::vector<TypePtr> params;
        std::vector<const Symbol *> paramSyms;
        for (auto &p : ctor.params) {
          TypePtr pt = resolveType(*p.type);
          if (pt->isErased() && !pt->isError())
            diags_.error(p.loc, "constructor parameter '" + p.name +
                                    "' has erased type '" + pt->str() +
                                    "'; constructor payloads must be data");
          Symbol ps;
          ps.kind = SymbolKind::Param;
          ps.name = p.name;
          ps.type = pt;
          ps.loc = p.loc;
          Symbol *s = declareLocal(std::move(ps));
          params.push_back(pt);
          paramSyms.push_back(s);
        }
        TypePtr result;
        if (ctor.pathType) {
          if (td->isEnum)
            diags_.error(ctor.loc, "enum '" + td->name +
                                       "' cannot have a path constructor; use 'type' for a higher inductive");
          result = resolveType(*ctor.pathType);
          if (result->kind != TypeKind::Path || result->elem->kind != TypeKind::Inductive ||
              result->elem->inductive != td->symbol) {
            if (!result->isError())
              diags_.error(ctor.loc, "path constructor '" + ctor.name +
                                         "' must have type Path(" + td->name +
                                         ", a, b), found '" + result->str() + "'");
            result = Type::error();
          }
        } else {
          result = td->symbol->type;
        }
        popScope();
        ctor.symbol->type =
            params.empty() ? result : Type::fn(std::move(params), std::move(paramSyms), result);
      }
    }
    popScope();
  }

  // --- structs (dependent fields see the symbols of earlier fields)
  for (auto &sd : file.structs) {
    if (!sd->symbol)
      continue;
    pushScope();
    sd->symbol->typeBinders = openTypeBinders(sd->typeBinders);
    std::vector<TypePtr> fields;
    std::vector<const Symbol *> fieldSyms;
    for (auto &field : sd->fields) {
      TypePtr ft = resolveType(*field.type);
      if (ft->kind == TypeKind::Struct && ft->inductive == sd->symbol)
        diags_.error(field.loc, "struct '" + sd->name + "' cannot contain itself by value");
      Symbol fs;
      fs.kind = SymbolKind::Param;
      fs.name = field.name;
      fs.type = ft;
      fs.loc = field.loc;
      Symbol *s = declareLocal(std::move(fs));
      fields.push_back(ft);
      fieldSyms.push_back(s);
    }
    popScope();
    sd->symbol->type = Type::structOf(sd->symbol, std::move(fields), std::move(fieldSyms));
    {
      auto edited = std::const_pointer_cast<Type>(sd->symbol->type);
      edited->typeParams = sd->symbol->typeBinders;
      for (const Symbol *b : sd->symbol->typeBinders)
        edited->typeParamLevels.push_back(b && b->type ? b->type->level : 0);
    }
    if (sd->visibility == Visibility::Public)
      ensureNotLeakingPrivate(*sd->symbol->type, sd->loc, "public struct '" + sd->name + "'");
  }

  return;
resolve_functions:
  // --- globals
  for (auto &g : file.globals) {
    if (!g->symbol)
      continue;
    TypePtr t = g->type ? resolveType(*g->type) : nullptr;
    if (t && isStaticOnlyType(*t) && !g->isConst)
      diags_.error(g->loc, "a 'var' cannot have the static-only type '" + t->str() +
                               "'; use 'const'");
    g->symbol->type = t;
    g->symbol->shared = g->shared;
    if (t && g->visibility == Visibility::Public)
      ensureNotLeakingPrivate(*t, g->loc, "public variable '" + g->name + "'");
  }

  // --- functions. Impl methods are resolved with their impl's binders below.
  for (auto &fn : file.functions) {
    if (!fn->symbol || fn->implIndex >= 0)
      continue;
    assignFunctionType(*fn, nullptr);
  }

  for (auto &ax : file.axioms) {
    if (!ax->symbol || !ax->type)
      continue;
    TypePtr t = resolveType(*ax->type);
    ax->symbol->type = t;
    if (!t->isError() && t->kind != TypeKind::Leak)
      diags_.error(ax->loc, "axiom '" + ax->name + "' must inhabit a Leak type, found '" + t->str() +
                                "'");
  }

  for (auto &fn : file.functions) {
    if (!fn->isDrop)
      continue;
    Symbol *target = nullptr;
    for (auto &sd : file.structs)
      if (sd->name == fn->dropTypeName)
        target = sd->symbol;
    if (!target) {
      diags_.error(fn->loc, "drop target '" + fn->dropTypeName + "' is not a struct in this package");
      continue;
    }
    if (target->dropFn) {
      diags_.error(fn->loc, "struct '" + fn->dropTypeName + "' already has a drop");
      continue;
    }
    target->dropFn = fn.get();
    if (fn->params.size() != 1 || !fn->symbol || !fn->symbol->type ||
        fn->symbol->type->params.size() != 1 || !fn->symbol->type->params[0] ||
        fn->symbol->type->params[0]->kind != TypeKind::Ref ||
        !fn->symbol->type->params[0]->elem ||
        fn->symbol->type->params[0]->elem->inductive != target)
      diags_.error(fn->loc, "drop " + fn->dropTypeName + " must take one '&" + fn->dropTypeName +
                                "' parameter");
  }

  resolveImpls(file);
}

void TypeChecker::assignFunctionType(FnDecl &fn, const std::vector<const Symbol *> *sharedBinders) {
  if (!fn.symbol)
    return;
  const bool ownScope = sharedBinders == nullptr;
  if (ownScope)
    pushScope();
  std::vector<const Symbol *> typeParams =
      sharedBinders ? *sharedBinders : openTypeBinders(fn.typeBinders);
  std::vector<int> typeLevels;
  for (const Symbol *b : typeParams)
    typeLevels.push_back(b && b->type ? b->type->level : 0);
  const Symbol *typeParam = typeParams.empty() ? nullptr : typeParams.front();
  int typeParamLevel = typeLevels.empty() ? -1 : typeLevels.front();
  if ((fn.effects & effectNet) != 0)
    diags_.warning(fn.loc, "effect 'net' has no runtime; the row is checked and then ignored");
  std::vector<TypePtr> params;
  std::vector<const Symbol *> paramSyms;
  std::vector<Usage> paramUses;
  for (auto &p : fn.params) {
    if (!p.type) {
      diags_.error(p.loc, "parameter '" + p.name + "' needs a type");
      params.push_back(Type::error());
      paramSyms.push_back(nullptr);
      paramUses.push_back(Usage::Many);
      continue;
    }
    TypePtr pt = resolveType(*p.type);
    if (isStaticOnlyType(*pt))
      diags_.error(p.loc, "parameter '" + p.name + "' has static-only type '" + pt->str() +
                              "'; type-level data must be known at compile time");
    if (lookupLocal(p.name))
      diags_.error(p.loc, "duplicate parameter name '" + p.name + "'");
    Symbol ps;
    ps.kind = SymbolKind::Param;
    ps.name = p.name;
    ps.type = pt;
    ps.loc = p.loc;
    ps.usage = resolveUsage(p.usage, *pt, false, false);
    paramSyms.push_back(declareLocal(std::move(ps)));
    params.push_back(pt);
    paramUses.push_back(paramSyms.back()->usage);
  }
  TypePtr ret = fn.ret ? resolveType(*fn.ret) : Type::voidTy();
  if (ownScope)
    popScope();

  std::string rowVar = fn.effectsDeclared && fn.effects == 0 ? fn.effectVar : std::string{};
  TypePtr fnType = Type::fn(std::move(params), paramSyms, ret, paramUses, fn.effects, rowVar,
                            fn.effectsDeclared, typeParam, typeParamLevel, fn.isVarArg,
                            typeParams, typeLevels);
  fn.symbol->type = fnType;

  if (fn.isExtern && typeParam)
    diags_.error(fn.loc, "extern function '" + fn.name + "' cannot have a type binder");

  if (fn.isExtern) {
    for (size_t i = 0; i < fnType->params.size(); ++i)
      if (fnType->params[i] && !fnType->params[i]->isCCompatible() && !fnType->params[i]->isError())
        diags_.error(fn.params[i].loc,
                     "extern function '" + fn.name + "': parameter '" + fn.params[i].name +
                         "' has type '" + fnType->params[i]->str() +
                         "' which is not C-ABI compatible (allowed: i32, i64, usize, isize, f32, f64, "
                         "bool, ptr, Slice(T), and C-compatible function pointers)");
    if (ret && !ret->isCCompatible() && !ret->isError())
      diags_.error(fn.loc, "extern function '" + fn.name + "': return type '" + ret->str() +
                               "' is not C-ABI compatible");
  } else if (fn.visibility == Visibility::Public) {
    ensureNotLeakingPrivate(*fnType, fn.loc, "public function '" + fn.name + "'");
  }
}

namespace {

int binderSlot(const Symbol *sym, const std::vector<const Symbol *> &binders) {
  if (!sym)
    return -1;
  for (size_t i = 0; i < binders.size(); ++i)
    if (binders[i] == sym)
      return static_cast<int>(i);
  return -1;
}

} // namespace

bool TypeChecker::typeMentions(const Type &type, const Symbol *var) const {
  if (!var)
    return false;
  if (type.kind == TypeKind::TyVar && type.tyVar == var)
    return true;
  if (type.elem && typeMentions(*type.elem, var))
    return true;
  if (type.a && typeMentions(*type.a, var))
    return true;
  if (type.b && typeMentions(*type.b, var))
    return true;
  if (type.ret && typeMentions(*type.ret, var))
    return true;
  for (const auto &p : type.params)
    if (p && typeMentions(*p, var))
      return true;
  for (const auto &arg : type.typeArgs)
    if (arg && typeMentions(*arg, var))
      return true;
  return false;
}

bool TypeChecker::traitTypesEqual(const Type &a, const Type &b) {
  if (a.kind != b.kind)
    return false;
  switch (a.kind) {
  case TypeKind::Ref:
    return a.refMut == b.refMut && a.elem && b.elem && traitTypesEqual(*a.elem, *b.elem);
  case TypeKind::Fn: {
    if (a.params.size() != b.params.size() || !a.ret || !b.ret)
      return false;
    for (size_t i = 0; i < a.params.size(); ++i)
      if (!a.params[i] || !b.params[i] || !traitTypesEqual(*a.params[i], *b.params[i]))
        return false;
    return traitTypesEqual(*a.ret, *b.ret);
  }
  case TypeKind::Vector:
  case TypeKind::Slice:
    return a.elem && b.elem && traitTypesEqual(*a.elem, *b.elem);
  case TypeKind::Labeled:
    return a.secrecy == b.secrecy && a.elem && b.elem && traitTypesEqual(*a.elem, *b.elem);
  case TypeKind::Struct: {
    if (a.inductive != b.inductive || a.params.size() != b.params.size() ||
        a.typeArgs.size() != b.typeArgs.size())
      return false;
    for (size_t i = 0; i < a.typeArgs.size(); ++i)
      if (!a.typeArgs[i] || !b.typeArgs[i] || !traitTypesEqual(*a.typeArgs[i], *b.typeArgs[i]))
        return false;
    for (size_t i = 0; i < a.params.size(); ++i)
      if (!a.params[i] || !b.params[i] || !traitTypesEqual(*a.params[i], *b.params[i]))
        return false;
    return true;
  }
  case TypeKind::TyVar:
    return a.tyVar == b.tyVar;
  case TypeKind::Map:
    return a.a && b.a && a.b && b.b && traitTypesEqual(*a.a, *b.a) && traitTypesEqual(*a.b, *b.b);
  case TypeKind::Inductive: {
    if (a.inductive != b.inductive || a.typeArgs.size() != b.typeArgs.size())
      return false;
    for (size_t i = 0; i < a.typeArgs.size(); ++i)
      if (!a.typeArgs[i] || !b.typeArgs[i] || !traitTypesEqual(*a.typeArgs[i], *b.typeArgs[i]))
        return false;
    return true;
  }
  default:
    return normalizer_.typeEqual(a, b);
  }
}

bool TypeChecker::patternEqual(const Type &a, const Type &b, const std::vector<const Symbol *> &bindersA,
                               const std::vector<const Symbol *> &bindersB) {
  const int ia = a.kind == TypeKind::TyVar ? binderSlot(a.tyVar, bindersA) : -1;
  const int ib = b.kind == TypeKind::TyVar ? binderSlot(b.tyVar, bindersB) : -1;
  if (ia >= 0 || ib >= 0)
    return ia == ib;
  if (a.kind != b.kind)
    return false;
  switch (a.kind) {
  case TypeKind::Ref:
    return a.refMut == b.refMut && a.elem && b.elem &&
           patternEqual(*a.elem, *b.elem, bindersA, bindersB);
  case TypeKind::Vector:
  case TypeKind::Slice:
    return a.elem && b.elem && patternEqual(*a.elem, *b.elem, bindersA, bindersB);
  case TypeKind::Labeled:
    return a.secrecy == b.secrecy && a.elem && b.elem &&
           patternEqual(*a.elem, *b.elem, bindersA, bindersB);
  case TypeKind::Struct: {
    if (a.inductive != b.inductive || a.params.size() != b.params.size() ||
        a.typeArgs.size() != b.typeArgs.size())
      return false;
    for (size_t i = 0; i < a.typeArgs.size(); ++i)
      if (!a.typeArgs[i] || !b.typeArgs[i] ||
          !patternEqual(*a.typeArgs[i], *b.typeArgs[i], bindersA, bindersB))
        return false;
    for (size_t i = 0; i < a.params.size(); ++i)
      if (!a.params[i] || !b.params[i] ||
          !patternEqual(*a.params[i], *b.params[i], bindersA, bindersB))
        return false;
    return true;
  }
  case TypeKind::Map:
    return a.a && b.a && a.b && b.b && patternEqual(*a.a, *b.a, bindersA, bindersB) &&
           patternEqual(*a.b, *b.b, bindersA, bindersB);
  case TypeKind::TyVar:
    return a.tyVar == b.tyVar;
  default:
    return normalizer_.typeEqual(a, b);
  }
}

bool TypeChecker::bindPattern(const Type &pattern, const TypePtr &actual,
                              const std::vector<const Symbol *> &binders,
                              std::vector<TypePtr> &args) {
  if (!actual)
    return false;
  if (pattern.kind == TypeKind::TyVar) {
    const int slot = binderSlot(pattern.tyVar, binders);
    if (slot >= 0) {
      if (!args[static_cast<size_t>(slot)]) {
        args[static_cast<size_t>(slot)] = actual;
        return true;
      }
      return traitTypesEqual(*args[static_cast<size_t>(slot)], *actual);
    }
    return actual->kind == TypeKind::TyVar && actual->tyVar == pattern.tyVar;
  }
  if (pattern.kind != actual->kind)
    return false;
  switch (pattern.kind) {
  case TypeKind::Ref:
    return pattern.refMut == actual->refMut && pattern.elem &&
           bindPattern(*pattern.elem, actual->elem, binders, args);
  case TypeKind::Vector:
  case TypeKind::Slice:
  case TypeKind::Labeled:
    return pattern.elem && bindPattern(*pattern.elem, actual->elem, binders, args);
  case TypeKind::Struct: {
    if (pattern.inductive != actual->inductive || pattern.params.size() != actual->params.size() ||
        pattern.typeArgs.size() != actual->typeArgs.size())
      return false;
    for (size_t i = 0; i < pattern.typeArgs.size(); ++i)
      if (!pattern.typeArgs[i] || !bindPattern(*pattern.typeArgs[i], actual->typeArgs[i], binders, args))
        return false;
    for (size_t i = 0; i < pattern.params.size(); ++i)
      if (!pattern.params[i] || !bindPattern(*pattern.params[i], actual->params[i], binders, args))
        return false;
    return true;
  }
  case TypeKind::Map:
    return pattern.a && pattern.b && bindPattern(*pattern.a, actual->a, binders, args) &&
           bindPattern(*pattern.b, actual->b, binders, args);
  case TypeKind::Inductive: {
    if (pattern.inductive != actual->inductive || pattern.typeArgs.size() != actual->typeArgs.size())
      return false;
    for (size_t i = 0; i < pattern.typeArgs.size(); ++i)
      if (!pattern.typeArgs[i] || !bindPattern(*pattern.typeArgs[i], actual->typeArgs[i], binders, args))
        return false;
    return true;
  }
  default:
    return normalizer_.typeEqual(pattern, *actual);
  }
}

bool TypeChecker::matchImpl(const ImplInfo &impl, const TypePtr &recv,
                            std::vector<TypePtr> &args) {
  if (!impl.target || !recv)
    return false;
  args.assign(impl.binders.size(), nullptr);
  if (!bindPattern(*impl.target, recv, impl.binders, args))
    return false;
  for (const auto &arg : args)
    if (!arg)
      return false;
  if (impl.binders.empty())
    return true;
  TypePtr inst = instantiateBinders(impl.target, impl.binders, args);
  return inst && traitTypesEqual(*inst, *recv);
}

void TypeChecker::resolveTraits(File &file) {
  for (auto &tr : file.traits) {
    if (!tr->symbol)
      continue;
    pushScope();
    Symbol selfSym;
    selfSym.kind = SymbolKind::TypeParam;
    selfSym.name = "Self";
    selfSym.type = Type::universe(0);
    selfSym.loc = tr->loc;
    const Symbol *self = declareLocal(std::move(selfSym));
    traitSelf_[tr->symbol] = self;
    traitBinders_[tr->symbol] = openTypeBinders(tr->typeBinders);
    std::vector<TraitMethod> methods;
    for (auto &method : tr->methods) {
      bool duplicate = false;
      for (const auto &seen : methods)
        if (seen.name == method->name)
          duplicate = true;
      if (duplicate) {
        diags_.error(method->loc, "trait '" + tr->name + "' already declares '" + method->name + "'");
        continue;
      }
      pushScope();
      std::vector<TypePtr> params;
      std::vector<const Symbol *> paramSyms;
      std::vector<Usage> paramUses;
      for (auto &p : method->params) {
        if (!p.type) {
          diags_.error(p.loc, "parameter '" + p.name + "' needs a type");
          params.push_back(Type::error());
          paramSyms.push_back(nullptr);
          paramUses.push_back(Usage::Many);
          continue;
        }
        TypePtr pt = resolveType(*p.type);
        if (lookupLocal(p.name))
          diags_.error(p.loc, "duplicate parameter name '" + p.name + "'");
        Symbol ps;
        ps.kind = SymbolKind::Param;
        ps.name = p.name;
        ps.type = pt;
        ps.loc = p.loc;
        paramSyms.push_back(declareLocal(std::move(ps)));
        params.push_back(pt);
        paramUses.push_back(Usage::Many);
      }
      TypePtr ret = method->ret ? resolveType(*method->ret) : Type::voidTy();
      popScope();
      if (params.empty() || !params[0] || !typeMentions(*params[0], self))
        diags_.error(method->loc, "trait method '" + method->name +
                                      "' must take a receiver whose type mentions Self");
      TypePtr ty = Type::fn(std::move(params), paramSyms, ret, paramUses, method->effects, {},
                            method->effectsDeclared);
      if (tr->visibility == Visibility::Public && ty && !ty->isError())
        ensureNotLeakingPrivate(*ty, method->loc, "public trait method '" + method->name + "'");
      methods.push_back(TraitMethod{method->name, ty, method->loc});
    }
    popScope();
    traitMethods_[tr->symbol] = std::move(methods);
  }
}

void TypeChecker::resolveImpls(File &file) {
  for (size_t i = 0; i < file.impls.size(); ++i) {
    ImplDecl &decl = *file.impls[i];
    Symbol *trait = resolveName(decl.traitPackage, decl.traitName, decl.loc);
    if (!trait)
      continue;
    if (trait->kind != SymbolKind::Trait) {
      diags_.error(decl.loc, "'" + trait->name + "' is not a trait");
      continue;
    }
    pushScope();
    std::vector<const Symbol *> binders = openTypeBinders(decl.binders);
    TypePtr target = decl.target ? resolveType(*decl.target) : Type::error();
    if (!target || target->isError()) {
      popScope();
      continue;
    }
    for (const Symbol *b : binders)
      if (b && !typeMentions(*target, b))
        diags_.error(decl.loc, "type binder '" + b->name + "' does not appear in the impl type");
    currentSelf_ = target;
    std::vector<TypePtr> supplied;
    for (const auto &arg : decl.traitArgs)
      supplied.push_back(arg ? resolveType(*arg) : Type::error());
    for (auto &fn : file.functions) {
      if (!fn || fn->implIndex != static_cast<int>(i) || !fn->symbol)
        continue;
      pushScope();
      assignFunctionType(*fn, &binders);
      popScope();
      if (decl.visibility == Visibility::Public && fn->symbol->type)
        ensureNotLeakingPrivate(*fn->symbol->type, fn->loc,
                                "public impl method '" + fn->name + "'");
    }
    currentSelf_ = nullptr;
    popScope();

    ImplInfo info;
    info.trait = trait;
    info.target = target;
    info.binders = binders;
    info.visibility = decl.visibility;
    info.package = package_;
    info.loc = decl.loc;
    const auto &required = traitMethods_[trait];
    const Symbol *self = nullptr;
    if (auto it = traitSelf_.find(trait); it != traitSelf_.end())
      self = it->second;
    for (auto &fn : file.functions) {
      if (!fn || fn->implIndex != static_cast<int>(i) || !fn->symbol || !fn->symbol->type)
        continue;
      if (info.methods.count(fn->name)) {
        diags_.error(fn->loc, "impl of '" + trait->name + "' already defines '" + fn->name + "'");
        continue;
      }
      info.methods[fn->name] = fn->symbol;
      const TraitMethod *tm = nullptr;
      for (const auto &method : required)
        if (method.name == fn->name)
          tm = &method;
      if (!tm) {
        diags_.error(fn->loc, "method '" + fn->name + "' is not declared by trait '" + trait->name + "'");
        continue;
      }
      TypePtr expected = self ? substTypeVar(tm->type, self, target) : tm->type;
      const std::vector<const Symbol *> traitBinders = traitBinders_.count(trait)
                                                            ? traitBinders_.find(trait)->second
                                                            : std::vector<const Symbol *>{};
      if (!traitBinders.empty() && expected) {
        if (!binders.empty()) {
          if (binders.size() != traitBinders.size())
            diags_.error(decl.loc, "impl of '" + trait->name + "' expects " +
                                       std::to_string(traitBinders.size()) + " type binder(s)");
          else
            for (size_t b = 0; b < traitBinders.size(); ++b)
              expected = substTypeVar(expected, traitBinders[b], Type::typeVariable(binders[b]));
        } else if (supplied.size() != traitBinders.size()) {
          diags_.error(decl.loc, "impl of '" + trait->name + "' expects " +
                                     std::to_string(traitBinders.size()) + " type argument(s), found " +
                                     std::to_string(supplied.size()));
        } else {
          for (size_t b = 0; b < traitBinders.size(); ++b)
            expected = substTypeVar(expected, traitBinders[b], supplied[b]);
        }
      } else if (!supplied.empty()) {
        diags_.error(decl.loc, "trait '" + trait->name + "' does not take type arguments");
      }
      if (!expected || !traitTypesEqual(*expected, *fn->symbol->type))
        diags_.error(fn->loc, "method '" + fn->name + "' of impl '" + trait->name +
                                  "' does not match the trait");
    }
    for (const auto &method : required) {
      if (!info.methods.count(method.name))
        diags_.error(decl.loc, "impl of '" + trait->name + "' for '" + target->str() +
                                   "' is missing method '" + method.name + "'");
    }
    for (const auto &prev : impls_) {
      if (sameImplTarget(prev, info)) {
        diags_.error(decl.loc, "duplicate impl of '" + trait->name + "' for '" + target->str() + "'");
        diags_.note(prev.loc, "previous impl is here");
      }
    }
    impls_.push_back(std::move(info));
  }
}

TypeChecker::TraitLookup TypeChecker::lookupTrait(const std::string &name, const TypePtr &recv,
                                                   const SourceLocation &loc, TraitDispatch &out) {
  if (!recv)
    return TraitLookup::None;
  int hits = 0;
  for (const auto &impl : impls_) {
    if (impl.visibility != Visibility::Public && impl.package != package_)
      continue;
    auto it = impl.methods.find(name);
    if (it == impl.methods.end() || !it->second || !it->second->type)
      continue;
    std::vector<TypePtr> args;
    if (!matchImpl(impl, recv, args))
      continue;
    ++hits;
    out.fn = it->second;
    out.typeArgs = std::move(args);
  }
  if (hits == 0)
    return TraitLookup::None;
  if (hits > 1) {
    fail(loc, "ambiguous trait method '" + name + "' for '" + recv->str() + "'");
    return TraitLookup::Failed;
  }
  return TraitLookup::Found;
}

TypePtr TypeChecker::tryTraitMethod(ExprPtr &e, const TypePtr &recv, const Type *expected) {
  auto &m = as<MethodCallExpr>(*e);
  TraitDispatch hit;
  const TraitLookup found = lookupTrait(m.method, recv, m.loc, hit);
  if (found == TraitLookup::None)
    return nullptr;
  if (found == TraitLookup::Failed || !hit.fn || !hit.fn->type)
    return Type::error();
  auto callee = std::make_unique<IdentExpr>(m.loc, std::string{}, hit.fn->name);
  callee->symbol = hit.fn;
  callee->type = hit.fn->type;
  std::vector<ExprPtr> args;
  args.push_back(std::move(m.receiver));
  for (auto &arg : m.args)
    args.push_back(std::move(arg));
  auto call = std::make_unique<CallExpr>(m.loc, std::move(callee), std::move(args));
  call->fn = hit.fn;
  call->typeArguments = std::move(hit.typeArgs);
  e = std::move(call);
  return checkFnCall(as<CallExpr>(*e), *hit.fn->type, expected);
}

bool TypeChecker::tryTraitFunction(CallExpr &call, const Type *expected, TypePtr &out) {
  if (call.callee->kind != ExprKind::Ident || call.args.empty())
    return false;
  auto &callee = as<IdentExpr>(*call.callee);
  if (!callee.package.empty())
    return false;
  bool saved = consume_;
  consume_ = false;
  TypePtr probed = checkExpr(call.args[0]);
  consume_ = saved;
  if (!probed || probed->isError()) {
    out = Type::error();
    return true;
  }
  TypePtr recv = probed;
  if (recv->kind == TypeKind::Ref && recv->elem)
    recv = recv->elem;
  TraitDispatch hit;
  const TraitLookup found = lookupTrait(callee.name, recv, call.loc, hit);
  if (found == TraitLookup::None)
    return false;
  if (found == TraitLookup::Failed || !hit.fn || !hit.fn->type) {
    out = Type::error();
    return true;
  }
  callee.symbol = hit.fn;
  callee.type = hit.fn->type;
  call.fn = hit.fn;
  call.typeArguments = std::move(hit.typeArgs);
  out = checkFnCall(call, *hit.fn->type, expected);
  return true;
}

bool TypeChecker::sameImplTarget(const ImplInfo &a, const ImplInfo &b) {
  if (a.trait != b.trait || a.binders.size() != b.binders.size() || !a.target || !b.target)
    return false;
  return patternEqual(*a.target, *b.target, a.binders, b.binders);
}

TypePtr TypeChecker::resolveType(const TypeExpr &t) {
  switch (t.kind) {
  case TypeExprKind::Ref: {
    if (!t.elem)
      return fail(t.loc, "reference type is missing its element");
    TypePtr elem = resolveType(*t.elem);
    if (elem->isError())
      return elem;
    if (elem->isErased())
      return fail(t.loc, "cannot form a reference to erased type '" + elem->str() + "'");
    return Type::refTy(std::move(elem), t.refMut, currentRegion_);
  }
  case TypeExprKind::Named: {
    if (t.package.empty() && t.name == "Self" && currentSelf_) {
      if (!t.params.empty())
        return fail(t.loc, "'Self' does not take type arguments");
      return currentSelf_;
    }
    if (t.package.empty()) {
      if (t.name == "i8") return Type::i8();
      if (t.name == "i16") return Type::i16();
      if (t.name == "i32") return Type::i32();
      if (t.name == "i64") return Type::i64();
      if (t.name == "u8") return Type::u8();
      if (t.name == "u16") return Type::u16();
      if (t.name == "u32") return Type::u32();
      if (t.name == "u64") return Type::u64();
      if (t.name == "usize") return Type::usize();
      if (t.name == "isize") return Type::isize();
      if (t.name == "f32") return Type::f32();
      if (t.name == "f64") return Type::f64();
      if (t.name == "bool") return Type::boolean();
      if (t.name == "I") return Type::interval();
      if (t.name == "Type") {
        if (!t.params.empty())
          return fail(t.loc, "'Type' takes a level in parentheses, as in Type(1)");
        return Type::universe(t.level);
      }
      if (t.name == "Text") {
        if (!t.params.empty())
          return fail(t.loc, "'Text' takes no type arguments");
        return Type::text();
      }
      if (t.name == "Simd") {
        if (t.params.size() != 1 || !t.lhs)
          return fail(t.loc, "'Simd' expects an element type and a lane count, as in Simd(i32, 4)");
        TypePtr elem = resolveType(*t.params[0]);
        if (elem->isError())
          return elem;
        if (!elem->isInteger() && !elem->isFloat())
          return fail(t.loc, "Simd element must be an integer or a float, found '" + elem->str() + "'");
        if (t.lhs->kind != ExprKind::IntLit)
          return fail(t.lhs->loc, "Simd lane count must be 2, 4, 8, or 16");
        const auto n = as<IntLitExpr>(*t.lhs).value;
        if (n != 2 && n != 4 && n != 8 && n != 16)
          return fail(t.lhs->loc, "Simd lane count must be 2, 4, 8, or 16");
        return Type::simd(std::move(elem), static_cast<int>(n));
      }
      if (t.name == "Vec") {
        if (t.params.size() != 1 || !t.lhs)
          return fail(t.loc, "'Vec' expects an element type and a length, as in Vec(i32, 2)");
        TypePtr elem = resolveType(*t.params[0]);
        if (elem->isError())
          return elem;
        ExprPtr len = t.lhs->clone();
        TypePtr lt = checkExpr(len, Type::i32().get());
        if (lt->isError())
          return lt;
        if (lt->kind != TypeKind::I32)
          return fail(t.lhs->loc, "Vec length must be 'i32', found '" + lt->str() + "'");
        return Type::vector(std::move(elem), currentRegion_, std::move(len));
      }
      if (t.name == "String" || t.name == "str") {
        if (!t.params.empty())
          return fail(t.loc, "'String' takes no type arguments");
        return Type::stringTy(currentRegion_);
      }
      if (t.name == "ptr")
        return Type::ptr();
      if (t.name == "Vector") {
        if (t.params.size() != 1)
          return fail(t.loc, "'Vector' expects one type argument, as in Vector(i32)");
        TypePtr elem = resolveType(*t.params[0]);
        if (elem->isError())
          return elem;
        return Type::vector(std::move(elem), currentRegion_);
      }
      if (t.name == "Map") {
        if (t.params.size() != 2)
          return fail(t.loc, "'Map' expects two type arguments, as in Map(i32, i32)");
        TypePtr key = resolveType(*t.params[0]);
        TypePtr val = resolveType(*t.params[1]);
        if (key->isError() || val->isError())
          return Type::error();
        return Type::map(std::move(key), std::move(val), currentRegion_);
      }
      if (t.name == "Slice") {
        if (t.params.size() != 1)
          return fail(t.loc, "'Slice' expects one type argument, as in Slice(i32)");
        TypePtr elem = resolveType(*t.params[0]);
        if (elem->isError())
          return elem;
        if (elem->isErased() || elem->kind == TypeKind::Void)
          return fail(t.loc, "'Slice' element type '" + elem->str() + "' has no runtime representation");
        return Type::slice(std::move(elem), currentRegion_);
      }
      if (t.name == "Secret" || t.name == "Private") {
        if (t.params.size() != 1)
          return fail(t.loc, "'" + t.name + "' expects one type argument, as in Secret(i32)");
        TypePtr elem = resolveType(*t.params[0]);
        if (elem->isError())
          return elem;
        return Type::labeled(std::move(elem),
                             t.name == "Private" ? Secrecy::Private : Secrecy::Secret);
      }
      if (t.name == "Leak") {
        if (t.params.size() != 2)
          return fail(t.loc, "'Leak' expects two labels, as in Leak(Secret, Public)");
        auto label = [](const TypeExpr &lab, Secrecy &out) {
          if (lab.kind != TypeExprKind::Named || !lab.package.empty() || !lab.params.empty())
            return false;
          if (lab.name == "Public") {
            out = Secrecy::Public;
            return true;
          }
          if (lab.name == "Private") {
            out = Secrecy::Private;
            return true;
          }
          if (lab.name == "Secret") {
            out = Secrecy::Secret;
            return true;
          }
          return false;
        };
        Secrecy from = Secrecy::Public, to = Secrecy::Public;
        if (!label(*t.params[0], from) || !label(*t.params[1], to))
          return fail(t.loc, "'Leak' labels must be Public, Private or Secret");
        return Type::leak(from, to);
      }
    }
    Symbol *sym = resolveName(t.package, t.name, t.loc);
    if (!sym)
      return Type::error();
    if (sym->kind == SymbolKind::TypeParam)
      return Type::typeVariable(sym);
    if (sym->kind != SymbolKind::Type)
      return fail(t.loc, "'" + sym->qualifiedName() + "' is not a type");
    if (sym->typeBinders.empty()) {
      if (!t.params.empty())
        return fail(t.loc, "'" + sym->name + "' does not take type arguments");
      return sym->type;
    }
    if (t.params.size() != sym->typeBinders.size())
      return fail(t.loc, "'" + sym->name + "' expects " + std::to_string(sym->typeBinders.size()) +
                             " type argument(s), found " + std::to_string(t.params.size()));
    std::vector<TypePtr> args;
    for (size_t i = 0; i < t.params.size(); ++i) {
      TypePtr arg = resolveType(*t.params[i]);
      if (arg->isError())
        return arg;
      int level = universeOf(*arg);
      int bound = 0;
      if (sym->type && i < sym->type->typeParamLevels.size())
        bound = sym->type->typeParamLevels[i];
      if (level > bound)
        return fail(t.params[i]->loc, "type '" + arg->str() + "' lives in Type(" +
                                           std::to_string(level) + "), above the binder Type(" +
                                           std::to_string(bound) + ")");
      args.push_back(std::move(arg));
    }
    return instantiateSymbol(*sym, args, currentRegion_);
  }
  case TypeExprKind::Path: {
    TypePtr elem = t.elem ? resolveType(*t.elem) : nullptr;
    if (elem && elem->isError())
      return elem;
    ExprPtr lhs = t.lhs->clone();
    ExprPtr rhs = t.rhs->clone();
    bool savedConsume = consume_;
    consume_ = false;
    TypePtr lt = checkExpr(lhs, elem.get());
    consume_ = savedConsume;
    if (lt->isError())
      return lt;
    if (!elem)
      elem = lt;
    else     if (!unify(*lt, *elem, t.lhs->loc, "left endpoint of path type"))
      return Type::error();
    savedConsume = consume_;
    consume_ = false;
    TypePtr rt = checkExpr(rhs, elem.get());
    consume_ = savedConsume;
    if (rt->isError())
      return Type::error();
    if (!unify(*rt, *elem, t.rhs->loc, "right endpoint of path type"))
      return Type::error();
    return Type::path(std::move(elem), std::move(lhs), std::move(rhs));
  }
  case TypeExprKind::Equiv: {
    TypePtr a = resolveType(*t.params[0]);
    TypePtr b = resolveType(*t.params[1]);
    if (a->isError() || b->isError())
      return Type::error();
    return Type::equiv(a, b);
  }
  case TypeExprKind::Fn: {
    std::vector<TypePtr> params;
    for (const auto &p : t.params) {
      params.push_back(resolveType(*p));
      if (params.back()->isError())
        return Type::error();
    }
    TypePtr ret = t.ret ? resolveType(*t.ret) : Type::voidTy();
    if (ret->isError())
      return ret;
    return Type::fn(std::move(params), {}, std::move(ret), {}, t.effects, t.effectVar,
                    t.effectsDeclared);
  }
  case TypeExprKind::Quotient: {
    if (!t.elem || !t.lhs || !t.rhs)
      return fail(t.loc, "'Quotient' expects a carrier, a relation and a canon function");
    TypePtr carrier = resolveType(*t.elem);
    if (carrier->isError())
      return carrier;
    ExprPtr rel = t.lhs->clone();
    ExprPtr canon = t.rhs->clone();
    TypePtr relT = checkExpr(rel);
    TypePtr canonT = checkExpr(canon);
    if (relT->isError() || canonT->isError())
      return Type::error();
    auto unaryOk = [&](const Type &fn, const char *what) {
      if (fn.kind != TypeKind::Fn)
        return fail(t.loc, std::string(what) + " must be a function, found '" + fn.str() + "'"),
               false;
      return true;
    };
    if (!unaryOk(*relT, "quotient relation") || !unaryOk(*canonT, "quotient canon"))
      return Type::error();
    if (relT->params.size() != 2 || !relT->ret || relT->ret->kind != TypeKind::Universe) {
      return fail(t.loc, "quotient relation must have type fn(T, T): Type, found '" + relT->str() +
                             "'");
    }
    if (!unify(*relT->params[0], *carrier, t.lhs->loc, "quotient relation") ||
        !unify(*relT->params[1], *carrier, t.lhs->loc, "quotient relation"))
      return Type::error();
    if (canonT->params.size() != 1 || !canonT->ret ||
        !unify(*canonT->params[0], *carrier, t.rhs->loc, "quotient canon") ||
        !unify(*canonT->ret, *carrier, t.rhs->loc, "quotient canon"))
      return Type::error();
    const Symbol *canonFn = canon->kind == ExprKind::Ident ? as<IdentExpr>(*canon).symbol : nullptr;
    if (!canonFn || canonFn->kind != SymbolKind::Function)
      return fail(t.rhs->loc, "quotient canon must be a named function");
    return Type::quotient(std::move(carrier), std::move(rel), std::move(canon), canonFn);
  }
  }
  return Type::error();
}

const UseBinding *TypeChecker::findUse(const std::string &name) const {
  if (!file_)
    return nullptr;
  for (const UseBinding &use : file_->uses)
    if (!use.symbol.empty() && use.local == name)
      return &use;
  return nullptr;
}

Symbol *TypeChecker::resolveName(const std::string &pkg, const std::string &name,
                                 const SourceLocation &loc, bool reportErrors) {
  if (pkg.empty()) {
    if (Symbol *local = lookupLocal(name))
      return local;
    if (const UseBinding *use = findUse(name))
      return resolveName(use->package, use->symbol, loc, reportErrors);
  }

  auto res = registry_.lookup(pkg, name, *file_, package_);
  switch (res.status) {
  case ModuleRegistry::LookupStatus::Found:
    return res.symbol;
  case ModuleRegistry::LookupStatus::NotFound:
    if (reportErrors)
      diags_.error(loc, pkg.empty() ? "use of undeclared identifier '" + name + "'"
                                    : "package '" + pkg + "' has no symbol named '" + name + "'");
    return nullptr;
  case ModuleRegistry::LookupStatus::Private:
    if (reportErrors) {
      diags_.error(loc, "'" + name + "' is private to package '" + pkg +
                            "' and cannot be accessed from package '" + package_ + "'");
      diags_.note(res.symbol->loc, "'" + name + "' is declared 'pri' here");
    }
    return nullptr;
  case ModuleRegistry::LookupStatus::NotImported:
    if (reportErrors)
      diags_.error(loc, "package '" + pkg + "' is not imported in this file; add 'import " + pkg +
                            "'");
    return nullptr;
  case ModuleRegistry::LookupStatus::NoSuchPackage:
    if (reportErrors)
      diags_.error(loc, "unknown package '" + pkg + "'");
    return nullptr;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Scopes
// ---------------------------------------------------------------------------

Symbol *TypeChecker::declareLocal(Symbol sym) {
  Symbol *s = registry_.create(std::move(sym));
  if (!scopes_.empty())
    scopes_.back().names[s->name] = s;
  return s;
}

Symbol *TypeChecker::lookupLocal(const std::string &name) const {
  for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
    auto f = it->names.find(name);
    if (f != it->names.end())
      return f->second;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Phase 4: globals
// ---------------------------------------------------------------------------

void TypeChecker::checkGlobals() {
  for (auto &file : program_.files) {
    if (file->package.empty())
      continue;
    file_ = file.get();
    package_ = file->package;
    for (auto &g : file->globals) {
      if (!g->symbol)
        continue;
      TypePtr t;
      if (!g->symbol->type) {
        t = checkExpr(g->init, nullptr);
        g->symbol->type = t;
      } else {
        t = checkExpr(g->init, g->symbol->type.get());
      }
      if (t->isError())
        continue;
      if (!unify(*t, *g->symbol->type, g->init->loc, "initializer of '" + g->name + "'"))
        continue;
      if (g->symbol->type->isErased())
        continue; // proofs / type-level constants: no runtime representation
      ExprPtr nf = normalizer_.normalize(*g->init);
      if (!nf || !Normalizer::isValue(*nf))
        diags_.error(g->init->loc, "initializer of module-level '" + g->name +
                                       "' must be a compile-time constant (it normalises to '" +
                                       (nf ? toString(*nf) : std::string("<nothing>")) + "')");
    }
  }
}

// ---------------------------------------------------------------------------
// Phase 5: function bodies
// ---------------------------------------------------------------------------

void TypeChecker::checkConstFunctions() {
  for (auto &file : program_.files) {
    if (file->package.empty())
      continue;
    file_ = file.get();
    package_ = file->package;
    for (auto &fn : file->functions)
      if (fn->symbol && fn->isConstFn)
        checkFunction(*fn);
  }
}

void TypeChecker::checkFunctionBodies() {
  for (auto &file : program_.files) {
    if (file->package.empty())
      continue;
    file_ = file.get();
    package_ = file->package;
    for (auto &fn : file->functions)
      if (fn->symbol && !fn->isExtern && !fn->isConstFn)
        checkFunction(*fn);
  }
}

void TypeChecker::checkFunction(FnDecl &fn) {
  const Type &fnType = *fn.symbol->type;
  currentFn_ = &fn;
  currentRet_ = fnType.ret;
  currentRegion_ = 0;
  moved_.clear();

  if (fn.isCofn) {
    std::set<const Symbol *> seen;
    for (size_t i = 0; i < fnType.params.size(); ++i) {
      if (fnType.params[i] && !isFixedSize(*fnType.params[i], seen))
        diags_.error(fn.params[i].loc, "cofn state parameter '" + fn.params[i].name +
                                           "' must have a fixed-size layout (no Vector, Map, String or Slice)");
    }
  }

  EffectRow row;
  row.bits = fnType.effects;
  row.var = fnType.effectVar;
  row.declared = fnType.effectsDeclared;
  effectStack_.push_back(row);

  pushScope();
  for (const Symbol *binder : fnType.typeParams)
    if (binder)
      scopes_.back().names[binder->name] = const_cast<Symbol *>(binder);
  for (size_t i = 0; i < fn.params.size(); ++i)
    if (i < fnType.paramSymbols.size() && fnType.paramSymbols[i])
      scopes_.back().names[fn.params[i].name] = const_cast<Symbol *>(fnType.paramSymbols[i]);
  checkBlock(fn.body);
  if (fn.isCofn)
    verifyCofn(fn);
  std::vector<const Symbol *> indices;
  for (size_t i = 0; i < fnType.params.size(); ++i)
    if (fnType.params[i] && fnType.params[i]->kind == TypeKind::Interval &&
        i < fnType.paramSymbols.size() && fnType.paramSymbols[i])
      indices.push_back(fnType.paramSymbols[i]);
  if (!indices.empty())
    checkIndexResidues(fn.body, indices);
  bool returns = blockAlwaysReturns(fn.body);
  if (!returns)
    exhaustScope(scopes_.size() - 1, fn.loc);
  popScope();

  if (currentRet_->kind != TypeKind::Void && !returns)
    diags_.error(fn.loc, "function '" + fn.name + "' declares return type '" + currentRet_->str() +
                             "' but not all control paths end in 'ret'");
  effectStack_.pop_back();
  currentFn_ = nullptr;
  currentRet_ = nullptr;
  if (fn.name == "main" && fn.implIndex < 0 && !fn.isExtern)
    checkMain(fn);
  if (fn.isConstFn)
    checkConstFn(fn);
}

void TypeChecker::checkMain(const FnDecl &fn) {
  if (!fn.symbol || !fn.symbol->type || !fn.symbol->type->ret)
    return;
  const Type &ty = *fn.symbol->type;
  const bool retOk = ty.ret->kind == TypeKind::I32;
  const bool noArgs = ty.params.empty();
  const bool cArgs = ty.params.size() == 2 && ty.params[0] && ty.params[0]->kind == TypeKind::I32 &&
                     ty.params[1] && ty.params[1]->kind == TypeKind::Ptr;
  if (!retOk || (!noArgs && !cArgs))
    diags_.error(fn.loc, "main must be 'fn main(): i32' or 'fn main(argc: i32, argv: ptr): i32'");
}

void TypeChecker::checkForIterator(ForStmt &f) {
  if (!f.iter || f.iter->kind != ExprKind::Ident) {
    diags_.error(f.loc, "'for' over an iterator needs a local variable, as in 'for x in it'");
    return;
  }
  bool saved = consume_;
  consume_ = false;
  TypePtr src = checkExpr(f.iter);
  consume_ = saved;
  if (!src || src->isError())
    return;
  const Symbol *sym = as<IdentExpr>(*f.iter).symbol;
  if (!sym || sym->kind != SymbolKind::Local || sym->isConst) {
    diags_.error(f.iter->loc, "'for' iterator must be a 'var'");
    return;
  }
  TraitDispatch hit;
  const TraitLookup found = lookupTrait("next", src, f.loc, hit);
  if (found != TraitLookup::Found || !hit.fn || !hit.fn->type) {
    diags_.error(f.loc, "no iterator method 'next' for '" + src->str() + "'");
    return;
  }
  const Type &sig = *hit.fn->type;
  if (sig.params.empty() || !sig.params[0] || sig.params[0]->kind != TypeKind::Ref ||
      !sig.params[0]->refMut) {
    diags_.error(f.loc, "iterator method 'next' must take '&mut Self'");
    return;
  }
  auto callee = std::make_unique<IdentExpr>(f.loc, std::string{}, hit.fn->name);
  callee->symbol = hit.fn;
  callee->type = hit.fn->type;
  std::vector<ExprPtr> args;
  args.push_back(f.iter->clone());
  auto call = std::make_unique<CallExpr>(f.loc, std::move(callee), std::move(args));
  call->fn = hit.fn;
  call->typeArguments = hit.typeArgs;
  TypePtr ret = checkFnCall(*call, *hit.fn->type, nullptr);
  call->type = ret;
  f.step = std::move(call);
  releaseStatementBorrows();
  if (!ret || ret->isError())
    return;
  if (ret->kind != TypeKind::Inductive || !ret->inductive || ret->inductive->name != "Option" ||
      ret->typeArgs.size() != 1 || !ret->typeArgs[0]) {
    diags_.error(f.loc, "iterator method 'next' must return Option(T), found '" + ret->str() + "'");
    return;
  }
  const Symbol *none = nullptr;
  const Symbol *some = nullptr;
  for (const Symbol *ctor : ret->inductive->ctors) {
    if (!ctor)
      continue;
    if (ctor->name == "None")
      none = ctor;
    if (ctor->name == "Some")
      some = ctor;
  }
  if (!none || !some) {
    diags_.error(f.loc, "iterator method 'next' must return Option(T), found '" + ret->str() + "'");
    return;
  }
  f.noneTag = none->ctorIndex;
  f.someTag = some->ctorIndex;
  f.someCtor = some;
  Symbol bs;
  bs.kind = SymbolKind::Local;
  bs.name = f.name;
  bs.type = ret->typeArgs[0];
  bs.loc = f.loc;
  bs.isConst = true;
  pushScope();
  f.binder = declareLocal(std::move(bs));
  ++loopDepth_;
  checkBlock(f.body);
  --loopDepth_;
  popScope();
}

void TypeChecker::checkFieldAssign(FieldAssignStmt &stmt) {
  if (!stmt.place || !stmt.value)
    return;
  bool saved = consume_;
  consume_ = false;
  TypePtr placeTy = checkExpr(stmt.place);
  consume_ = saved;
  if (!placeTy || placeTy->isError())
    return;
  bool viaRef = false;
  bool viaMut = false;
  if (placeTy->kind == TypeKind::Ref && placeTy->elem) {
    viaRef = true;
    viaMut = placeTy->refMut;
    placeTy = placeTy->elem;
  }
  if (placeTy->kind != TypeKind::Struct) {
    diags_.error(stmt.loc, "field assignment needs a struct, found '" + placeTy->str() + "'");
    return;
  }
  const Type *fieldTy = nullptr;
  for (size_t i = 0; i < placeTy->params.size(); ++i) {
    const Symbol *field = i < placeTy->paramSymbols.size() ? placeTy->paramSymbols[i] : nullptr;
    if (!field || field->name != stmt.field)
      continue;
    fieldTy = placeTy->params[i].get();
    break;
  }
  if (!fieldTy) {
    diags_.error(stmt.loc, "struct has no field '" + stmt.field + "'");
    return;
  }
  if (fieldTy->isStorage()) {
    diags_.error(stmt.loc, "cannot assign owning field '" + stmt.field + "'");
    return;
  }
  if (viaRef) {
    if (!viaMut)
      diags_.error(stmt.loc, "field assignment needs '&mut'");
  } else if (stmt.place->kind == ExprKind::Ident) {
    const Symbol *sym = as<IdentExpr>(*stmt.place).symbol;
    if (!sym || (sym->kind != SymbolKind::Local && sym->kind != SymbolKind::Global))
      diags_.error(stmt.loc, "field assignment updates a named variable");
    else if (sym->isConst)
      diags_.error(stmt.loc, "cannot assign to constant '" + sym->name + "'");
    else
      rejectIfBorrowed(sym, stmt.loc, true);
  } else {
    diags_.error(stmt.loc, "field assignment updates a named variable or '&mut'");
  }
  TypePtr vt = checkExpr(stmt.value, fieldTy);
  if (vt && !vt->isError())
    unify(*vt, *fieldTy, stmt.value->loc, "assignment to '" + stmt.field + "'");
}

bool TypeChecker::isConstType(const Type &type, std::set<const Symbol *> &seen) const {
  if (type.isInteger() || type.isFloat() || type.kind == TypeKind::Bool)
    return true;
  if (type.kind != TypeKind::Struct || !type.inductive)
    return false;
  if (!seen.insert(type.inductive).second)
    return false;
  for (const auto &field : type.params) {
    if (!field || !isConstType(*field, seen)) {
      seen.erase(type.inductive);
      return false;
    }
  }
  seen.erase(type.inductive);
  return true;
}

void TypeChecker::checkConstFn(const FnDecl &fn) {
  if (!fn.typeBinders.empty())
    diags_.error(fn.loc, "const fn '" + fn.name + "' cannot have a type binder");
  if (fn.isExtern || fn.isCofn)
    diags_.error(fn.loc, "const fn '" + fn.name + "' cannot be 'extern' or 'cofn'");
  if ((fn.effectsDeclared && fn.effects != 0) || !fn.effectVar.empty())
    diags_.error(fn.loc, "const fn '" + fn.name + "' cannot have an effect");
  std::set<const Symbol *> seen;
  if (fn.symbol && fn.symbol->type) {
    const Type &ty = *fn.symbol->type;
    if (!ty.ret || !isConstType(*ty.ret, seen))
      diags_.error(fn.loc, "const fn '" + fn.name +
                               "' must return a scalar or a fixed struct, found '" +
                               (ty.ret ? ty.ret->str() : std::string("void")) + "'");
    for (size_t i = 0; i < ty.params.size() && i < fn.params.size(); ++i) {
      seen.clear();
      if (ty.params[i] && !isConstType(*ty.params[i], seen))
        diags_.error(fn.params[i].loc, "const fn parameter '" + fn.params[i].name +
                                           "' must be a scalar or a fixed struct, found '" +
                                           ty.params[i]->str() + "'");
    }
  }
  checkConstBlock(fn.body);
}

void TypeChecker::checkConstBlock(const Block &block) {
  for (const auto &stmt : block) {
    if (!stmt)
      continue;
    switch (stmt->kind) {
    case StmtKind::VarDecl: {
      const auto &d = static_cast<const VarDeclStmt &>(*stmt);
      if (!d.isConst)
        diags_.error(d.loc, "const fn cannot declare 'var'");
      else if (d.init)
        checkConstExpr(*d.init);
      break;
    }
    case StmtKind::Return: {
      const auto &r = static_cast<const ReturnStmt &>(*stmt);
      if (r.value)
        checkConstExpr(*r.value);
      break;
    }
    case StmtKind::If: {
      const auto &i = static_cast<const IfStmt &>(*stmt);
      if (i.cond)
        checkConstExpr(*i.cond);
      checkConstBlock(i.thenBody);
      checkConstBlock(i.elseBody);
      break;
    }
    default:
      diags_.error(stmt->loc, "const fn body can only use 'const', 'if' and 'ret'");
      break;
    }
  }
}

void TypeChecker::checkConstExpr(const Expr &e) {
  switch (e.kind) {
  case ExprKind::IntLit:
  case ExprKind::FloatLit:
  case ExprKind::BoolLit:
    return;
  case ExprKind::Unary: {
    const auto &u = as<UnaryExpr>(e);
    if (u.op == UnaryOp::IntervalRev)
      diags_.error(e.loc, "const fn cannot use an interval");
    if (u.operand)
      checkConstExpr(*u.operand);
    return;
  }
  case ExprKind::Binary: {
    const auto &b = as<BinaryExpr>(e);
    if (b.lhs)
      checkConstExpr(*b.lhs);
    if (b.rhs)
      checkConstExpr(*b.rhs);
    return;
  }
  case ExprKind::Cast: {
    const auto &c = as<CastExpr>(e);
    if (c.operand)
      checkConstExpr(*c.operand);
    if ((e.type && e.type->kind == TypeKind::Ptr) ||
        (c.operand && c.operand->type && c.operand->type->kind == TypeKind::Ptr))
      diags_.error(e.loc, "const fn cannot cast through 'ptr'");
    return;
  }
  case ExprKind::Ident: {
    const Symbol *sym = as<IdentExpr>(e).symbol;
    if (!sym)
      return;
    if (sym->kind == SymbolKind::Local || sym->kind == SymbolKind::Param) {
      if (sym->kind == SymbolKind::Local && !sym->isConst)
        diags_.error(e.loc, "const fn cannot use 'var' '" + sym->name + "'");
      return;
    }
    if (sym->kind == SymbolKind::Global && sym->isConst)
      return;
    diags_.error(e.loc, "const fn cannot use '" + sym->name + "' as a value");
    return;
  }
  case ExprKind::Call: {
    const auto &c = as<CallExpr>(e);
    for (const auto &arg : c.args)
      if (arg)
        checkConstExpr(*arg);
    if (c.fn && c.fn->kind == SymbolKind::Type && c.fn->type &&
        c.fn->type->kind == TypeKind::Struct)
      return;
    if (c.fn && c.fn->fnDecl && c.fn->fnDecl->isConstFn)
      return;
    diags_.error(e.loc, "const fn can only call a const fn, found '" +
                            (c.fn ? c.fn->name : std::string("?")) + "'");
    return;
  }
  case ExprKind::Method: {
    const auto &m = as<MethodCallExpr>(e);
    if (m.invoked) {
      diags_.error(e.loc, "const fn cannot call a method");
      return;
    }
    if (m.receiver)
      checkConstExpr(*m.receiver);
    return;
  }
  default:
    diags_.error(e.loc, "const fn cannot evaluate this expression");
    return;
  }
}

bool TypeChecker::blockAlwaysReturns(const Block &block) {
  for (const auto &s : block) {
    if (s->kind == StmtKind::Return)
      return true;
    if (s->kind == StmtKind::ScopeArena) {
      if (blockAlwaysReturns(static_cast<const ScopeArenaStmt &>(*s).body))
        return true;
    }
    if (s->kind == StmtKind::Unsafe) {
      if (blockAlwaysReturns(static_cast<const UnsafeStmt &>(*s).body))
        return true;
    }
    if (s->kind == StmtKind::If) {
      const auto &i = static_cast<const IfStmt &>(*s);
      if (!i.elseBody.empty() && blockAlwaysReturns(i.thenBody) && blockAlwaysReturns(i.elseBody))
        return true;
    }
    if (s->kind == StmtKind::Foreach) {
      if (blockAlwaysReturns(static_cast<const ForeachStmt &>(*s).body))
        return true;
    }
    if (s->kind == StmtKind::Expr) {
      const auto &e = static_cast<const ExprStmt &>(*s);
      if (e.expr && e.expr->kind == ExprKind::Call && static_cast<const CallExpr &>(*e.expr).tailSelf)
        return true;
    }
  }
  return false;
}

void TypeChecker::checkBlock(Block &block) {
  pushScope();
  size_t scopeIndex = scopes_.size() - 1;
  for (auto &s : block)
    checkStmt(*s);
  if (!blockAlwaysReturns(block)) {
    SourceLocation loc = block.empty() ? SourceLocation{} : block.back()->loc;
    exhaustScope(scopeIndex, loc);
  }
  popScope();
}

void TypeChecker::checkStmt(Stmt &stmt) {
  switch (stmt.kind) {
  case StmtKind::VarDecl: {
    auto &d = static_cast<VarDeclStmt &>(stmt);
    TypePtr declared = d.type ? resolveType(*d.type) : nullptr;
    TypePtr it = Type::error();
    if (!declared || !declared->isError())
      it = checkExpr(d.init, declared.get());
    TypePtr t = declared ? declared : it;
    if (t && !t->isError()) {
      if (isStaticOnlyType(*t) && !d.isConst)
        diags_.error(d.loc, "a 'var' cannot have the static-only type '" + t->str() +
                                "'; use 'const'");
      if (declared && !it->isError())
        unify(*it, *declared, d.init->loc, "initializer of '" + d.name + "'");
      else if (!declared && t->isRegionBound())
        requireLives(*t, currentRegion_, d.init->loc, "initializer of '" + d.name + "'");
    }
    bool shared = d.shared;
    if (!shared && d.init && d.init->kind == ExprKind::Ident) {
      const Symbol *src = as<IdentExpr>(*d.init).symbol;
      if (src && src->shared)
        shared = true;
    }
    if (scopes_.back().names.count(d.name))
      diags_.error(d.loc, "redeclaration of '" + d.name + "' in the same scope");
    if ((shared || d.isConst) && d.usage != Usage::Many)
      diags_.error(d.loc, "'lin' and 'aff' cannot be combined with 'shared' or 'const'");
    Symbol s;
    s.kind = SymbolKind::Local;
    s.name = d.name;
    s.type = t ? t : Type::error();
    s.loc = d.loc;
    s.isConst = d.isConst;
    s.shared = shared && d.usage != Usage::Linear;
    s.usage = resolveUsage(d.usage, s.type ? *s.type : *Type::error(), s.shared, d.isConst);
    s.constInit = d.isConst ? d.init.get() : nullptr;
    d.symbol = declareLocal(std::move(s));
    if (d.symbol && d.init)
      bindLoan(*d.init, d.symbol);
    break;
  }
  case StmtKind::Assign: {
    auto &a = static_cast<AssignStmt &>(stmt);
    Symbol *sym = resolveName("", a.name, a.loc);
    if (!sym)
      break;
    if (sym->kind != SymbolKind::Local && sym->kind != SymbolKind::Global &&
        sym->kind != SymbolKind::Param) {
      diags_.error(a.loc, "cannot assign to '" + a.name + "'");
      break;
    }
    if (sym->isConst) {
      diags_.error(a.loc, "cannot assign to constant '" + a.name + "'");
      break;
    }
    if (sym->kind == SymbolKind::Param) {
      diags_.error(a.loc, "parameters are immutable; copy '" + a.name + "' into a 'var' first");
      break;
    }
    a.symbol = sym;
    TypePtr vt = checkExpr(a.value, sym->type.get());
    if (!vt->isError())
      unify(*vt, *sym->type, a.value->loc, "assignment to '" + a.name + "'");
    if (sym->usage == Usage::Linear && !moved_.count(sym))
      diags_.error(a.loc, "assignment drops linear binding '" + sym->name + "'");
    if (sym->usage != Usage::Many)
      moved_.erase(sym);
    rejectIfBorrowed(sym, a.loc, true);
    break;
  }
  case StmtKind::Return: {
    auto &r = static_cast<ReturnStmt &>(stmt);
    if (!currentRet_)
      break;
    if (currentRet_->kind == TypeKind::Void) {
      if (r.value)
        diags_.error(r.loc, "'ret' with a value in a function without a return type");
      break;
    }
    if (!r.value) {
      diags_.error(r.loc, "'ret' must return a value of type '" + currentRet_->str() + "'");
      break;
    }
    TypePtr t = checkExpr(r.value, currentRet_.get());
    auto localOwner = [](const Symbol *owner) {
      return owner && (owner->kind == SymbolKind::Local || owner->kind == SymbolKind::Param);
    };
    if (r.value->kind == ExprKind::Borrow) {
      const auto &b = as<BorrowExpr>(*r.value);
      if (localOwner(b.owner))
        diags_.error(r.loc, "cannot return a borrow of '" + b.owner->name + "'");
    } else if (r.value->kind == ExprKind::Ident) {
      const Symbol *sym = as<IdentExpr>(*r.value).symbol;
      for (const auto &b : borrows_)
        if (b.refBinding == sym && localOwner(b.owner))
          diags_.error(r.loc, "cannot return '" + sym->name + "', it borrows '" + b.owner->name + "'");
    } else if (localOwner(r.value->loanOwner)) {
      diags_.error(r.loc, "cannot return a value that borrows '" + r.value->loanOwner->name + "'");
    }
    if (!t->isError())
      unify(*t, *currentRet_, r.value->loc, "return value");
    exhaustInScope(r.loc);
    break;
  }
  case StmtKind::If: {
    auto &i = static_cast<IfStmt &>(stmt);
    TypePtr ct = checkExpr(i.cond, Type::boolean().get());
    if (!ct->isError() && ct->kind != TypeKind::Bool)
      diags_.error(i.cond->loc, "'if' condition must be 'bool', found '" + ct->str() + "'");
    auto saved = moved_;
    checkBlock(i.thenBody);
    auto thenSet = moved_;
    bool thenDiv = blockAlwaysReturns(i.thenBody);
    moved_ = saved;
    bool hasElse = !i.elseBody.empty();
    bool elseDiv = false;
    std::set<const Symbol *> elseSet = saved;
    if (hasElse) {
      checkBlock(i.elseBody);
      elseSet = moved_;
      elseDiv = blockAlwaysReturns(i.elseBody);
    }
    joinUses(saved, thenSet, elseSet, thenDiv, elseDiv, hasElse, i.loc);
    break;
  }
  case StmtKind::While: {
    auto &w = static_cast<WhileStmt &>(stmt);
    TypePtr ct = checkExpr(w.cond, Type::boolean().get());
    if (!ct->isError() && ct->kind != TypeKind::Bool)
      diags_.error(w.cond->loc, "'while' condition must be 'bool', found '" + ct->str() + "'");
    auto saved = moved_;
    auto captured = symbolsInScope();
    ++loopDepth_;
    checkBlock(w.body);
    --loopDepth_;
    for (const Symbol *sym : moved_) {
      if (saved.count(sym) || !captured.count(sym) || sym->usage == Usage::Many)
        continue;
      diags_.error(w.loc, "owned binding '" + sym->name + "' cannot be consumed inside a loop");
    }
    moved_ = std::move(saved);
    break;
  }
  case StmtKind::For: {
    auto &f = static_cast<ForStmt &>(stmt);
    if (f.iter) {
      checkForIterator(f);
      break;
    }
    TypePtr low = checkExpr(f.low, Type::i64().get());
    TypePtr high = checkExpr(f.high, Type::i64().get());
    if (!low->isError() && low->kind != TypeKind::I64)
      diags_.error(f.low->loc, "'for' lower bound must be 'i64', found '" + low->str() + "'");
    if (!high->isError() && high->kind != TypeKind::I64)
      diags_.error(f.high->loc, "'for' upper bound must be 'i64', found '" + high->str() + "'");
    Symbol bs;
    bs.kind = SymbolKind::Local;
    bs.name = f.name;
    bs.type = Type::i64();
    bs.loc = f.loc;
    bs.isConst = true;
    pushScope();
    f.binder = declareLocal(std::move(bs));
    ++loopDepth_;
    checkBlock(f.body);
    --loopDepth_;
    popScope();
    break;
  }
  case StmtKind::FieldAssign:
    checkFieldAssign(static_cast<FieldAssignStmt &>(stmt));
    break;
  case StmtKind::Break:
    if (loopDepth_ == 0)
      diags_.error(stmt.loc, "'break' is outside a 'while' or 'for' loop");
    break;
  case StmtKind::Continue:
    if (loopDepth_ == 0)
      diags_.error(stmt.loc, "'continue' is outside a 'while' or 'for' loop");
    break;
  case StmtKind::IndexAssign: {
    auto &ix = static_cast<IndexAssignStmt &>(stmt);
    bool saved = consume_;
    consume_ = false;
    TypePtr place = checkExpr(ix.place);
    consume_ = saved;
    if (place->isError())
      break;
    TypePtr elem;
    bool mutablePlace = false;
    if (place->kind == TypeKind::Vector || place->kind == TypeKind::Slice) {
      elem = place->elem;
      mutablePlace = true;
    } else if (place->kind == TypeKind::Ref && place->elem &&
               (place->elem->kind == TypeKind::Vector || place->elem->kind == TypeKind::Slice)) {
      elem = place->elem->elem;
      mutablePlace = place->refMut;
    }
    if (!elem)
      diags_.error(ix.loc, "index assignment requires a Vector or Slice");
    else if (!mutablePlace && place->kind != TypeKind::Vector)
      diags_.error(ix.loc, "index assignment needs a mutable place");
    else if (place->kind == TypeKind::Vector && ix.place->kind != ExprKind::Ident)
      diags_.error(ix.place->loc, "index assignment updates a named variable or an '&mut' place");
    if (place->kind == TypeKind::Vector && ix.place->kind == ExprKind::Ident)
      rejectIfBorrowed(as<IdentExpr>(*ix.place).symbol, ix.loc, true);
    TypePtr idx = checkExpr(ix.index, Type::i64().get());
    if (!idx->isError() && idx->kind != TypeKind::I64)
      diags_.error(ix.index->loc, "index must be 'i64', found '" + idx->str() + "'");
    if (elem) {
      TypePtr vt = checkExpr(ix.value, elem.get());
      if (!vt->isError())
        unify(*vt, *elem, ix.value->loc, "index assignment");
    }
    break;
  }
  case StmtKind::Foreach: {
    auto &f = static_cast<ForeachStmt &>(stmt);
    Symbol bs;
    bs.kind = SymbolKind::IntervalVar;
    bs.name = f.var;
    bs.type = Type::interval();
    bs.loc = f.loc;
    pushScope();
    Symbol *binder = declareLocal(std::move(bs));
    f.binder = binder;
    checkBlock(f.body);
    popScope();
    checkIndexResidues(f.body, {binder});
    break;
  }
  case StmtKind::ScopeArena: {
    auto &a = static_cast<ScopeArenaStmt &>(stmt);
    // The region is pushed first so the Arena scope records that region id.
    pushRegion(true, a.loc);
    pushScope(ScopeKind::Arena);
    for (auto &s : a.body)
      checkStmt(*s);
    popScope();
    popRegion();
    break;
  }
  case StmtKind::Unsafe: {
    auto &u = static_cast<UnsafeStmt &>(stmt);
    ++unsafeDepth_;
    checkBlock(u.body);
    --unsafeDepth_;
    break;
  }
  case StmtKind::Expr: {
    auto &e = static_cast<ExprStmt &>(stmt);
    TypePtr t = checkExpr(e.expr);
    if (!t->isError() && t->kind != TypeKind::Void && e.expr->kind != ExprKind::Call &&
        e.expr->kind != ExprKind::Method)
      diags_.warning(e.loc, "expression result of type '" + t->str() + "' is unused");
    break;
  }
  case StmtKind::Error:
    break;
  }
  releaseStatementBorrows();
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

bool TypeChecker::unify(const Type &actual, const Type &expected, const SourceLocation &loc,
                        const std::string &context) {
  if (actual.isError() || expected.isError())
    return false;
  if (actual.kind == TypeKind::Universe && expected.kind == TypeKind::Universe &&
      actual.level <= expected.level) {
    return true;
  }
  if (normalizer_.typeEqual(actual, expected)) {
    if (actual.kind == TypeKind::Fn && expected.kind == TypeKind::Fn &&
        !effectsCompatible(actual, expected)) {
      diags_.error(loc, "effect mismatch in " + context + ": expected '" + expected.str() +
                            "', found '" + actual.str() + "'");
      return false;
    }
    if (actual.isRegionBound() && !regionOutlives(actual.region, expected.region)) {
      requireLives(actual, expected.region, loc, context);
      return false;
    }
    return true;
  }
  const Type *payloadA = &actual;
  const Type *payloadE = &expected;
  Secrecy labelA = Secrecy::Public;
  Secrecy labelE = Secrecy::Public;
  if (payloadA->kind == TypeKind::Labeled && payloadA->elem) {
    labelA = payloadA->secrecy;
    payloadA = payloadA->elem.get();
  }
  if (payloadE->kind == TypeKind::Labeled && payloadE->elem) {
    labelE = payloadE->secrecy;
    payloadE = payloadE->elem.get();
  }
  if (payloadA != &actual || payloadE != &expected) {
    if (secrecyRank(labelA) > secrecyRank(labelE)) {
      diags_.error(loc, "PathObstructionError: no path from " + actual.str() + " to " + expected.str());
      return false;
    }
    if (!normalizer_.typeEqual(*payloadA, *payloadE)) {
      diags_.error(loc, "type mismatch in " + context + ": expected '" + expected.str() +
                            "', found '" + actual.str() + "' (Cruft performs no implicit coercions)");
      return false;
    }
    if (actual.isRegionBound() && !regionOutlives(actual.region, expected.region)) {
      requireLives(actual, expected.region, loc, context);
      return false;
    }
    return true;
  }
  diags_.error(loc, "type mismatch in " + context + ": expected '" + expected.str() + "', found '" +
                        actual.str() + "' (Cruft performs no implicit coercions)");
  return false;
}

ExprPtr TypeChecker::makeTypeLit(const TypePtr &t, const SourceLocation &loc) const {
  auto te = std::make_unique<TypeExpr>(TypeExprKind::Named, loc);
  te->name = t->str();
  auto lit = std::make_unique<TypeLitExpr>(loc, std::move(te));
  lit->denoted = t;
  lit->type = typeOfDenoted(*t);
  return lit;
}

/// For a term `e : Type`, the type it denotes (after normalisation).
TypePtr TypeChecker::typeOfTypeTerm(const Expr &e) {
  ExprPtr nf = normalizer_.normalize(e);
  if (nf->kind == ExprKind::TypeLit && as<TypeLitExpr>(*nf).denoted)
    return as<TypeLitExpr>(*nf).denoted;
  return nullptr;
}

TypePtr TypeChecker::checkExpr(ExprPtr &e, const Type *expected) {
  struct Restore {
    bool &slot;
    bool saved;
    explicit Restore(bool &s) : slot(s), saved(s) {}
    ~Restore() { slot = saved; }
  } restore(consume_);
  if (expected && expected->isErased())
    consume_ = false;

  TypePtr t;
  switch (e->kind) {
  case ExprKind::IntLit: {
    auto &lit = as<IntLitExpr>(*e);
    if (expected && expected->isFloat()) {
      // Literal adaptation (not a coercion): `3` may denote the f64 3.0.
      auto f = std::make_unique<FloatLitExpr>(e->loc, static_cast<double>(lit.value));
      e = std::move(f);
      t = expected->kind == TypeKind::F32 ? Type::f32() : Type::f64();
      break;
    }
    auto fits = [&](TypeKind k) {
      const std::int64_t v = lit.value;
      switch (k) {
      case TypeKind::I8: return v >= -128 && v <= 127;
      case TypeKind::U8: return v >= 0 && v <= 255;
      case TypeKind::I16: return v >= -32768 && v <= 32767;
      case TypeKind::U16: return v >= 0 && v <= 65535;
      case TypeKind::I32: return v >= INT32_MIN && v <= INT32_MAX;
      case TypeKind::U32: return v >= 0 && v <= static_cast<std::int64_t>(UINT32_MAX);
      case TypeKind::I64: return true;
      case TypeKind::U64: return v >= 0;
      case TypeKind::USize:
        return Type::pointerBits() >= 64
                   ? v >= 0
                   : v >= 0 && v <= static_cast<std::int64_t>(UINT32_MAX);
      case TypeKind::ISize:
        return Type::pointerBits() >= 64 || (v >= INT32_MIN && v <= INT32_MAX);
      default: return false;
      }
    };
    TypeKind chosen = TypeKind::I32;
    if (expected && expected->isInteger() && fits(expected->kind))
      chosen = expected->kind;
    else if (!fits(TypeKind::I32))
      return fail(e->loc, "integer literal " + std::to_string(lit.value) + " does not fit in i32");
    switch (chosen) {
    case TypeKind::I8: t = Type::i8(); break;
    case TypeKind::I16: t = Type::i16(); break;
    case TypeKind::I32: t = Type::i32(); break;
    case TypeKind::I64: t = Type::i64(); break;
    case TypeKind::U8: t = Type::u8(); break;
    case TypeKind::U16: t = Type::u16(); break;
    case TypeKind::U32: t = Type::u32(); break;
    case TypeKind::U64: t = Type::u64(); break;
    case TypeKind::USize: t = Type::usize(); break;
    case TypeKind::ISize: t = Type::isize(); break;
    default: t = Type::i32(); break;
    }
    break;
  }
  case ExprKind::FloatLit:
    t = expected && expected->kind == TypeKind::F32 ? Type::f32() : Type::f64();
    break;
  case ExprKind::BoolLit:
    t = Type::boolean();
    break;
  case ExprKind::IntervalLit:
    t = Type::interval();
    break;
  case ExprKind::Ident:
    t = checkIdent(e, expected);
    break;
  case ExprKind::Binary:
    t = checkBinary(as<BinaryExpr>(*e), expected);
    break;
  case ExprKind::Unary:
    t = checkUnary(as<UnaryExpr>(*e), expected);
    break;
  case ExprKind::Call:
    t = checkCall(e, expected);
    break;
  case ExprKind::TypeLit: {
    auto &lit = as<TypeLitExpr>(*e);
    lit.denoted = resolveType(*lit.typeExpr);
    t = lit.denoted->isError() ? Type::error() : typeOfDenoted(*lit.denoted);
    break;
  }
  case ExprKind::PathAbs:
    t = checkPathAbs(as<PathAbsExpr>(*e), expected);
    break;
  case ExprKind::PathApp:
    t = checkPathApp(as<PathAppExpr>(*e));
    break;
  case ExprKind::Hcomp:
    t = checkHcomp(as<HcompExpr>(*e));
    break;
  case ExprKind::Match:
    t = checkMatch(as<MatchExpr>(*e), expected);
    break;
  case ExprKind::StringLit:
    t = Type::stringTy(currentRegion_);
    break;
  case ExprKind::ArrayLit:
    t = checkArray(as<ArrayLitExpr>(*e), expected);
    break;
  case ExprKind::Lambda:
    t = checkLambda(as<LambdaExpr>(*e), expected);
    break;
  case ExprKind::Method:
    t = checkMethod(e, expected);
    break;
  case ExprKind::Cast:
    t = checkCast(as<CastExpr>(*e));
    break;
  case ExprKind::Borrow:
    t = checkBorrow(as<BorrowExpr>(*e));
    break;
  case ExprKind::Try:
    t = checkTry(as<TryExpr>(*e));
    break;
  }
  if (!t)
    t = Type::error();
  e->type = t;
  if (t->kind == TypeKind::Labeled)
    e->flow = Usage::Linear;
  else if (e->flow == Usage::Many && t->isStorage())
    e->flow = Usage::Affine;
  if (!t->isError() && t->kind == TypeKind::Ptr)
    requireUnsafe(e->loc, "a raw 'ptr' operation");
  return t;
}

TypePtr TypeChecker::checkIdent(ExprPtr &e, const Type *expected) {
  auto &id = as<IdentExpr>(*e);
  Symbol *sym = resolveName(id.package, id.name, id.loc);
  if (!sym)
    return Type::error();
  id.symbol = sym;
  switch (sym->kind) {
  case SymbolKind::Local:
  case SymbolKind::Param:
  case SymbolKind::Global:
  case SymbolKind::IntervalVar:
  case SymbolKind::PatternVar:
    noteUse(sym, id.loc);
    if (!inBorrowPlace_ && !consume_)
      rejectIfBorrowed(sym, id.loc, false);
    for (const auto &b : borrows_) {
      if (b.refBinding == sym && b.owner) {
        e->loanOwner = b.owner;
        break;
      }
    }
    id.flow = sym->usage;
    return sym->type ? sym->type : Type::error();
  case SymbolKind::Axiom:
    return sym->type ? sym->type : Type::error();
  case SymbolKind::TypeParam: {
    ExprPtr lit = makeTypeLit(Type::typeVariable(sym), id.loc);
    e = std::move(lit);
    return typeOfDenoted(*Type::typeVariable(sym));
  }
  case SymbolKind::Function:
    return sym->type; // first-class reference => C function pointer
  case SymbolKind::Type: {
    // A type used as a term: rewrite to a type literal.
    ExprPtr lit = makeTypeLit(sym->type, id.loc);
    e = std::move(lit);
    return typeOfDenoted(*sym->type);
  }
  case SymbolKind::Trait:
    return fail(id.loc, "trait '" + sym->name + "' is not a value");
  case SymbolKind::Ctor: {
    if (!sym->type || sym->type->isError())
      return Type::error();
    if (sym->owner && !sym->owner->typeBinders.empty()) {
      if (!expected || expected->kind != TypeKind::Inductive || expected->inductive != sym->owner ||
          expected->typeArgs.size() != sym->owner->typeBinders.size())
        return fail(id.loc, "generic constructor '" + sym->name +
                                "' needs an expected type, as in 'var x: Option(i32) = None'");
      if (sym->type->kind == TypeKind::Fn)
        return fail(id.loc, "constructor '" + sym->name + "' expects " +
                                std::to_string(sym->type->params.size()) + " argument(s)");
      auto call = std::make_unique<CallExpr>(id.loc, std::move(e), std::vector<ExprPtr>{});
      call->fn = sym;
      e = std::move(call);
      return instantiateSymbol(*sym->owner, expected->typeArgs, expected->region);
    }
    if (sym->type->kind == TypeKind::Fn)
      return fail(id.loc, "constructor '" + sym->name + "' expects " +
                              std::to_string(sym->type->params.size()) +
                              " argument(s); constructors are not first-class functions");
    // Nullary constructor: canonicalise to a zero-argument constructor call
    // so that the normaliser and CodeGen see a single shape.
    auto call = std::make_unique<CallExpr>(id.loc, std::move(e), std::vector<ExprPtr>{});
    call->fn = sym;
    call->type = sym->type;
    e = std::move(call);
    return sym->type;
  }
  }
  (void)expected;
  return Type::error();
}

TypePtr TypeChecker::checkBinary(BinaryExpr &b, const Type *expected) {
  switch (b.op) {
  case BinaryOp::Add:
  case BinaryOp::Sub:
  case BinaryOp::Mul:
  case BinaryOp::Div:
  case BinaryOp::Rem: {
    const Type *hint = expected && expected->isNumeric() ? expected : nullptr;
    TypePtr lt = checkExpr(b.lhs, hint);
    if (lt->isError())
      return lt;
    TypePtr rt = checkExpr(b.rhs, lt.get());
    if (rt->isError())
      return rt;
    if (!lt->isNumeric())
      return fail(b.loc, std::string("operator '") + binaryOpSpelling(b.op) +
                             "' requires numeric operands, found '" + lt->str() + "'");
    if (!unify(*rt, *lt, b.rhs->loc, std::string("operands of '") + binaryOpSpelling(b.op) + "'"))
      return Type::error();
    return lt;
  }
  case BinaryOp::Eq:
  case BinaryOp::Ne:
  case BinaryOp::Lt:
  case BinaryOp::Le:
  case BinaryOp::Gt:
  case BinaryOp::Ge: {
    TypePtr lt = checkExpr(b.lhs);
    if (lt->isError())
      return lt;
    TypePtr rt = checkExpr(b.rhs, lt.get());
    if (rt->isError())
      return rt;
    bool eqOp = b.op == BinaryOp::Eq || b.op == BinaryOp::Ne;
    bool ptrEq = eqOp && lt->kind == TypeKind::Ptr;
    if (!(lt->isNumeric() || (eqOp && lt->kind == TypeKind::Bool) || ptrEq))
      return fail(b.loc, std::string("operator '") + binaryOpSpelling(b.op) +
                             "' is not defined on type '" + lt->str() +
                             "' (propositional equality of arbitrary types is 'Path')");
    if (!unify(*rt, *lt, b.rhs->loc, std::string("operands of '") + binaryOpSpelling(b.op) + "'"))
      return Type::error();
    return Type::boolean();
  }
  case BinaryOp::BitAnd:
  case BinaryOp::BitOr:
  case BinaryOp::BitXor:
  case BinaryOp::Shl:
  case BinaryOp::Shr: {
    TypePtr lt = checkExpr(b.lhs);
    if (lt->isError())
      return lt;
    TypePtr rt = checkExpr(b.rhs, lt.get());
    if (rt->isError())
      return rt;
    if (!lt->isInteger() || !rt->isInteger())
      return fail(b.loc, std::string("operator '") + binaryOpSpelling(b.op) +
                             "' requires integer operands, found '" + lt->str() + "'");
    if (!unify(*rt, *lt, b.rhs->loc, std::string("operands of '") + binaryOpSpelling(b.op) + "'"))
      return Type::error();
    return lt;
  }
  case BinaryOp::And:
  case BinaryOp::Or: {
    TypePtr lt = checkExpr(b.lhs, Type::boolean().get());
    TypePtr rt = checkExpr(b.rhs, Type::boolean().get());
    if (lt->isError() || rt->isError())
      return Type::error();
    if (lt->kind != TypeKind::Bool || rt->kind != TypeKind::Bool)
      return fail(b.loc, std::string("operator '") + binaryOpSpelling(b.op) +
                             "' requires 'bool' operands");
    return Type::boolean();
  }
  case BinaryOp::Meet:
  case BinaryOp::Join: {
    TypePtr lt = checkExpr(b.lhs, Type::interval().get());
    TypePtr rt = checkExpr(b.rhs, Type::interval().get());
    if (lt->isError() || rt->isError())
      return Type::error();
    if (lt->kind != TypeKind::Interval || rt->kind != TypeKind::Interval)
      return fail(b.loc, std::string("operator '") + binaryOpSpelling(b.op) +
                             "' requires interval ('I') operands");
    return Type::interval();
  }
  }
  return Type::error();
}

TypePtr TypeChecker::checkUnary(UnaryExpr &u, const Type *expected) {
  switch (u.op) {
  case UnaryOp::Neg: {
    TypePtr t = checkExpr(u.operand, expected && expected->isNumeric() ? expected : nullptr);
    if (t->isError())
      return t;
    if (!t->isNumeric())
      return fail(u.loc, "unary '-' requires a numeric operand, found '" + t->str() + "'");
    return t;
  }
  case UnaryOp::Not: {
    TypePtr t = checkExpr(u.operand, Type::boolean().get());
    if (t->isError())
      return t;
    if (t->kind != TypeKind::Bool)
      return fail(u.loc, "'not' requires a 'bool' operand, found '" + t->str() + "'");
    return t;
  }
  case UnaryOp::IntervalRev: {
    TypePtr t = checkExpr(u.operand, expected && (expected->isInteger() || expected->kind == TypeKind::Interval)
                                          ? expected
                                          : nullptr);
    if (t->isError())
      return t;
    if (t->isInteger() || t->kind == TypeKind::Interval)
      return t;
    return fail(u.loc, "'~' requires an integer or interval operand, found '" + t->str() + "'");
  }
  }
  return Type::error();
}

TypePtr TypeChecker::checkCall(ExprPtr &e, const Type *expected) {
  auto &call = as<CallExpr>(*e);

  // Builtins are recognised by name unless shadowed by a user symbol.
  if (call.callee->kind == ExprKind::Ident) {
    auto &callee = as<IdentExpr>(*call.callee);
    if (callee.package.empty()) {
      auto bi = builtinTable().find(callee.name);
      if (bi != builtinTable().end() && !resolveName("", callee.name, callee.loc, false)) {
        call.builtin = bi->second;
        return checkBuiltinCall(call, bi->second, expected);
      }
    }
    Symbol *sym = resolveName(callee.package, callee.name, callee.loc, false);
    if (!sym) {
      TypePtr dispatched;
      if (tryTraitFunction(call, expected, dispatched))
        return dispatched;
      resolveName(callee.package, callee.name, callee.loc, true);
      return Type::error();
    }
    callee.symbol = sym;
    if (sym->kind == SymbolKind::Type && sym->type && sym->type->kind == TypeKind::Struct) {
      call.fn = sym;
      if (!sym->typeBinders.empty()) {
        if (!expected || expected->kind != TypeKind::Struct || expected->inductive != sym ||
            expected->typeArgs.size() != sym->typeBinders.size())
          return fail(call.loc, "generic struct '" + sym->name +
                                    "' needs an expected type, as in 'var p: Pair(i32, i64) = ...'");
        callee.type = sym->type;
        return checkStructCall(call, *expected);
      }
      callee.type = sym->type;
      return checkStructCall(call, *sym->type);
    }
    if (sym->kind == SymbolKind::Function || sym->kind == SymbolKind::Ctor) {
      if (!sym->type || sym->type->isError())
        return Type::error();
      call.fn = sym;
      if (sym->kind == SymbolKind::Ctor && sym->owner && !sym->owner->typeBinders.empty()) {
        if (!expected || expected->kind != TypeKind::Inductive || expected->inductive != sym->owner ||
            expected->typeArgs.size() != sym->owner->typeBinders.size())
          return fail(call.loc, "generic constructor '" + sym->name +
                                    "' needs an expected type, as in 'var x: Option(i32) = None()'");
        TypePtr ctorTy = instantiateBinders(sym->type, sym->owner->typeBinders, expected->typeArgs);
        TypePtr got = checkFnCall(call, *ctorTy, expected);
        if (got->isError())
          return got;
        return instantiateSymbol(*sym->owner, expected->typeArgs, expected->region);
      }
      if (sym->type->kind != TypeKind::Fn) {
        if (sym->kind == SymbolKind::Ctor)
          return fail(call.loc, "constructor '" + sym->name + "' takes no arguments");
        return Type::error();
      }
      callee.type = sym->type;
      TypePtr got = checkFnCall(call, *sym->type, expected);
      if (sym->package == "thread" && sym->name == "spawn")
        noteSpawnShared(call);
      return got;
    }
  }

  TypePtr ct = checkExpr(call.callee);
  if (ct->isError())
    return ct;
  if (ct->kind != TypeKind::Fn)
    return fail(call.loc, "called value has non-function type '" + ct->str() + "'");
  return checkFnCall(call, *ct, expected);
}

TypePtr TypeChecker::checkFnCall(CallExpr &call, const Type &fnType, const Type *expected) {
  (void)expected;
  TypePtr instantiated;
  const Type *use = &fnType;
  if (!fnType.typeParams.empty()) {
    std::vector<TypePtr> args;
    if (call.typeArguments.size() == fnType.typeParams.size()) {
      args = call.typeArguments;
    } else if (call.hasTypeArg) {
      if (call.typeArgs.size() != fnType.typeParams.size())
        return fail(call.loc, "expected " + std::to_string(fnType.typeParams.size()) +
                                  " type argument(s), found " + std::to_string(call.typeArgs.size()));
      for (size_t i = 0; i < call.typeArgs.size(); ++i) {
        TypePtr argTy = resolveType(*call.typeArgs[i]);
        if (argTy->isError())
          return argTy;
        int level = universeOf(*argTy);
        int bound = i < fnType.typeParamLevels.size() ? fnType.typeParamLevels[i] : 0;
        if (level > bound) {
          diags_.error(call.typeArgs[i]->loc, "type '" + argTy->str() + "' lives in Type(" +
                                                   std::to_string(level) + "), above the binder Type(" +
                                                   std::to_string(bound) + ")");
          return Type::error();
        }
        args.push_back(std::move(argTy));
      }
    } else if (fnType.typeParams.size() == 1) {
      const Symbol *var = fnType.typeParams[0];
      bool saved = consume_;
      consume_ = false;
      TypePtr solved;
      for (size_t i = 0; i < call.args.size() && i < fnType.params.size(); ++i) {
        TypePtr at = checkExpr(call.args[i]);
        if (at->isError()) {
          consume_ = saved;
          return at;
        }
        if (TypePtr got = inferTypeArg(*fnType.params[i], at, var)) {
          if (solved && !normalizer_.typeEqual(*solved, *got)) {
            consume_ = saved;
            return fail(call.loc, "type argument '" + var->name +
                                      "' is not determined; write an explicit instantiation");
          }
          solved = got;
        }
      }
      consume_ = saved;
      if (!solved)
        return fail(call.loc, "function '" + var->name +
                                  "' must be instantiated, as in name[i32](...)");
      args.push_back(solved);
    } else {
      return fail(call.loc, "function must be instantiated, as in name[i32, i64](...)");
    }
    call.typeArguments = args;
    if (!args.empty())
      call.typeArgument = args.front();
    instantiated = Type::fn(fnType.params, fnType.paramSymbols, fnType.ret, fnType.paramUse,
                            fnType.effects, fnType.effectVar, fnType.effectsDeclared, nullptr, -1,
                            fnType.varArg);
    instantiated = instantiateBinders(instantiated, fnType.typeParams, args);
    use = instantiated.get();
  } else if (call.hasTypeArg) {
    return fail(call.loc, "type instantiation requires a binder, as in 'fn id[T: Type(0)](...)'");
  }
  if (call.hasEffectInst) {
    if (fnType.effectVar.empty())
      return fail(call.loc, "effect instantiation requires an effect variable, as in 'fn f[e](...) ! e'");
    instantiated = Type::fn(fnType.params, fnType.paramSymbols, fnType.ret, fnType.paramUse,
                            fnType.effects, fnType.effectVar, fnType.effectsDeclared);
    instantiated = instantiateEffects(instantiated, fnType.effectVar, call.effectInst);
    use = instantiated.get();
  }
  if (use->varArg) {
    if (call.args.size() < use->params.size())
      return fail(call.loc, "wrong number of arguments: expected at least " +
                                std::to_string(use->params.size()) + ", found " +
                                std::to_string(call.args.size()));
  } else if (call.args.size() != use->params.size()) {
    return fail(call.loc, "wrong number of arguments: expected " +
                              std::to_string(use->params.size()) + ", found " +
                              std::to_string(call.args.size()));
  }
  // Dependent application: later parameter types and the return type may
  // mention earlier parameters, so substitute as we go.
  std::vector<TypePtr> paramTypes = use->params;
  TypePtr ret = use->ret;
  bool boundary = (call.fn && call.fn->isExtern) ||
                  (use->effectsDeclared && (use->effects & effectIo) != 0);
  bool ok = true;
  for (size_t i = 0; i < call.args.size(); ++i) {
    const bool extra = i >= paramTypes.size();
    const Type *hint = extra ? nullptr : paramTypes[i].get();
    if (!extra && hint && hint->kind == TypeKind::Ref && call.args[i]->kind != ExprKind::Borrow) {
      bool saved = consume_;
      consume_ = false;
      TypePtr probed = checkExpr(call.args[i]);
      consume_ = saved;
      if (!probed->isError() && probed->kind != TypeKind::Ref && hint->elem &&
          normalizer_.typeEqual(*probed, *hint->elem)) {
        auto borrowed = std::make_unique<BorrowExpr>(call.args[i]->loc, hint->refMut,
                                                     std::move(call.args[i]));
        call.args[i] = std::move(borrowed);
      }
    }
    TypePtr at = checkExpr(call.args[i], hint);
    if (at->isError()) {
      ok = false;
      continue;
    }
    if (extra) {
      if (!at->isCCompatible() || at->isStorage() || at->kind == TypeKind::Slice)
        return fail(call.args[i]->loc, "vararg argument must be a C scalar, found '" + at->str() + "'");
    } else if (!unify(*at, *paramTypes[i], call.args[i]->loc, "argument " + std::to_string(i + 1))) {
      ok = false;
    } else if (boundary)
      rejectBoundary(*call.args[i], true, call.args[i]->loc, "extern or io boundary");
    if (i < use->paramSymbols.size()) {
      if (const Symbol *ps = use->paramSymbols[i]) {
        for (size_t j = i + 1; j < paramTypes.size(); ++j)
          paramTypes[j] = normalizer_.substituteType(paramTypes[j], ps, *call.args[i]);
        ret = normalizer_.substituteType(ret, ps, *call.args[i]);
      }
    }
  }
  if (ok)
    noteEffect(*use, call.fn, call.loc);
  if (call.along)
    checkAlong(call, *use);
  return ok && ret ? ret : Type::error();
}

TypePtr TypeChecker::checkBuiltinCall(CallExpr &call, Builtin builtin, const Type *expected) {
  auto arity = [&](size_t n, const char *name) {
    if (call.args.size() != n) {
      fail(call.loc, std::string("'") + name + "' expects " + std::to_string(n) +
                         " argument(s), found " + std::to_string(call.args.size()));
      return false;
    }
    return true;
  };

  switch (builtin) {
  case Builtin::Print: {
    if (!arity(1, "print"))
      return Type::error();
    TypePtr t = checkExpr(call.args[0]);
    if (t->isError())
      return t;
    rejectBoundary(*call.args[0], true, call.loc, "io boundary");
    Type io;
    io.kind = TypeKind::Fn;
    io.effects = effectIo;
    io.effectsDeclared = true;
    noteEffect(io, nullptr, call.loc);
    if (!t->isNumeric() && t->kind != TypeKind::Bool)
      return fail(call.loc, "'print' supports integers, floats and bool, found '" + t->str() + "'");
    return Type::voidTy();
  }
  case Builtin::Refl: {
    if (!arity(1, "refl"))
      return Type::error();
    const Type *hint = expected && expected->kind == TypeKind::Path ? expected->elem.get() : nullptr;
    bool savedConsume = consume_;
    consume_ = false;
    TypePtr t = checkExpr(call.args[0], hint);
    consume_ = savedConsume;
    if (t->isError())
      return t;
    return Type::path(t, call.args[0]->clone(), call.args[0]->clone());
  }
  case Builtin::Sym: {
    if (!arity(1, "sym"))
      return Type::error();
    bool savedConsume = consume_;
    consume_ = false;
    TypePtr t = checkExpr(call.args[0], expected);
    consume_ = savedConsume;
    if (t->isError())
      return t;
    if (t->kind != TypeKind::Path)
      return fail(call.loc, "'sym' expects a path, found '" + t->str() + "'");
    return Type::path(t->elem, t->rhs->clone(), t->lhs->clone());
  }
  case Builtin::Ua: {
    if (!arity(1, "ua"))
      return Type::error();
    bool savedConsume = consume_;
    consume_ = false;
    TypePtr t = checkExpr(call.args[0]);
    consume_ = savedConsume;
    if (t->isError())
      return t;
    if (t->kind != TypeKind::Equiv)
      return fail(call.loc, "'ua' expects an equivalence 'Equiv(A, B)', found '" + t->str() + "'");
    return Type::path(Type::universe(), makeTypeLit(t->a, call.loc), makeTypeLit(t->b, call.loc));
  }
  case Builtin::Transport: {
    if (!arity(2, "transport"))
      return Type::error();
    return resolveTransport(call);
  }
  case Builtin::Iso: {
    if (!arity(4, "iso"))
      return Type::error();
    TypePtr ft = checkExpr(call.args[0]);
    TypePtr gt = checkExpr(call.args[1]);
    if (ft->isError() || gt->isError())
      return Type::error();
    auto unaryFn = [&](const Type &t, const char *role) -> bool {
      if (t.kind != TypeKind::Fn || t.params.size() != 1) {
        fail(call.loc, std::string("'iso': ") + role + " must be a unary function, found '" +
                           t.str() + "'");
        return false;
      }
      return true;
    };
    if (!unaryFn(*ft, "forward map") || !unaryFn(*gt, "backward map"))
      return Type::error();
    TypePtr A = ft->params[0], B = ft->ret;
    if (!unify(*gt->params[0], *B, call.args[1]->loc, "'iso' backward map domain") ||
        !unify(*gt->ret, *A, call.args[1]->loc, "'iso' backward map codomain"))
      return Type::error();

    // sec : (b: B) -> Path(B, f(g(b)), b)      retr : (a: A) -> Path(A, g(f(a)), a)
    auto checkHomotopy = [&](ExprPtr &proof, const TypePtr &dom, const Expr &outer,
                             const Expr &inner, const char *role) -> bool {
      TypePtr pt = checkExpr(proof);
      if (pt->isError())
        return false;
      if (pt->kind != TypeKind::Fn || pt->params.size() != 1 || !pt->paramSymbols[0] ||
          pt->ret->kind != TypeKind::Path) {
        fail(proof->loc, std::string("'iso': ") + role +
                             " must be a declared function returning a path, found '" +
                             pt->str() + "'");
        return false;
      }
      if (!unify(*pt->params[0], *dom, proof->loc, std::string("'iso' ") + role + " domain"))
        return false;
      const Symbol *x = pt->paramSymbols[0];
      ExprPtr xRef = Normalizer::identOf(x, proof->loc);
      std::vector<ExprPtr> innerArgs;
      innerArgs.push_back(xRef->clone());
      auto innerCall = std::make_unique<CallExpr>(proof->loc, inner.clone(), std::move(innerArgs));
      if (inner.kind == ExprKind::Ident)
        innerCall->fn = as<IdentExpr>(inner).symbol;
      innerCall->type = inner.type ? inner.type->ret : nullptr;
      std::vector<ExprPtr> outerArgs;
      outerArgs.push_back(std::move(innerCall));
      auto outerCall = std::make_unique<CallExpr>(proof->loc, outer.clone(), std::move(outerArgs));
      if (outer.kind == ExprKind::Ident)
        outerCall->fn = as<IdentExpr>(outer).symbol;
      outerCall->type = dom;
      TypePtr want = Type::path(dom, std::move(outerCall), std::move(xRef));
      return unify(*pt->ret, *want, proof->loc, std::string("'iso' ") + role);
    };
    bool okSec = checkHomotopy(call.args[2], B, *call.args[0], *call.args[1], "section");
    bool okRetr = checkHomotopy(call.args[3], A, *call.args[1], *call.args[0], "retraction");
    if (!okSec || !okRetr)
      return Type::error();
    return Type::equiv(A, B);
  }
  case Builtin::NullPtr: {
    if (!arity(0, "null"))
      return Type::error();
    return Type::ptr();
  }
  case Builtin::AllocString: {
    if (!arity(1, "alloc_string"))
      return Type::error();
    TypePtr n = checkExpr(call.args[0], Type::i64().get());
    if (n->isError())
      return n;
    if (n->kind != TypeKind::I64)
      return fail(call.args[0]->loc, "'alloc_string' length must be 'i64', found '" + n->str() + "'");
    Type alloc;
    alloc.kind = TypeKind::Fn;
    alloc.effects = effectAlloc;
    alloc.effectsDeclared = true;
    noteEffect(alloc, nullptr, call.loc);
    // Heap, not the enclosing arena: the buffer must outlive the caller.
    return Type::stringTy(0);
  }
  case Builtin::Seal: {
    if (!arity(1, "seal"))
      return Type::error();
    TypePtr t = checkExpr(call.args[0]);
    if (t->isError())
      return t;
    return Type::labeled(t, Secrecy::Secret);
  }
  case Builtin::Reveal: {
    if (!arity(2, "reveal"))
      return Type::error();
    TypePtr t = checkExpr(call.args[0]);
    if (t->isError())
      return t;
    if (t->kind != TypeKind::Labeled || !t->elem)
      return fail(call.args[0]->loc, "'reveal' expects a Secret or Private value, found '" + t->str() +
                                          "'");
    bool savedConsume = consume_;
    consume_ = false;
    TypePtr proof = checkExpr(call.args[1]);
    consume_ = savedConsume;
    if (proof->isError())
      return proof;
    if (proof->kind != TypeKind::Leak || proof->secrecy != t->secrecy ||
        proof->secrecyTo != Secrecy::Public) {
      diags_.error(call.args[1]->loc, "PathObstructionError: no path from " + t->str() + " to " +
                                           t->elem->str());
      return Type::error();
    }
    return t->elem;
  }
  case Builtin::Quot: {
    if (!arity(2, "quot"))
      return Type::error();
    if (!expected || expected->kind != TypeKind::Quotient || !expected->elem)
      return fail(call.loc, "'quot' needs an expected Quotient type; annotate the binding");
    TypePtr xt = checkExpr(call.args[0], expected->elem.get());
    if (xt->isError() || !unify(*xt, *expected->elem, call.args[0]->loc, "'quot' carrier"))
      return Type::error();
    bool savedConsume = consume_;
    consume_ = false;
    TypePtr proof = checkExpr(call.args[1]);
    consume_ = savedConsume;
    if (proof->isError())
      return proof;
    if (!proof->isErased())
      return fail(call.args[1]->loc, "'quot' proof must be an erased term, found '" + proof->str() + "'");
    if (!expected->canonFn)
      return fail(call.loc, "quotient has no canon function");
    call.quotCanon = expected->canonFn;
    return Type::quotient(expected->elem, expected->lhs ? expected->lhs->clone() : nullptr,
                          expected->rhs ? expected->rhs->clone() : nullptr, expected->canonFn);
  }
  case Builtin::Assert: {
    if (!arity(1, "assert"))
      return Type::error();
    TypePtr c = checkExpr(call.args[0], Type::boolean().get());
    if (c->isError())
      return c;
    if (c->kind != TypeKind::Bool)
      return fail(call.args[0]->loc, "'assert' expects 'bool', found '" + c->str() + "'");
    return Type::voidTy();
  }
  case Builtin::Trap: {
    if (!arity(1, "trap"))
      return Type::error();
    TypePtr m = checkExpr(call.args[0]);
    if (m->isError())
      return m;
    if (call.args[0]->kind != ExprKind::StringLit)
      return fail(call.args[0]->loc, "'trap' expects a string literal");
    return Type::voidTy();
  }
  case Builtin::CmpXchg:
  case Builtin::AtomicRmw:
  case Builtin::AtomicLoad:
  case Builtin::AtomicStore: {
    const char *name = builtin == Builtin::CmpXchg     ? "cmpxchg"
                       : builtin == Builtin::AtomicRmw  ? "atomicrmw"
                       : builtin == Builtin::AtomicLoad ? "atomic_load"
                                                        : "atomic_store";
    // cmpxchg(&p, expected, desired, success, failure)
    // atomicrmw(op, &p, value, order)
    // atomic_load(&p, order)
    // atomic_store(&p, value, order)
    const size_t expectN = builtin == Builtin::CmpXchg     ? 5
                           : builtin == Builtin::AtomicRmw  ? 4
                           : builtin == Builtin::AtomicLoad ? 2
                                                            : 3;
    if (!arity(expectN, name))
      return Type::error();
    requireUnsafe(call.loc, name);
    const size_t placeAt = builtin == Builtin::AtomicRmw ? 1 : 0;
    const size_t orderAt = call.args.size() - 1;
    TypePtr placeTy = checkExpr(call.args[placeAt]);
    if (!placeTy || placeTy->isError())
      return Type::error();
    const Type *elem = placeTy->kind == TypeKind::Ref ? placeTy->elem.get() : nullptr;
    if (!elem || !atomicWidth(*elem))
      return fail(call.args[placeAt]->loc,
                  "atomic operation needs a reference to an 8, 16, 32, or 64 bit integer");
    auto takeOrder = [&](size_t index, const char *what) -> std::string {
      TypePtr ot = checkExpr(call.args[index]);
      if (!ot || ot->isError())
        return {};
      const Symbol *ctor = nullaryCtor(*call.args[index]);
      if (!ctor || ctor->owner->name != "Order" || orderRank(ctor->name) == 0) {
        fail(call.args[index]->loc,
             std::string(what) + " must be an Order constructor, as in SeqCst");
        return {};
      }
      return ctor->name;
    };
    if (builtin == Builtin::AtomicRmw) {
      TypePtr opTy = checkExpr(call.args[0]);
      if (!opTy || opTy->isError())
        return Type::error();
      const Symbol *op = nullaryCtor(*call.args[0]);
      if (!op || op->owner->name != "Rmw" ||
          (op->name != "Xchg" && op->name != "Add" && op->name != "Sub" && op->name != "And" &&
           op->name != "Or" && op->name != "Xor"))
        return fail(call.args[0]->loc, "rmw operation must be an Rmw constructor, as in Add");
      TypePtr val = checkExpr(call.args[2], elem);
      if (!val || val->isError())
        return Type::error();
      if (!unify(*val, *elem, call.args[2]->loc, "atomicrmw value"))
        return Type::error();
    } else if (builtin == Builtin::CmpXchg) {
      TypePtr expectedV = checkExpr(call.args[1], elem);
      TypePtr desired = checkExpr(call.args[2], elem);
      if (!expectedV || expectedV->isError() || !desired || desired->isError())
        return Type::error();
      if (!unify(*expectedV, *elem, call.args[1]->loc, "cmpxchg expected") ||
          !unify(*desired, *elem, call.args[2]->loc, "cmpxchg desired"))
        return Type::error();
      std::string success = takeOrder(3, "cmpxchg success order");
      std::string failure = takeOrder(4, "cmpxchg failure order");
      if (success.empty() || failure.empty())
        return Type::error();
      if (failure == "Release" || failure == "AcqRel" || orderRank(failure) > orderRank(success))
        return fail(call.args[4]->loc,
                    "cmpxchg failure order cannot be Release, AcqRel, or stronger than the success order");
      return Type::boolean();
    } else if (builtin == Builtin::AtomicStore) {
      TypePtr val = checkExpr(call.args[1], elem);
      if (!val || val->isError())
        return Type::error();
      if (!unify(*val, *elem, call.args[1]->loc, "atomic_store value"))
        return Type::error();
    }
    std::string order = takeOrder(orderAt, name);
    if (order.empty())
      return Type::error();
    if (builtin == Builtin::AtomicLoad && order != "Relaxed" && order != "Acquire" && order != "SeqCst")
      return fail(call.args[orderAt]->loc, "atomic_load order must be Relaxed, Acquire, or SeqCst");
    if (builtin == Builtin::AtomicStore && order != "Relaxed" && order != "Release" && order != "SeqCst")
      return fail(call.args[orderAt]->loc, "atomic_store order must be Relaxed, Release, or SeqCst");
    if (builtin == Builtin::AtomicRmw && orderRank(order) == 0)
      return Type::error();
    if (builtin == Builtin::AtomicStore)
      return Type::voidTy();
    return elem->kind == TypeKind::I8    ? Type::i8()
           : elem->kind == TypeKind::I16 ? Type::i16()
           : elem->kind == TypeKind::I32 ? Type::i32()
           : elem->kind == TypeKind::I64 ? Type::i64()
           : elem->kind == TypeKind::U8  ? Type::u8()
           : elem->kind == TypeKind::U16 ? Type::u16()
           : elem->kind == TypeKind::U32 ? Type::u32()
           : elem->kind == TypeKind::U64 ? Type::u64()
           : elem->kind == TypeKind::USize ? Type::usize()
                                         : Type::isize();
  }
  case Builtin::Simd: {
    requireUnsafe(call.loc, "simd");
    if (!expected || expected->kind != TypeKind::Simd || !expected->elem)
      return fail(call.loc, "'simd' needs an expected type, as in 'var v: Simd(i32, 4) = simd(1, 2, 3, 4)'");
    if (call.args.size() != static_cast<size_t>(expected->lanes))
      return fail(call.loc, "'simd' expects " + std::to_string(expected->lanes) + " lane(s), found " +
                                std::to_string(call.args.size()));
    for (auto &arg : call.args) {
      TypePtr at = checkExpr(arg, expected->elem.get());
      if (!at || at->isError())
        return Type::error();
      if (!unify(*at, *expected->elem, arg->loc, "simd lane"))
        return Type::error();
    }
    return Type::simd(expected->elem, expected->lanes);
  }
  case Builtin::SimdAdd: {
    requireUnsafe(call.loc, "simd_add");
    if (!arity(2, "simd_add"))
      return Type::error();
    TypePtr lhs = checkExpr(call.args[0]);
    if (!lhs || lhs->isError())
      return Type::error();
    if (lhs->kind != TypeKind::Simd || !lhs->elem)
      return fail(call.args[0]->loc, "'simd_add' expects a Simd value, found '" + lhs->str() + "'");
    TypePtr rhs = checkExpr(call.args[1], lhs.get());
    if (!rhs || rhs->isError())
      return Type::error();
    if (!unify(*rhs, *lhs, call.args[1]->loc, "simd_add"))
      return Type::error();
    return Type::simd(lhs->elem, lhs->lanes);
  }
  case Builtin::SimdExtract: {
    requireUnsafe(call.loc, "simd_extract");
    if (!arity(2, "simd_extract"))
      return Type::error();
    TypePtr vec = checkExpr(call.args[0]);
    if (!vec || vec->isError())
      return Type::error();
    if (vec->kind != TypeKind::Simd || !vec->elem)
      return fail(call.args[0]->loc, "'simd_extract' expects a Simd value, found '" + vec->str() + "'");
    TypePtr idx = checkExpr(call.args[1], Type::i32().get());
    if (!idx || idx->isError())
      return Type::error();
    if (call.args[1]->kind != ExprKind::IntLit)
      return fail(call.args[1]->loc, "simd_extract index must be a constant");
    const auto lane = as<IntLitExpr>(*call.args[1]).value;
    if (lane < 0 || lane >= vec->lanes)
      return fail(call.args[1]->loc, "simd_extract index is outside the vector");
    return vec->elem;
  }
  case Builtin::Asm: {
    requireUnsafe(call.loc, "asm");
    if (!arity(1, "asm"))
      return Type::error();
    TypePtr m = checkExpr(call.args[0]);
    if (!m || m->isError())
      return Type::error();
    if (call.args[0]->kind != ExprKind::StringLit)
      return fail(call.args[0]->loc, "'asm' expects a string literal");
    return Type::voidTy();
  }
  case Builtin::Text: {
    if (!arity(1, "text"))
      return Type::error();
    TypePtr t = checkExpr(call.args[0]);
    if (t->isError())
      return t;
    if (call.args[0]->kind != ExprKind::StringLit)
      return fail(call.args[0]->loc, "'text' expects a string literal");
    return Type::text();
  }
  case Builtin::RawSlice: {
    if (!arity(2, "raw_slice"))
      return Type::error();
    requireUnsafe(call.loc, "integer-to-pointer conversion 'raw_slice'");
    TypePtr addr = checkExpr(call.args[0], Type::i64().get());
    TypePtr len = checkExpr(call.args[1], Type::i64().get());
    if (addr->isError() || len->isError())
      return Type::error();
    if (addr->kind != TypeKind::I64)
      return fail(call.args[0]->loc, "'raw_slice' address must be 'i64', found '" + addr->str() + "'");
    if (len->kind != TypeKind::I64)
      return fail(call.args[1]->loc, "'raw_slice' length must be 'i64', found '" + len->str() + "'");
    if (!expected || expected->kind != TypeKind::Slice || !expected->elem)
      return fail(call.loc, "'raw_slice' needs an expected Slice(T); annotate the binding or return");
    if (!expected->elem->isCCompatible())
      return fail(call.loc, "'raw_slice' element type '" + expected->elem->str() +
                                "' is not C-ABI compatible");
    return Type::slice(expected->elem, currentRegion_);
  }
  case Builtin::None:
    break;
  }
  return Type::error();
}

TypePtr TypeChecker::resolveTransport(CallExpr &call) {
  TypePtr pt = checkExpr(call.args[0]);
  if (pt->isError())
    return pt;
  if (pt->kind != TypeKind::Path || pt->elem->kind != TypeKind::Universe)
    return fail(call.args[0]->loc,
                "'transport' expects a path between types 'Path(Type, A, B)', found '" +
                    pt->str() + "'");
  TypePtr A = typeOfTypeTerm(*pt->lhs);
  TypePtr B = typeOfTypeTerm(*pt->rhs);
  if (!A || !B)
    return fail(call.args[0]->loc, "'transport': endpoints of the type path must be concrete types");
  TypePtr at = checkExpr(call.args[1], A.get());
  if (at->isError())
    return at;
  if (!unify(*at, *A, call.args[1]->loc, "'transport' payload"))
    return Type::error();

  // Decide how the transport computes. Univalence is computational: the path
  // must normalise to refl, ua(iso(..)) or sym(ua(iso(..))).
  ExprPtr nf = normalizer_.normalize(*call.args[0]);
  auto isBuiltin = [](const Expr &e, Builtin b) {
    return e.kind == ExprKind::Call && as<CallExpr>(e).builtin == b;
  };
  if (isBuiltin(*nf, Builtin::Refl)) {
    call.transportDir = TransportDir::Identity;
    return B;
  }
  const Expr *ua = nullptr;
  bool reversed = false;
  if (isBuiltin(*nf, Builtin::Ua)) {
    ua = nf.get();
  } else if (isBuiltin(*nf, Builtin::Sym) && isBuiltin(*as<CallExpr>(*nf).args[0], Builtin::Ua)) {
    ua = as<CallExpr>(*nf).args[0].get();
    reversed = true;
  }
  auto blocksTransport = [&]() {
    bool labeled = A->kind == TypeKind::Labeled;
    bool resource = labeled || A->isStorage() || call.args[1]->flow == Usage::Linear ||
                    call.args[1]->flow == Usage::Affine;
    if (!resource)
      return false;
    if (labeled)
      diags_.error(call.loc, "PathObstructionError: no path from " + A->str() + " to " + B->str());
    else
      diags_.error(call.loc, "linear resource cannot be transported along a non-refl path");
    return true;
  };
  if (ua) {
    const Expr &eq = *as<CallExpr>(*ua).args[0];
    if (isBuiltin(eq, Builtin::Iso)) {
      const Expr &map = *as<CallExpr>(eq).args[reversed ? 1 : 0];
      if (map.kind == ExprKind::Ident && as<IdentExpr>(map).symbol &&
          as<IdentExpr>(map).symbol->kind == SymbolKind::Function) {
        if (blocksTransport())
          return Type::error();
        call.transportDir = reversed ? TransportDir::Backward : TransportDir::Forward;
        call.transportFn = as<IdentExpr>(map).symbol;
        return B;
      }
    }
  }
  return fail(call.loc, "'transport' along a non-canonical type path cannot be computed; the "
                        "path normalises to '" +
                            toString(*nf) + "' (expected refl, ua(iso(..)) or sym(ua(iso(..))))");
}

TypePtr TypeChecker::checkPathAbs(PathAbsExpr &p, const Type *expected) {
  Symbol bs;
  bs.kind = SymbolKind::IntervalVar;
  bs.name = p.var;
  bs.type = Type::interval();
  bs.loc = p.loc;
  pushScope();
  Symbol *binder = declareLocal(std::move(bs));
  p.binder = binder;
  const Type *hint = expected && expected->kind == TypeKind::Path ? expected->elem.get() : nullptr;
  bool savedConsume = consume_;
  consume_ = false;
  TypePtr bt = checkExpr(p.body, hint);
  consume_ = savedConsume;
  popScope();
  if (bt->isError())
    return bt;
  if (bt->kind == TypeKind::Interval)
    return fail(p.loc, "a path abstraction cannot produce an interval value");
  auto i0 = std::make_unique<IntervalLitExpr>(p.loc, false);
  i0->type = Type::interval();
  auto i1 = std::make_unique<IntervalLitExpr>(p.loc, true);
  i1->type = Type::interval();
  ExprPtr lhs = normalizer_.substitute(*p.body, binder, *i0);
  ExprPtr rhs = normalizer_.substitute(*p.body, binder, *i1);
  return Type::path(bt, std::move(lhs), std::move(rhs));
}

TypePtr TypeChecker::checkPathApp(PathAppExpr &a) {
  TypePtr pt = checkExpr(a.path);
  TypePtr rt = checkExpr(a.arg, Type::interval().get());
  if (pt->isError() || rt->isError())
    return Type::error();
  if (pt->kind != TypeKind::Path)
    return fail(a.loc, "'@' applies a path to an interval point, but found '" + pt->str() + "'");
  if (rt->kind != TypeKind::Interval)
    return fail(a.arg->loc, "path argument must be an interval term, found '" + rt->str() + "'");
  return pt->elem;
}

TypePtr TypeChecker::checkHcomp(HcompExpr &h) {
  TypePtr T = resolveType(*h.typeExpr);
  if (T->isError())
    return T;
  TypePtr bt = checkExpr(h.base, T.get());
  if (bt->isError() || !unify(*bt, *T, h.base->loc, "'hcomp' base"))
    return Type::error();

  struct Restriction {
    std::vector<std::pair<const Symbol *, bool>> atoms;
  };
  std::vector<Restriction> restrictions;
  bool ok = true;

  for (Face &f : h.faces) {
    Restriction r;
    for (const Face::Atom &atom : f.constraints) {
      Symbol *v = lookupLocal(atom.var);
      if (!v || v->kind != SymbolKind::IntervalVar) {
        diags_.error(f.loc, "face constraint mentions '" + atom.var +
                                "' which is not an interval variable in scope");
        ok = false;
        continue;
      }
      r.atoms.emplace_back(v, atom.one);
    }
    TypePtr st = checkExpr(f.side);
    if (st->isError()) {
      ok = false;
      continue;
    }
    if (st->kind != TypeKind::Path || !normalizer_.typeEqual(*st->elem, *T)) {
      diags_.error(f.side->loc, "partial element must be a path in '" + T->str() + "', found '" +
                                    st->str() + "'");
      ok = false;
      continue;
    }
    restrictions.push_back(std::move(r));
  }
  if (!ok)
    return Type::error();

  auto restrict = [&](const Expr &e, const Restriction &r) {
    ExprPtr out = e.clone();
    for (const auto &[sym, one] : r.atoms) {
      auto lit = std::make_unique<IntervalLitExpr>(e.loc, one);
      lit->type = Type::interval();
      out = normalizer_.substitute(*out, sym, *lit);
    }
    return out;
  };
  auto consistent = [](const Restriction &a, const Restriction &b) {
    for (const auto &[sa, va] : a.atoms)
      for (const auto &[sb, vb] : b.atoms)
        if (sa == sb && va != vb)
          return false;
    return true;
  };
  auto sideAt = [&](const Expr &side, bool one) {
    auto lit = std::make_unique<IntervalLitExpr>(side.loc, one);
    lit->type = Type::interval();
    auto app = std::make_unique<PathAppExpr>(side.loc, side.clone(), std::move(lit));
    app->type = T;
    return app;
  };

  // (1) Each side must agree with the base at the bottom of the composition.
  for (size_t i = 0; i < h.faces.size(); ++i) {
    ExprPtr side0 = sideAt(*restrict(*h.faces[i].side, restrictions[i]), false);
    ExprPtr base = restrict(*h.base, restrictions[i]);
    if (!normalizer_.equal(*side0, *base)) {
      diags_.error(h.faces[i].loc, "hcomp face does not agree with the base on its boundary: '" +
                                       toString(*normalizer_.normalize(*side0)) + "' vs '" +
                                       toString(*normalizer_.normalize(*base)) + "'");
      ok = false;
    }
  }
  // (2) Overlapping faces must agree where both constraints hold.
  for (size_t i = 0; i < h.faces.size(); ++i) {
    for (size_t j = i + 1; j < h.faces.size(); ++j) {
      if (!consistent(restrictions[i], restrictions[j]))
        continue;
      Restriction both = restrictions[i];
      both.atoms.insert(both.atoms.end(), restrictions[j].atoms.begin(), restrictions[j].atoms.end());
      ExprPtr si = restrict(*h.faces[i].side, both);
      ExprPtr sj = restrict(*h.faces[j].side, both);
      if (!normalizer_.equal(*si, *sj)) {
        diags_.error(h.faces[j].loc, "hcomp faces overlap but disagree on their intersection");
        ok = false;
      }
    }
  }
  return ok ? T : Type::error();
}

TypePtr TypeChecker::checkMatch(MatchExpr &m, const Type *expected) {
  TypePtr st = checkExpr(m.scrutinee);
  if (st->isError())
    return st;
  if (st->kind != TypeKind::Inductive || !st->inductive)
    return fail(m.scrutinee->loc, "'match' requires a value of an inductive type, found '" +
                                      st->str() + "'");
  const Symbol &typeSym = *st->inductive;

  TypePtr result;
  std::set<const Symbol *> covered;
  bool ok = true;
  for (MatchCase &c : m.cases) {
    const Symbol *ctor = nullptr;
    for (const Symbol *cand : typeSym.ctors)
      if (cand->name == c.ctor)
        ctor = cand;
    if (!ctor) {
      diags_.error(c.loc, "'" + c.ctor + "' is not a constructor of '" + typeSym.qualifiedName() + "'");
      ok = false;
      continue;
    }
    if (ctor->isPathCtor) {
      diags_.error(c.loc, "'" + c.ctor +
                              "' is a path constructor; it has no runtime representation and is "
                              "eliminated by the coherence check instead of a 'case'");
      ok = false;
      continue;
    }
    if (!covered.insert(ctor).second) {
      diags_.error(c.loc, "duplicate 'case' for constructor '" + c.ctor + "'");
      ok = false;
      continue;
    }
    c.ctorSymbol = ctor;
    size_t arity = ctor->type->kind == TypeKind::Fn ? ctor->type->params.size() : 0;
    if (c.binders.size() != arity) {
      diags_.error(c.loc, "constructor '" + c.ctor + "' has " + std::to_string(arity) +
                              " field(s) but the pattern binds " + std::to_string(c.binders.size()));
      ok = false;
      continue;
    }
    pushScope();
    for (size_t i = 0; i < arity; ++i) {
      Symbol ps;
      ps.kind = SymbolKind::PatternVar;
      ps.name = c.binders[i];
      ps.type = ctor->type->params[i];
      if (ps.type && !st->typeArgs.empty())
        ps.type = instantiateBinders(ps.type, typeSym.typeBinders, st->typeArgs);
      ps.loc = c.loc;
      c.binderSymbols.push_back(declareLocal(std::move(ps)));
    }
    TypePtr bt = checkExpr(c.body, result ? result.get() : expected);
    popScope();
    if (bt->isError()) {
      ok = false;
      continue;
    }
    if (!result)
      result = bt;
    else if (!unify(*bt, *result, c.body->loc, "'match' branches"))
      ok = false;
  }
  if (!ok)
    return Type::error();

  for (const Symbol *ctor : typeSym.ctors)
    if (!ctor->isPathCtor && !covered.count(ctor)) {
      diags_.error(m.loc, "'match' is not exhaustive: missing case for constructor '" +
                              ctor->name + "'");
      ok = false;
    }
  if (!ok)
    return Type::error();
  if (!result)
    return fail(m.loc, "'match' must have at least one case");

  m.type = result;
  checkMatchRespectsPaths(m, typeSym);
  return result;
}

/// For every path constructor `p : Path(T, C1(args), C2(args'))` the
/// eliminator must map both endpoints to definitionally equal results; this
/// is the computation rule an eliminator of a HIT has to satisfy on paths
/// (here for non-dependent elimination into a set).
void TypeChecker::checkMatchRespectsPaths(MatchExpr &m, const Symbol &typeSym) {
  auto branchFor = [&](const Expr &endpoint) -> ExprPtr {
    ExprPtr nf = normalizer_.normalize(endpoint);
    if (nf->kind != ExprKind::Call || !as<CallExpr>(*nf).fn ||
        as<CallExpr>(*nf).fn->kind != SymbolKind::Ctor)
      return nullptr;
    const auto &call = as<CallExpr>(*nf);
    for (const MatchCase &c : m.cases) {
      if (c.ctorSymbol != call.fn)
        continue;
      ExprPtr body = c.body->clone();
      for (size_t i = 0; i < c.binderSymbols.size() && i < call.args.size(); ++i)
        body = normalizer_.substitute(*body, c.binderSymbols[i], *call.args[i]);
      return body;
    }
    return nullptr;
  };

  for (const Symbol *ctor : typeSym.ctors) {
    if (!ctor->isPathCtor || !ctor->type)
      continue;
    const Type *pathTy = ctor->type->kind == TypeKind::Fn ? ctor->type->ret.get() : ctor->type.get();
    if (!pathTy || pathTy->kind != TypeKind::Path)
      continue;
    ExprPtr lhsBranch = branchFor(*pathTy->lhs);
    ExprPtr rhsBranch = branchFor(*pathTy->rhs);
    if (!lhsBranch || !rhsBranch) {
      diags_.error(m.loc, "cannot verify that this 'match' respects path constructor '" +
                              ctor->name + "': its endpoints are not constructor applications");
      continue;
    }
    if (!normalizer_.equal(*lhsBranch, *rhsBranch)) {
      diags_.error(m.loc, "'match' does not respect the path constructor '" + ctor->name +
                              "' of '" + typeSym.qualifiedName() + "': the branches evaluate to '" +
                              toString(*normalizer_.normalize(*lhsBranch)) + "' and '" +
                              toString(*normalizer_.normalize(*rhsBranch)) +
                              "' on its endpoints, which are not definitionally equal");
      diags_.note(ctor->loc, "path constructor '" + ctor->name + "' is declared here");
    }
  }
}

// ---------------------------------------------------------------------------
// Regions, lifetimes, collections
// ---------------------------------------------------------------------------

namespace {

bool isReflTerm(const Expr &e) {
  return e.kind == ExprKind::Call && as<CallExpr>(e).builtin == Builtin::Refl;
}

bool capturesLocal(const Expr &e, const std::vector<const Symbol *> &params) {
  auto forbidden = [&](const Symbol *s) {
    if (!s || (s->kind != SymbolKind::Local && s->kind != SymbolKind::Param))
      return false;
    for (const Symbol *p : params)
      if (p == s)
        return false;
    return true;
  };
  std::function<bool(const Expr &)> walk = [&](const Expr &x) {
    switch (x.kind) {
    case ExprKind::Ident:
      return forbidden(as<IdentExpr>(x).symbol);
    case ExprKind::Binary:
      return walk(*as<BinaryExpr>(x).lhs) || walk(*as<BinaryExpr>(x).rhs);
    case ExprKind::Unary:
      return walk(*as<UnaryExpr>(x).operand);
    case ExprKind::Call: {
      const auto &c = as<CallExpr>(x);
      if (walk(*c.callee))
        return true;
      for (const auto &a : c.args)
        if (walk(*a))
          return true;
      return false;
    }
    case ExprKind::ArrayLit:
      for (const auto &el : as<ArrayLitExpr>(x).elems)
        if (walk(*el))
          return true;
      return false;
    case ExprKind::Lambda:
      return walk(*as<LambdaExpr>(x).body);
    case ExprKind::Method: {
      const auto &m = as<MethodCallExpr>(x);
      if (walk(*m.receiver))
        return true;
      for (const auto &a : m.args)
        if (walk(*a))
          return true;
      return false;
    }
    case ExprKind::PathAbs:
      return walk(*as<PathAbsExpr>(x).body);
    case ExprKind::PathApp:
      return walk(*as<PathAppExpr>(x).path) || walk(*as<PathAppExpr>(x).arg);
    default:
      return false;
    }
  };
  return walk(e);
}

} // namespace

ExprPtr TypeChecker::lifetimeWitness(bool constant, SourceLocation loc) {
  Symbol iv;
  iv.kind = SymbolKind::IntervalVar;
  iv.name = constant ? "heap" : "arena";
  iv.type = Type::interval();
  iv.loc = loc;
  Symbol *binder = registry_.create(std::move(iv));
  ExprPtr body;
  if (constant) {
    auto lit = std::make_unique<IntervalLitExpr>(loc, false);
    lit->type = Type::interval();
    body = std::move(lit);
  } else {
    body = Normalizer::identOf(binder, loc);
  }
  auto abs = std::make_unique<PathAbsExpr>(loc, binder->name, std::move(body));
  abs->binder = binder;
  abs->type = Type::interval();
  return abs;
}

void TypeChecker::initRegions() {
  regions_.clear();
  currentRegion_ = 0;
  Region root;
  root.id = 0;
  root.parent = -1;
  root.arena = false;
  root.witness = lifetimeWitness(true, {});
  regions_.push_back(std::move(root));
}

int TypeChecker::pushRegion(bool arena, SourceLocation loc) {
  Region r;
  r.id = static_cast<int>(regions_.size());
  r.parent = currentRegion_;
  r.arena = arena;
  r.loc = loc;
  // Arena lifetimes depend on the scope interval, so the witness mentions its
  // dimension and does not normalise to refl. Heap / non-arena scopes are
  // constant paths: transport along them is the identity.
  r.witness = lifetimeWitness(!arena, loc);
  regions_.push_back(std::move(r));
  currentRegion_ = r.id;
  return currentRegion_;
}

void TypeChecker::popRegion() {
  if (currentRegion_ > 0)
    currentRegion_ = regions_[static_cast<size_t>(currentRegion_)].parent;
}

bool TypeChecker::regionOutlives(int valueRegion, int destRegion) {
  if (valueRegion < 0 || valueRegion >= static_cast<int>(regions_.size()))
    return false;
  if (valueRegion == destRegion)
    return true;
  if (valueRegion == 0) {
    // The heap region is a constant path. Transport along refl is the identity,
    // so a heap value is valid in every nested scope.
    ExprPtr nf = normalizer_.normalize(*regions_[0].witness);
    return isReflTerm(*nf);
  }
  for (int r = destRegion; r > 0 && r < static_cast<int>(regions_.size());
       r = regions_[static_cast<size_t>(r)].parent) {
    if (r == valueRegion)
      return true;
  }
  ExprPtr nf = normalizer_.normalize(*regions_[static_cast<size_t>(valueRegion)].witness);
  if (!isReflTerm(*nf))
    return false;
  int parent = regions_[static_cast<size_t>(valueRegion)].parent;
  if (parent < 0)
    return false;
  return regionOutlives(parent, destRegion);
}

void TypeChecker::requireLives(const Type &value, int destRegion, const SourceLocation &loc,
                               const std::string &what) {
  if (value.isError() || !value.isRegionBound() || regionOutlives(value.region, destRegion))
    return;
  int vr = value.region;
  std::string path = "<opaque>";
  if (vr >= 0 && vr < static_cast<int>(regions_.size()) && regions_[static_cast<size_t>(vr)].witness) {
    ExprPtr nf = normalizer_.normalize(*regions_[static_cast<size_t>(vr)].witness);
    path = toString(*nf);
  }
  diags_.error(loc, what + " does not live long enough");
  diags_.note(loc, "inferred lifetime path normalises to '" + path +
                       "'; a non-constant path cannot be transported outside its scope "
                       "(Cruft inferred this bound from lexical nesting)");
  if (vr >= 0 && vr < static_cast<int>(regions_.size()) && regions_[static_cast<size_t>(vr)].arena) {
    diags_.note(regions_[static_cast<size_t>(vr)].loc, "allocated in this 'scope arena'");
    if (insideArena())
      diags_.note(loc, "a reference allocated inside 'scope arena' cannot be returned "
                       "or assigned to a variable declared in an outer scope");
  }
}

bool TypeChecker::insideArena() const {
  for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it)
    if (it->kind == ScopeKind::Arena)
      return true;
  return false;
}

void TypeChecker::noteUse(const Symbol *sym, const SourceLocation &loc) {
  if (!sym || sym->usage == Usage::Many)
    return;
  if (sym->kind != SymbolKind::Local && sym->kind != SymbolKind::Param &&
      sym->kind != SymbolKind::Global)
    return;
  const char *why = sym->usage == Usage::Linear
                         ? " (linear bindings are used exactly once)"
                         : " (owned bindings are affine; declare 'shared var' to copy)";
  if (consume_) {
    for (const auto &b : borrows_)
      if (b.owner == sym && b.refBinding)
        diags_.error(loc, "cannot move '" + sym->name + "' while '" + b.refBinding->name +
                              "' borrows it");
  }
  if (!consume_) {
    if (moved_.count(sym))
      diags_.error(loc, "use of moved value '" + sym->name + "'" + why);
    return;
  }
  if (!moved_.insert(sym).second)
    diags_.error(loc, "use of moved value '" + sym->name + "'" + why);
}

TypePtr TypeChecker::checkArray(ArrayLitExpr &a, const Type *expected) {
  if (expected && expected->kind == TypeKind::Map) {
    if (!a.elems.empty())
      return fail(a.loc, "build a Map with 'var m: Map(K, V) = []' and '.insert(key, value)'");
    return Type::map(expected->a, expected->b, currentRegion_);
  }
  TypePtr elem = expected && expected->kind == TypeKind::Vector ? expected->elem : nullptr;
  ast::ExprPtr vecLen = expected && expected->kind == TypeKind::Vector && expected->vecLen
                            ? expected->vecLen->clone()
                            : nullptr;
  if (vecLen) {
    ExprPtr nf = normalizer_.normalize(*vecLen);
    if (nf->kind != ExprKind::IntLit)
      return fail(a.loc, "Vec length must be a constant at this construction site, found '" +
                             toString(*nf) + "'");
    if (as<IntLitExpr>(*nf).value != static_cast<std::int64_t>(a.elems.size()))
      return fail(a.loc, "Vec length is " + std::to_string(as<IntLitExpr>(*nf).value) +
                             " but the array has " + std::to_string(a.elems.size()) + " element(s)");
  }
  if (a.elems.empty()) {
    if (!elem)
      return fail(a.loc, "cannot infer the element type of '[]'; write 'var xs: Vector(i32) = []'");
    return Type::vector(elem, currentRegion_, std::move(vecLen));
  }
  for (auto &el : a.elems) {
    TypePtr et = checkExpr(el, elem.get());
    if (et->isError())
      return et;
    if (!elem)
      elem = et;
    else if (!unify(*et, *elem, el->loc, "array element"))
      return Type::error();
  }
  if (elem->isErased() || elem->kind == TypeKind::Void)
    return fail(a.loc, "array elements must be runtime data");
  return Type::vector(std::move(elem), currentRegion_, std::move(vecLen));
}

TypePtr TypeChecker::checkLambda(LambdaExpr &l, const Type *expected) {
  if (!expected || expected->kind != TypeKind::Fn || l.params.size() != expected->params.size())
    return fail(l.loc, "cannot infer this lambda; write it as the argument of filter or map "
                       "('fn(x) => ...'), where the parameter type is known");
  pushScope();
  l.paramSymbols.clear();
  bool paramsOk = true;
  for (size_t i = 0; i < l.params.size(); ++i) {
    TypePtr pt = expected->params[i];
    if (l.params[i].type) {
      TypePtr written = resolveType(*l.params[i].type);
      if (written->isError() || !unify(*written, *pt, l.params[i].loc, "lambda parameter"))
        paramsOk = false;
      else
        pt = written;
    }
    Symbol ps;
    ps.kind = SymbolKind::Param;
    ps.name = l.params[i].name;
    ps.type = pt;
    ps.loc = l.params[i].loc;
    Usage annotated = l.params[i].usage;
    if (annotated == Usage::Many && i < expected->paramUse.size())
      annotated = expected->paramUse[i];
    ps.usage = resolveUsage(annotated, *pt, false, false);
    l.paramSymbols.push_back(declareLocal(std::move(ps)));
  }
  EffectRow row;
  if (expected->effectsDeclared) {
    row.bits = expected->effects;
    row.var = expected->effectVar;
    row.declared = true;
  }
  effectStack_.push_back(row);
  const Type *retHint = expected->ret ? expected->ret.get() : nullptr;
  TypePtr bt = checkExpr(l.body, retHint);
  effectStack_.pop_back();
  popScope();
  if (!paramsOk || bt->isError())
    return Type::error();
  if (expected->ret && !unify(*bt, *expected->ret, l.body->loc, "lambda body"))
    return Type::error();
  if (capturesLocal(*l.body, l.paramSymbols))
    return fail(l.loc, "this lambda captures a local binding; pass that data as a parameter instead");
  std::vector<Usage> uses = expected->paramUse;
  if (uses.size() < expected->params.size())
    uses.resize(expected->params.size(), Usage::Many);
  for (size_t i = 0; i < l.paramSymbols.size() && i < uses.size(); ++i)
    if (l.paramSymbols[i])
      uses[i] = l.paramSymbols[i]->usage;
  return Type::fn(expected->params, l.paramSymbols, expected->ret ? expected->ret : bt, uses,
                  expected->effects, expected->effectVar, expected->effectsDeclared);
}

TypePtr TypeChecker::checkMethod(ExprPtr &e, const Type *expected) {
  auto &m = as<MethodCallExpr>(*e);
  if (m.receiver->kind == ExprKind::Ident) {
    auto &id = as<IdentExpr>(*m.receiver);
    if (id.package.empty() && !lookupLocal(id.name) && registry_.find(id.name)) {
      auto qual = std::make_unique<IdentExpr>(id.loc, id.name, m.method);
      if (!m.invoked) {
        e = std::move(qual);
        return checkIdent(e, expected);
      }
      auto call = std::make_unique<CallExpr>(m.loc, std::move(qual), std::move(m.args));
      e = std::move(call);
      return checkCall(e, expected);
    }
  }

  bool savedConsume = consume_;
  consume_ = false;
  TypePtr recv = checkExpr(m.receiver);
  consume_ = savedConsume;
  if (recv->isError())
    return recv;
  bool viaRef = false;
  bool viaMut = false;
  if (recv->kind == TypeKind::Ref && recv->elem) {
    viaRef = true;
    viaMut = recv->refMut;
    recv = recv->elem;
  }
  auto mutablePlace = [&](const char *what) {
    if (viaRef)
      return viaMut ? true : (fail(m.loc, std::string("'") + what + "' needs '&mut'"), false);
    if (m.receiver->kind != ExprKind::Ident)
      return fail(m.receiver->loc, std::string("'") + what + "' updates a named variable"), false;
    const Symbol *sym = as<IdentExpr>(*m.receiver).symbol;
    bool throughLoan = false;
    if (sym) {
      for (const auto &b : borrows_)
        if (b.refBinding == sym)
          throughLoan = true;
    }
    if (sym && !throughLoan)
      rejectIfBorrowed(sym, m.loc, true);
    return true;
  };
  if (recv->kind == TypeKind::Struct && !m.invoked) {
    for (size_t i = 0; i < recv->params.size(); ++i) {
      const Symbol *field = i < recv->paramSymbols.size() ? recv->paramSymbols[i] : nullptr;
      if (!field || field->name != m.method)
        continue;
      if (recv->params[i] && recv->params[i]->isStorage() && m.receiver->kind == ExprKind::Ident)
        noteUse(as<IdentExpr>(*m.receiver).symbol, m.loc);
      return recv->params[i] ? recv->params[i] : Type::error();
    }
    return fail(m.loc, "struct has no field '" + m.method + "'");
  }
  if (!m.invoked)
    return fail(m.loc, "use '.' for a call, as in value." + m.method + "(...)");

  auto arity = [&](size_t n) {
    if (m.args.size() == n)
      return true;
    fail(m.loc, "'" + m.method + "' expects " + std::to_string(n) + " argument(s), found " +
                    std::to_string(m.args.size()));
    return false;
  };

  auto unknown = [&](const std::string &msg) -> TypePtr {
    if (TypePtr dispatched = tryTraitMethod(e, recv, expected))
      return dispatched;
    return fail(m.loc, msg);
  };

  if (recv->kind == TypeKind::Text) {
    if (m.method == "length")
      return arity(0) ? Type::i64() : Type::error();
    if (m.method == "data")
      return arity(0) ? Type::ptr() : Type::error();
    return unknown("Text has no method '" + m.method + "' (expected length or data)");
  }

  if (recv->kind == TypeKind::Ptr) {
    if (m.method == "addr") {
      if (!arity(0))
        return Type::error();
      requireUnsafe(m.loc, "pointer-to-integer conversion 'ptr.addr()'");
      return Type::i64();
    }
    return unknown("ptr has no method '" + m.method + "'");
  }

  if (recv->kind == TypeKind::Slice) {
    if (m.method == "length")
      return arity(0) ? Type::i64() : Type::error();
    if (m.method == "at" || m.method == "at_unchecked") {
      if (!arity(1))
        return Type::error();
      if (m.method == "at_unchecked")
        requireUnsafe(m.loc, "unchecked indexing");
      TypePtr idx = checkExpr(m.args[0], Type::i64().get());
      if (idx->isError())
        return idx;
      if (idx->kind != TypeKind::I64)
        return fail(m.args[0]->loc, "'at' index must be 'i64', found '" + idx->str() + "'");
      return recv->elem;
    }
    return unknown("Slice has no method '" + m.method +
                       "' (expected length, at or at_unchecked)");
  }

  if (recv->kind == TypeKind::Vector) {
    if (m.method == "length")
      return arity(0) ? Type::i64() : Type::error();
    if (m.method == "data")
      return arity(0) ? Type::ptr() : Type::error();
    if (m.method == "slice") {
      if (!arity(0))
        return Type::error();
      if (m.receiver->kind != ExprKind::Ident)
        return fail(m.loc, "'slice' requires a named Vector");
      const Symbol *named = as<IdentExpr>(*m.receiver).symbol;
      if (!named)
        return fail(m.loc, "'slice' requires a named Vector");
      const Symbol *root = named;
      for (const auto &b : borrows_)
        if (b.refBinding == named && b.owner)
          root = b.owner;
      e->loanOwner = root;
      acquireBorrow(root, nullptr, false, m.loc);
      return Type::slice(recv->elem, recv->region);
    }
    if (m.method == "push") {
      if (!arity(1) || !mutablePlace("push"))
        return Type::error();
      TypePtr vt = checkExpr(m.args[0], recv->elem.get());
      if (vt->isError() || !unify(*vt, *recv->elem, m.args[0]->loc, "'push'"))
        return Type::error();
      return Type::voidTy();
    }
    if (m.method == "pop") {
      if (!arity(0) || !mutablePlace("pop"))
        return Type::error();
      return recv->elem;
    }
    if (m.method == "set") {
      if (!arity(2) || !mutablePlace("set"))
        return Type::error();
      TypePtr idx = checkExpr(m.args[0], Type::i64().get());
      TypePtr vt = checkExpr(m.args[1], recv->elem.get());
      if (idx->isError() || vt->isError())
        return Type::error();
      if (idx->kind != TypeKind::I64)
        return fail(m.args[0]->loc, "'set' index must be 'i64', found '" + idx->str() + "'");
      if (!unify(*vt, *recv->elem, m.args[1]->loc, "'set'"))
        return Type::error();
      return Type::voidTy();
    }
    if (m.method == "reserve" || m.method == "clear") {
      if (m.method == "clear") {
        if (!arity(0) || !mutablePlace("clear"))
          return Type::error();
        return Type::voidTy();
      }
      if (!arity(1) || !mutablePlace("reserve"))
        return Type::error();
      TypePtr n = checkExpr(m.args[0], Type::i64().get());
      if (n->isError())
        return n;
      if (n->kind != TypeKind::I64)
        return fail(m.args[0]->loc, "'reserve' length must be 'i64', found '" + n->str() + "'");
      return Type::voidTy();
    }
    if (m.method == "at" || m.method == "at_unchecked") {
      if (!arity(1))
        return Type::error();
      if (m.method == "at_unchecked")
        requireUnsafe(m.loc, "unchecked indexing");
      TypePtr idx = checkExpr(m.args[0], Type::i64().get());
      if (idx->isError())
        return idx;
      if (idx->kind != TypeKind::I64)
        return fail(m.args[0]->loc, "'at' index must be 'i64', found '" + idx->str() + "'");
      return recv->elem;
    }
    if (m.method == "sum") {
      if (!arity(0))
        return Type::error();
      if (!recv->elem->isNumeric())
        return fail(m.loc, "'sum' requires numeric elements, found '" + recv->elem->str() + "'");
      return recv->elem;
    }
    if (m.method == "filter") {
      if (!arity(1))
        return Type::error();
      TypePtr pred = Type::fn({recv->elem}, {}, Type::boolean());
      TypePtr at = checkExpr(m.args[0], pred.get());
      if (at->isError() || !unify(*at, *pred, m.args[0]->loc, "'filter' predicate"))
        return Type::error();
      return Type::vector(recv->elem, currentRegion_);
    }
    if (m.method == "map") {
      if (!arity(1))
        return Type::error();
      TypePtr retHint = expected && expected->kind == TypeKind::Vector ? expected->elem : nullptr;
      TypePtr mapper = Type::fn({recv->elem}, {}, retHint);
      TypePtr at = checkExpr(m.args[0], mapper.get());
      if (at->isError() || at->kind != TypeKind::Fn || at->params.size() != 1)
        return Type::error();
      if (!unify(*at->params[0], *recv->elem, m.args[0]->loc, "'map' function input"))
        return Type::error();
      if (at->ret->isErased() || at->ret->kind == TypeKind::Void)
        return fail(m.args[0]->loc, "'map' must produce runtime data, found '" + at->ret->str() + "'");
      return Type::vector(at->ret, currentRegion_);
    }
    return unknown("Vector has no method '" + m.method +
                       "' (expected length, data, slice, at, at_unchecked, set, push, pop, reserve, clear, sum, filter or map)");
  }

  if (recv->kind == TypeKind::String) {
    if (m.method == "length" || m.method == "byte_len")
      return arity(0) ? Type::i64() : Type::error();
    if (m.method == "data")
      return arity(0) ? Type::ptr() : Type::error();
    if (m.method == "bytes") {
      if (!arity(0))
        return Type::error();
      if (m.receiver->kind == ExprKind::Ident) {
        const Symbol *named = as<IdentExpr>(*m.receiver).symbol;
        if (named) {
          const Symbol *root = named;
          for (const auto &b : borrows_)
            if (b.refBinding == named && b.owner)
              root = b.owner;
          e->loanOwner = root;
          acquireBorrow(root, nullptr, false, m.loc);
        }
      }
      return Type::slice(Type::u8(), recv->region);
    }
    if (m.method == "append") {
      if (!arity(1) || !mutablePlace("append"))
        return Type::error();
      TypePtr other = checkExpr(m.args[0], Type::stringTy(0).get());
      if (other->isError())
        return other;
      if (other->kind != TypeKind::String)
        return fail(m.args[0]->loc, "'append' expects a String, found '" + other->str() + "'");
      return Type::voidTy();
    }
    if (m.method == "set_len") {
      if (!arity(1))
        return Type::error();
      if (!mutablePlace("set_len"))
        return Type::error();
      TypePtr n = checkExpr(m.args[0], Type::i64().get());
      if (n->isError())
        return n;
      if (n->kind != TypeKind::I64)
        return fail(m.args[0]->loc, "'set_len' length must be 'i64', found '" + n->str() + "'");
      return Type::voidTy();
    }
    return unknown("String has no method '" + m.method +
                       "' (expected length, byte_len, data, bytes, append or set_len)");
  }

  if (recv->kind == TypeKind::Map) {
    if (m.method == "length")
      return arity(0) ? Type::i64() : Type::error();
    if (!recv->a->isInteger() && recv->a->kind != TypeKind::Bool && recv->a->kind != TypeKind::Ptr &&
        !recv->a->isFloat())
      return fail(m.loc, "Map keys must be an integer, float, bool or ptr, found '" + recv->a->str() +
                             "'");
    if (m.method == "insert") {
      if (!arity(2))
        return Type::error();
      if (!mutablePlace("insert"))
        return Type::error();
      TypePtr kt = checkExpr(m.args[0], recv->a.get());
      TypePtr vt = checkExpr(m.args[1], recv->b.get());
      if (kt->isError() || vt->isError())
        return Type::error();
      if (!unify(*kt, *recv->a, m.args[0]->loc, "'insert' key") ||
          !unify(*vt, *recv->b, m.args[1]->loc, "'insert' value"))
        return Type::error();
      return Type::voidTy();
    }
    if (m.method == "contains") {
      if (!arity(1))
        return Type::error();
      TypePtr kt = checkExpr(m.args[0], recv->a.get());
      if (kt->isError() || !unify(*kt, *recv->a, m.args[0]->loc, "'contains' key"))
        return Type::error();
      return Type::boolean();
    }
    if (m.method == "remove") {
      if (!arity(1) || !mutablePlace("remove"))
        return Type::error();
      TypePtr kt = checkExpr(m.args[0], recv->a.get());
      if (kt->isError() || !unify(*kt, *recv->a, m.args[0]->loc, "'remove' key"))
        return Type::error();
      return Type::boolean();
    }
    if (m.method == "get") {
      if (!arity(1))
        return Type::error();
      TypePtr kt = checkExpr(m.args[0], recv->a.get());
      if (kt->isError() || !unify(*kt, *recv->a, m.args[0]->loc, "'get' key"))
        return Type::error();
      Symbol *opt = nullptr;
      if (const Module *mod = registry_.find("option"))
        if (auto it = mod->symbols.find("Option"); it != mod->symbols.end())
          opt = it->second;
      if (!opt || opt->kind != SymbolKind::Type)
        return fail(m.loc, "'get' returns option.Option; add 'import option'");
      return instantiateSymbol(*opt, {recv->b}, currentRegion_);
    }
    return unknown("Map has no method '" + m.method +
                       "' (expected insert, contains, remove, get or length)");
  }

  return unknown("no implementation of method '" + m.method + "' for '" + recv->str() + "'");
}

Usage TypeChecker::resolveUsage(Usage annotated, const Type &type, bool shared, bool isConst) const {
  if (shared || isConst || type.isError())
    return Usage::Many;
  if (annotated == Usage::Linear || annotated == Usage::Affine)
    return annotated;
  if (type.kind == TypeKind::Labeled)
    return Usage::Linear;
  if (type.isStorage())
    return Usage::Affine;
  return Usage::Many;
}

namespace {

std::string formatEffects(EffectSet bits) {
  std::string s;
  auto add = [&](EffectSet bit, const char *name) {
    if ((bits & bit) == 0)
      return;
    if (!s.empty())
      s += ", ";
    s += name;
  };
  add(effectIo, "io");
  add(effectAlloc, "alloc");
  add(effectNet, "net");
  return s.empty() ? std::string("?") : s;
}

void walkExprTree(const Expr &e, const std::function<void(const Expr &)> &fn) {
  fn(e);
  switch (e.kind) {
  case ExprKind::Binary:
    walkExprTree(*as<BinaryExpr>(e).lhs, fn);
    walkExprTree(*as<BinaryExpr>(e).rhs, fn);
    break;
  case ExprKind::Unary:
    walkExprTree(*as<UnaryExpr>(e).operand, fn);
    break;
  case ExprKind::Call: {
    const auto &c = as<CallExpr>(e);
    if (c.callee)
      walkExprTree(*c.callee, fn);
    for (const auto &a : c.args)
      if (a)
        walkExprTree(*a, fn);
    if (c.along)
      walkExprTree(*c.along, fn);
    break;
  }
  case ExprKind::PathAbs:
    walkExprTree(*as<PathAbsExpr>(e).body, fn);
    break;
  case ExprKind::PathApp:
    walkExprTree(*as<PathAppExpr>(e).path, fn);
    walkExprTree(*as<PathAppExpr>(e).arg, fn);
    break;
  case ExprKind::Hcomp: {
    const auto &h = as<HcompExpr>(e);
    if (h.base)
      walkExprTree(*h.base, fn);
    for (const auto &face : h.faces)
      if (face.side)
        walkExprTree(*face.side, fn);
    break;
  }
  case ExprKind::Match: {
    const auto &m = as<MatchExpr>(e);
    if (m.scrutinee)
      walkExprTree(*m.scrutinee, fn);
    for (const auto &c : m.cases)
      if (c.body)
        walkExprTree(*c.body, fn);
    break;
  }
  case ExprKind::ArrayLit:
    for (const auto &el : as<ArrayLitExpr>(e).elems)
      if (el)
        walkExprTree(*el, fn);
    break;
  case ExprKind::Lambda:
    walkExprTree(*as<LambdaExpr>(e).body, fn);
    break;
  case ExprKind::Cast:
    walkExprTree(*as<CastExpr>(e).operand, fn);
    break;
  case ExprKind::Borrow:
    walkExprTree(*as<BorrowExpr>(e).place, fn);
    break;
  case ExprKind::Try:
    walkExprTree(*as<TryExpr>(e).operand, fn);
    break;
  case ExprKind::Method: {
    const auto &m = as<MethodCallExpr>(e);
    if (m.receiver)
      walkExprTree(*m.receiver, fn);
    for (const auto &a : m.args)
      if (a)
        walkExprTree(*a, fn);
    break;
  }
  default:
    break;
  }
}

void walkBlockExprs(const Block &block, const std::function<void(const Expr &)> &fn) {
  for (const auto &s : block) {
    if (!s)
      continue;
    switch (s->kind) {
    case StmtKind::VarDecl:
      if (static_cast<const VarDeclStmt &>(*s).init)
        walkExprTree(*static_cast<const VarDeclStmt &>(*s).init, fn);
      break;
    case StmtKind::Assign:
      if (static_cast<const AssignStmt &>(*s).value)
        walkExprTree(*static_cast<const AssignStmt &>(*s).value, fn);
      break;
    case StmtKind::Return:
      if (static_cast<const ReturnStmt &>(*s).value)
        walkExprTree(*static_cast<const ReturnStmt &>(*s).value, fn);
      break;
    case StmtKind::If: {
      const auto &i = static_cast<const IfStmt &>(*s);
      if (i.cond)
        walkExprTree(*i.cond, fn);
      walkBlockExprs(i.thenBody, fn);
      walkBlockExprs(i.elseBody, fn);
      break;
    }
    case StmtKind::While: {
      const auto &w = static_cast<const WhileStmt &>(*s);
      if (w.cond)
        walkExprTree(*w.cond, fn);
      walkBlockExprs(w.body, fn);
      break;
    }
    case StmtKind::For: {
      const auto &f = static_cast<const ForStmt &>(*s);
      if (f.low)
        walkExprTree(*f.low, fn);
      if (f.high)
        walkExprTree(*f.high, fn);
      if (f.iter)
        walkExprTree(*f.iter, fn);
      if (f.step)
        walkExprTree(*f.step, fn);
      walkBlockExprs(f.body, fn);
      break;
    }
    case StmtKind::FieldAssign: {
      const auto &a = static_cast<const FieldAssignStmt &>(*s);
      if (a.place)
        walkExprTree(*a.place, fn);
      if (a.value)
        walkExprTree(*a.value, fn);
      break;
    }
    case StmtKind::IndexAssign: {
      const auto &ix = static_cast<const IndexAssignStmt &>(*s);
      if (ix.place)
        walkExprTree(*ix.place, fn);
      if (ix.index)
        walkExprTree(*ix.index, fn);
      if (ix.value)
        walkExprTree(*ix.value, fn);
      break;
    }
    case StmtKind::Break:
    case StmtKind::Continue:
      break;
    case StmtKind::Foreach:
      walkBlockExprs(static_cast<const ForeachStmt &>(*s).body, fn);
      break;
    case StmtKind::Expr:
      if (static_cast<const ExprStmt &>(*s).expr)
        walkExprTree(*static_cast<const ExprStmt &>(*s).expr, fn);
      break;
    case StmtKind::ScopeArena:
      walkBlockExprs(static_cast<const ScopeArenaStmt &>(*s).body, fn);
      break;
    case StmtKind::Unsafe:
      walkBlockExprs(static_cast<const UnsafeStmt &>(*s).body, fn);
      break;
    case StmtKind::Error:
      break;
    }
  }
}

} // namespace

void TypeChecker::noteEffect(const Type &callee, const Symbol *sym, const SourceLocation &loc) {
  if (effectStack_.empty() || !effectStack_.back().declared)
    return;
  const EffectRow &allowed = effectStack_.back();
  if (!callee.effectVar.empty()) {
    if (callee.effectVar == allowed.var && (callee.effects & ~allowed.bits) == 0)
      return;
    diags_.error(loc, "effect variable '" + callee.effectVar + "' is not allowed in this function");
    return;
  }
  if (!callee.effectsDeclared) {
    if (sym && (sym->isExtern || sym->kind == SymbolKind::Ctor))
      return;
    diags_.error(loc, "call has an unknown effect row; annotate the callee with '!'");
    return;
  }
  EffectSet extra = static_cast<EffectSet>(callee.effects & ~allowed.bits);
  if (extra == 0)
    return;
  diags_.error(loc, "effect {" + formatEffects(extra) + "} is not allowed in this function");
}

void TypeChecker::rejectBoundary(const Expr &arg, bool boundary, const SourceLocation &loc,
                                 const std::string &what) {
  if (!boundary)
    return;
  if (arg.type && arg.type->kind == TypeKind::Labeled) {
    diags_.error(loc, "PathObstructionError: no path from " + arg.type->str() + " to a public " + what);
    return;
  }
  if (arg.flow == Usage::Linear)
    diags_.error(loc, "linear value cannot cross a " + what + " without 'reveal'");
}

bool TypeChecker::effectsCompatible(const Type &actual, const Type &expected) const {
  if (!expected.effectsDeclared)
    return true;
  if (!actual.effectVar.empty() || !expected.effectVar.empty())
    return actual.effectVar == expected.effectVar && (actual.effects & ~expected.effects) == 0;
  if (!actual.effectsDeclared)
    return false;
  return (actual.effects & ~expected.effects) == 0;
}

TypePtr TypeChecker::instantiateEffects(const TypePtr &type, const std::string &var,
                                        EffectSet bits) const {
  if (!type)
    return type;
  if (type->kind == TypeKind::Labeled && type->elem)
    return Type::labeled(instantiateEffects(type->elem, var, bits), type->secrecy);
  if (type->kind != TypeKind::Fn)
    return type;
  bool changed = false;
  std::vector<TypePtr> params;
  params.reserve(type->params.size());
  for (const auto &p : type->params) {
    params.push_back(instantiateEffects(p, var, bits));
    changed = changed || params.back() != p;
  }
  TypePtr ret = instantiateEffects(type->ret, var, bits);
  changed = changed || ret != type->ret;
  EffectSet effects = type->effects;
  std::string effectVar = type->effectVar;
  bool declared = type->effectsDeclared;
  if (effectVar == var) {
    effects = static_cast<EffectSet>(effects | bits);
    effectVar.clear();
    declared = true;
    changed = true;
  }
  if (!changed)
    return type;
  return Type::fn(std::move(params), type->paramSymbols, std::move(ret), type->paramUse, effects,
                  std::move(effectVar), declared, type->typeParam, type->typeParamLevel);
}

void TypeChecker::joinUses(const std::set<const Symbol *> &saved, const std::set<const Symbol *> &thenSet,
                           const std::set<const Symbol *> &elseSet, bool thenDiv, bool elseDiv,
                           bool hasElse, const SourceLocation &loc) {
  if (thenDiv && (!hasElse || elseDiv)) {
    moved_ = saved;
    return;
  }
  if (thenDiv && hasElse) {
    moved_ = elseSet;
    return;
  }
  if (hasElse && elseDiv) {
    moved_ = thenSet;
    return;
  }
  moved_ = saved;
  std::set<const Symbol *> touched;
  for (const Symbol *sym : thenSet)
    if (!saved.count(sym))
      touched.insert(sym);
  for (const Symbol *sym : elseSet)
    if (!saved.count(sym))
      touched.insert(sym);
  for (const Symbol *sym : touched) {
    bool inThen = thenSet.count(sym) > 0;
    bool inElse = hasElse && elseSet.count(sym) > 0;
    if (sym->usage == Usage::Linear) {
      if (!hasElse || inThen != inElse)
        diags_.error(loc, "linear binding '" + sym->name + "' is not used on every path");
      else if (inThen && inElse)
        moved_.insert(sym);
    } else if (sym->usage == Usage::Affine) {
      if (inThen || inElse)
        moved_.insert(sym);
    }
  }
}

void TypeChecker::exhaustScope(size_t scopeIndex, const SourceLocation &loc) {
  if (scopeIndex >= scopes_.size())
    return;
  for (const auto &entry : scopes_[scopeIndex].names) {
    const Symbol *sym = entry.second;
    if (sym && sym->usage == Usage::Linear && !moved_.count(sym))
      diags_.error(loc, "linear binding '" + sym->name + "' is not consumed");
  }
}

void TypeChecker::exhaustInScope(const SourceLocation &loc) {
  for (size_t i = 0; i < scopes_.size(); ++i)
    exhaustScope(i, loc);
}

std::set<const Symbol *> TypeChecker::symbolsInScope() const {
  std::set<const Symbol *> out;
  for (const auto &scope : scopes_)
    for (const auto &entry : scope.names)
      out.insert(entry.second);
  return out;
}

void TypeChecker::checkIndexResidues(const Block &block, const std::vector<const Symbol *> &indices) {
  for (const Symbol *index : indices) {
    if (!index)
      continue;
    bool reported = false;
    std::function<void(const Expr &)> check = [&](const Expr &e) {
      if (reported)
        return;
      if (e.type && e.type->isErased())
        return; // proof content may depend on the interval
      if (e.type && Normalizer::mentions(e, index)) {
        auto i0 = std::make_unique<IntervalLitExpr>(e.loc, false);
        i0->type = Type::interval();
        auto i1 = std::make_unique<IntervalLitExpr>(e.loc, true);
        i1->type = Type::interval();
        ExprPtr left = normalizer_.substitute(e, index, *i0);
        ExprPtr right = normalizer_.substitute(e, index, *i1);
        if (!normalizer_.equal(*left, *right)) {
          reported = true;
          diags_.error(e.loc, "interval index '" + index->name +
                                  "' changes the computational result ('" +
                                  toString(*normalizer_.normalize(*left)) + "' vs '" +
                                  toString(*normalizer_.normalize(*right)) + "')");
          return;
        }
      }
      switch (e.kind) {
      case ExprKind::Binary:
        check(*as<BinaryExpr>(e).lhs);
        check(*as<BinaryExpr>(e).rhs);
        break;
      case ExprKind::Unary:
        check(*as<UnaryExpr>(e).operand);
        break;
      case ExprKind::Call: {
        const auto &c = as<CallExpr>(e);
        if (c.callee)
          check(*c.callee);
        for (const auto &a : c.args)
          if (a)
            check(*a);
        break;
      }
      case ExprKind::PathApp:
        check(*as<PathAppExpr>(e).path);
        check(*as<PathAppExpr>(e).arg);
        break;
      case ExprKind::Hcomp:
        if (as<HcompExpr>(e).base)
          check(*as<HcompExpr>(e).base);
        break;
      case ExprKind::Match: {
        const auto &m = as<MatchExpr>(e);
        if (m.scrutinee)
          check(*m.scrutinee);
        for (const auto &c : m.cases)
          if (c.body)
            check(*c.body);
        break;
      }
      case ExprKind::Lambda:
        if (as<LambdaExpr>(e).body)
          check(*as<LambdaExpr>(e).body);
        break;
      case ExprKind::ArrayLit:
        for (const auto &el : as<ArrayLitExpr>(e).elems)
          if (el)
            check(*el);
        break;
      case ExprKind::Method: {
        const auto &m = as<MethodCallExpr>(e);
        if (m.receiver)
          check(*m.receiver);
        for (const auto &a : m.args)
          if (a)
            check(*a);
        break;
      }
      default:
        break;
      }
    };
    // walkExprTree invokes the callback on the root and then on children, and
    // `check` also walks children. Visit statement roots only.
    for (const auto &stmt : block) {
      if (!stmt || reported)
        continue;
      switch (stmt->kind) {
      case StmtKind::VarDecl:
        if (static_cast<const VarDeclStmt &>(*stmt).init)
          check(*static_cast<const VarDeclStmt &>(*stmt).init);
        break;
      case StmtKind::Assign:
        if (static_cast<const AssignStmt &>(*stmt).value)
          check(*static_cast<const AssignStmt &>(*stmt).value);
        break;
      case StmtKind::Return:
        if (static_cast<const ReturnStmt &>(*stmt).value)
          check(*static_cast<const ReturnStmt &>(*stmt).value);
        break;
      case StmtKind::If: {
        const auto &i = static_cast<const IfStmt &>(*stmt);
        if (i.cond)
          check(*i.cond);
        checkIndexResidues(i.thenBody, {index});
        checkIndexResidues(i.elseBody, {index});
        break;
      }
      case StmtKind::While: {
        const auto &w = static_cast<const WhileStmt &>(*stmt);
        if (w.cond)
          check(*w.cond);
        checkIndexResidues(w.body, {index});
        break;
      }
      case StmtKind::Foreach:
        checkIndexResidues(static_cast<const ForeachStmt &>(*stmt).body, {index});
        break;
      case StmtKind::For: {
        const auto &f = static_cast<const ForStmt &>(*stmt);
        if (f.low)
          check(*f.low);
        if (f.high)
          check(*f.high);
        if (f.iter)
          check(*f.iter);
        if (f.step)
          check(*f.step);
        checkIndexResidues(f.body, {index});
        break;
      }
      case StmtKind::FieldAssign: {
        const auto &a = static_cast<const FieldAssignStmt &>(*stmt);
        if (a.place)
          check(*a.place);
        if (a.value)
          check(*a.value);
        break;
      }
      case StmtKind::IndexAssign: {
        const auto &ix = static_cast<const IndexAssignStmt &>(*stmt);
        if (ix.place)
          check(*ix.place);
        if (ix.index)
          check(*ix.index);
        if (ix.value)
          check(*ix.value);
        break;
      }
      case StmtKind::Break:
      case StmtKind::Continue:
        break;
      case StmtKind::Expr:
        if (static_cast<const ExprStmt &>(*stmt).expr)
          check(*static_cast<const ExprStmt &>(*stmt).expr);
        break;
      case StmtKind::ScopeArena:
        checkIndexResidues(static_cast<const ScopeArenaStmt &>(*stmt).body, {index});
        break;
      case StmtKind::Unsafe:
        checkIndexResidues(static_cast<const UnsafeStmt &>(*stmt).body, {index});
        break;
      case StmtKind::Error:
        break;
      }
    }
  }
}

void TypeChecker::verifyCofn(FnDecl &fn) {
  if (!fn.symbol)
    return;
  if (!cofnTerminates(fn.body, fn.symbol))
    diags_.error(fn.loc, "cofn '" + fn.name + "' must end every path in a tail self-call or 'ret'");
  rejectUntailCalls(fn.body, fn.symbol);
}

bool TypeChecker::cofnTerminates(Block &block, const Symbol *self) {
  if (block.empty())
    return false;
  Stmt &last = *block.back();
  switch (last.kind) {
  case StmtKind::Return:
    return true;
  case StmtKind::Expr: {
    auto &e = static_cast<ExprStmt &>(last);
    if (e.expr && e.expr->kind == ExprKind::Call && static_cast<CallExpr &>(*e.expr).fn == self) {
      static_cast<CallExpr &>(*e.expr).tailSelf = true;
      return true;
    }
    return false;
  }
  case StmtKind::If: {
    auto &i = static_cast<IfStmt &>(last);
    if (i.elseBody.empty())
      return false;
    return cofnTerminates(i.thenBody, self) && cofnTerminates(i.elseBody, self);
  }
  case StmtKind::ScopeArena:
    return cofnTerminates(static_cast<ScopeArenaStmt &>(last).body, self);
  case StmtKind::Unsafe:
    return cofnTerminates(static_cast<UnsafeStmt &>(last).body, self);
  case StmtKind::Foreach:
    return cofnTerminates(static_cast<ForeachStmt &>(last).body, self);
  default:
    return false;
  }
}

void TypeChecker::rejectUntailCalls(const Block &block, const Symbol *self) {
  walkBlockExprs(block, [&](const Expr &e) {
    if (e.kind != ExprKind::Call)
      return;
    const auto &call = as<CallExpr>(e);
    if (call.fn == self && !call.tailSelf)
      diags_.error(e.loc, "cofn self-call must be in tail position");
  });
}

bool TypeChecker::isFixedSize(const Type &type, std::set<const Symbol *> &seen) const {
  switch (type.kind) {
  case TypeKind::Vector:
  case TypeKind::Map:
  case TypeKind::String:
  case TypeKind::Slice:
  case TypeKind::Error:
    return false;
  case TypeKind::Labeled:
  case TypeKind::Quotient:
    return type.elem && isFixedSize(*type.elem, seen);
  case TypeKind::Inductive: {
    if (!type.inductive)
      return false;
    if (!seen.insert(type.inductive).second)
      return false;
    for (const Symbol *ctor : type.inductive->ctors) {
      if (!ctor || ctor->isPathCtor || !ctor->type)
        continue;
      if (ctor->type->kind == TypeKind::Fn) {
        for (const auto &p : ctor->type->params)
          if (p && !isFixedSize(*p, seen)) {
            seen.erase(type.inductive);
            return false;
          }
      } else if (!isFixedSize(*ctor->type, seen)) {
        seen.erase(type.inductive);
        return false;
      }
    }
    seen.erase(type.inductive);
    return true;
  }
  default:
    return true;
  }
}

TypePtr TypeChecker::substTypeVar(const TypePtr &type, const Symbol *var,
                                  const TypePtr &replacement) const {
  if (!type || !var)
    return type;
  if (type->kind == TypeKind::TyVar && type->tyVar == var)
    return replacement;
  if (type->kind == TypeKind::Labeled && type->elem)
    return Type::labeled(substTypeVar(type->elem, var, replacement), type->secrecy);
  if (type->kind == TypeKind::Vector && type->elem) {
    TypePtr elem = substTypeVar(type->elem, var, replacement);
    if (elem == type->elem)
      return type;
    return Type::vector(std::move(elem), type->region, type->vecLen ? type->vecLen->clone() : nullptr);
  }
  if (type->kind == TypeKind::Ref && type->elem) {
    TypePtr elem = substTypeVar(type->elem, var, replacement);
    if (elem == type->elem)
      return type;
    return Type::refTy(std::move(elem), type->refMut, type->region);
  }
  if (type->kind == TypeKind::Slice && type->elem) {
    TypePtr elem = substTypeVar(type->elem, var, replacement);
    if (elem == type->elem)
      return type;
    return Type::slice(std::move(elem), type->region);
  }
  if (type->kind == TypeKind::Simd && type->elem) {
    TypePtr elem = substTypeVar(type->elem, var, replacement);
    if (elem == type->elem)
      return type;
    return Type::simd(std::move(elem), type->lanes);
  }
  if (type->kind == TypeKind::Path && type->elem) {
    TypePtr elem = substTypeVar(type->elem, var, replacement);
    if (elem == type->elem)
      return type;
    return Type::path(std::move(elem), type->lhs ? type->lhs->clone() : nullptr,
                      type->rhs ? type->rhs->clone() : nullptr);
  }
  if (type->kind == TypeKind::Struct) {
    bool changed = false;
    std::vector<TypePtr> fields;
    for (const auto &field : type->params) {
      fields.push_back(substTypeVar(field, var, replacement));
      changed = changed || fields.back() != field;
    }
    std::vector<TypePtr> targs;
    for (const auto &arg : type->typeArgs) {
      targs.push_back(substTypeVar(arg, var, replacement));
      changed = changed || targs.back() != arg;
    }
    if (!changed)
      return type;
    TypePtr out = Type::structOf(type->inductive, std::move(fields), type->paramSymbols);
    auto edited = std::const_pointer_cast<Type>(out);
    edited->typeArgs = std::move(targs);
    edited->typeParams = type->typeParams;
    edited->region = type->region;
    return out;
  }
  if (type->kind == TypeKind::Inductive) {
    bool changed = false;
    std::vector<TypePtr> targs;
    for (const auto &arg : type->typeArgs) {
      targs.push_back(substTypeVar(arg, var, replacement));
      changed = changed || targs.back() != arg;
    }
    if (!changed)
      return type;
    TypePtr out = Type::inductiveOf(type->inductive);
    auto edited = std::const_pointer_cast<Type>(out);
    edited->typeArgs = std::move(targs);
    edited->typeParams = type->typeParams;
    edited->region = type->region;
    return out;
  }
  if (type->kind != TypeKind::Fn)
    return type;
  bool changed = false;
  std::vector<TypePtr> params;
  for (const auto &p : type->params) {
    params.push_back(substTypeVar(p, var, replacement));
    changed = changed || params.back() != p;
  }
  TypePtr ret = substTypeVar(type->ret, var, replacement);
  changed = changed || ret != type->ret;
  if (!changed)
    return type;
  return Type::fn(std::move(params), type->paramSymbols, std::move(ret), type->paramUse, type->effects,
                  type->effectVar, type->effectsDeclared);
}

int TypeChecker::universeOf(const Type &type) const {
  if (type.kind == TypeKind::Universe)
    return type.level + 1;
  if (type.kind == TypeKind::TyVar && type.tyVar && type.tyVar->type &&
      type.tyVar->type->kind == TypeKind::Universe)
    return type.tyVar->type->level;
  return 0;
}

TypePtr TypeChecker::typeOfDenoted(const Type &denoted) const {
  if (denoted.kind == TypeKind::Universe)
    return Type::universe(denoted.level + 1);
  if (denoted.kind == TypeKind::TyVar && denoted.tyVar && denoted.tyVar->type)
    return denoted.tyVar->type;
  return Type::universe(0);
}

TypePtr TypeChecker::checkStructCall(CallExpr &call, const Type &structType) {
  if (call.args.size() != structType.params.size())
    return fail(call.loc, "struct '" + structType.str() + "' expects " +
                              std::to_string(structType.params.size()) + " field(s), found " +
                              std::to_string(call.args.size()));
  std::vector<TypePtr> fieldTypes = structType.params;
  bool ok = true;
  for (size_t i = 0; i < call.args.size(); ++i) {
    TypePtr at = checkExpr(call.args[i], fieldTypes[i].get());
    if (at->isError() || !unify(*at, *fieldTypes[i], call.args[i]->loc,
                                "field " + std::to_string(i + 1) + " of '" + structType.str() + "'")) {
      ok = false;
      continue;
    }
    if (i < structType.paramSymbols.size() && structType.paramSymbols[i]) {
      for (size_t j = i + 1; j < fieldTypes.size(); ++j)
        fieldTypes[j] = normalizer_.substituteType(fieldTypes[j], structType.paramSymbols[i],
                                                    *call.args[i]);
    }
  }
  if (!ok)
    return Type::error();
  TypePtr result = Type::structOf(structType.inductive, structType.params, structType.paramSymbols);
  auto edited = std::const_pointer_cast<Type>(result);
  edited->typeArgs = structType.typeArgs;
  edited->region = structType.region;
  return result;
}

void TypeChecker::checkAlong(CallExpr &call, const Type &fnType) {
  if (!call.along)
    return;
  if (!currentFn_ || !currentFn_->isCofn)
    diags_.error(call.along->loc, "'along' is only valid on a cofn self-call");
  if (fnType.params.empty() || call.args.empty() || fnType.paramSymbols.empty() || !fnType.paramSymbols[0]) {
    diags_.error(call.along->loc, "'along' requires a state parameter and a new state");
    return;
  }
  bool saved = consume_;
  consume_ = false;
  TypePtr proof = checkExpr(call.along);
  consume_ = saved;
  if (!proof || proof->isError())
    return;
  ExprPtr oldState = Normalizer::identOf(fnType.paramSymbols[0], call.loc);
  ExprPtr newState = call.args[0]->clone();
  TypePtr want = Type::path(fnType.params[0], std::move(oldState), std::move(newState));
  unify(*proof, *want, call.along->loc, "cofn step");
}

std::vector<const Symbol *> TypeChecker::openTypeBinders(const std::vector<FnDecl::TypeBinder> &binders) {
  std::vector<const Symbol *> out;
  for (const auto &b : binders) {
    int level = 0;
    if (!b.bound) {
      diags_.error(b.loc, "type binder '" + b.name + "' needs a universe, as in [T: Type(0)]");
    } else {
      TypePtr bound = resolveType(*b.bound);
      if (bound->kind != TypeKind::Universe)
        diags_.error(b.bound->loc, "type binder '" + b.name +
                                       "' must be bounded by a universe, found '" + bound->str() + "'");
      else
        level = bound->level;
    }
    Symbol ts;
    ts.kind = SymbolKind::TypeParam;
    ts.name = b.name;
    ts.type = Type::universe(level);
    ts.loc = b.loc;
    out.push_back(declareLocal(std::move(ts)));
  }
  return out;
}

TypePtr TypeChecker::instantiateBinders(TypePtr type, const std::vector<const Symbol *> &binders,
                                        const std::vector<TypePtr> &args) const {
  for (size_t i = 0; i < binders.size() && i < args.size(); ++i)
    type = substTypeVar(type, binders[i], args[i]);
  return type;
}

TypePtr TypeChecker::instantiateSymbol(const Symbol &sym, const std::vector<TypePtr> &args,
                                       int region) {
  if (!sym.type)
    return Type::error();
  if (sym.type->kind == TypeKind::Struct) {
    TypePtr t = instantiateBinders(sym.type, sym.typeBinders, args);
    auto edited = std::const_pointer_cast<Type>(t);
    edited->typeArgs = args;
    edited->region = region;
    return t;
  }
  TypePtr t = Type::inductiveOf(&sym);
  auto edited = std::const_pointer_cast<Type>(t);
  edited->typeArgs = args;
  edited->typeParams = sym.typeBinders;
  edited->region = region;
  return t;
}

TypePtr TypeChecker::inferTypeArg(const Type &param, const TypePtr &arg, const Symbol *var) const {
  if (!arg || !var)
    return nullptr;
  if (param.kind == TypeKind::TyVar && param.tyVar == var)
    return arg;
  if (param.elem && arg->elem && param.kind == arg->kind &&
      (param.kind == TypeKind::Vector || param.kind == TypeKind::Slice || param.kind == TypeKind::Ref ||
       param.kind == TypeKind::Labeled || param.kind == TypeKind::Quotient))
    return inferTypeArg(*param.elem, arg->elem, var);
  if (param.kind == TypeKind::Map && arg->kind == TypeKind::Map && param.a && param.b && arg->a &&
      arg->b) {
    if (TypePtr key = inferTypeArg(*param.a, arg->a, var))
      return key;
    return inferTypeArg(*param.b, arg->b, var);
  }
  if ((param.kind == TypeKind::Struct || param.kind == TypeKind::Inductive) &&
      param.kind == arg->kind && param.inductive == arg->inductive) {
    for (size_t i = 0; i < param.typeArgs.size() && i < arg->typeArgs.size(); ++i)
      if (TypePtr got = inferTypeArg(*param.typeArgs[i], arg->typeArgs[i], var))
        return got;
  }
  return nullptr;
}

TypePtr TypeChecker::checkCast(CastExpr &e) {
  TypePtr target = resolveType(*e.typeExpr);
  if (target->isError())
    return target;
  TypePtr src = checkExpr(e.operand, target->isNumeric() ? target.get() : nullptr);
  if (src->isError())
    return src;
  if (normalizer_.typeEqual(*src, *target))
    return target;
  bool srcInt = src->isInteger();
  bool dstInt = target->isInteger();
  bool srcFloat = src->isFloat();
  bool dstFloat = target->isFloat();
  auto addressSized = [](const Type &ty) {
    return ty.kind == TypeKind::I64 || ty.kind == TypeKind::U64 || ty.kind == TypeKind::ISize ||
           ty.kind == TypeKind::USize;
  };
  bool ok = (srcInt && dstInt) || (srcInt && dstFloat) || (srcFloat && dstInt) ||
            (srcFloat && dstFloat) || (src->kind == TypeKind::Bool && dstInt) ||
            (src->kind == TypeKind::Ptr && addressSized(*target)) ||
            (addressSized(*src) && target->kind == TypeKind::Ptr);
  if (!ok)
    return fail(e.loc, "cannot cast '" + src->str() + "' to '" + target->str() + "'");
  if ((src->kind == TypeKind::Ptr && target->isInteger()) ||
      (src->isInteger() && target->kind == TypeKind::Ptr))
    requireUnsafe(e.loc, "a pointer/integer conversion");
  return target;
}

TypePtr TypeChecker::checkBorrow(BorrowExpr &e) {
  bool saved = consume_;
  bool savedPlace = inBorrowPlace_;
  consume_ = false;
  inBorrowPlace_ = true;
  TypePtr pt = checkExpr(e.place);
  consume_ = saved;
  inBorrowPlace_ = savedPlace;
  if (pt->isError())
    return pt;
  if (pt->isErased())
    return fail(e.loc, "cannot borrow erased type '" + pt->str() + "'");
  if (e.place->kind == ExprKind::Ident)
    e.owner = as<IdentExpr>(*e.place).symbol;
  if (e.mut) {
    if (!e.owner || e.owner->isConst || e.owner->kind == SymbolKind::Param)
      return fail(e.loc, "'&mut' requires a mutable 'var'");
  }
  if (e.owner) {
    acquireBorrow(e.owner, nullptr, e.mut, e.loc);
    e.loanOwner = e.owner;
  }
  int region = pt->region;
  // A borrow of a local, a parameter, or an arena temporary dies with that
  // scope. Arena regions already fail requireLives at the boundary. Locals
  // share the heap region, so returning one is rejected at `ret`.
  if (insideArena())
    region = currentRegion_;
  return Type::refTy(pt, e.mut, region);
}

TypePtr TypeChecker::checkTry(TryExpr &e) {
  TypePtr inner = checkExpr(e.operand);
  if (inner->isError())
    return inner;
  if (!currentRet_ || inner->kind != TypeKind::Inductive || !inner->inductive)
    return fail(e.loc, "'?' expects a Result or Option in a function that returns one");
  const std::string &name = inner->inductive->name;
  if (name == "Option" && inner->typeArgs.size() == 1) {
    if (currentRet_->kind != TypeKind::Inductive || !currentRet_->inductive ||
        currentRet_->inductive->name != "Option")
      return fail(e.loc, "'?' on Option requires the function to return Option");
    return inner->typeArgs[0];
  }
  if (name == "Result" && inner->typeArgs.size() == 2) {
    if (currentRet_->kind != TypeKind::Inductive || !currentRet_->inductive ||
        currentRet_->inductive->name != "Result" || currentRet_->typeArgs.size() != 2 ||
        !normalizer_.typeEqual(*currentRet_->typeArgs[1], *inner->typeArgs[1]))
      return fail(e.loc, "'?' error type must match the function's Result error");
    return inner->typeArgs[0];
  }
  return fail(e.loc, "'?' expects Result(T, E) or Option(T), found '" + inner->str() + "'");
}

void TypeChecker::popScope() {
  scopes_.pop_back();
  releaseScopeBorrows();
}

void TypeChecker::releaseScopeBorrows() {
  int depth = static_cast<int>(scopes_.size());
  borrows_.erase(std::remove_if(borrows_.begin(), borrows_.end(),
                                [depth](const LiveBorrow &b) { return b.scopeDepth > depth; }),
                 borrows_.end());
}

void TypeChecker::releaseStatementBorrows() {
  borrows_.erase(std::remove_if(borrows_.begin(), borrows_.end(),
                                [](const LiveBorrow &b) { return b.refBinding == nullptr; }),
                 borrows_.end());
}

void TypeChecker::acquireBorrow(const Symbol *owner, const Symbol *binding, bool mut,
                                const SourceLocation &loc) {
  if (!owner)
    return;
  for (const auto &b : borrows_) {
    if (b.owner != owner)
      continue;
    if (mut || b.mut) {
      diags_.error(loc, "overlapping mutable borrow of '" + owner->name + "'");
      break;
    }
  }
  borrows_.push_back(LiveBorrow{owner, binding, mut, static_cast<int>(scopes_.size())});
}

void TypeChecker::bindLoan(const Expr &init, const Symbol *binding) {
  if (!binding)
    return;
  const Symbol *owner = init.loanOwner;
  bool mut = false;
  if (init.kind == ExprKind::Borrow) {
    const auto &b = as<BorrowExpr>(init);
    if (b.owner)
      owner = b.owner;
    mut = b.mut;
  } else if (init.type && init.type->kind == TypeKind::Ref && init.type->refMut) {
    mut = true;
  }
  if (!owner)
    return;
  for (auto it = borrows_.rbegin(); it != borrows_.rend(); ++it) {
    if (it->owner == owner && it->refBinding == nullptr && it->mut == mut) {
      it->refBinding = binding;
      return;
    }
  }
  acquireBorrow(owner, binding, mut, init.loc);
}

void TypeChecker::rejectIfBorrowed(const Symbol *owner, const SourceLocation &loc, bool mutating) {
  if (!owner)
    return;
  for (const auto &b : borrows_) {
    if (b.owner != owner)
      continue;
    if (!mutating && !b.mut)
      continue;
    if (mutating)
      diags_.error(loc, "cannot mutate '" + owner->name + "' while it is borrowed");
    else
      diags_.error(loc, "cannot use '" + owner->name + "' while it is mutably borrowed");
    return;
  }
}

void TypeChecker::requireUnsafe(const SourceLocation &loc, const std::string &what) {
  if (unsafeDepth_ > 0)
    return;
  if (currentFn_ && currentFn_->isExtern)
    return;
  diags_.error(loc, what + " requires 'unsafe'");
}

void TypeChecker::noteSpawnShared(const CallExpr &call) {
  if (call.args.size() < 2 || !call.args[1])
    return;
  const Expr &arg = *call.args[1];
  const Symbol *sym = nullptr;
  if (arg.kind == ExprKind::Borrow)
    sym = as<BorrowExpr>(arg).owner;
  else if (arg.kind == ExprKind::Ident)
    sym = as<IdentExpr>(arg).symbol;
  if (!sym || !sym->shared)
    diags_.error(arg.loc, "spawn requires a shared value");
}

} // namespace cruft::sema
