// src/main.cpp
#include "codegen/CodegenC.h"
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include <fstream>
#include <iostream>
#include <sstream>

using namespace cruft;

int main(int argc, char **argv) {
  bool toC = false;
  std::string filename;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-c") {
      toC = true;
    } else {
      filename = arg;
    }
  }

  if (filename.empty()) {
    std::cerr << "Usage: " << argv[0] << " [-c] <file.cruft>\n";
    return 1;
  }

  std::ifstream file(filename);
  if (!file) {
    std::cerr << "Failed to open " << filename << "\n";
    return 1;
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string source = buffer.str();

  // Lex & parse
  cruft::Lexer lexer(source);
  auto tokens = lexer.getTokens();
  cruft::Parser parser(tokens);
  auto prog = parser.parseProgram();

  // Generate C++ if requested
  if (toC) {
    cruft::codegen::cpp::CodeGenC generator(std::cout);
    generator.generate(*prog);
  }

  return 0;
}
