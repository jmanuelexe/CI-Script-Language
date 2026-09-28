#pragma once
#include "types.h"

//character type. The possible classification for the characters found on the source string
typedef enum CTYPE {
	ttError,
	ttWhitespace,
	ttSpecial,
	ttLeter,
	ttDigit
}CTYPE;

typedef struct
{
	Loc loc;
	TString text;
	TTOKEN type;					//the type of token it is
}Token;

typedef struct lexer
{
	//const char* s;		//pointer to the zero terminate string that contains the code
	const char* input;		// temporary string
	struct Loc loc;
}lexer;

void lex_init(struct lexer* self, const char* input, const char* filename);
lexer* lex_create(const char* input, const char* filename);
Token  lex_getNextToken(lexer* self);
Token lex_peek(lexer* self);
void lex_dispose(lexer** self);