// ModuleLoader.h - resolve `import` / `use` / `export` against the filesystem.
//
// Packages already present in the program (named by `package`, never by the
// file name) are left alone. A missing package is loaded from, in order:
//   <dir>/<pkg>.cruft
//   <dir>/<pkg>/mod.cruft
//   <dir>/<pkg>/<pkg>.cruft
// where <dir> is the importing file's directory and each --lib / manifest
// search path. The directory that contains std/mem.cruft is added when it
// is found by walking parents of an input. Cycles are rejected.
#pragma once

#include "ast/AST.h"
#include "Diagnostics.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace cruft {

class ModuleLoader {
public:
  ModuleLoader(DiagnosticEngine &diags, std::vector<std::string> searchPaths);

  void addSearchPath(std::string path);
  /// Load every import that is not already part of `program`.
  bool resolve(ast::Program &program);
  /// Load one package named by a manifest dependency. `fromFile` is the
  /// anchor for relative search.
  bool loadDependency(const std::string &package, const std::string &fromFile,
                      ast::Program &program);

private:
  bool ensurePackage(const std::string &package, SourceLocation loc, const std::string &fromFile,
                     ast::Program &program);
  void discoverStd(const std::string &fromFile);
  [[nodiscard]] std::string findFile(const std::string &package, const std::string &fromFile) const;
  [[nodiscard]] static bool hasPackage(const ast::Program &program, const std::string &package);

  DiagnosticEngine &diags_;
  std::vector<std::string> search_;
  std::unordered_set<std::string> loadedPaths_;
};

} // namespace cruft
