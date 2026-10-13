// main.cpp - `cruftc` command line driver.
//
//   cruftc [options] <file.cruft>...
//
//   -o <path>         output path (default: derived from the first input)
//   -emit-llvm        emit textual LLVM IR (.ll)          [default]
//   -c                emit a native object file (.o)
//   -S                emit native assembly (.s)
//   --check           stop after semantic analysis
//   -O0 | -O1 | -O2 | -O3
//   --target <triple> cross-compile for the given LLVM target triple
//   --module <name>   LLVM module identifier (default: first package name)
//
// All input files are compiled as one compilation unit so that packages can
// reference each other; the package name comes from `package <name>` inside
// each file, never from the file name.
#include "codegen/CodeGen.h"
#include "Diagnostics.h"
#include "lexer/Lexer.h"
#include "Manifest.h"
#include "ModuleLoader.h"
#include "parser/Parser.h"
#include "sema/TypeChecker.h"
#include "middle/ArenaOpt.h"
#include "middle/ProofErasure.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

enum class OutputKind { LLVMIR, Object, Assembly, CheckOnly };

struct Options {
  std::vector<std::string> inputs;
  std::vector<std::string> libPaths;
  std::string output;
  std::string target;
  std::string moduleName;
  std::string manifest;
  std::string cc;
  OutputKind kind = OutputKind::LLVMIR;
  unsigned optLevel = 0;
  bool link = false;
};

void usage(std::ostream &os) {
  os << "usage: cruftc [options] <file.cruft>...\n"
        "  -o <path>          output file\n"
        "  -emit-llvm         emit LLVM IR (.ll) [default]\n"
        "  -c                 emit object file (.o)\n"
        "  -S                 emit assembly (.s)\n"
        "  --check            type-check only\n"
        "  -O0 .. -O3         optimisation level (default -O0)\n"
        "  --target <triple>  target triple (default: host, or cruft.toml)\n"
        "  --module <name>    LLVM module name\n"
        "  --manifest <path>  package manifest (default: cruft.toml beside the first input)\n"
        "  --lib <dir>        directory searched for imported packages\n"
        "  --link             link an executable (requires --cc)\n"
        "  --cc <path>        C compiler used by --link\n"
        "  -h, --help         show this help\n";
}

bool parseArgs(int argc, char **argv, Options &opts) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto needValue = [&](const char *flag) -> const char * {
      if (i + 1 >= argc) {
        std::cerr << "cruftc: '" << flag << "' requires an argument\n";
        return nullptr;
      }
      return argv[++i];
    };
    if (a == "-h" || a == "--help") {
      usage(std::cout);
      std::exit(0);
    } else if (a == "-o") {
      const char *v = needValue("-o");
      if (!v)
        return false;
      opts.output = v;
    } else if (a == "--target") {
      const char *v = needValue("--target");
      if (!v)
        return false;
      opts.target = v;
    } else if (a == "--module") {
      const char *v = needValue("--module");
      if (!v)
        return false;
      opts.moduleName = v;
    } else if (a == "--manifest") {
      const char *v = needValue("--manifest");
      if (!v)
        return false;
      opts.manifest = v;
    } else if (a == "--lib") {
      const char *v = needValue("--lib");
      if (!v)
        return false;
      opts.libPaths.emplace_back(v);
    } else if (a == "--cc") {
      const char *v = needValue("--cc");
      if (!v)
        return false;
      opts.cc = v;
    } else if (a == "--link") {
      opts.link = true;
    } else if (a == "-emit-llvm") {
      opts.kind = OutputKind::LLVMIR;
    } else if (a == "-c") {
      opts.kind = OutputKind::Object;
    } else if (a == "-S") {
      opts.kind = OutputKind::Assembly;
    } else if (a == "--check") {
      opts.kind = OutputKind::CheckOnly;
    } else if (a.size() == 3 && a[0] == '-' && a[1] == 'O' && a[2] >= '0' && a[2] <= '3') {
      opts.optLevel = static_cast<unsigned>(a[2] - '0');
    } else if (!a.empty() && a[0] == '-' && a != "-") {
      std::cerr << "cruftc: unknown option '" << a << "'\n";
      return false;
    } else {
      opts.inputs.push_back(a);
    }
  }
  if (opts.inputs.empty()) {
    std::cerr << "cruftc: no input files\n";
    return false;
  }
  return true;
}

std::string defaultOutput(const Options &opts) {
  std::string base = opts.inputs.front();
  size_t slash = base.find_last_of("/\\");
  size_t dot = base.find_last_of('.');
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
    base = base.substr(0, dot);
  switch (opts.kind) {
  case OutputKind::LLVMIR: return base + ".ll";
  case OutputKind::Object: return base + ".o";
  case OutputKind::Assembly: return base + ".s";
  case OutputKind::CheckOnly: return "";
  }
  return base;
}

bool readFile(const std::string &path, std::string &out) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

} // namespace

namespace fs = std::filesystem;

std::string findUp(const std::string &start, const fs::path &relative) {
  std::error_code ec;
  fs::path dir = fs::absolute(fs::path(start), ec).parent_path();
  if (ec)
    dir = fs::path(start).parent_path();
  for (int i = 0; i < 8 && !dir.empty(); ++i) {
    fs::path candidate = dir / relative;
    if (fs::is_regular_file(candidate))
      return candidate.string();
    if (!dir.has_parent_path() || dir == dir.parent_path())
      break;
    dir = dir.parent_path();
  }
  return {};
}

bool linkExecutable(const Options &opts, const cruft::Manifest &manifest, const std::string &object,
                    const std::string &output) {
  if (opts.cc.empty()) {
    std::cerr << "cruftc: --link requires --cc <compiler>\n";
    return false;
  }
  std::vector<std::string> cmd = {opts.cc, object};
  if (!opts.inputs.empty()) {
    if (std::string arena = findUp(opts.inputs.front(), fs::path("src") / "runtime" / "arena.c");
        !arena.empty())
      cmd.push_back(std::move(arena));
    if (std::string mem = findUp(opts.inputs.front(), fs::path("src") / "runtime" / "mem.c");
        !mem.empty())
      cmd.push_back(std::move(mem));
    if (std::string fsrt = findUp(opts.inputs.front(), fs::path("src") / "runtime" / "fs.c");
        !fsrt.empty())
      cmd.push_back(std::move(fsrt));
    if (std::string iort = findUp(opts.inputs.front(), fs::path("src") / "runtime" / "io.c");
        !iort.empty())
      cmd.push_back(std::move(iort));
  }
  cmd.push_back("-o");
  cmd.push_back(output);
  for (const std::string &dir : manifest.libraryPaths)
    cmd.push_back("-L" + dir);
  for (const std::string &lib : manifest.libraries)
    cmd.push_back("-l" + lib);

  std::string line;
  for (size_t i = 0; i < cmd.size(); ++i) {
    if (i)
      line += ' ';
    line += cmd[i];
  }
  int rc = std::system(line.c_str());
  if (rc != 0) {
    std::cerr << "cruftc: link failed (" << rc << "): " << line << '\n';
    return false;
  }
  return true;
}

int main(int argc, char **argv) {
  Options opts;
  if (!parseArgs(argc, argv, opts)) {
    usage(std::cerr);
    return 2;
  }
  if (opts.link && opts.cc.empty()) {
    std::cerr << "cruftc: --link requires --cc <compiler>\n";
    return 2;
  }

  cruft::DiagnosticEngine diags;
  cruft::ast::Program program;
  cruft::Manifest manifest;

  std::string manifestPath = opts.manifest;
  if (manifestPath.empty())
    manifestPath = cruft::Manifest::discover(opts.inputs.front());
  if (!manifestPath.empty()) {
    if (!manifest.loadFile(manifestPath, diags)) {
      diags.print(std::cerr);
      return 1;
    }
  }
  if (opts.target.empty())
    opts.target = manifest.targetTriple;

  // ---- Lex + Parse every input ------------------------------------------
  for (const std::string &path : opts.inputs) {
    std::string source;
    if (!readFile(path, source)) {
      std::cerr << "cruftc: cannot read '" << path << "'\n";
      return 1;
    }
    cruft::Lexer lexer(std::move(source), path, diags);
    std::vector<cruft::Token> tokens = lexer.tokenize();
    cruft::Parser parser(std::move(tokens), path, diags);
    program.files.push_back(parser.parseFile());
  }

  cruft::ModuleLoader loader(diags, opts.libPaths);
  for (const std::string &path : manifest.modulePaths)
    loader.addSearchPath(path);
  for (const std::string &dep : manifest.dependencies) {
    if (!loader.loadDependency(dep, opts.inputs.front(), program)) {
      diags.print(std::cerr);
      return 1;
    }
  }
  if (!loader.resolve(program) || diags.hasErrors()) {
    diags.print(std::cerr);
    return 1;
  }

  // ---- Semantic analysis / module system ---------------------------------
  cruft::sema::TypeChecker checker(program, diags);
  bool ok = checker.check();
  diags.print(std::cerr);
  if (!ok)
    return 1;
  if (opts.kind == OutputKind::CheckOnly)
    return 0;

  // ---- LLVM IR generation ------------------------------------------------
  std::string moduleName = opts.moduleName;
  if (moduleName.empty()) {
    for (const auto &f : program.files)
      if (!f->package.empty()) {
        moduleName = f->package;
        break;
      }
  }
  cruft::mid::ProofErasure::run(program);
  cruft::mid::ArenaOpt::run(program);

  cruft::CodeGenOptions cgOpts;
  cgOpts.targetTriple = opts.target;
  cgOpts.optLevel = opts.optLevel;
  cgOpts.linkLibraries = manifest.libraries;
  cruft::CodeGen codegen(program, checker, diags, moduleName, cgOpts);
  if (!codegen.generate()) {
    diags.print(std::cerr);
    return 1;
  }
  codegen.optimize();

  if (opts.link) {
    std::string exe = opts.output;
    if (exe.empty()) {
      std::string base = opts.inputs.front();
      size_t slash = base.find_last_of("/\\");
      size_t dot = base.find_last_of('.');
      if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        base = base.substr(0, dot);
      exe = base;
#ifdef _WIN32
      exe += ".exe";
#endif
    }
    std::string object = exe + ".o";
    if (!codegen.writeObject(object)) {
      diags.print(std::cerr);
      return 1;
    }
    if (!linkExecutable(opts, manifest, object, exe))
      return 1;
    return 0;
  }

  std::string output = opts.output.empty() ? defaultOutput(opts) : opts.output;
  bool written = false;
  switch (opts.kind) {
  case OutputKind::LLVMIR: written = codegen.writeIR(output); break;
  case OutputKind::Object: written = codegen.writeObject(output); break;
  case OutputKind::Assembly: written = codegen.writeAssembly(output); break;
  case OutputKind::CheckOnly: written = true; break;
  }
  if (!written) {
    diags.print(std::cerr);
    return 1;
  }
  return 0;
}
