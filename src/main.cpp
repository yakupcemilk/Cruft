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
#include "CodeGen.h"
#include "Diagnostics.h"
#include "Lexer.h"
#include "Parser.h"
#include "TypeChecker.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

enum class OutputKind { LLVMIR, Object, Assembly, CheckOnly };

struct Options {
  std::vector<std::string> inputs;
  std::string output;
  std::string target;
  std::string moduleName;
  OutputKind kind = OutputKind::LLVMIR;
  unsigned optLevel = 0;
};

void usage(std::ostream &os) {
  os << "usage: cruftc [options] <file.cruft>...\n"
        "  -o <path>          output file\n"
        "  -emit-llvm         emit LLVM IR (.ll) [default]\n"
        "  -c                 emit object file (.o)\n"
        "  -S                 emit assembly (.s)\n"
        "  --check            type-check only\n"
        "  -O0 .. -O3         optimisation level (default -O0)\n"
        "  --target <triple>  target triple (default: host)\n"
        "  --module <name>    LLVM module name\n"
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

int main(int argc, char **argv) {
  Options opts;
  if (!parseArgs(argc, argv, opts)) {
    usage(std::cerr);
    return 2;
  }

  cruft::DiagnosticEngine diags;
  cruft::ast::Program program;

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
  if (diags.hasErrors()) {
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
  cruft::CodeGenOptions cgOpts;
  cgOpts.targetTriple = opts.target;
  cgOpts.optLevel = opts.optLevel;
  cruft::CodeGen codegen(program, checker, diags, moduleName, cgOpts);
  if (!codegen.generate()) {
    diags.print(std::cerr);
    return 1;
  }
  codegen.optimize();

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
