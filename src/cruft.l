%{
#include "mathematics.tab.h"
#include <stdlib.h>
#include <string.h>
void showError();
%}

numbers          ([0-9])+
strings          [a-zA-Z_][a-zA-Z0-9_]* 
whitespace       [ \t\n]+ 

%%
"package"        { return (PACK); }
"secure"         { return (SEC); }
"to"             { return (TO); }
"import"         { return (IMPORT); }
"use"            { return (USE); }
"pub"            { return (PUB); }
"pri"            { return (PRI); }
"fn"             { return (FUNC); }
"ret"            { return (RETURN); }
"end"            { return (END); }
"if"             { return (IF); }
"else"           { return (ELSE); }
"switch"         { return (SWI); }
"case"           { return (CASE); }
"print"          { return (PRINT); }
"const"          { return (CONST); }
"var"            { return (VAR); }
"mut"            { return (MUT); }
"i64"            { return (I_64); }
"i32"            { return (I_32); }
"i16"            { return (I_16); }
"i8"             { return (I_8); }
"f64"            { return (F_64); }
"f32"            { return (F_32); }
"f16"            { return (F_16); }
"f8"             { return (F_8); }
"null"           { return (NULL); }
"("              { return (LEFT_PARENT); }
")"              { return (RIGHT_PARENT); }
"["              { return (LEFT_ARR_PARENT); }
"]"              { return (RIGHT_ARR_PARENT); }
":"              { return (COLON); }
","              { return (COMMA); }
"="              { return (EQU); }
"-"              { return (SUB); }
"+"              { return (ADD); }
"*"              { return (MUL); }
"/"              { return (DIV); }
"!"              { return (BAD); }
"//"             { return (COMMENT); }
{numbers}        { yylval.numbers = atoi(yytext); return (NUM);}
{strings}        { sscanf(yytext, "%s", yylval.str); return (IDENTIFIER); }
{whitespace}     { /* Ignore whitespace */ }
.                { showError(); return (OTHER); }
%%

int yywrap(void) {
    return 1;
}

void showError() {
    printf("Unexpected syntax. LOG ERR: 001");
}