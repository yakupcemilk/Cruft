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
  };
  return table;
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
    fs.name = fn->name;
    fs.visibility = fn->visibility;
    fs.loc = fn->loc;
    fs.isExtern = fn->isExtern;
    fs.fnDecl = fn.get();
    fn->symbol = declare(std::move(fs));
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
  if (t.kind == TypeKind::Universe || t.kind == TypeKind::Equiv)
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
    case TypeKind::Vector:
    case TypeKind::Slice:
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
  for (auto &file : program_.files) {
    if (file->package.empty())
      continue;
    file_ = file.get();
    package_ = file->package;
    resolveFileSignatures(*file);
  }
}

void TypeChecker::resolveFileSignatures(File &file) {
  // --- inductive types: point constructors first, then path constructors
  //     (path constructor boundaries mention point constructors).
  for (auto &td : file.types) {
    if (!td->symbol)
      continue;
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
  }

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

  // --- functions
  for (auto &fn : file.functions) {
    if (!fn->symbol)
      continue;
    pushScope();
    std::vector<TypePtr> params;
    std::vector<const Symbol *> paramSyms;
    for (auto &p : fn->params) {
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
      paramSyms.push_back(declareLocal(std::move(ps)));
      params.push_back(pt);
    }
    TypePtr ret = fn->ret ? resolveType(*fn->ret) : Type::voidTy();
    popScope();

    TypePtr fnType = Type::fn(params, paramSyms, ret);
    fn->symbol->type = fnType;

    if (fn->isExtern) {
      // FFI: everything crossing the boundary must have a C layout.
      for (size_t i = 0; i < params.size(); ++i)
        if (!params[i]->isCCompatible() && !params[i]->isError())
          diags_.error(fn->params[i].loc,
                       "extern function '" + fn->name + "': parameter '" + fn->params[i].name +
                           "' has type '" + params[i]->str() +
                           "' which is not C-ABI compatible (allowed: i32, i64, f32, f64, bool, ptr, "
                           "Slice(T), and C-compatible function pointers)");
      if (!ret->isCCompatible() && !ret->isError())
        diags_.error(fn->loc, "extern function '" + fn->name + "': return type '" + ret->str() +
                                  "' is not C-ABI compatible");
    } else if (fn->visibility == Visibility::Public) {
      ensureNotLeakingPrivate(*fnType, fn->loc, "public function '" + fn->name + "'");
    }
  }
}

TypePtr TypeChecker::resolveType(const TypeExpr &t) {
  switch (t.kind) {
  case TypeExprKind::Named: {
    if (t.package.empty()) {
      if (t.name == "i32") return Type::i32();
      if (t.name == "i64") return Type::i64();
      if (t.name == "f32") return Type::f32();
      if (t.name == "f64") return Type::f64();
      if (t.name == "bool") return Type::boolean();
      if (t.name == "I") return Type::interval();
      if (t.name == "Type") return Type::universe();
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
    }
    Symbol *sym = resolveName(t.package, t.name, t.loc);
    if (!sym)
      return Type::error();
    if (sym->kind != SymbolKind::Type)
      return fail(t.loc, "'" + sym->qualifiedName() + "' is not a type");
    return sym->type;
  }
  case TypeExprKind::Path: {
    TypePtr elem = t.elem ? resolveType(*t.elem) : nullptr;
    if (elem && elem->isError())
      return elem;
    ExprPtr lhs = t.lhs->clone();
    ExprPtr rhs = t.rhs->clone();
    TypePtr lt = checkExpr(lhs, elem.get());
    if (lt->isError())
      return lt;
    if (!elem)
      elem = lt;
    else if (!unify(*lt, *elem, t.lhs->loc, "left endpoint of path type"))
      return Type::error();
    TypePtr rt = checkExpr(rhs, elem.get());
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
    return Type::fn(std::move(params), {}, std::move(ret));
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
      bool literal = nf->kind == ExprKind::IntLit || nf->kind == ExprKind::FloatLit ||
                     nf->kind == ExprKind::BoolLit;
      if (!literal)
        diags_.error(g->init->loc, "initializer of module-level '" + g->name +
                                       "' must be a compile-time constant (it normalises to '" +
                                       toString(*nf) + "')");
    }
  }
}

// ---------------------------------------------------------------------------
// Phase 5: function bodies
// ---------------------------------------------------------------------------

void TypeChecker::checkFunctionBodies() {
  for (auto &file : program_.files) {
    if (file->package.empty())
      continue;
    file_ = file.get();
    package_ = file->package;
    for (auto &fn : file->functions)
      if (fn->symbol && !fn->isExtern)
        checkFunction(*fn);
  }
}

void TypeChecker::checkFunction(FnDecl &fn) {
  const Type &fnType = *fn.symbol->type;
  currentFn_ = &fn;
  currentRet_ = fnType.ret;
  currentRegion_ = 0;
  moved_.clear();

  pushScope();
  for (size_t i = 0; i < fn.params.size(); ++i)
    scopes_.back().names[fn.params[i].name] = const_cast<Symbol *>(fnType.paramSymbols[i]);
  checkBlock(fn.body);
  popScope();

  if (currentRet_->kind != TypeKind::Void && !blockAlwaysReturns(fn.body))
    diags_.error(fn.loc, "function '" + fn.name + "' declares return type '" + currentRet_->str() +
                             "' but not all control paths end in 'ret'");
  currentFn_ = nullptr;
  currentRet_ = nullptr;
}

bool TypeChecker::blockAlwaysReturns(const Block &block) {
  for (const auto &s : block) {
    if (s->kind == StmtKind::Return)
      return true;
    if (s->kind == StmtKind::ScopeArena) {
      if (blockAlwaysReturns(static_cast<const ScopeArenaStmt &>(*s).body))
        return true;
    }
    if (s->kind == StmtKind::If) {
      const auto &i = static_cast<const IfStmt &>(*s);
      if (!i.elseBody.empty() && blockAlwaysReturns(i.thenBody) && blockAlwaysReturns(i.elseBody))
        return true;
    }
  }
  return false;
}

void TypeChecker::checkBlock(Block &block) {
  pushScope();
  for (auto &s : block)
    checkStmt(*s);
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
    Symbol s;
    s.kind = SymbolKind::Local;
    s.name = d.name;
    s.type = t ? t : Type::error();
    s.loc = d.loc;
    s.isConst = d.isConst;
    s.shared = shared;
    s.constInit = d.isConst ? d.init.get() : nullptr;
    d.symbol = declareLocal(std::move(s));
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
    if (!t->isError())
      unify(*t, *currentRet_, r.value->loc, "return value");
    break;
  }
  case StmtKind::If: {
    auto &i = static_cast<IfStmt &>(stmt);
    TypePtr ct = checkExpr(i.cond, Type::boolean().get());
    if (!ct->isError() && ct->kind != TypeKind::Bool)
      diags_.error(i.cond->loc, "'if' condition must be 'bool', found '" + ct->str() + "'");
    checkBlock(i.thenBody);
    checkBlock(i.elseBody);
    break;
  }
  case StmtKind::While: {
    auto &w = static_cast<WhileStmt &>(stmt);
    TypePtr ct = checkExpr(w.cond, Type::boolean().get());
    if (!ct->isError() && ct->kind != TypeKind::Bool)
      diags_.error(w.cond->loc, "'while' condition must be 'bool', found '" + ct->str() + "'");
    checkBlock(w.body);
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
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

bool TypeChecker::unify(const Type &actual, const Type &expected, const SourceLocation &loc,
                        const std::string &context) {
  if (actual.isError() || expected.isError())
    return false;
  if (normalizer_.typeEqual(actual, expected)) {
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
  lit->type = Type::universe();
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
    t = expected && expected->kind == TypeKind::I64 ? Type::i64() : Type::i32();
    if (t->kind == TypeKind::I32 && (lit.value > INT32_MAX || lit.value < INT32_MIN))
      return fail(e->loc, "integer literal " + std::to_string(lit.value) + " does not fit in i32");
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
    t = lit.denoted->isError() ? Type::error() : Type::universe();
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
  }
  if (!t)
    t = Type::error();
  e->type = t;
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
    return sym->type ? sym->type : Type::error();
  case SymbolKind::Function:
    return sym->type; // first-class reference => C function pointer
  case SymbolKind::Type: {
    // A type used as a term: rewrite to a type literal.
    ExprPtr lit = makeTypeLit(sym->type, id.loc);
    e = std::move(lit);
    return Type::universe();
  }
  case SymbolKind::Ctor: {
    if (!sym->type || sym->type->isError())
      return Type::error();
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
    TypePtr t = checkExpr(u.operand, Type::interval().get());
    if (t->isError())
      return t;
    if (t->kind != TypeKind::Interval)
      return fail(u.loc, "'~' requires an interval operand, found '" + t->str() + "'");
    return t;
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
    Symbol *sym = resolveName(callee.package, callee.name, callee.loc);
    if (!sym)
      return Type::error();
    callee.symbol = sym;
    if (sym->kind == SymbolKind::Function || sym->kind == SymbolKind::Ctor) {
      if (!sym->type || sym->type->isError())
        return Type::error();
      call.fn = sym;
      if (sym->type->kind != TypeKind::Fn) {
        if (sym->kind == SymbolKind::Ctor)
          return fail(call.loc, "constructor '" + sym->name + "' takes no arguments");
        return Type::error();
      }
      callee.type = sym->type;
      return checkFnCall(call, *sym->type, expected);
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
  if (call.args.size() != fnType.params.size())
    return fail(call.loc, "wrong number of arguments: expected " +
                              std::to_string(fnType.params.size()) + ", found " +
                              std::to_string(call.args.size()));
  // Dependent application: later parameter types and the return type may
  // mention earlier parameters, so substitute as we go.
  std::vector<TypePtr> paramTypes = fnType.params;
  TypePtr ret = fnType.ret;
  bool ok = true;
  for (size_t i = 0; i < call.args.size(); ++i) {
    TypePtr at = checkExpr(call.args[i], paramTypes[i].get());
    if (at->isError()) {
      ok = false;
      continue;
    }
    if (!unify(*at, *paramTypes[i], call.args[i]->loc, "argument " + std::to_string(i + 1)))
      ok = false;
    if (const Symbol *ps = fnType.paramSymbols[i]) {
      for (size_t j = i + 1; j < paramTypes.size(); ++j)
        paramTypes[j] = normalizer_.substituteType(paramTypes[j], ps, *call.args[i]);
      ret = normalizer_.substituteType(ret, ps, *call.args[i]);
    }
  }
  return ok ? ret : Type::error();
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
    if (!t->isNumeric() && t->kind != TypeKind::Bool)
      return fail(call.loc, "'print' supports i32, i64, f32, f64 and bool, found '" + t->str() + "'");
    return Type::voidTy();
  }
  case Builtin::Refl: {
    if (!arity(1, "refl"))
      return Type::error();
    const Type *hint = expected && expected->kind == TypeKind::Path ? expected->elem.get() : nullptr;
    TypePtr t = checkExpr(call.args[0], hint);
    if (t->isError())
      return t;
    return Type::path(t, call.args[0]->clone(), call.args[0]->clone());
  }
  case Builtin::Sym: {
    if (!arity(1, "sym"))
      return Type::error();
    TypePtr t = checkExpr(call.args[0], expected);
    if (t->isError())
      return t;
    if (t->kind != TypeKind::Path)
      return fail(call.loc, "'sym' expects a path, found '" + t->str() + "'");
    return Type::path(t->elem, t->rhs->clone(), t->lhs->clone());
  }
  case Builtin::Ua: {
    if (!arity(1, "ua"))
      return Type::error();
    TypePtr t = checkExpr(call.args[0]);
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
    // Heap, not the enclosing arena: the buffer must outlive the caller.
    return Type::stringTy(0);
  }
  case Builtin::RawSlice: {
    if (!arity(2, "raw_slice"))
      return Type::error();
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
  if (ua) {
    const Expr &eq = *as<CallExpr>(*ua).args[0];
    if (isBuiltin(eq, Builtin::Iso)) {
      const Expr &map = *as<CallExpr>(eq).args[reversed ? 1 : 0];
      if (map.kind == ExprKind::Ident && as<IdentExpr>(map).symbol &&
          as<IdentExpr>(map).symbol->kind == SymbolKind::Function) {
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
  TypePtr bt = checkExpr(p.body, hint);
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
  if (!sym || !sym->type || !sym->type->isStorage() || sym->shared || sym->isConst)
    return;
  if (sym->kind != SymbolKind::Local && sym->kind != SymbolKind::Param &&
      sym->kind != SymbolKind::Global)
    return;
  if (!consume_) {
    if (moved_.count(sym))
      diags_.error(loc, "use of moved value '" + sym->name +
                            "' (owned bindings are affine; declare 'shared var' to copy)");
    return;
  }
  if (!moved_.insert(sym).second)
    diags_.error(loc, "use of moved value '" + sym->name +
                          "' (owned bindings are affine; declare 'shared var' to copy)");
}

TypePtr TypeChecker::checkArray(ArrayLitExpr &a, const Type *expected) {
  if (expected && expected->kind == TypeKind::Map) {
    if (!a.elems.empty())
      return fail(a.loc, "build a Map with 'var m: Map(K, V) = []' and '.insert(key, value)'");
    return Type::map(expected->a, expected->b, currentRegion_);
  }
  TypePtr elem = expected && expected->kind == TypeKind::Vector ? expected->elem : nullptr;
  if (a.elems.empty()) {
    if (!elem)
      return fail(a.loc, "cannot infer the element type of '[]'; write 'var xs: Vector(i32) = []'");
    return Type::vector(elem, currentRegion_);
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
  return Type::vector(std::move(elem), currentRegion_);
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
    l.paramSymbols.push_back(declareLocal(std::move(ps)));
  }
  const Type *retHint = expected->ret ? expected->ret.get() : nullptr;
  TypePtr bt = checkExpr(l.body, retHint);
  popScope();
  if (!paramsOk || bt->isError())
    return Type::error();
  if (expected->ret && !unify(*bt, *expected->ret, l.body->loc, "lambda body"))
    return Type::error();
  if (capturesLocal(*l.body, l.paramSymbols))
    return fail(l.loc, "this lambda captures a local binding; pass that data as a parameter instead");
  return Type::fn(expected->params, l.paramSymbols, expected->ret ? expected->ret : bt);
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
  if (!m.invoked)
    return fail(m.loc, "use '.' for a call, as in value." + m.method + "(...)");

  auto arity = [&](size_t n) {
    if (m.args.size() == n)
      return true;
    fail(m.loc, "'" + m.method + "' expects " + std::to_string(n) + " argument(s), found " +
                    std::to_string(m.args.size()));
    return false;
  };

  if (recv->kind == TypeKind::Ptr) {
    if (m.method == "addr")
      return arity(0) ? Type::i64() : Type::error();
    return fail(m.loc, "ptr has no method '" + m.method + "'");
  }

  if (recv->kind == TypeKind::Slice) {
    if (m.method == "length")
      return arity(0) ? Type::i64() : Type::error();
    if (m.method == "at") {
      if (!arity(1))
        return Type::error();
      TypePtr idx = checkExpr(m.args[0], Type::i64().get());
      if (idx->isError())
        return idx;
      if (idx->kind != TypeKind::I64)
        return fail(m.args[0]->loc, "'at' index must be 'i64', found '" + idx->str() + "'");
      return recv->elem;
    }
    return fail(m.loc, "Slice has no method '" + m.method + "' (expected length or at)");
  }

  if (recv->kind == TypeKind::Vector) {
    if (m.method == "length")
      return arity(0) ? Type::i32() : Type::error();
    if (m.method == "data")
      return arity(0) ? Type::ptr() : Type::error();
    if (m.method == "at") {
      if (!arity(1))
        return Type::error();
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
    return fail(m.loc, "Vector has no method '" + m.method +
                          "' (expected length, data, at, sum, filter or map)");
  }

  if (recv->kind == TypeKind::String) {
    if (m.method == "length")
      return arity(0) ? Type::i32() : Type::error();
    if (m.method == "byte_len")
      return arity(0) ? Type::i64() : Type::error();
    if (m.method == "data")
      return arity(0) ? Type::ptr() : Type::error();
    if (m.method == "set_len") {
      if (!arity(1))
        return Type::error();
      if (m.receiver->kind != ExprKind::Ident)
        return fail(m.receiver->loc, "'set_len' updates a named variable");
      TypePtr n = checkExpr(m.args[0], Type::i64().get());
      if (n->isError())
        return n;
      if (n->kind != TypeKind::I64)
        return fail(m.args[0]->loc, "'set_len' length must be 'i64', found '" + n->str() + "'");
      return Type::voidTy();
    }
    return fail(m.loc, "String has no method '" + m.method +
                           "' (expected length, byte_len, data or set_len)");
  }

  if (recv->kind == TypeKind::Map) {
    if (m.method == "length")
      return arity(0) ? Type::i32() : Type::error();
    if (m.method == "insert") {
      if (!arity(2))
        return Type::error();
      if (m.receiver->kind != ExprKind::Ident)
        return fail(m.receiver->loc, "'insert' updates a named variable");
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
    return fail(m.loc, "Map has no method '" + m.method + "' (expected insert, contains or length)");
  }

  return fail(m.loc, "type '" + recv->str() + "' has no method '" + m.method + "'");
}

} // namespace cruft::sema
