#pragma once
#include "AST.h"
#include "array.h"
#include "opcode.h"
#include "stdbool.h"

//#define bool unsigned char
void printByteCode(State* state);
//void emit(State* state, OPCode opcode, TypeID opType, ...);
void generateIR(State* state, ASTNode* node);
bool compile(State* state, const char* filename);
void saveBytecode(State* state, const char* path);
void loadBytecode(State* state, const char* path);
static int disassembleInstruction(State* state, int offset);

State* createState();
void freeConstants(State* state);
void freeCompiler(CompilerDef* compiler);
void freeState(State* state);
void assemble(State* state);
