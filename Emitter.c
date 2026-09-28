#include "Emitter.h"
#include <stdlib.h>
#include <stdbool.h>

// Creates a node and appends it to the list. 
// Returns the pointer to the new node (useful if we need to patch it later).
IRNode* emitNode(State* state, IREntry entry) {
    if (entry.loc.pos.line <= 0 && state)
        entry.loc = state->current_loc;
    IRNode* node = (IRNode*)arena_alloc(&state->irArena, sizeof(IRNode), sizeof(void*));
    if (!node) return NULL;
    node->data = entry;
    node->next = NULL;
    node->prev = state->ir_tail;

    if (state->ir_tail) {
        state->ir_tail->next = node;
    }
    else {
        state->ir_head = node; // First node
    }
    state->ir_tail = node;

    return node;
}


// Helper for Label IDs
int createLabel(State* state) {
    return ++state->next_label_id;
}

void emitABC_IR(State* state, OPCode op, int a, int b, int c) {
    IREntry ir = { 0 };
    ir.type = IR_INSTRUCTION;
    ir.op = op;
    ir.a = a;
    ir.b = b;
    ir.c = c;
    ir.is_AD = 0;
    emitNode(state, ir);
}

void emitAD_IR(State* state, OPCode op, int a, short d) {
    IREntry ir = { 0 };
    ir.type = IR_INSTRUCTION;
    ir.op = op;
    ir.a = a;
    ir.c = d; // Store D in the C slot
    ir.is_AD = 1;
    emitNode(state, ir);
}

void emitConstant_IR(State* state, Value value, int destRegister) {
    if (value.type.baseType == T_INT && value.v.i32 >= -32768 && value.v.i32 <= 32767) {
        emitAD_IR(state, OP_LOAD_Ki, destRegister, (short)value.v.i32);
        return;
    }
    int constIndex = addConstant(state, value);
    emitAD_IR(state, OP_LOAD_K, destRegister, (short)constIndex);
}

// Logic remains: Create a label ID, return the ID.
int emitJump_IR(State* state, OPCode opcode, int conditionRegister) {
    int label_id = createLabel(state);

    IREntry ir = { 0 };
    ir.type = IR_INSTRUCTION;
    ir.op = opcode;
    ir.a = conditionRegister;
    ir.label_id = label_id;

    emitNode(state, ir);

    return label_id;
}

// Logic remains: Append a Label Definition node.
void emitLabel_IR(State* state, int label_id) {
    IREntry ir = { 0 };
    ir.type = IR_LABEL_DEF;
    ir.label_id = label_id;

    emitNode(state, ir);
}

void patchJump8_IR(State* state, int label_id) {
    emitLabel_IR(state, label_id);
}

// --- Loops ---

void emitLoopJump_IR(State* state, int loopStartLabelID) {
    IREntry ir = { 0 };
    ir.type = IR_INSTRUCTION;
    ir.op = OP_JUMP;
    ir.label_id = loopStartLabelID;

    emitNode(state, ir);
}

void patchC_IR(IRNode* node, int c) {
    if (node && node->data.type == IR_INSTRUCTION) {
        node->data.c = c;
    }
}


//-------------------------------------------------------------------------------------
// assambler function

extern inline void emitABC(State* state, OPCode op, int a, int b, int c) {
    int32 instruction = (op) | (a << 8) | (b << 16) | (c << 24);
    array_append(&state->bytecode, &instruction);
}

extern inline void emitAD(State* state, OPCode op, int a, short d) {
    int32 instruction = (op) | (a << 8) | (((uint16_t)d) << 16);
    array_append(&state->bytecode, &instruction);
}

void emitConstant(State* state, Value value, int destRegister) {
    if (value.type.baseType == T_INT && value.v.i32 >= -32768 && value.v.i32 <= 32767)
    {
        emitAD_IR(state, OP_LOAD_Ki, destRegister, value.v.i32);
        return;
    }
    int constIndex = addConstant(state, value);
    emitAD(state, OP_LOAD_K, destRegister, constIndex);    
}

int emitJump(State* state, OPCode opcode, int conditionRegister)
{
    emitAD_IR(state, opcode, conditionRegister, 0xFFFF);
    return state->bytecode.count - 1;
}

void patchJump(State* state, int instructionIndex) {
    int offset = state->bytecode.count - instructionIndex;
    int32* instruction_to_patch = (int32*)array_get(&state->bytecode, instructionIndex);
    OPCode opcode = GET_OP(*instruction_to_patch);
    int register_a = GET_A(*instruction_to_patch);
    *instruction_to_patch = (opcode) | (register_a << 8) | (offset << 16);
}

void patchC(State* state, int instructionIndex, int c) {
    int32* instruction_to_patch = (int32*)array_get(&state->bytecode, instructionIndex);
    OPCode opcode = GET_OP(*instruction_to_patch);
    int register_a = GET_A(*instruction_to_patch);
    int register_b = GET_B(*instruction_to_patch);
    *instruction_to_patch = (opcode) | (register_a << 8) | (register_b << 16) | c << 24;
}

// Patches the 'C' operand (Bits 24-31) with a signed 8-bit offset
void patchJump8(State* state, int instructionIndex) {
    int offset = state->bytecode.count - instructionIndex;

    // Safety Check: 8-bit signed range is -128 to 127
    if (offset > 127 || offset < -128) {
        state->compiler->onError(LOC_NONE, "Code block too large for optimized jump (-128 to 127 instructions).", state->compiler);
        return;
    }

    int32* instruction_to_patch = (int32*)array_get(&state->bytecode, instructionIndex);

    // Clear existing C (if any) and OR in the new offset
    // Note: We cast offset to uint8_t to handle negative numbers correctly in bitwise ops
    *instruction_to_patch = (*instruction_to_patch & 0x00FFFFFF) | ((offset & 0xFF) << 24);
}

void emitLoopJump(State* state, int loopStart) {
    int jumpAddress = state->bytecode.count;
    int offset = loopStart - jumpAddress;
    emitAD_IR(state, OP_JUMP, 0, offset);
}

//---------------------------------------------------------------------------------------------------
// The Raw Emitters (Used only by the Assembler now)
void rawEmitABC(State* state, OPCode op, int a, int b, int c) {
    int32 instruction = (op) | (a << 8) | (b << 16) | (c << 24);
    array_append(&state->bytecode, &instruction);
}

void rawEmitAD(State* state, OPCode op, int a, short d) {
    int32 instruction = (op) | (a << 8) | (((uint16_t)d) << 16);
    array_append(&state->bytecode, &instruction);
}
void rawEmitE(State* state, OPCode op, int value) {
    // Format: [OP:8] [Value:24]
    // Mask value to 24 bits to prevent overflow bleeding into opcode
    int32 instruction = (op) | ((value & 0xFFFFFF) << 8);
    array_append(&state->bytecode, &instruction);
}
