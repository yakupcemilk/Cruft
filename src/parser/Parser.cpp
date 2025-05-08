// src/parser/Parser.cpp
#include "Parser.h"
#include "../lexer/Token.h"
#include "stm/AssignmentStatement.h"
#include "stm/BinaryExpr.h"
#include "stm/CallExpr.h"
#include "stm/FunctionDecl.h"
#include "stm/IfStatement.h"
#include "stm/IntegerExpr.h"
#include "stm/PrintStatement.h"
#include "stm/ReturnStatement.h"
#include "stm/VarDeclStatement.h"
#include "stm/VariableExpr.h"
#include "stm/WhileStatement.h"
#include <iostream>
#include <stdexcept>

using namespace cruft;

// Entry point: parse a sequence of statements into a Program
std::shared_ptr<Program> Parser::parseProgram() {
  std::cerr << "[parser] Entering parseProgram, tokens size = " << tokens.size()
            << "\n";

  auto prog = std::make_shared<Program>();

  while (currentToken().type != TokenType::EndOfFile) {
    std::cerr << "[parser] Current token: type=" << (int)currentToken().type
              << ", value='" << currentToken().value << "'\n";

    auto stmt = parseStatement();

    if (stmt) {
      std::cerr << "[parser] Parsed a statement successfully\n";
      prog->statements.push_back(stmt);
    } else {
      std::cerr << "[parser] parseStatement() returned null, advancing\n";
      advance(); // Avoid infinite loop
    }
  }

  std::cerr << "[parser] Finished parseProgram\n";
  return prog;
}

// Peek / current / advance
Token Parser::peekToken(size_t offset) const {
  size_t idx = index + offset;
  return (idx < tokens.size() ? tokens[idx]
                              : Token{TokenType::EndOfFile, "", 0, 0});
}

Token Parser::currentToken() const { return tokens[index]; }
Token Parser::currentToken() { return tokens[index]; }
void Parser::advance() {
  if (index + 1 < tokens.size())
    ++index;
}

// --- Statement parsing (unchanged) ---
std::shared_ptr<Statement> Parser::parseStatement() {
  if (currentToken().type == TokenType::Keyword) {
    if (currentToken().value == "package") {
      // ignore package <ident>
      return parsePackage();
    }
    if (currentToken().value == "const") {
      return parseConstDecl();
    }
    if (currentToken().value == "if")
      return parseIfStatement();
    if (currentToken().value == "while")
      return parseWhileStatement();
    if (currentToken().value == "ret")
      return parseReturnStatement();
    if (currentToken().value == "print")
      return parsePrintStatement();
    if (currentToken().value == "var")
      return parseVarDecl();
    if (currentToken().value == "fn")
      return parseFunctionDecl();
    // unrecognized keyword: skip it
    advance();
    return nullptr;
  }
  if (currentToken().type == TokenType::Identifier &&
      peekToken().type == TokenType::Operator && peekToken().value == "=") {
    return parseAssignment();
  }
  return nullptr;
}

// consume "package <ident>" and ignore
std::shared_ptr<Statement> Parser::parsePackage() {
  advance(); // consume 'package'
  if (currentToken().type == TokenType::Identifier)
    advance();
  return nullptr;
}

std::shared_ptr<Statement> Parser::parseConstDecl() {
  advance(); // consume 'const'
  // skip name, colon, and type if present
  if (currentToken().type == TokenType::Identifier)
    advance();
  if (currentToken().value == ":")
    advance();
  if (currentToken().type == TokenType::Identifier)
    advance();
  // optional initializer; just parse & discard
  if (currentToken().value == "=") {
    advance();
    auto _ = parseExpression();
    (void)_;
  }
  return nullptr; // ignore const declarations
}

std::shared_ptr<Statement> Parser::parseIfStatement() {
  advance(); // consume 'if'
  auto cond = parseExpression();
  std::vector<std::shared_ptr<Statement>> thenBranch;
  while (!(currentToken().type == TokenType::Keyword &&
           currentToken().value == "else") &&
         !(currentToken().type == TokenType::Keyword &&
           currentToken().value == "end")) {
    thenBranch.push_back(parseStatement() ?: (advance(), nullptr));
  }
  std::vector<std::shared_ptr<Statement>> elseBranch;
  if (currentToken().value == "else") {
    advance();
    while (!(currentToken().type == TokenType::Keyword &&
             currentToken().value == "end")) {
      elseBranch.push_back(parseStatement() ?: (advance(), nullptr));
    }
  }
  if (currentToken().value == "end")
    advance();
  return std::make_shared<IfStatement>(cond, thenBranch, elseBranch);
}

std::shared_ptr<Statement> Parser::parseWhileStatement() {
  advance(); // 'while'
  auto cond = parseExpression();
  std::vector<std::shared_ptr<Statement>> body;
  while (!(currentToken().type == TokenType::Keyword &&
           currentToken().value == "end")) {
    body.push_back(parseStatement() ?: (advance(), nullptr));
  }
  advance(); // consume 'end'
  return std::make_shared<WhileStatement>(cond, body);
}

std::shared_ptr<Statement> Parser::parseReturnStatement() {
  advance(); // 'ret'
  auto expr = parseExpression();
  return std::make_shared<ReturnStatement>(expr);
}

std::shared_ptr<Statement> Parser::parsePrintStatement() {
  advance(); // 'print'
  if (currentToken().value != "(")
    throw std::runtime_error("Expected '(' after print");
  advance();
  auto expr = parseExpression();
  if (currentToken().value != ")")
    throw std::runtime_error("Expected ')'");
  advance();
  return std::make_shared<PrintStatement>(expr);
}

std::shared_ptr<Statement> Parser::parseVarDecl() {
  advance(); // 'var'
  if (currentToken().type != TokenType::Identifier)
    throw std::runtime_error("Expected identifier in var decl");
  std::string name = currentToken().value;
  advance();
  if (currentToken().value != ":")
    throw std::runtime_error("Expected ':'");
  advance();
  if (currentToken().type != TokenType::Identifier)
    throw std::runtime_error("Expected type after ':'");
  advance();
  std::shared_ptr<Expr> init = nullptr;
  if (currentToken().value == "=") {
    advance();
    init = parseExpression();
  }
  return std::make_shared<VarDeclStatement>(name, init);
}

std::shared_ptr<Statement> Parser::parseAssignment() {
  std::string name = currentToken().value;
  advance(); // identifier
  advance(); // '='
  auto val = parseExpression();
  return std::make_shared<AssignmentStatement>(name, val);
}

std::shared_ptr<Statement> Parser::parseFunctionDecl() {
  advance(); // 'fn'
  std::string name = currentToken().value;
  advance();
  std::vector<std::string> params;
  if (currentToken().value == "(") {
    advance();
    while (currentToken().value != ")") {
      if (currentToken().type == TokenType::Identifier) {
        params.push_back(currentToken().value);
        advance();
      }
      if (currentToken().value == ",")
        advance();
    }
    advance(); // ')'
  }
  if (currentToken().value == ":") {
    advance(); // return type (skip)
    advance();
  }
  std::vector<std::shared_ptr<Statement>> body;
  int safety = 0;
  while (!(currentToken().type == TokenType::Keyword &&
           currentToken().value == "end")) {

    std::cerr << "[parser] Inside function body, current token: "
              << currentToken().value << "\n";

    auto stmt = parseStatement();
    if (stmt) {
      body.push_back(stmt);
    } else {
      std::cerr << "[parser] Null statement inside function, advancing\n";
      advance();
    }

    if (++safety > 1000) {
      std::cerr << "[parser] Safety break: too many iterations in function "
                   "body parsing\n";
      break;
    }
  }
  advance(); // 'end'
  return std::make_shared<FunctionDecl>(name, params, body);
}

// --- Expression parsing ---

std::shared_ptr<Expr> Parser::parseExpression() {
  return parseBinaryExpression(0);
}

int Parser::getPrecedence(const Token &tok) {
  if (tok.type != TokenType::Operator)
    return -1;
  if (tok.value == "+" || tok.value == "-")
    return 10;
  if (tok.value == "*" || tok.value == "/")
    return 20;
  return -1;
}

std::shared_ptr<Expr> Parser::parseBinaryExpression(int exprPrec) {
  auto lhs = parsePrimaryExpression();
  while (true) {
    int prec = getPrecedence(currentToken());
    if (prec < exprPrec)
      break;
    std::string op = currentToken().value;
    advance();
    auto rhs = parseBinaryExpression(prec + 1);
    lhs = std::make_shared<BinaryExpr>(
        lhs, Token{TokenType::Operator, op, 0, 0}, rhs);
  }
  return lhs;
}

std::shared_ptr<Expr> Parser::parsePrimaryExpression() {
  if (currentToken().type == TokenType::Integer) {
    int val = std::stoi(currentToken().value);
    advance();
    return std::make_shared<IntegerExpr>(val);
  }
  if (currentToken().type == TokenType::Identifier) {
    std::string name = currentToken().value;
    if (peekToken().value == "(") {
      // call expr
      advance(); // name
      advance(); // '('
      std::vector<std::shared_ptr<Expr>> args;
      while (currentToken().value != ")") {
        args.push_back(parseExpression());
        if (currentToken().value == ",")
          advance();
      }
      advance(); // ')'
      return std::make_shared<CallExpr>(name, args);
    } else {
      advance();
      return std::make_shared<VariableExpr>(name);
    }
  }
  if (currentToken().value == "(") {
    advance();
    auto e = parseExpression();
    if (currentToken().value != ")")
      throw std::runtime_error("Expected ')'");
    advance();
    return e;
  }
  throw std::runtime_error("Unknown primary expression");
}