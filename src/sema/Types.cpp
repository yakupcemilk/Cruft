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
                 TypePtr ret) {
  auto t = std::make_shared<Type>();
  t->kind = TypeKind::Fn;
  t->params = std::move(params);
  t->paramSymbols = std::move(paramSymbols);
  if (t->paramSymbols.size() < t->params.size())
    t->paramSymbols.resize(t->params.size(), nullptr);
  t->ret = std::move(ret);
  return t;
}

TypePtr Type::inductiveOf(const Symbol *sym) {
  auto t = std::make_shared<Type>();
  t->kind = TypeKind::Inductive;
  t->inductive = sym;
  return t;
}

TypePtr Type::vector(TypePtr elem, int region) {
  auto t = std::make_shared<Type>();
  t->kind = TypeKind::Vector;
  t->elem = std::move(elem);
  t->region = region;
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
    return ret->isCCompatible();
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
  case TypeKind::Universe: return "Type";
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
      if (paramSymbols[i])
        s += paramSymbols[i]->name + ": ";
      s += params[i]->str();
    }
    s += ")";
    if (ret && ret->kind != TypeKind::Void)
      s += ": " + ret->str();
    return s;
  }
  case TypeKind::Inductive:
    return inductive ? inductive->qualifiedName() : "<inductive>";
  case TypeKind::Vector:
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
