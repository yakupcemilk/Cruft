#include "ModuleLoader.h"

#include "lexer/Lexer.h"
#include "parser/Parser.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace cruft {

namespace fs = std::filesystem;

namespace {

std::string readAll(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string canonicalPath(const fs::path &path) {
  std::error_code ec;
  fs::path out = fs::weakly_canonical(path, ec);
  if (ec)
    out = path;
  return out.string();
}

} // namespace

ModuleLoader::ModuleLoader(DiagnosticEngine &diags, std::vector<std::string> searchPaths)
    : diags_(diags), search_(std::move(searchPaths)) {}

void ModuleLoader::addSearchPath(std::string path) {
  if (!path.empty())
    search_.push_back(std::move(path));
}

bool ModuleLoader::hasPackage(const ast::Program &program, const std::string &package) {
  for (const auto &file : program.files)
    if (file && file->package == package)
      return true;
  return false;
}

void ModuleLoader::discoverStd(const std::string &fromFile) {
  std::error_code ec;
  fs::path dir = fs::absolute(fs::path(fromFile), ec).parent_path();
  if (ec)
    dir = fs::path(fromFile).parent_path();
  for (int i = 0; i < 8 && !dir.empty(); ++i) {
    fs::path marker = dir / "std" / "mem.cruft";
    if (fs::exists(marker)) {
      std::string stdDir = (dir / "std").string();
      bool known = false;
      for (const std::string &path : search_)
        if (path == stdDir)
          known = true;
      if (!known)
        search_.push_back(std::move(stdDir));
      return;
    }
    if (!dir.has_parent_path() || dir == dir.parent_path())
      break;
    dir = dir.parent_path();
  }
}

std::string ModuleLoader::findFile(const std::string &package, const std::string &fromFile) const {
  std::vector<fs::path> roots;
  if (!fromFile.empty())
    roots.push_back(fs::path(fromFile).parent_path());
  for (const std::string &path : search_)
    roots.emplace_back(path);

  for (const fs::path &root : roots) {
    if (root.empty())
      continue;
    const fs::path candidates[] = {
        root / (package + ".cruft"),
        root / package / "mod.cruft",
        root / package / (package + ".cruft"),
    };
    for (const fs::path &candidate : candidates) {
      std::error_code ec;
      if (fs::is_regular_file(candidate, ec))
        return canonicalPath(candidate);
    }
  }
  return {};
}

bool ModuleLoader::loadDependency(const std::string &package, const std::string &fromFile,
                                  ast::Program &program) {
  if (package.empty())
    return true;
  discoverStd(fromFile);
  if (hasPackage(program, package))
    return true;
  SourceLocation loc;
  if (!program.files.empty())
    loc = program.files.front()->packageLoc;
  std::string path = findFile(package, fromFile);
  if (path.empty()) {
    diags_.error(loc, "dependency '" + package + "' from the manifest was not found");
    return false;
  }
  return ensurePackage(package, loc, fromFile, program);
}

bool ModuleLoader::ensurePackage(const std::string &package, SourceLocation loc,
                                 const std::string &fromFile, ast::Program &program) {
  if (package.empty() || hasPackage(program, package))
    return true;

  std::string path = findFile(package, fromFile);
  if (path.empty()) {
    // The type checker reports a stable message for packages that were never
    // supplied on the command line and do not exist on disk. Cycles among
    // packages that did load are reported there as well.
    return true;
  }
  if (!loadedPaths_.insert(path).second)
    return true;

  std::string source = readAll(path);
  if (source.empty() && !fs::is_regular_file(path)) {
    diags_.error(loc, "cannot read '" + path + "'");
    return false;
  }

  Lexer lexer(std::move(source), path, diags_);
  Parser parser(lexer.tokenize(), path, diags_);
  std::unique_ptr<ast::File> file = parser.parseFile();
  if (file->package.empty())
    return false;
  if (file->package != package) {
    diags_.error(file->packageLoc, "file '" + path + "' declares package '" + file->package +
                                       "', not '" + package + "'");
    program.files.push_back(std::move(file));
    return false;
  }
  program.files.push_back(std::move(file));
  return true;
}

bool ModuleLoader::resolve(ast::Program &program) {
  for (const auto &file : program.files)
    if (file)
      discoverStd(file->filename);

  for (size_t i = 0; i < program.files.size(); ++i) {
    // Copy before loading: appending a file can reallocate `program.files`.
    const std::string from = program.files[i]->filename;
    const std::vector<ast::ImportDecl> imports = program.files[i]->imports;
    for (const ast::ImportDecl &imp : imports) {
      if (!ensurePackage(imp.package, imp.loc, from, program))
        return false;
    }
  }
  return !diags_.hasErrors();
}

} // namespace cruft
