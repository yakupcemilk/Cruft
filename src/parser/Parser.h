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

    std::shared_ptr<cruft::Program> parseProgram();
    std::shared_ptr<Statement> parse();

    // Peek ahead without consuming
    Token peekToken(size_t offset = 1) const;

  private:
    // for storing package name
    std::string packageName;

    // Handle 'package' and 'const' keywords
    std::shared_ptr<Statement> parsePackage();
    std::shared_ptr<Statement> parseConstDecl();

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

    // newly declared parsing helpers:
    std::shared_ptr<Statement> parseElseIfStatement();
    std::shared_ptr<Statement> parseElseStatement();
    std::shared_ptr<Statement> parseDoWhileStatement();
    std::shared_ptr<Statement> parseSwitchStatement();
    std::shared_ptr<Statement> parseCaseStatement();
    std::shared_ptr<Statement> parseBreakStatement();
    std::shared_ptr<Statement> parseClassDecl();
    std::shared_ptr<Statement> parseImportDecl();
    std::shared_ptr<Statement> parseExportDecl();
    std::shared_ptr<Statement> parseUseDecl();
    std::shared_ptr<Statement> parsePubModifier();
    std::shared_ptr<Statement> parsePriModifier();
    std::shared_ptr<Statement> parseSecureDecl();
    std::shared_ptr<Statement> parseToClause();

    // Expression parsing
    std::shared_ptr<Expr> parseExpression();
    std::shared_ptr<Expr> parseBinaryExpression(int precedence);
    std::shared_ptr<Expr> parsePrimaryExpression();
    int getPrecedence(const Token& token);
  };
}