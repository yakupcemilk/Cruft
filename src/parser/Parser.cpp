#include "Parser.h"
#include "../lexer/Token.h"
#include "stm/IfStatement.h"
#include "stm/WhileStatement.h"
#include "stm/ReturnStatement.h"
#include "stm/PrintStatement.h"
#include "stm/VarDeclStatement.h"
#include "stm/AssignmentStatement.h"
#include "stm/FunctionDecl.h"

#include <stdexcept>

using namespace cruft;

Token Parser::currentToken() const {
    return tokens[index];
}

Token Parser::currentToken() {
    return static_cast<const Parser&>(*this).currentToken();
}

void Parser::advance() {
    if (index + 1 < tokens.size()) {
        ++index;
    }
}

std::shared_ptr<Statement> Parser::parseStatement() {
    if (currentToken().type == TokenType::Keyword) {
        if (currentToken().value == "if")    return parseIfStatement();
        if (currentToken().value == "while") return parseWhileStatement();
        if (currentToken().value == "ret")   return parseReturnStatement();
        if (currentToken().value == "print") return parsePrintStatement();
        if (currentToken().value == "var")   return parseVarDecl();
        if (currentToken().value == "fn")    return parseFunctionDecl();
    }
    if (currentToken().type == TokenType::Identifier &&
        peekToken().type == TokenType::Operator &&
        peekToken().value == "=")
    {
        return parseAssignment();
    }
    return nullptr;
}

std::shared_ptr<Statement> Parser::parseIfStatement() {
    advance();
    auto cond = parseExpression();
    // TODO: parse then/else blocks properly
    return std::make_shared<IfStatement>(
        std::move(cond),
        std::vector<std::shared_ptr<Statement>>{},
        std::vector<std::shared_ptr<Statement>>{}
    );
}

std::shared_ptr<Statement> Parser::parseWhileStatement() {
    advance();
    auto cond = parseExpression();
    // TODO: parse body properly
    return std::make_shared<WhileStatement>(
        std::move(cond),
        std::vector<std::shared_ptr<Statement>>{}
    );
}

std::shared_ptr<Statement> Parser::parseReturnStatement() {
    advance(); 
    auto expr = parseExpression();
    return std::make_shared<ReturnStatement>(std::move(expr));
}

std::shared_ptr<Statement> Parser::parsePrintStatement() {
    advance(); 
    if (currentToken().type != TokenType::Punctuation || currentToken().value != "(")
        throw std::runtime_error("Expected '(' after 'print'");
    advance(); 
    auto expr = parseExpression();
    if (currentToken().type != TokenType::Punctuation || currentToken().value != ")")
        throw std::runtime_error("Expected ')' after print argument");
    advance(); 
    return std::make_shared<PrintStatement>(std::move(expr));
}

std::shared_ptr<Statement> Parser::parseVarDecl() {
    advance(); 
    if (currentToken().type != TokenType::Identifier)
        throw std::runtime_error("Expected identifier in var declaration");
    std::string name = currentToken().value;
    advance(); 

    if (currentToken().type != TokenType::Punctuation || currentToken().value != ":")
        throw std::runtime_error("Expected ':' after var name");
    advance(); 

    if (currentToken().type != TokenType::Identifier)
        throw std::runtime_error("Expected type after ':' in var declaration");
    advance(); 

    std::shared_ptr<Expr> init = nullptr;
    if (currentToken().type == TokenType::Operator && currentToken().value == "=") {
        advance(); 
        init = parseExpression();
    }
    return std::make_shared<VarDeclStatement>(name, std::move(init));
}

std::shared_ptr<Statement> Parser::parseAssignment() {
    std::string name = currentToken().value;
    advance(); 

    if (currentToken().type != TokenType::Operator || currentToken().value != "=")
        throw std::runtime_error("Expected '=' in assignment");
    advance(); 

    auto val = parseExpression();
    return std::make_shared<AssignmentStatement>(name, std::move(val));
}

std::shared_ptr<Statement> Parser::parseFunctionDecl() {
    advance(); 
    if (currentToken().type != TokenType::Identifier)
        throw std::runtime_error("Expected function name after 'fn'");
    std::string name = currentToken().value;
    advance(); 

    std::vector<std::string> params;
    if (currentToken().type == TokenType::Punctuation && currentToken().value == "(") {
        advance(); 
        while (!(currentToken().type == TokenType::Punctuation && currentToken().value == ")")) {
            if (currentToken().type == TokenType::Identifier) {
                params.push_back(currentToken().value);
                advance();
            }
            if (currentToken().type == TokenType::Punctuation && currentToken().value == ",")
                advance();
        }
        advance(); 
    }

    if (currentToken().type == TokenType::Punctuation && currentToken().value == ":") {
        advance();
        if (currentToken().type == TokenType::Identifier) advance();
    }

    std::vector<std::shared_ptr<Statement>> body;
    while (!(currentToken().type == TokenType::Keyword && currentToken().value == "end")) {
        auto stmt = parseStatement();
        if (stmt) body.push_back(stmt);
        else advance();
    }
    advance(); 

    return std::make_shared<FunctionDecl>(name, params, std::move(body));
}

Token Parser::peekToken(size_t offset) const {
    size_t idx = index + offset;
    if (idx < tokens.size()) return tokens[idx];
    return Token{TokenType::EndOfFile, "", 0, 0};
}
