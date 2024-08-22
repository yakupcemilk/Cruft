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
%token <ival> NUM
%token PACK PUB PRI FUNC RETURN END IF ELSE SWI CASE PRINT CONST VAR MUT I_64 I_32 I_16 I_8 F_64 F_32 F_16 F_8 NULL LEFT_PARENT RIGHT_PARENT LEFT_ARR_PARENT RIGHT_ARR_PARENT COLON COMMA EQU SUB ADD MUL DIV BAD COMMENT NUM IDENTIFIER OTHER
%type  <ival> expression

%%

program:
    package_declaration include_list variable_statement function_list
    ;

package_declaration:
    | PACK IDENTIFIER { printf("Package: %s\n", $2); }
    ;

include_type:
    USE
    | IMPORT
    | OTHER
;

include_list:
    | include_type IDENTIFIER
    | OTHER
;

variable_type_list:
    CONST
    | VAR
    | MUT
    | OTHER
    ;

variables:
    IDENTIFIER COLON type_list
    | IDENTIFIER
    | OTHER
    ;

variables_list:
    | variables_list COMMA variables
    | OTHER
    ;

variable_statement:
    variable_type_list variables_list
    | variable_statement  COMMA COLON type_list
    ;

arithmetic_statements:
    ADD
    | SUB
    | MUL
    | DIV
    | OTHER
    ;

function_list:
    function_declaration
    | function_list function_declaration
    ;

function_declaration:
    security FN IDENTIFIER LEFT_PARENT param_list RIGHT_PARENT COLON type_list statement_list END
    ;

security:
    PUB
    | PRI
    | OTHER
    ;

type_list:
    i_list
    | f_list
    | OTHER
    ;

i_list:
    I_64
    | I_32
    | I_16
    | I_8
    | OTHER
    ;

f_list:
    F_64
    | F_32
    | F_16
    | F_8
    | OTHER
    ;

param_list:
    param
    | param_list COMMA param
    | OTHER
    ;

param:
    IDENTIFIER COLON type_list
    ;

statement_list:
    statement
    | statement_list statement
    | OTHER
    ;

statement:
    assignment
    | function_call
    | if_statement
    | OTHER
    ;

assignment:
    variable_type_list IDENTIFIER COLON type_list EQU expression
    ;

function_call:
    IDENTIFIER LEFT_PARENT argument_list RIGHT_PARENT
    ;

argument_list:
    expression
    | argument_list COMMA expression
    | OTHER
    ;

expression:
    NUM
    | IDENTIFIER
    | expression arithmetic_statements expression 
    | OTHER
    ;

notability:
    | BAD
    ;

if_statement:
    IF LEFT_PARENT notability expression RIGHT_PARENT statement_list else_statement END 
    ;

else_statement:
    | ELSE LEFT_PARENT notability expression RIGHT_PARENT statement_list
    ;

elsebility:
    | else_statement
    ;

switch_statement:
    SWI LEFT_PARENT notability case RIGHT_PARENT statement_list
    ;

case:
    IDENTIFIER
    | OTHER
    ;

case_list
    case
    | case_list case
    | OTHER
    ;
%%

void yyerror(const char *s) {
    fprintf(stderr, "Error: %s\n", s);
}

int main() {
    yyparse();
    return 0;
}
