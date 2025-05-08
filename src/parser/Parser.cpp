// src/parser/Parser.cpp
#include "Parser.h"
#include "../lexer/Token.h"
#include "stm/AssignmentStatement.h"
#include "stm/BinaryExpr.h"
#include "stm/CallExpr.h"
#include "stm/ConstDeclStatement.h"
#include "stm/Expr.h"
#include "stm/FunctionDecl.h"
#include "stm/IfStatement.h"
#include "stm/IntegerExpr.h"
#include "stm/PrintStatement.h"
#include "stm/ReturnStatement.h"
#include "stm/Statement.h"
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

  while (currentToken().type != TokenType::END_OF_FILE) {
    std::cerr << "[parser] Current token: type=" << (int)currentToken().type
              << ", lexeme='" << currentToken().lexeme << "'\n";
    size_t beforeIndex = index;
    auto stmt = parseStatement();

    if (stmt) {
      std::cerr << "[parser] Parsed a statement successfully\n";
      prog->statements.push_back(stmt);
    } else {
      if (index == beforeIndex) {
        std::cerr << "[parser] parseStatement() returned null and consumed no "
                     "tokens, advancing\n";
        advance();
      } else {
        std::cerr << "[parser] parseStatement() returned null but consumed "
                     "tokens, not advancing\n";
      }
    }
  }

  // assign parsed package name
  prog->packageName = packageName;
  std::cerr << "[parser] Finished parseProgram (package=" << packageName
            << ")\n";
  return prog;
}

// Peek / current / advance
Token Parser::peekToken(size_t offset) const {
  size_t idx = index + offset;
  return (idx < tokens.size() ? tokens[idx]
                              : Token{TokenType::END_OF_FILE, "", 0 });
}

Token Parser::currentToken() const { return tokens[index]; }
Token Parser::currentToken() { return tokens[index]; }
void Parser::advance() {
  if (index + 1 < tokens.size())
    ++index;
}

// --- Statement parsing (unchanged) ---
std::shared_ptr<Statement> Parser::parseStatement() {
  using T = TokenType;

  switch (currentToken().type) {
  case T::PACKAGE: {
    advance(); // 'package'
    if (currentToken().type == T::IDENTIFIER) {
      packageName = currentToken().lexeme;
      advance();
    }
    return nullptr;
  }

    // 1) Dispatch on every keyword token:
    switch (currentToken().type) {
    case T::FUNC: // fn
      return parseFunctionDecl();

    case T::CONST:
      return parseConstDecl();

    case T::VAR:
      return parseVarDecl();

    case T::IF:
      return parseIfStatement();

    case T::ELSEIF:
      return parseElseIfStatement();

    case T::ELSE:
      return parseElseStatement();

    case T::WHILE:
      return parseWhileStatement();

    case T::DO:
      return parseDoWhileStatement();

    case T::SWI: // switch
      return parseSwitchStatement();

    case T::CASE:
      return parseCaseStatement();

    case T::RETURN:
      return parseReturnStatement();

    case T::PRINT:
      return parsePrintStatement();

    case T::BREAK:
      return parseBreakStatement();

    case T::CLASS:
      return parseClassDecl();

    case T::IMPORT:
      return parseImportDecl();

    case T::EXPORT:
      return parseExportDecl();

    case T::USE:
      return parseUseDecl();

    case T::PUB:
      return parsePubModifier();

    case T::PRI:
      return parsePriModifier();

    case T::SEC:
      return parseSecureDecl();

    case T::TO:
      return parseToClause();

    case T::PACKAGE: {
      // consume 'package' and record the package name
      advance();
      if (currentToken().type == T::IDENTIFIER) {
        packageName = currentToken().lexeme;
        advance();
      }
      return nullptr;
    }

    default:
      break;
    }

    // 2) Fallback: an assignment (identifier = expr)
    if (currentToken().type == T::IDENTIFIER && peekToken().type == T::EQU) {
      return parseAssignment();
    }

    // 3) Nothing matched? skip one token to avoid an infinite loop.
    advance();
    return nullptr;
  }

  // assignment: <identifier> '=' <expr>
  if (currentToken().type == T::IDENTIFIER && peekToken().type == T::EQU) {
    return parseAssignment();
  }

  // nothing matched → skip token to avoid infinite loop
  advance();
  return nullptr;
}

// consume "package <ident>" and ignore
std::shared_ptr<Statement> Parser::parsePackage() {
  advance(); // consume 'package'
  if (currentToken().type == TokenType::IDENTIFIER) {
    packageName = currentToken().lexeme;
    advance();
  }
  return nullptr;
}

std::shared_ptr<Statement> Parser::parseConstDecl() {
  advance(); // consume 'const'

  // --- get the name ---
  if (currentToken().type != TokenType::IDENTIFIER)
    throw std::runtime_error("Expected IDENTIFIER after 'const'");
  std::string name = currentToken().lexeme;
  advance();

  // --- skip the `: type` ---
  if (currentToken().lexeme == ":") {
    advance();
    if (currentToken().type == TokenType::IDENTIFIER)
      advance();
  }

  // --- optional initializer ---
  std::shared_ptr<IntegerExpr> init = nullptr;
  if (currentToken().lexeme == "=") {
    advance();
    init = std::dynamic_pointer_cast<IntegerExpr>(parseExpression());
    if (!init)
      throw std::runtime_error("Expected integer initializer for const");
  }

  // return a real AST node instead of nullptr
  return std::make_shared<ConstDeclStatement>(name, init);
}

std::shared_ptr<Statement> Parser::parseIfStatement() {
  advance(); // consume 'if'
  auto cond = parseExpression();
  std::vector<std::shared_ptr<Statement>> thenBranch;

  while (
      currentToken().type != TokenType::ELSE &&
      currentToken().type != TokenType::END) {
    auto stmt = parseStatement();
    if (stmt) {
      thenBranch.push_back(stmt);
    } else {
      advance(); // to avoid infinite loop on null
    }
  }

  std::vector<std::shared_ptr<Statement>> elseBranch;
  if (currentToken().type == TokenType::ELSE) {
    advance();
    while (currentToken().type != TokenType::END) {
      auto stmt = parseStatement();
      if (stmt) {
        elseBranch.push_back(stmt);
      } else {
        advance();
      }
    }
  }

  if (currentToken().type == TokenType::END)
    advance();

  return std::make_shared<IfStatement>(cond, thenBranch, elseBranch);
}

std::shared_ptr<Statement> Parser::parseWhileStatement() {
  advance(); // 'while'
  auto cond = parseExpression();
  std::vector<std::shared_ptr<Statement>> body;
  while (
      !(currentToken().type != TokenType::END)) {
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
  if (currentToken().lexeme != "(")
    throw std::runtime_error("Expected '(' after print");
  advance();
  auto expr = parseExpression();
  if (currentToken().lexeme != ")")
    throw std::runtime_error("Expected ')'");
  advance();
  return std::make_shared<PrintStatement>(expr);
}

std::shared_ptr<Statement> Parser::parseVarDecl() {
  advance(); // 'var'
  if (currentToken().type != TokenType::IDENTIFIER)
    throw std::runtime_error("Expected IDENTIFIER in var decl");
  std::string name = currentToken().lexeme;
  advance();
  if (currentToken().lexeme != ":")
    throw std::runtime_error("Expected ':'");
  advance();
  if (currentToken().type != TokenType::IDENTIFIER)
    throw std::runtime_error("Expected type after ':'");
  advance();
  std::shared_ptr<Expr> init = nullptr;
  if (currentToken().lexeme == "=") {
    advance();
    init = parseExpression();
  }
  return std::make_shared<VarDeclStatement>(name, init);
}

std::shared_ptr<Statement> Parser::parseAssignment() {
  std::string name = currentToken().lexeme;
  advance(); // IDENTIFIER
  advance(); // '='
  auto val = parseExpression();
  return std::make_shared<AssignmentStatement>(name, val);
}

std::shared_ptr<Statement> Parser::parseFunctionDecl() {
  advance(); // 'fn'
  std::string name = currentToken().lexeme;
  advance();
  std::vector<std::string> params;
  if (currentToken().lexeme == "(") {
    advance();
    while (currentToken().lexeme != ")") {
      if (currentToken().type == TokenType::IDENTIFIER) {
        params.push_back(currentToken().lexeme);
        advance();
      }
      if (currentToken().lexeme == ",")
        advance();
    }
    advance(); // ')'
  }
  if (currentToken().lexeme == ":") {
    advance(); // return type (skip)
    advance();
  }
  std::vector<std::shared_ptr<Statement>> body;
  int safety = 0;
  while (
      !(currentToken().type != TokenType::END)) {

    std::cerr << "[parser] Inside function body, current token: "
              << currentToken().lexeme << "\n";

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
  if (tok.type != TokenType::OPERATOR)
    return -1;
  if (tok.lexeme == "+" || tok.lexeme == "-")
    return 10;
  if (tok.lexeme == "*" || tok.lexeme == "/")
    return 20;
  return -1;
}

std::shared_ptr<Expr> Parser::parseBinaryExpression(int exprPrec) {
  auto lhs = parsePrimaryExpression();
  while (true) {
    int prec = getPrecedence(currentToken());
    if (prec < exprPrec)
      break;
    std::string op = currentToken().lexeme;
    advance();
    auto rhs = parseBinaryExpression(prec + 1);
    lhs = std::make_shared<BinaryExpr>(
      lhs, Token{TokenType::OPERATOR, op, 0}, rhs);
  }
  return lhs;
}

std::shared_ptr<Expr> Parser::parsePrimaryExpression() {
  if (currentToken().type == TokenType::NUM) {
    int val = std::stoi(currentToken().lexeme);
    advance();
    return std::make_shared<IntegerExpr>(val);
  }
  if (currentToken().type == TokenType::IDENTIFIER) {
    std::string name = currentToken().lexeme;
    if (peekToken().lexeme == "(") {
      // call expr
      advance(); // name
      advance(); // '('
      std::vector<std::shared_ptr<Expr>> args;
      while (currentToken().lexeme != ")") {
        args.push_back(parseExpression());
        if (currentToken().lexeme == ",")
          advance();
      }
      advance(); // ')'
      return std::make_shared<CallExpr>(name, args);
    } else {
      advance();
      return std::make_shared<VariableExpr>(name);
    }
  }
  if (currentToken().lexeme == "(") {
    advance();
    auto e = parseExpression();
    if (currentToken().lexeme != ")")
      throw std::runtime_error("Expected ')'");
    advance();
    return e;
  }
  throw std::runtime_error("Unknown primary expression");
}

std::shared_ptr<Statement> Parser::parseElseIfStatement() {
  // consume 'elseif'
  advance();
  // parse condition
  auto cond = parseExpression();
  // skip 'then' if present
  if (currentToken().type == TokenType::THEN)
    advance();
  // parse then-branch until ELSEIF/ELSE/END
  while (currentToken().type != TokenType::ELSEIF &&
         currentToken().type != TokenType::ELSE &&
         currentToken().type != TokenType::END &&
         currentToken().type != TokenType::END_OF_FILE) {
    parseStatement();
  }
  return nullptr;
}

std::shared_ptr<Statement> Parser::parseElseStatement() {
  // consume 'else'
  advance();
  // parse else-branch until END
  while (currentToken().type != TokenType::END &&
         currentToken().type != TokenType::END_OF_FILE) {
    parseStatement();
  }
  return nullptr;
}

std::shared_ptr<Statement> Parser::parseDoWhileStatement() {
  // consume 'do'
  advance();
  // parse body until 'while'
  while (currentToken().type != TokenType::WHILE &&
         currentToken().type != TokenType::END_OF_FILE) {
    parseStatement();
  }
  // consume 'while' and its condition
  if (currentToken().type == TokenType::WHILE) {
    advance();
    parseExpression();
  }
  return nullptr;
}

std::shared_ptr<Statement> Parser::parseSwitchStatement() {
  // consume 'switch'
  advance();
  // parse expression to switch on
  parseExpression();
  // parse cases until 'end'
  while (currentToken().type != TokenType::END &&
         currentToken().type != TokenType::END_OF_FILE) {
    parseStatement();
  }
  return nullptr;
}

std::shared_ptr<Statement> Parser::parseCaseStatement() {
  // consume 'case'
  advance();
  // parse case value
  parseExpression();
  // skip colon if present
  if (currentToken().type == TokenType::COLON)
    advance();
  // parse statements until next case/else/end
  while (currentToken().type != TokenType::CASE &&
         currentToken().type != TokenType::ELSE &&
         currentToken().type != TokenType::END &&
         currentToken().type != TokenType::END_OF_FILE) {
    parseStatement();
  }
  return nullptr;
}

std::shared_ptr<Statement> Parser::parseBreakStatement() {
  // consume 'break'
  advance();
  return nullptr;
}

std::shared_ptr<Statement> Parser::parseClassDecl() {
  // consume 'class'
  advance();
  // class name
  if (currentToken().type == TokenType::IDENTIFIER)
    advance();
  // skip body until 'end'
  while (currentToken().type != TokenType::END &&
         currentToken().type != TokenType::END_OF_FILE) {
    parseStatement();
  }
  return nullptr;
}

std::shared_ptr<Statement> Parser::parseImportDecl() {
  advance(); // 'import'
  if (currentToken().type == TokenType::STRING ||
      currentToken().type == TokenType::IDENTIFIER) {
    advance();
  }
  return nullptr;
}

std::shared_ptr<Statement> Parser::parseExportDecl() {
  advance(); // 'export'
  // reuse import logic
  return parseImportDecl();
}

std::shared_ptr<Statement> Parser::parseUseDecl() {
  advance(); // 'use'
  if (currentToken().type == TokenType::IDENTIFIER)
    advance();
  return nullptr;
}

std::shared_ptr<Statement> Parser::parsePubModifier() {
  advance(); // 'pub'
  return nullptr;
}

std::shared_ptr<Statement> Parser::parsePriModifier() {
  advance(); // 'pri'
  return nullptr;
}

std::shared_ptr<Statement> Parser::parseSecureDecl() {
  advance(); // 'secure'
  return nullptr;
}

std::shared_ptr<Statement> Parser::parseToClause() {
  advance(); // 'to'
  // likely followed by type or target
  if (currentToken().type == TokenType::IDENTIFIER)
    advance();
  return nullptr;
}
