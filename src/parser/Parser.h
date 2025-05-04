#pragma once
#include "../lexer/Lexer.h"
#include <memory>
#include <vector>

#include "stm/Statement.h"
#include "stm/Expr.h"
#include "stm/IfStatement.h"
#include "stm/WhileStatement.h"
#include "stm/BinaryExpr.h"
#include "stm/VariableExpr.h"
#include "stm/IntegerExpr.h"

class Parser {
    public:
        Parser(const std::vector<Token>& tokens) : tokens(tokens), index(0) {}
    
        std::shared_ptr<Statement> parse();
    
    private:
        const std::vector<Token>& tokens;
        size_t index;
    
        Token currentToken();
        void advance();
        
        std::shared_ptr<Statement> parseStatement();
        std::shared_ptr<Expr> parseExpression();
        
        std::shared_ptr<Statement> parseIfStatement();
        std::shared_ptr<Statement> parseWhileStatement();
        std::shared_ptr<Statement> parseReturnStatement();
        std::shared_ptr<Statement> parsePrintStatement();
    
        std::shared_ptr<Expr> parseBinaryExpression(int precedence);
        std::shared_ptr<Expr> parsePrimaryExpression();
        std::shared_ptr<Expr> parseVariableOrLiteral();
        
        int getPrecedence(const Token& token);
    };
    