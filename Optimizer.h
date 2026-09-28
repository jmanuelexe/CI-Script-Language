#pragma once
#include "vm.h"
#include "opcode.h"

void optimize_ast(CompilerDef* compiler, ASTNode** root);
void optimizeIR(State* state);


void link_and_replace(State* s, array* ir);

bool isComparisonJump(OPCode op);
bool isForStepJump(OPCode op);
