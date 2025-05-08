// src/main.cpp
#include <fstream>
#include <iostream>
#include <sstream>

#include "codegen/CodeGen.h"
#include "codegen/cpp/CodegenC.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"

int main(int argc, char **argv) {
  bool toC = false;
  std::string filename;

  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "-c") {
      toC = true;
    } else {
      filename = argv[i];
    }
  }

  if (filename.empty()) {
    std::cerr << "Usage: " << argv[0] << " [-c] <file.cruft>\n";
    return 1;
  }

  std::ifstream file(filename);
  if (!file.is_open()) {
    std::cerr << "Failed to open " << filename << "\n";
    return 1;
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string source = buffer.str();

  std::cerr << "[main] Read " << source.size() << " bytes of input\n";

  // Lex & parse
  Lexer lexer(source);
  std::cerr << "[main] Starting lexing...\n";
  auto tokens = lexer.tokenize();
  std::cerr << "[main] Starting parsing...\n";
  cruft::Parser parser(tokens);
  auto prog = parser.parseProgram();
  std::cerr << "[main] Parsed program, functions: " << prog->statements.size()
            << "\n";
  if (toC) {
    // Fully‑qualified path into cruft::codegen::cpp
    cruft::codegen::cpp::CodeGenC generator(std::cout);
    generator.generate(*prog);

    std::cerr << "[main] Generating C++...\n";
    generator.generate(*prog);
    std::cerr << "[main] Done.\n";
  } else {
    // Fully‑qualified use of the assembler back‑end
    codegen::CodeGen generator(std::cout);
    generator.generate(*prog);

    std::cerr << "[main] Generating x86 NASM...\n";
    generator.generate(*prog);
    std::cerr << "[main] Done.\n";
  }

  return 0;
}
