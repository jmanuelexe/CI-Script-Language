#pragma once
#include "AST.h"
#include "Lexer.h"

typedef struct Parser {
	struct lexer* lexer; // Pointer to the lexer
	Token token; // Current token being processed
	int error; // Error flag
	State* state;
}Parser;

static ASTNode* statement(Parser* p);

ASTNode* parse(const char* source, const char* filename, State* state);

static ASTNode* primary(Parser* p);

static ASTNode* grouping(Parser* p);

static ASTNode* unary(Parser* p);

static ASTNode* postfix(Parser* p, ASTNode* left);

static ASTNode* binary(Parser* p, ASTNode* left);

static ASTNode* call(Parser* p, ASTNode* left);

static ASTNode* subscript(Parser* p, ASTNode* left);

static ASTNode* member(Parser* p, ASTNode* left);

static ASTNode* parseNew(Parser* p);

static TypeID parseType(Parser* p);

char* readFile(const char* path);
