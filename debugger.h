#ifndef DEBUGGER_H
#define DEBUGGER_H

#include <stdint.h>

// 1. Forward Declarations
// This tells the compiler: "There are structs with these names, pointers are 8 bytes."
typedef struct VMContext VMContext;
typedef struct State State;

// 2. Enum Definition
typedef enum {
    DEBUG_RUNNING,
    DEBUG_STEPPING,
    DEBUG_QUIT
} DebugMode;

// 3. Function Prototypes
// NOTICE: We removed 'static' and added 'struct' keyword before the types.
DebugMode debug_hook(State* state, VMContext* vm, int ip_offset);
int debug_disassembleInstruction(State* state, VMContext* vm, int32_t instruction, int offset);

// Other prototypes (ensure you use 'struct' here too)
void debug_printLocals(VMContext* vm);
void debug_printGlobals(VMContext* vm);
void debug_printRegisters(VMContext* vm);

#endif // DEBUGGER_H
