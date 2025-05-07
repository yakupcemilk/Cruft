// src/parser/Parser.h
#pragma once

#include <memory>
#include <vector>
#include <string>
#include "../lexer/Token.h"
#include "AST.h"
#include "stm/Statement.h"
#include "stm/Expr.h"
#include "stm/IfStatement.h"
#include "stm/WhileStatement.h"
#include "stm/ReturnStatement.h"
#include "stm/PrintStatement.h"
#include "stm/BinaryExpr.h"
#include "stm/VariableExpr.h"
#include "stm/IntegerExpr.h"
#include "stm/AssignmentStatement.h"
#include "stm/VarDeclStatement.h"
#include "stm/FunctionDecl.h"

namespace cruft {
  class Parser {
  public:
    Parser(const std::vector<Token>& tokens)
      : tokens(tokens), index(0) {}

    // Parses entire token stream into a Program AST node
    std::shared_ptr<cruft::Program> parseProgram();
    std::shared_ptr<Statement> parse();

    // Peek ahead without consuming
    Token peekToken(size_t offset = 1) const;

    // Handle 'package' and 'const' keywords
    std::shared_ptr<Statement> parsePackage();
    std::shared_ptr<Statement> parseConstDecl();

  private:
    const std::vector<Token>& tokens;
    size_t index;

    Token currentToken();
    Token currentToken() const;
    void advance();

    // Statement parsing
    std::shared_ptr<Statement> parseStatement();
    std::shared_ptr<Statement> parseIfStatement();
    std::shared_ptr<Statement> parseWhileStatement();
    std::shared_ptr<Statement> parseReturnStatement();
    std::shared_ptr<Statement> parsePrintStatement();
    std::shared_ptr<Statement> parseVarDecl();
    std::shared_ptr<Statement> parseAssignment();
    std::shared_ptr<Statement> parseFunctionDecl();

    // Expression parsing
    std::shared_ptr<Expr> parseExpression();
    std::shared_ptr<Expr> parseBinaryExpression(int precedence);
    std::shared_ptr<Expr> parsePrimaryExpression();
    std::shared_ptr<Expr> parseVariableOrLiteral();

    int getPrecedence(const Token& token);
  };
}