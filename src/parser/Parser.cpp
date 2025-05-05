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
    // completion: provide empty then- and else-branch vectors to match the constructor signature
    auto statement = std::make_unique<IfStatement>(
        std::move(expr),
        std::vector<std::shared_ptr<Statement>>{}, 
        std::vector<std::shared_ptr<Statement>>{});
    return statement;
}

std::shared_ptr<Statement> Parser::parseWhileStatement() {
    advance();  // 'while' keyword'ünü geç
    auto expr = parseExpression();  // 'while' koşulunu al
    // completion: pass an empty body vector to match the constructor signature
    auto statement = std::make_unique<WhileStatement>(std::move(expr), std::vector<std::shared_ptr<Statement>>{});
    return statement;
}

std::shared_ptr<Statement> Parser::parseReturnStatement() {
    auto expr = parseExpression();
    // hatalı: std::make_unique<RetStatement>
    return std::make_shared<ReturnStatement>(std::move(expr));
}

std::shared_ptr<Expr> Parser::parsePrimaryExpression() {
    // handle parenthesized sub‐expressions
    if (currentToken().type == TokenType::Punctuation && currentToken().value == "(") {
        advance();  // consume '('
        auto expr = parseExpression();
        if (currentToken().type != TokenType::Punctuation || currentToken().value != ")")
            throw std::runtime_error("Expected ')' after sub-expression");
        advance();  // consume ')'
        return expr;
    }
    // identifier or integer
    if (currentToken().type == TokenType::Identifier || currentToken().type == TokenType::Integer) {
        return parseVariableOrLiteral();
    }
    throw std::runtime_error("Unexpected token in expression");
}

std::shared_ptr<Statement> Parser::parsePrintStatement() {
    advance(); // consume 'print'
    // expect '('
    if (currentToken().type != TokenType::Punctuation || currentToken().value != "(")
        throw std::runtime_error("Expected '(' after 'print'");
    advance(); // consume '('
    auto expr = parseExpression();
    // expect ')'
    if (currentToken().type != TokenType::Punctuation || currentToken().value != ")")
        throw std::runtime_error("Expected ')' after print argument");
    advance(); // consume ')'
    return std::make_shared<PrintStatement>(std::move(expr));
}

std::shared_ptr<Expr> Parser::parseExpression() {
    return parseBinaryExpression(0);  // Başlangıçta düşük bir öncelik seviyesi
}

std::shared_ptr<Expr> Parser::parseBinaryExpression(int precedence) {
    auto left = parsePrimaryExpression();  // İlk terimi (operand) al
    while (getPrecedence(currentToken()) > precedence) {
            // Capture the full Token, not just its string value
            Token opToken = currentToken();
            advance();  // skip operator
            auto right = parsePrimaryExpression();
            left = std::make_unique<BinaryExpr>(std::move(left), opToken, std::move(right));
        }
    return left;
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
std::shared_ptr<Program> Parser::parseProgram() {
    auto prog = std::make_shared<Program>();
    // expect 'package' Identifier
    if (currentToken().value == "package") {
        advance();
        prog->packageName = currentToken().value;
        advance();
    }
    while (currentToken().type != TokenType::EndOfFile) {
        auto stmt = parseStatement();
        if (stmt) prog->statements.push_back(stmt);
        else advance();  // skip unrecognized
    }
    return prog;
}