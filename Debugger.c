#include  "types.h"
#include "opcode.h"
#include "vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEBUG_JUMP_CASES(op) case OP_##op: printf("OP_" #op "       GP%d, GP%d, offset:%d (to %d) \n", a, b, (signed char)c, offset + c); break;
#define DEBUG_MATH_CASES(op) case OP_##op: printf("OP_" #op "       GP%d, GP%d, GP%d (%d" #op " %d)\n", a, b, c, vm->GP[b].i32,vm->GP[c].i32); break;
#define DEBUG_SG_CASES(op, LOCAL_GLOBAL) case op: printf(#op "      "#LOCAL_GLOBAL"[%d], GP%d\n", d, a); break;
#define DEBUG_SF_CASES(op, LOCAL_GLOBAL) case op: printf(#op "      "#LOCAL_GLOBAL"[%d], RP%d\n", d, a); break;
#define DEBUG_LG_CASES(op, LOCAL_GLOBAL) case op: printf(#op "       GP%d, "#LOCAL_GLOBAL"[%d]\n", a, d); break;
#define DEBUG_ALOAD_CASE(type) case type:   printf("" #type "        arr:RP%d, idx:GP%d, val:GP%d\n", a, b, c); break;
#define DEBUG_ASTORE_CASE(type) case type:   printf("" #type "        arr:RP%d, idx:GP%d(%d), val:GP%d(%d)\n", a, b, vm->GP[b], c, vm->GP[c]); break;

int debug_disassembleInstruction(State* state, VMContext* vm, int32 instruction, int offset) {
    OPCode op = GET_OP(instruction);
    int16_t a = GET_A(instruction);
    int16_t b = GET_B(instruction);
    int16_t c = GET_C(instruction);
    int16_t bp = vm ? vm->current_frame->prim_bp : 0;
    int16_t d = GET_D(instruction);
    int16_t e = GET_E(instruction);

    printf("%04d: ", offset);
    switch (op) {
	case OP_DEF_K_FLOAT: printf("OP_DEF_K_FLOAT    VALUE=%d\n", d); break;
	case OP_DEF_K_INT: printf("OP_DEF_K_INT    VALUE=%d\n", d); break;
	case OP_DEF_K_STR: printf("OP_DEF_K_STR    LENGTH=%d\n", d); break;
    case OP_FUNC_DEF :    printf("OP_FUNC_DEF         Address=%d\n", e); break;
    case OP_ALLOCATE:   printf("OP_ALLOCATE         RP%d, size=%d\n", a, d); break;
    case OP_LOAD_Ki:    printf("OP_LOAD_Ki          GP%d, %d\n", a, (int16_t)d); break;
    case OP_LOAD_K:
    {
        Value val = *(Value*)array_get(&vm->constants, d);
        if (val.type.baseType == T_STRING)
            printf("OP_LOAD_K        RP%d, k[%d]('%s')\n", a, d, (char*)val.v.p);
        else {
            printf("OP_LOAD_K        GP%d, k[%d]", a, d);
            if (val.type.baseType == T_INT) printf("(%d)\n", val.v.i32);
            else if (val.type.baseType == T_FLOAT) printf("(%f)\n", val.v.f32);
        }
    }
    break;
    case OP_STORE32_LOCAL_K:
    {
        Value val = *(Value*)array_get(&vm->constants, d);
        if (val.type.baseType == T_STRING)
            printf("OP_STORE32_LOCAL_K        RP%d, k[%d]('%s')\n", a, d, (char*)val.v.p);
        else
            printf("OP_STORE32_LOCAL_K        GP%d, k[%d]\n", a, d);
    }
    break;
    case OP_STORE32_GLOBAL_K:
    {
        Value val = *(Value*)array_get(&vm->constants, d);
        if (val.type.baseType == T_STRING)
             printf("OP_STORE32_GLOBAL_K        gloval[%d], k[%d]('%s')\n", a, d, (char*)val.v.p);
        else printf("OP_STORE32_GLOBAL_K        global[%d], k[%d]\n", a, d);
    }
    break;
    DEBUG_SG_CASES(OP_STORE8_LOCAL, local)
    DEBUG_SG_CASES(OP_STORE32_LOCAL, local)
    DEBUG_SF_CASES(OP_STOREp_LOCAL, localRef)
    DEBUG_LG_CASES(OP_LOAD8_LOCAL, local)
    DEBUG_LG_CASES(OP_LOAD32_LOCAL, local)
    DEBUG_LG_CASES(OP_LOADf_LOCAL, local)
    DEBUG_LG_CASES(OP_LOADp_LOCAL, localRef)// local[%d+%d]\n", a, bp, d

    DEBUG_SG_CASES(OP_STORE8_GLOBAL,    global)
    DEBUG_SG_CASES(OP_STORE32_GLOBAL,   global)
    DEBUG_SG_CASES(OP_STOREp_GLOBAL,   globalRef)
    DEBUG_LG_CASES(OP_LOADb_GLOBAL,     global)
    DEBUG_LG_CASES(OP_LOADi_GLOBAL,     global)
    DEBUG_LG_CASES(OP_LOADf_GLOBAL,     global)
    //DEBUG_LG_CASES(OP_LOADs_GLOBAL,     global)
    case OP_LOAD_NULL:      printf("OP_LOAD_NULL         RP%d, NULL\n", a); break;
    case OP_LOADp_GLOBAL:   printf("OP_LOADp_GLOBAL      RP%d, globalRef[%d]\n", a, d); break;
    case OP_GET_FIELDp :    printf("OP_GET_FIELDp        RP%d, obj:RP%d, slot:%d\n", a, b, c); break;
    case OP_LOAD_FIELDi:    printf("OP_LOAD_FIELDi       RP%d, obj:RP%d, slot:%d\n", a, b, c); break;
    case OP_LOAD_FIELDf:    printf("OP_LOAD_FIELDf       RP%d, obj:RP%d, slot:%d\n", a, b, c); break;
    case OP_STORE_FIELDi:   printf("OP_STORE_FIELDi      obj:RP%d, slot:%d, val:GP%d\n", a, b, c); break;
    case OP_STORE_FIELDf:   printf("OP_STORE_FIELDf      obj:RP%d, slot:%d, val:GP%d\n", a, b, c); break;
    case OP_STORE_FIELDp:   printf("OP_STORE_FIELDp      obj:RP%d, slot:%d, val:RP%d\n", a, b, c); break;
    case OP_STORE_FIELDp_BORROW: printf("OP_STORE_FIELDp_BORROW obj:RP%d, slot:%d, val:RP%d\n", a, b, c); break;
    case OP_CONCAT:         printf("OP_CONCAT            r%d, r%d, r%d\n", a, b, c); break;
		DEBUG_MATH_CASES(ADD);
		DEBUG_MATH_CASES(SUB);
		DEBUG_MATH_CASES(MUL);
		DEBUG_MATH_CASES(DIV);

        DEBUG_MATH_CASES(ADDf);
        DEBUG_MATH_CASES(SUBf);
        DEBUG_MATH_CASES(MULf);
        DEBUG_MATH_CASES(DIVf);
    case OP_JUMP:           printf("OP_JUMP              %d (to %d)\n", d, offset + d); break;
    case OP_JUMP_IF_FALSE:  printf("OP_JUMP_IF_F         r%d, %d (to %d)\n", a, d, offset + d); break;
    case OP_JUMP_IF_TRUE:   printf("OP_JUMP_IF_T         r%d, %d (to %d)\n", a, d, offset + d); break;
    case OP_CALL_C:         printf("OP_CALL_C            GP%d+%d, cfunc[%d], nargs=%d\n", a, bp, b, c); break;
    case OP_CALL:           printf("OP_CALL              GP%d+%d, addr=%d, nargs=%d\n", a, bp, b, c); break;
    case OP_RETURN:         printf("OP_RETURN            GP%d\n", a); break;
    case OP_RETURN_PRIM:    printf("OP_RETURN_PRIM       GP%d\n", a); break;
    case OP_DESTROYp_LOCAL: printf("OP_DESTROYp_LOCAL    refLocal[%d]\n", d); break;
    case OP_CLEARp_LOCAL:   printf("OP_CLEARp_LOCAL      refLocal[%d]\n", d); break;
    case OP_MARK_OWNED_LOCAL: printf("OP_MARK_OWNED_LOCAL  refLocal[%d]\n", d); break;
    case OP_MARK_UNCLAIMED_REF: printf("OP_MARK_UNCLAIMED_REF RP%d\n", a); break;
    case OP_DESTROY_UNCLAIMED: printf("OP_DESTROY_UNCLAIMED\n"); break;
    case OP_DESTROY_UNCLAIMED_KEEP_REF:
        printf("OP_DESTROY_UNCLAIMED_KEEP_REF RP%d\n", a); break;
    case OP_HALT:           printf("OP_HALT\n");         return 1;
    case OP_CAST_I2F:       printf("OP_CAST_I2F          GP%d, GP%d\n", a, d); break;
    case OP_CAST_F2I:       printf("OP_CAST_F2I          GP%d, GP%d\n", a, d); break;
    case OP_SQRTf:          printf("OP_SQRTf             GP%d, GP%d\n", a, b); break;
    case OP_SINf:           printf("OP_SINf              GP%d, GP%d\n", a, b); break;
    case OP_COSf:           printf("OP_COSf              GP%d, GP%d\n", a, b); break;
    case OP_ABSf:           printf("OP_ABSf              GP%d, GP%d\n", a, b); break;
    case OP_ATAN2f:         printf("OP_ATAN2f            GP%d, GP%d, GP%d\n", a, b, c); break;
    case OP_MINf:           printf("OP_MINf              GP%d, GP%d, GP%d\n", a, b, c); break;
    case OP_MAXf:           printf("OP_MAXf              GP%d, GP%d, GP%d\n", a, b, c); break;
    case OP_EQ:             printf("OP_EQ                GP%d, GP%d, GP%d\n", a, b, c); break;
    case OP_NE:             printf("OP_NEG               GP%d, GP%d, GP%d\n", a, b, c); break;
    case OP_NEf:            printf("OP_NEGf              GP%d, GP%d, GP%d\n", a, b, c); break;
    case OP_NEG:            printf("OP_NEG               GP%d, GP%d, GP%d\n", a, b, c); break;
    case OP_NEGf:           printf("OP_NEG               GP%d, GP%d, GP%d\n", a, b, c); break;
    case OP_LT:             printf("OP_LT                GP%d, GP%d, GP%d\n", a, b, c); break;
    case OP_LE:             printf("OP_LE                GP%d, GP%d, GP%d\n", a, b, c); break;
    case OP_GT:             printf("OP_GT                GP%d, GP%d, GP%d\n", a, b, c); break;
    case OP_GE:             printf("OP_GE                GP%d, GP%d, GP%d\n", a, b, c); break;
    case OP_FOR_INC:        printf("OP_FOR_INC           GP%d, GP%d, %d\n", a, b, offset + (char)c); break;
    case OP_FOR_DEC:        printf("OP_FOR_DEC           GP%d, GP%d, %d\n", a, b, c); break;
    case OP_ADDi_LOCAL_IMM: printf("OP_ADDi_LOCAL_IMM    local[%d+%d], %d\n", bp, d, (int8_t)a); break;
    case OP_FOR_STEP_LT_I32: case OP_FOR_STEP_LE_I32:
    case OP_FOR_STEP_GT_I32: case OP_FOR_STEP_GE_I32: {
        const char* names[] = { "LT", "LE", "GT", "GE" };
        int32_t* code = vm ? vm->base : (int32_t*)state->bytecode.data;
        printf("OP_FOR_STEP_%s_I32 local[%d+%d], bound %d, step %d, to %d\n",
            names[op - OP_FOR_STEP_LT_I32], bp, a, code[offset + 1], (int8_t)b, offset + code[offset + 2]);
        break;
    }
    case OP_FOR_STEP1_LT_I32: {
        int32_t* code = vm ? vm->base : (int32_t*)state->bytecode.data;
        printf("OP_FOR_STEP1_LT_I32 local[%d+%d], bound %d, to %d\n",
            bp, a, code[offset + 1], offset + code[offset + 2]);
        break;
    }
    case OP_ALOAD_I_LOCALS: case OP_ALOAD_F_LOCALS:
        printf("OP_ALOAD_%s_LOCALS GP%d, refLocal[%d], indexLocal[%d]\n",
            op == OP_ALOAD_I_LOCALS ? "I" : "F", a, b, c); break;
    case OP_LOAD_STORE_LOCAL:
        printf("OP_LOAD_STORE_LOCAL GP%d, local[%d+%d], local[%d+%d]\n", a, bp, b, bp, c); break;
    case OP_LOADKi_STORE_LOCAL:
        printf("OP_LOADKi_STORE_LOCAL GP%d, local[%d+%d], %d\n", a, bp, b, (int8_t)c); break;
    case OP_ADDf_LOCAL_K: case OP_SUBf_LOCAL_K:
        printf("OP_%s_LOCAL_K local[%d+%d], K%d\n",
            op == OP_ADDf_LOCAL_K ? "ADDf" : "SUBf", bp, d, a); break;
    case OP_RETURN_ADD_LOCAL: case OP_RETURN_SUB_LOCAL: case OP_RETURN_MUL_LOCAL:
    case OP_RETURN_ADDf_LOCAL: case OP_RETURN_SUBf_LOCAL: case OP_RETURN_MULf_LOCAL: {
        const char* names[] = { "ADD", "SUB", "MUL", "ADDf", "SUBf", "MULf" };
        printf("OP_RETURN_%s_LOCAL local[%d+%d], local[%d+%d]\n",
            names[op - OP_RETURN_ADD_LOCAL], bp, a, bp, b);
        break;
    }
    case OP_JEQ_LOCAL_K: case OP_JNE_LOCAL_K:
    case OP_JLT_LOCAL_K: case OP_JLE_LOCAL_K:
    case OP_JGT_LOCAL_K: case OP_JGE_LOCAL_K: {
        const char* names[] = { "JEQ", "JNE", "JLT", "JLE", "JGT", "JGE" };
        printf("OP_%s_LOCAL_K local[%d+%d], K%d, to %d\n",
            names[op - OP_JEQ_LOCAL_K], bp, a, b, offset + (int8_t)c);
        break;
    }
    case OP_INCi_LOCAL:     printf("OP_INCi_LOCAL        local[%d+%d], %d\n", bp, d, a); break;
    case OP_INCf_LOCAL:     printf("OP_INCf_LOCAL        local[%d+%d], %d\n", bp, d, a); break;
    case OP_INCi_GLOBAL:    printf("OP_INCi_GLOBAL       global[%d], %d\n", d, a); break;
    case OP_INCf_GLOBAL:    printf("OP_INCf_GLOBAL       global[%d], %d\n", d, a); break;
    case OP_MOVE:           printf("OP_MOVE              GP%d, GP%d\n", a, d); break;
	case OP_NEWARRAY:		printf("OP_NEWARRAY          RP%d, size:GP%d, count:GP%d\n", a, b, c); break;
	case OP_ALLOCATE_ARRAY_PTR:		printf("OP_ALLOCATE_ARRAY_PTR          RP%d, size:GP%d, count:GP%d\n", a, b, c); break;

	case OP_NOP:		    printf("OP_NOP\n"); break;
	case OP_GET_THIS_FIELDp: printf("OP_GET_THIS_FIELDp   RP%d, off:%d\n", a, b); break;
        DEBUG_ALOAD_CASE(OP_ALOAD_I);
        DEBUG_ALOAD_CASE(OP_ALOAD_F);
        DEBUG_ALOAD_CASE(OP_ALOAD_P);    
		//DEBUG_ASTORE_CASE(OP_ASTORE_I);
		//DEBUG_ASTORE_CASE(OP_ASTORE_F);
		//DEBUG_ASTORE_CASE(OP_ASTORE_P);
        DEBUG_JUMP_CASES(JLE);
        DEBUG_JUMP_CASES(JGT);
        DEBUG_JUMP_CASES(JLT);
        DEBUG_JUMP_CASES(JGE);
        DEBUG_JUMP_CASES(JNE);
        DEBUG_JUMP_CASES(JLEf);
        DEBUG_JUMP_CASES(JGTf);
        DEBUG_JUMP_CASES(JLTf);
        DEBUG_JUMP_CASES(JGEf);
        DEBUG_JUMP_CASES(JNEf);
    default:                printf("Unknown Opcode:      %d\n", op); break;
    }
    return 0;
}

void debug_print_register(VMContext* vm, int reg) {
    printf("  r%-2d = ", reg);
    // Primitive part
    printf("GP:%12d | %12f   ", vm->GP[reg].i32, vm->GP[reg].f32);
    // Reference part
    VMObject* obj = (VMObject*)vm->RP[reg];
    if (!obj) {
        printf("RP: (nil)\n");
    }
    else if (obj->type == T_STRING) {
        ObjString* s = (ObjString*)obj;
        printf("RP: \"%.*s\" (len=%u)\n", (int)s->length > 40 ? 40 : (int)s->length, s->data, (unsigned)s->length);
    }
    else if (obj->type == T_SHARED_PTR) {
        ObjRaw* raw = (ObjRaw*)obj;
        printf("RP: ObjRaw[%zu slots] @ %p\n", raw->slot_count, (void*)obj);
    }
    else {
        printf("RP: <%d> @ %p\n", obj->type, (void*)obj);
    }
}

void debug_printRegisters(VMContext* vm) {
    printf("--- Registers ---\n");
    for (int i = 0; i< 8; i++) {
        // Print GP (Primitives) and RP (References) side-by-side
        int32_t i32 = vm->GP[i].i32;
        float f32 = vm->GP[i].f32;
        void* ptr = vm->RP[i];

        printf(" R%d: [GP] %-10d | %-10.4f  [RP] %p", i32, i32, f32, ptr);

        // If RP is a string, print snippet
        if (ptr && ((VMObject*)ptr)->type == T_STRING) {
            printf(" \"%s\"", ((ObjString*)ptr)->data);
        }
        printf("\n");
    }
}

void debug_printGlobals(VMContext* vm) {
    printf("--- Globals (First 10) ---\n");
    for (int i32 = 0; i32 < 10 && i32 < (int)vm->global_size; i32++) {
        uint64_t gp_val = vm->prim_globals[i32];
        void* rp_val = vm->ref_globals[i32];

        // Only print if not zero/null to save space
        if (gp_val != 0 || rp_val != NULL) {
            printf(" G[%d]: %lld (prim) | %p (ref)\n", i32, gp_val, rp_val);
        }
    }
}

void debug_print_local(VMContext* vm, int local_offset) {
    CallFrame* frame = &vm->call_stack[vm->frame_pointer];
    uint32_t prim_idx = frame->prim_bp + local_offset;
    uint32_t ref_idx = frame->ref_bp + local_offset;

    if (prim_idx >= vm->primStack.capacity) {
        printf("  local[%d] out of bounds (prim stack)\n", local_offset);
        return;
    }

    uint64_t bits = vm->primStack.stack[prim_idx];
    int32_t i32; float f32; void* p = NULL;
    memcpy(&i32, &bits, 4);
    memcpy(&f32, &bits, 4);

    // Try to see if there's a corresponding reference slot
    VMObject* ref = (ref_idx < vm->refStack.ref_cap) ? vm->refStack.ref_stack[ref_idx] : NULL;

    printf("  local[%d] = %d | %f", local_offset, i32, f32);
    if (ref) {
        if (ref->type == T_STRING) {
            ObjString* s = (ObjString*)ref;
            printf("  ->  \"%.*s\"", (int)s->length > 30 ? 30 : (int)s->length, s->data);
        }
        else {
            printf("  ->  <%d>@%p", ref->type, (void*)ref);
        }
    }
    printf("\n");
}

void debug_print_k(VMContext *vm, int idx) 
{
    Value val = *(Value*)array_get(&vm->constants, idx);
	printf("  k[%d] = %d\n", idx, val.v.i32);
}

void debug_print_global(VMContext* vm, int idx) {
    if (idx >= (int)vm->global_size) {
        printf("  global[%d] out of bounds\n", idx);
        return;
    }
    uint64_t bits = vm->prim_globals[idx];
    int32_t i32; float f32;
    memcpy(&i32, &bits, 4);
    memcpy(&f32, &bits, 4);
    VMObject* ref = vm->ref_globals[idx];

    printf("  global[%d] = %d | %f", idx, i32, f32);
    if (ref) {
        if (ref->type == T_STRING) {
            ObjString* s = (ObjString*)ref;
            printf("  ->  \"%.*s\"", (int)s->length > 30 ? 30 : (int)s->length, s->data);
        }
        else {
            printf("  ->  <%d>@%p", ref->type, (void*)ref);
        }
    }
    printf("\n");
}

void debug_dump_object_memory(VMObject* obj) {
    if (!obj) {
        printf("    (null)\n");
        return;
    }
    if (obj->type == T_STRING) {
        ObjString* s = (ObjString*)obj;
        printf("    String: \"%.*s\" (len=%u)\n", (int)s->length, s->data, (unsigned)s->length);
    }
    else if (obj->type == T_SHARED_PTR) {
        ObjRaw* raw = (ObjRaw*)obj;
        printf("    Raw object, size = %zu slots:\n", raw->slot_count);
        for (size_t slot = 0; slot < raw->slot_count; ++slot) {
            {
                uint64_t word;
                memcpy(&word, raw->data + slot * sizeof(uint64_t), sizeof(word));
                printf("      [%zu] 0x%016llx  ", slot, (unsigned long long)word);
                if (word >= 0x1000 && word < 0x7fffffffffff) { // looks like a pointer?
                    printf(" -> possible ptr");
                }
                printf("\n");
            }
        }
    }
}

// Updated Hook to include the new info
DebugMode debugPrompt(State* state, VMContext* vm, int ip_offset) {
    char line[512];
    debug_disassembleInstruction(state, state->vm, ((int32*)state->bytecode.data)[ip_offset], ip_offset);

    while (1) {
        printf("(dbg) ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) return DEBUG_QUIT;

        char* cmd = strtok(line, " \t\n\r");
        if (!cmd || cmd[0] == '\0') continue;

        if (strcmp(cmd, "s") == 0 || strcmp(cmd, "step") == 0)     return DEBUG_STEPPING;
        if (strcmp(cmd, "c") == 0 || strcmp(cmd, "continue") == 0) return DEBUG_RUNNING;
        if (strcmp(cmd, "q") == 0 || strcmp(cmd, "quit") == 0)     return DEBUG_QUIT;

        // ── d [count] ─────────────────────────────────────
        if (strcmp(cmd, "d") == 0 || strcmp(cmd, "dis") == 0) {
            int count = 10;
            char* arg = strtok(NULL, " \t\n");
            if (arg) count = atoi(arg);
            if (count <= 0) count = 10;
            for (int i32 = 0; i32 < count; i32++) {
                int32_t inst = ((int32*)state->bytecode.data)[ip_offset + i32];
                debug_disassembleInstruction(state, vm, inst, ip_offset + i32);
                if (GET_OP(inst) == OP_HALT) break;
            }
            continue;
        }

        // ── p r0, p l3, p g5, p *r2 ─────────────────────────
        if (strcmp(cmd, "p") == 0 || strcmp(cmd, "print") == 0) {
            char* arg = strtok(NULL, " \t\n");
            if (!arg) { printf("Usage: p <rN|lN|gN|*rN>\n"); continue; }

            if (arg[0] == '*') {
                int reg = atoi(arg + 2);
                if (reg < 0 || reg >= 16) { printf("Bad register\n"); continue; }
                debug_dump_object_memory((VMObject*)vm->RP[reg]);
            }
            else if (strncmp(arg, "r", 1) == 0) {
                int reg = atoi(arg + 1);
                if (reg >= 0 && reg < 16) debug_print_register(vm, reg);
                else printf("r0..r15 only\n");
            }
            else if (strncmp(arg, "l", 1) == 0) {
                int off = atoi(arg + 1);
                debug_print_local(vm, off);
            }
            else if (strncmp(arg, "g", 1) == 0) {
                int idx = atoi(arg + 1);
                debug_print_global(vm, idx);
            }
            else if (strncmp(arg, "c", 1) == 0) {
                int idx = atoi(arg + 1);
                debug_print_k(vm, idx);
            }
        }

        // ── info locals / info regs ───────────────────────
        if (strcmp(cmd, "info") == 0) {
            char* sub = strtok(NULL, " \t\n");
            if (!sub) { printf("info locals | info regs\n"); continue; }
            if (strcmp(sub, "locals") == 0) {
                CallFrame* frame = vm->current_frame;
                printf("--- Locals (prim_bp=%u, ref_bp=%u, prim_sp=%u) ---\n",
                    frame->prim_bp, frame->ref_bp, vm->primStack.sp);

                uint32_t local_count = (vm->primStack.sp - frame->prim_bp) / 4;
                for (uint32_t i32 = 0; i32 < local_count; i32++) {
                    uint32_t prim_idx = frame->prim_bp + i32 * 4;
                    uint64_t bits = vm->primStack.stack[prim_idx];

                    int32_t i_val;
                    float   f_val;
                    memcpy(&i_val, &bits, 4);
                    memcpy(&f_val, &bits, 4);

                    printf("  l%-2u  %12d | %12.6g", i32, i_val, f_val);

                    uint32_t ref_idx = frame->ref_bp + i32;
                    VMObject* ref = (ref_idx < vm->refStack.ref_cap) ? vm->refStack.ref_stack[ref_idx] : NULL;
                    if (ref) {
                        if (ref->type == T_STRING) {
                            ObjString* s = (ObjString*)ref;
                            int len = (int)s->length;
                            printf("  ->  \"%.*s%s\"", len > 20 ? 20 : len, s->data, len > 20 ? "…" : "");
                        }
                        else {
                            printf("  ->  <%d>@%p", ref->type, (void*)ref);
                        }
                    }
                    printf("\n");
                }
            }
            else if (strcmp(sub, "regs") == 0) {
                printf("Registers:\n");
                for (int i32 = 0; i32 < 16; i32++) debug_print_register(vm, i32);
            }
        }

        // ── b 123, bl, bd 2 ───────────────────────────────
        if (strcmp(cmd, "b") == 0 || strcmp(cmd, "break") == 0) {
            char* arg = strtok(NULL, " \t\n");
            if (!arg) { printf("Usage: b <offset>\n"); continue; }
            int off = atoi(arg);
            if (vm->numBreakpoints >= MAX_BREAKPOINTS) {
                printf("Too many breakpoints\n"); continue;
            }
            vm->breakpoints[vm->numBreakpoints++] = off;
            printf("Breakpoint %d at offset %d\n", vm->numBreakpoints, off);
        }
        if (strcmp(cmd, "bl") == 0) {
            printf("Breakpoints:\n");
            for (int i32 = 0; i32 < vm->numBreakpoints; i32++) {
                printf("  %d: %d\n", i32 + 1, vm->breakpoints[i32]);
            }
        }
        if (strcmp(cmd, "bd") == 0 || strcmp(cmd, "delete") == 0) {
            char* arg = strtok(NULL, " \t\n");
            int n = arg ? atoi(arg) : -1;
            if (n <= 0 || n >= vm->numBreakpoints) { printf("Bad index\n"); continue; }
            for (int i32 = n; i32 < vm->numBreakpoints - 1; i32++) {
                vm->breakpoints[i32] = vm->breakpoints[i32 + 1];
            }
            vm->numBreakpoints--;
            printf("Deleted breakpoint %d\n", n + 1);
        }

        // ── help ───────────────────────────────────────
        if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
            printf("Commands:\n"
                "  s/step         - step one instruction\n"
                "  c/continue     - continue execution\n"
                "  q/quit         - quit debugger\n"
                "  d [n]          - disassemble next n instructions (def 10)\n"
                "  p rN           - print register r0..r15\n"
                "  p lN           - print local variable N\n"
                "  p gN           - print global N\n"
                "  p *rN          - dump object pointed by rN\n"
                "  info locals    - pretty print all locals\n"
                "  info regs      - show all registers\n"
                "  b <off>        - set breakpoint\n"
                "  bl             - list breakpoints\n"
                "  bd <n>         - delete breakpoint\n");
        }
    }
}

DebugMode debug_hook(State* state, VMContext* vm, int ip_offset) {
    if (vm->debugMode == DEBUG_STEPPING) {
        for (int i32 = 0; i32 < vm->numBreakpoints; i32++) {
            if (vm->breakpoints[i32] == ip_offset) {
                vm->debugMode = DEBUG_STEPPING;
                printf("Breakpoint hit at %04d\n", ip_offset);
                break;
            }
        }
        return debugPrompt(state, vm, ip_offset);
    }
    return DEBUG_RUNNING;
}
