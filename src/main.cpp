#include <iostream>
#include <fstream>
#include <sstream>
#include "lexer/Lexer.h"
#include "lexer/Token.h"
#include "parser/Parser.h"
#include "codegen/CodeGen.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <test.cruft>\n";
        return 1;
    }

    const char* filename = argv[1];
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Failed to open file: " << filename << "\n";
        return 1;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string source = buffer.str();

    // Lexing
    Lexer lexer(source);
    auto tokens = lexer.tokenize();

    // Parsing
    cruft::Parser parser(tokens);
    std::shared_ptr<cruft::Program> prog = parser.parseProgram();


    // Code generation
    codegen::CodeGen generator(std::cout);
    generator.generate(*prog);

    return 0;
}