#include <iostream>
#include <fstream>
#include <sstream>

#include "lexer/Lexer.h"
#include "lexer/Token.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Kullanım: " << argv[0] << " <test.cruft>\n";
        return 1;
    }

    const char* filename = argv[1];
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Dosya açılamadı: " << filename << "\n";
        return 1;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string source = buffer.str();

    Lexer lexer(source);

    while (true) {
        Token tok = lexer.nextToken();

        if (tok.type == TokenType::EndOfFile) {
            break;
        }

        // Whitespace veya yorum tipleri varsa, lexer skip ettiyse bu gerekmez.
        // Gerekiyorsa kendi TokenType enum'unuza göre kontrol ekleyin.

        std::cout
            << "Token: \"" << tok.value << "\""
            << "  [Type=" << static_cast<int>(tok.type)
            << ", Line=" << tok.line
            << ", Col=" << tok.column
            << "]\n";
    }

    return 0;
}