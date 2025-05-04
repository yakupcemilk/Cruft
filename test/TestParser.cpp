#include <iostream>
#include "src/lexer/Lexer.h"
#include "src/parser/Parser.h"
#include "src/parser/AST.h"

void testParser(const std::string& input) {
    Lexer lexer(input);
    std::vector<Token> tokens = lexer.tokenize();

    std::cout << "Tokens:" << std::endl;
    for (const auto& token : tokens) {
        std::cout << "Type: " << static_cast<int>(token.type) << ", Value: " << token.value << std::endl;
    }

    Parser parser(tokens);
    auto statement = parser.parse();

    if (statement) {
        std::cout << "AST Root Statement Parsed!" << std::endl;
    } else {
        std::cout << "Parsing failed." << std::endl;
    }
}

int main() {
    std::string input = "x + 5";
    testParser(input);
    
    return 0;
}