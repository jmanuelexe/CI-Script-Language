#pragma once
#include <stdint.h>

// 1. Define the masks and shifts for your fields. This is your standard.
#define OPCODE_CODE_SHIFT      (0)
#define OPCODE_CODE_MASK       (0xFF)   // 6 bits (2^8 - 1)=255
#define ADDRESS_SHIFT   (6)
#define ADDRESS_MASK    (0x3FF)  // 10 bits (2^10 - 1)=1023
#define OPCODE_IMMEDIATE_SHIFT (16)
#define OPCODE_IMMEDIATE_MASK  (0xFFFF) // 16 bits (2^16 - 1)=65535

#define MASK_26BITS 0x03FFFFFF
#define SHIFT_26BITS 26 // 32 - 6 (for opcode)

// Maximun negative value for a 26-bit signed integer (-2^25)
#define SIGNED_MAX_26_BIT (33554431) // (1 << 25) - 1
// Minimum negative value for a 26-bit signed integer (-2^25)
#define SIGNED_MIN_26_BIT (-33554432) // -(1 << 25)

#define GET_OPCODE(opcode) get_opcode(opcode, OPCODE_CODE_MASK, OPCODE_CODE_SHIFT)
#define GET_VALUE1(opcode) get_opcode(opcode, ADDRESS_MASK, ADDRESS_SHIFT)
#define GET_VALUE2(opcode) get_opcode(opcode, OPCODE_IMMEDIATE_MASK, OPCODE_IMMEDIATE_SHIFT)
#define GET_IMMEDIETE(opcode) get_opcode(opcode, MASK_26BITS, SHIFT_26BITS)
#define SET_OPCODE(opcode, value) opcode=set_opcode(opcode, value, OPCODE_CODE_MASK, OPCODE_CODE_SHIFT)
#define SET_ADDRESS(opcode, value) opcode=set_opcode(opcode, value, ADDRESS_MASK, ADDRESS_SHIFT)
#define SET_IMMEDIATE(opcode, value) opcode=set_opcode(opcode, value, OPCODE_IMMEDIATE_MASK, OPCODE_IMMEDIATE_SHIFT)
#define SET_LONGVALUE(opcode, value) opcode=set_opcode(opcode, value, MASK_26BITS, SHIFT_26BITS)
uint32_t set_opcode(uint32_t opcode, uint32_t value, uint32_t mask, uint8_t shift);
uint32_t get_opcode(uint32_t opcode, uint32_t mask, uint8_t shift);

typedef enum {
    // --- Constant Loading ---
    OP_NOP,
    OP_MOVE,
    OP_LOAD_Ki,
    OP_LOAD_K,
    OP_LOAD_NULL,
    OP_LOAD8_LOCAL,
    OP_LOAD32_LOCAL,
    OP_LOADf_LOCAL,
    OP_LOADs_LOCAL,
    OP_LOADp_LOCAL,

    OP_LOADb_GLOBAL,
    OP_LOADi_GLOBAL,
    OP_LOADf_GLOBAL,
    //OP_LOADs_GLOBAL,
    OP_LOADp_GLOBAL,

    OP_STORE32_GLOBAL_K,
    OP_STORE32_LOCAL_K,

    OP_STORE8_LOCAL,
    OP_STORE32_LOCAL,
    OP_STOREp_LOCAL,
    
    OP_STORE8_GLOBAL,
    OP_STORE32_GLOBAL,
    OP_STOREp_GLOBAL,
        
    OP_STORE_FIELDi,
    OP_STORE_FIELDb,
    OP_STORE_FIELDf,
    OP_LOAD_FIELDi,
    OP_LOAD_FIELDf,
    OP_LOAD_FIELDb,

	OP_GET_THIS_FIELDp, // super optimized get this.field
    OP_GET_FIELDp,
    OP_STORE_FIELDp,
    OP_ALLOCATE_ARRAY_PTR,
    OP_ALLOCATE,
    OP_NEWARRAY,
    // --- Indexed Operations ---
    OP_LOAD_LVAR_IDX,
    OP_STORE_LVAR_IDX,
    OP_LOAD_GVAR_IDX,
    OP_STORE_GVAR_IDX,

    // --- Arithmetic ---
    OP_ADD,OP_ADDf,
    OP_SUB,OP_SUBf,
    OP_MUL,OP_MULf,
    OP_DIV,OP_DIVf,

	//unary operations
	OP_NOT, OP_NEG, OP_NEGf,
    OP_EQ_REF,
	OP_EQ, OP_EQf,
    OP_NE, OP_NEf,
	OP_LT, OP_LTf,
    OP_GT, OP_GTf,
	OP_LE, OP_LEf,
    OP_GE, OP_GEf,

	//Cast operations
    OP_CAST_I2F,
	OP_CAST_F2I,
	//--string operations
	OP_CONCAT,
    OP_INCi_LOCAL,
    OP_INCf_LOCAL,
    OP_INCi_GLOBAL,
    OP_INCf_GLOBAL,
    OP_DECi_LOCAL,
    OP_DECf_LOCAL,
    OP_DECi_GLOBAL,
    OP_DECf_GLOBAL,
    OP_FOR_DEC,
    OP_FOR_INC,

    // --- Stack & Control Flow ---
	OP_JEQ,OP_JNE,
    OP_JLT,OP_JLE,
    OP_JGT,OP_JGE,

    OP_JEQf, OP_JNEf,
    OP_JLTf, OP_JLEf,
    OP_JGTf, OP_JGEf,

    OP_JUMP_IF_FALSE,
    OP_JUMP_IF_TRUE,
    //OP_POP,
    OP_JUMP,
    OP_HALT,
    OP_CALL_C,
    OP_CALL_C32, // lon call used when calling a functionid longer than 255
	OP_CALL,
	OP_CALL32,
    OP_RETURN,
    OP_RETURN_PRIM,

    OP_ALOAD_I,  // Load Int from Array
    OP_ALOAD_F,  // Load Float from Array
    OP_ALOAD_P,  // Load Pointer from Array

    OP_ASTORE_I, // Store Int to Array
    OP_ASTORE_F, // Store Float to Array
    OP_ASTORE_P, // Store Pointer to Array

    OP_FUNC_DEF,
    OP_DEF_SETUP,
    OP_DEF_UPDATE,
    OP_DEF_FIXED_UPDATE,
    OP_DEF_TRIGGER_ENTER,
    OP_DEF_TRIGGER_STAY,
    OP_DEF_TRIGGER_EXIT,
    OP_DEF_NETWORK_REMOTE,

	OP_DEF_K_FLOAT,  // Define Float: [OP] [FLOAT_BITS(4)]
	OP_DEF_K_INT,   // Define Int:   [OP] [INT_BITS(4)]
    OP_DEF_K_STR,   // Define String: [OP] [LENGTH] [CHAR_DATA...]
    OP_DEF_K_BOOL, // <--- Add this
    OP_DEF_K_CHAR, // <--- Add this (optional, but good practice)

    // Float math intrinsics. Appended to preserve existing opcode values.
    OP_SQRTf,
    OP_SINf,
    OP_COSf,
    OP_ABSf,
    OP_ATAN2f,
    OP_MINf,
    OP_MAXf,
    // A = signed 8-bit increment, D = primitive local slot.
    OP_ADDi_LOCAL_IMM,
    // A = primitive local slot, B = integer constant index, C = signed jump.
    OP_JEQ_LOCAL_K, OP_JNE_LOCAL_K,
    OP_JLT_LOCAL_K, OP_JLE_LOCAL_K,
    OP_JGT_LOCAL_K, OP_JGE_LOCAL_K,
    // Return arithmetic on two primitive locals, A and B (8-bit slots).
    OP_RETURN_ADD_LOCAL, OP_RETURN_SUB_LOCAL, OP_RETURN_MUL_LOCAL,
    OP_RETURN_ADDf_LOCAL, OP_RETURN_SUBf_LOCAL, OP_RETURN_MULf_LOCAL,
    // A = constant index, D = primitive local slot.
    OP_ADDf_LOCAL_K, OP_SUBf_LOCAL_K,
    // Preserve A while combining a primitive load with an argument/local store.
    OP_LOAD_STORE_LOCAL, // A = register, B = source slot, C = destination slot
    OP_LOADKi_STORE_LOCAL, // A = register, B = destination slot, C = signed int8
    // A = destination GP, B = array reference local, C = integer index local.
    OP_ALOAD_I_LOCALS, OP_ALOAD_F_LOCALS,
    // A = integer local, B = signed step. Word 2 = signed integer bound;
    // word 3 = signed displacement from this opcode to the loop body.
    OP_FOR_STEP_LT_I32, OP_FOR_STEP_LE_I32, OP_FOR_STEP_GT_I32, OP_FOR_STEP_GE_I32,
    // Common forward counted loop: A = integer local, implicit step +1.
    // Word 2 = signed integer bound; word 3 = signed displacement.
    OP_FOR_STEP1_LT_I32,
    // Width-aware primitive local operations. Slot addressing remains stable.
    OP_LOAD8_TYPED_LOCAL, OP_LOAD16_TYPED_LOCAL,
    OP_STORE8_TYPED_LOCAL, OP_STORE16_TYPED_LOCAL,
    OP_LOAD8_TYPED_GLOBAL, OP_LOAD16_TYPED_GLOBAL,
    OP_STORE8_TYPED_GLOBAL, OP_STORE16_TYPED_GLOBAL,
    OP_NEWARRAY_TYPED,
    OP_ALOAD_I8, OP_ALOAD_I16, OP_ALOAD_F32,
    OP_ASTORE_I8, OP_ASTORE_I16, OP_ASTORE_F32,
    OP_ALLOCATE_PACKED_OBJECT,
    OP_LOAD_PACKED_I8, OP_LOAD_PACKED_I16, OP_LOAD_PACKED_I32, OP_LOAD_PACKED_F32,
    OP_STORE_PACKED_I8, OP_STORE_PACKED_I16, OP_STORE_PACKED_I32, OP_STORE_PACKED_F32,
    OP_CALL_C_TYPED,
    OP_DESTROYp_LOCAL,
    OP_CLEARp_LOCAL,
    OP_CLONE_REF,
    OP_STORE_FIELDp_BORROW,
    OP_MARK_OWNED_LOCAL,
    OP_MARK_UNCLAIMED_REF,
    OP_DESTROY_UNCLAIMED,
    OP_DESTROY_UNCLAIMED_KEEP_REF,
    OP_COUNT
} OPCode;

#define GET_OP(i32)     ((i32) & 0xFF)
#define GET_A(i32)      (((i32) >> 8) & 0xFF)
#define GET_B(i32)      (((i32) >> 16) & 0xFF)
#define GET_C(i32)      (((i32) >> 24) & 0xFF)
#define GET_D(i32)      ((int16_t)(((uint32_t)(i32) >> 16) & 0xFFFF)) // Unsigned shift fix
#define GET_E(i) (((i) >> 8) & 0xFFFFFF)
