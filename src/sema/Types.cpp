// Types.cpp - type factories, printing, module registry.
#include "sema/Types.h"

#include <algorithm>
#include <functional>

namespace cruft::sema {

// ---------------------------------------------------------------------------
// Type factories
// ---------------------------------------------------------------------------

TypePtr Type::primitive(TypeKind k) {
  auto t = std::make_shared<Type>();
  t->kind = k;
  return t;
}

TypePtr Type::i32() {
  static TypePtr t = primitive(TypeKind::I32);
  return t;
}
TypePtr Type::i64() {
  static TypePtr t = primitive(TypeKind::I64);
  return t;
}
TypePtr Type::f32() {
  static TypePtr t = primitive(TypeKind::F32);
  return t;
}
TypePtr Type::f64() {
  static TypePtr t = primitive(TypeKind::F64);
  return t;
}
TypePtr Type::boolean() {
  static TypePtr t = primitive(TypeKind::Bool);
  return t;
}
TypePtr Type::voidTy() {
  static TypePtr t = primitive(TypeKind::Void);
  return t;
}
TypePtr Type::interval() {
  static TypePtr t = primitive(TypeKind::Interval);
  return t;
}
TypePtr Type::universe() {
  static TypePtr t = primitive(TypeKind::Universe);
  return t;
}

TypePtr Type::universe(int level) {
  if (level <= 0)
    return universe();
  static std::map<int, TypePtr> cache;
  auto it = cache.find(level);
  if (it != cache.end())
    return it->second;
  auto t = std::make_shared<Type>();
  t->kind = TypeKind::Universe;
  t->level = level;
  cache.emplace(level, t);
  return t;
}
TypePtr Type::error() {
  static TypePtr t = primitive(TypeKind::Error);
  return t;
}

TypePtr Type::path(TypePtr elem, ast::ExprPtr lhs, ast::ExprPtr rhs) {
  auto t = std::make_shared<Type>();
  t->kind = TypeKind::Path;
  t->elem = std::move(elem);
  t->lhs = std::move(lhs);
  t->rhs = std::move(rhs);
  return t;
}

TypePtr Type::equiv(TypePtr a, TypePtr b) {
  auto t = std::make_shared<Type>();
  t->kind = TypeKind::Equiv;
  t->a = std::move(a);
  t->b = std::move(b);
  return t;
}

TypePtr Type::fn(std::vector<TypePtr> params, std::vector<const Symbol *> paramSymbols,
                 TypePtr ret, std::vector<ast::Usage> paramUse, ast::EffectSet effects,
                 std::string effectVar, bool effectsDeclared, const Symbol *typeParam,
                 int typeParamLevel) {
  auto t = std::make_shared<Type>();
  t->kind = TypeKind::Fn;
  t->params = std::move(params);
  t->paramSymbols = std::move(paramSymbols);
  if (t->paramSymbols.size() < t->params.size())
    t->paramSymbols.resize(t->params.size(), nullptr);
  t->paramUse = std::move(paramUse);
  if (t->paramUse.size() < t->params.size())
    t->paramUse.resize(t->params.size(), ast::Usage::Many);
  t->ret = std::move(ret);
  t->effects = effects;
  t->effectVar = std::move(effectVar);
  t->effectsDeclared = effectsDeclared;
  t->typeParam = typeParam;
  t->typeParamLevel = typeParamLevel;
  return t;
}

TypePtr Type::labeled(TypePtr elem, ast::Secrecy secrecy) {
  auto t = std::make_shared<Type>();
  t->kind = TypeKind::Labeled;
  t->elem = std::move(elem);
  t->secrecy = secrecy;
  t->region = t->elem ? t->elem->region : 0;
  return t;
}

TypePtr Type::leak(ast::Secrecy from, ast::Secrecy to) {
  auto t = std::make_shared<Type>();
  t->kind = TypeKind::Leak;
  t->secrecy = from;
  t->secrecyTo = to;
  return t;
}

TypePtr Type::quotient(TypePtr carrier, ast::ExprPtr rel, ast::ExprPtr canon,
                       const Symbol *canonFn) {
  auto t = std::make_shared<Type>();
  t->kind = TypeKind::Quotient;
  t->elem = std::move(carrier);
  t->lhs = std::move(rel);
  t->rhs = std::move(canon);
  t->canonFn = canonFn;
  t->region = t->elem ? t->elem->region : 0;
  return t;
}

TypePtr Type::inductiveOf(const Symbol *sym) {
  auto t = std::make_shared<Type>();
  t->kind = TypeKind::Inductive;
  t->inductive = sym;
  return t;
}

TypePtr Type::vector(TypePtr elem, int region, ast::ExprPtr len) {
  auto t = std::make_shared<Type>();
  t->kind = TypeKind::Vector;
  t->elem = std::move(elem);
  t->region = region;
  t->vecLen = std::move(len);
  return t;
}

TypePtr Type::text() {
  static TypePtr t = primitive(TypeKind::Text);
  return t;
}

TypePtr Type::structOf(const Symbol *sym, std::vector<TypePtr> fields,
                       std::vector<const Symbol *> fieldSymbols) {
  auto t = std::make_shared<Type>();
  t->kind = TypeKind::Struct;
  t->inductive = sym;
  t->params = std::move(fields);
  t->paramSymbols = std::move(fieldSymbols);
  if (t->paramSymbols.size() < t->params.size())
    t->paramSymbols.resize(t->params.size(), nullptr);
  return t;
}

TypePtr Type::typeVariable(const Symbol *sym) {
  auto t = std::make_shared<Type>();
  t->kind = TypeKind::TyVar;
  t->tyVar = sym;
  return t;
}

TypePtr Type::map(TypePtr key, TypePtr val, int region) {
  auto t = std::make_shared<Type>();
  t->kind = TypeKind::Map;
  t->a = std::move(key);
  t->b = std::move(val);
  t->region = region;
  return t;
}

TypePtr Type::stringTy(int region) {
  auto t = std::make_shared<Type>();
  t->kind = TypeKind::String;
  t->region = region;
  return t;
}

TypePtr Type::slice(TypePtr elem, int region) {
  auto t = std::make_shared<Type>();
  t->kind = TypeKind::Slice;
  t->elem = std::move(elem);
  t->region = region;
  return t;
}

TypePtr Type::ptr() {
  static TypePtr t = primitive(TypeKind::Ptr);
  return t;
}

bool Type::isCCompatible() const {
  switch (kind) {
  case TypeKind::I32:
  case TypeKind::I64:
  case TypeKind::F32:
  case TypeKind::F64:
  case TypeKind::Bool:
  case TypeKind::Void:
  case TypeKind::Ptr:
    return true;
  case TypeKind::Fn:
    for (const auto &p : params)
      if (!p->isCCompatible())
        return false;
    for (ast::Usage u : paramUse)
      if (u == ast::Usage::Linear)
        return false;
    return ret && ret->isCCompatible();
  case TypeKind::Slice:
    return elem && elem->isCCompatible();
  default:
    return false;
  }
}

std::string Type::str() const {
  switch (kind) {
  case TypeKind::I32: return "i32";
  case TypeKind::I64: return "i64";
  case TypeKind::F32: return "f32";
  case TypeKind::F64: return "f64";
  case TypeKind::Bool: return "bool";
  case TypeKind::Ptr: return "ptr";
  case TypeKind::Void: return "void";
  case TypeKind::Interval: return "I";
  case TypeKind::Universe:
    return level == 0 ? "Type" : "Type(" + std::to_string(level) + ")";
  case TypeKind::Text: return "Text";
  case TypeKind::TyVar:
    return tyVar ? tyVar->name : "T";
  case TypeKind::Struct:
    return inductive ? inductive->qualifiedName() : "struct";
  case TypeKind::Error: return "<error>";
  case TypeKind::Path:
    return "Path(" + elem->str() + ", " + ast::toString(*lhs) + ", " + ast::toString(*rhs) + ")";
  case TypeKind::Equiv:
    return "Equiv(" + a->str() + ", " + b->str() + ")";
  case TypeKind::Fn: {
    std::string s = "fn(";
    for (size_t i = 0; i < params.size(); ++i) {
      if (i)
        s += ", ";
      if (i < paramUse.size() && paramUse[i] == ast::Usage::Linear)
        s += "lin ";
      else if (i < paramUse.size() && paramUse[i] == ast::Usage::Affine)
        s += "aff ";
      if (paramSymbols[i])
        s += paramSymbols[i]->name + ": ";
      s += params[i]->str();
    }
    s += ")";
    if (ret && ret->kind != TypeKind::Void)
      s += ": " + ret->str();
    if (effectsDeclared) {
      s += " ! ";
      if (!effectVar.empty() && effects == 0)
        s += effectVar;
      else {
        s += "{";
        bool first = true;
        auto bit = [&](ast::EffectSet b, const char *name) {
          if ((effects & b) == 0)
            return;
          if (!first)
            s += ", ";
          first = false;
          s += name;
        };
        bit(ast::effectIo, "io");
        bit(ast::effectAlloc, "alloc");
        bit(ast::effectNet, "net");
        s += "}";
      }
    }
    return s;
  }
  case TypeKind::Labeled:
    return std::string(secrecy == ast::Secrecy::Private ? "Private(" : "Secret(") +
           (elem ? elem->str() : "?") + ")";
  case TypeKind::Leak:
    return std::string("Leak(") + ast::secrecyName(secrecy) + ", " + ast::secrecyName(secrecyTo) +
           ")";
  case TypeKind::Quotient:
    return "Quotient(" + (elem ? elem->str() : std::string("?")) + ")";
  case TypeKind::Inductive:
    return inductive ? inductive->qualifiedName() : "<inductive>";
  case TypeKind::Vector:
    if (vecLen)
      return "Vec(" + (elem ? elem->str() : std::string("?")) + ", " + ast::toString(*vecLen) + ")";
    return "Vector(" + (elem ? elem->str() : std::string("?")) + ")";
  case TypeKind::Map:
    return "Map(" + (a ? a->str() : std::string("?")) + ", " + (b ? b->str() : std::string("?")) + ")";
  case TypeKind::String:
    return "String";
  case TypeKind::Slice:
    return "Slice(" + (elem ? elem->str() : std::string("?")) + ")";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// ModuleRegistry
// ---------------------------------------------------------------------------

Module &ModuleRegistry::getOrCreate(const std::string &name) {
  auto it = modules_.find(name);
  if (it == modules_.end()) {
    it = modules_.emplace(name, Module{}).first;
    it->second.name = name;
  }
  return it->second;
}

Module *ModuleRegistry::find(const std::string &name) {
  auto it = modules_.find(name);
  return it == modules_.end() ? nullptr : &it->second;
}

const Module *ModuleRegistry::find(const std::string &name) const {
  auto it = modules_.find(name);
  return it == modules_.end() ? nullptr : &it->second;
}

Symbol *ModuleRegistry::create(Symbol sym) {
  storage_.push_back(std::move(sym));
  return &storage_.back();
}

ModuleRegistry::LookupResult ModuleRegistry::lookup(const std::string &pkg, const std::string &name,
                                                    const ast::File &from,
                                                    const std::string &fromPackage) const {
  const std::string &target = pkg.empty() ? fromPackage : pkg;
  const Module *mod = find(target);
  if (!mod)
    return {LookupStatus::NoSuchPackage};

  if (target != fromPackage) {
    const Module *self = find(fromPackage);
    bool imported = false;
    if (self) {
      auto it = self->importsByFile.find(&from);
      imported = it != self->importsByFile.end() && it->second.count(target) > 0;
    }
    if (!imported)
      return {LookupStatus::NotImported};
  }

  auto it = mod->symbols.find(name);
  if (it == mod->symbols.end()) {
    auto exported = mod->reexports.find(name);
    if (exported == mod->reexports.end())
      return {LookupStatus::NotFound};
    return {LookupStatus::Found, exported->second};
  }
  if (target != fromPackage && it->second->visibility != ast::Visibility::Public)
    return {LookupStatus::Private, it->second};
  return {LookupStatus::Found, it->second};
}

std::vector<std::string> ModuleRegistry::findDependencyCycle() const {
  enum class Mark { None, Active, Done };
  std::map<std::string, Mark> marks;
  std::vector<std::string> stack;
  std::vector<std::string> cycle;

  std::function<bool(const std::string &)> visit = [&](const std::string &name) {
    Mark &m = marks[name];
    if (m == Mark::Done)
      return false;
    if (m == Mark::Active) {
      auto it = std::find(stack.begin(), stack.end(), name);
      cycle.assign(it, stack.end());
      cycle.push_back(name);
      return true;
    }
    m = Mark::Active;
    stack.push_back(name);
    if (const Module *mod = find(name))
      for (const auto &dep : mod->dependencies)
        if (visit(dep))
          return true;
    stack.pop_back();
    m = Mark::Done;
    return false;
  };

  for (const auto &[name, mod] : modules_)
    if (visit(name))
      return cycle;
  return {};
}

} // namespace cruft::sema
