#include "Parser.h"
#include "stm/IfStatement.h"
#include "stm/WhileStatement.h"
#include "stm/ReturnStatement.h"
#include "stm/PrintStatement.h"

Token Parser::currentToken() {
    return tokens[index];
}

void Parser::advance() {
    if (index < tokens.size()) {
        index++;
    }
}

std::shared_ptr<Statement> Parser::parse() {
    return parseStatement();
}

std::shared_ptr<Statement> Parser::parseStatement() {
    if (currentToken().type == TokenType::Keyword) {
        if (currentToken().value == "if") {
            return parseIfStatement();
        }
        if (currentToken().value == "while") {
            return parseWhileStatement();
        }
        if (currentToken().value == "ret") {
            return parseReturnStatement();
        }
        if (currentToken().value == "print") {
            return parsePrintStatement();
        }
    }
    
    // Diğer komutlar için işlem yapılacak
    return nullptr;
}

std::shared_ptr<Statement> Parser::parseIfStatement() {
    advance();  // 'if' keyword'ünü geç
    auto expr = parseExpression();  // 'if' koşulunu al
    auto statement = std::make_unique<IfStatement>(std::move(expr));
    // Kod bloğu, else, elseif gibi kısımlar burada işlenebilir
    return statement;
}

std::shared_ptr<Statement> Parser::parseWhileStatement() {
    advance();  // 'while' keyword'ünü geç
    auto expr = parseExpression();  // 'while' koşulunu al
    auto statement = std::make_unique<WhileStatement>(std::move(expr));
    // Döngü bloğu burada işlenebilir
    return statement;
}

std::shared_ptr<Statement> Parser::parseReturnStatement() {
    auto expr = parseExpression();
    // hatalı: std::make_unique<RetStatement>
    return std::make_shared<ReturnStatement>(std::move(expr));
}

std::shared_ptr<Statement> Parser::parsePrintStatement() {
    auto expr = parseExpression();
    return std::make_shared<PrintStatement>(std::move(expr));
}

std::shared_ptr<Expr> Parser::parseExpression() {
    return parseBinaryExpression(0);  // Başlangıçta düşük bir öncelik seviyesi
}

std::shared_ptr<Expr> Parser::parseBinaryExpression(int precedence) {
    auto left = parsePrimaryExpression();  // İlk terimi (operand) al
    while (getPrecedence(currentToken()) > precedence) {
        auto op = currentToken().value;  // Operatör
        advance();  // Operatörü geç
        auto right = parsePrimaryExpression();  // Sağ terimi al
        left = std::make_unique<BinaryExpr>(std::move(left), op, std::move(right));
    }
    return left;
}

std::shared_ptr<Expr> Parser::parsePrimaryExpression() {
    if (currentToken().type == TokenType::Identifier) {
        return parseVariableOrLiteral();  // Değişken veya literal bir değer
    }
    if (currentToken().type == TokenType::Integer) {
        return parseVariableOrLiteral();  // Tam sayı literal
    }
    throw std::runtime_error("Unexpected token in expression");
}

std::shared_ptr<Expr> Parser::parseVariableOrLiteral() {
    if (currentToken().type == TokenType::Identifier) {
        auto var = std::make_unique<VariableExpr>(currentToken().value);
        advance();
        return var;
    }
    if (currentToken().type == TokenType::Integer) {
        auto integer = std::make_unique<IntegerExpr>(std::stoi(currentToken().value));
        advance();
        return integer;
    }
    throw std::runtime_error("Expected identifier or integer");
}

int Parser::getPrecedence(const Token& token) {
    if (token.type == TokenType::Operator) {
        if (token.value == "+" || token.value == "-") return 1;
        if (token.value == "*" || token.value == "/") return 2;
    }
    return 0;  // En düşük öncelik
}
