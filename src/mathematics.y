%{
#include <stdio.h>
#include <stdlib.h>

void yyerror(const char *s);
int yylex(void);

%}

%union {
    int ival;
    char *str;
}

%token <str> IDENTIFIER
%token <ival> NUMBER
%token PACKAGE PUB FN RET END IF PRINT CONST I32
%type <ival> expression

%%

program:
    package_declaration function_list
    ;

package_declaration:
    PACKAGE IDENTIFIER { printf("Package: %s\n", $2); }
    ;

function_list:
    function_declaration
    | function_list function_declaration
    ;

function_declaration:
    PUB FN IDENTIFIER '(' param_list ')' ':' I32 statement_list { printf("Function: %s\n", $3); }
    ;

param_list:
    param
    | param_list ',' param
    ;

param:
    IDENTIFIER ':' I32 { printf("Parameter: %s\n", $1); }
    ;

statement_list:
    statement
    | statement_list statement
    ;

statement:
    assignment
    | function_call
    | if_statement
    ;

assignment:
    CONST IDENTIFIER ':' I32 '=' expression { printf("Assignment: %s = %d\n", $2, $5); }
    ;

function_call:
    IDENTIFIER '(' argument_list ')' { printf("Function call: %s\n", $1); }
    ;

argument_list:
    expression
    | argument_list ',' expression
    ;

expression:
    NUMBER
    | IDENTIFIER
    | expression '+' expression { $$ = $1 + $3; }
    ;

if_statement:
    IF '(' expression ')' statement_list { printf("If statement with condition %d\n", $3); }
    ;

%%

void yyerror(const char *s) {
    fprintf(stderr, "Error: %s\n", s);
}

int main() {
    return yyparse();
}
