%{
#include "mathematics.tab.h"
#include <stdlib.h>
#include <string.h>

%}

%%
"package"        { return PACKAGE; }
"pub"            { return PUB; }
"fn"             { return FN; }
"ret"            { return RET; }
"end"            { return END; }
"if"             { return IF; }
"print"          { return PRINT; }
"const"          { return CONST; }
"i32"            { return I32; }
"("              { return '('; }
")"              { return ')'; }
":"              { return ':'; }
","              { return ','; }
"="              { return '='; }
[0-9]+           { yylval.ival = atoi(yytext); return NUMBER; }
[a-zA-Z_][a-zA-Z0-9_]* { yylval.str = strdup(yytext); return IDENTIFIER; }
[ \t\n]+         { /* Ignore whitespace */ }
.                { printf("Unexpected character: %s\n", yytext); }
%%

int yywrap(void) {
    return 1;
}
