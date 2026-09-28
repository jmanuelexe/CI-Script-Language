#pragma once
#include "types.h"
#include "opcode.h"

typedef enum {
    IR_INSTRUCTION, // Represents a standard Opcode (e.g., OP_LOAD, OP_JUMP)
    IR_LABEL_DEF    // Represents a destination marker (e.g., "Label 5:")
} IRType;

typedef struct {
    IRType type;
    OPCode op;
    int a;
    int b;
    int c;        // Standard 'C' operand OR 'D' operand
    int label_id; // For INSTRUCTION: The target label (if jump). 
    char is_AD;
    Loc loc;
    // For LABEL_DEF: The ID of the label being defined.
} IREntry;

typedef struct IRNode {
    IREntry data;
    struct IRNode* next;
    struct IRNode* prev;
} IRNode;


IRNode* emitNode(State* state, IREntry entry);

int createLabel(State* state);

void emitABC_IR(State* state, OPCode op, int a, int b, int c);
void emitAD_IR(State* state, OPCode op, int a, short d);
void emitConstant_IR(State* state, Value value, int destRegister);
int emitJump_IR(State* state, OPCode opcode, int conditionRegister);
void emitLabel_IR(State* state, int label_id);
void patchJump8_IR(State* state, int label_id);
void emitLoopJump_IR(State* state, int loopStartLabelID);
void patchC_IR(IRNode* node, int c);

inline void emitAD(State* state, OPCode op, int a, short d);
inline void emitABC(State* state, OPCode op, int a, int b, int c);
int emitJump(State* state, OPCode opcode, int conditionRegister);
void patchJump(State* state, int instructionIndex);
void patchC(State* state, int instructionIndex, int c);
void patchJump8(State* state, int instructionIndex);
void emitLoopJump(State* state, int loopStart);
void rawEmitABC(State* state, OPCode op, int a, int b, int c);
void rawEmitAD(State* state, OPCode op, int a, short d);
void rawEmitE(State* state, OPCode op, int value);
int addConstant(State* state, Value value);
int getCurrentInstPos(State* state, OPCode opcde, int a, int b, int c);
