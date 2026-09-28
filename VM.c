#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <math.h>
#include "vm.h"
#include "opcode.h"
#include "Ownership.h"

ObjRaw* create_native_handle(VMContext* vm, void* ptr, CDtor cleanupFn) {
    // 1. Allocate payload size (ObjRaw header + void* pointer)
    size_t payload_size = sizeof(void*);
    ObjRaw* obj = (ObjRaw*)ownership_allocate(vm, sizeof(ObjRaw) + payload_size, T_SHARED_PTR);

    // 2. CRITICAL: Set the Specific Kind to 1 (OBJ_NATIVE_HANDLE)
    obj->kind = OBJ_NATIVE_HANDLE;
    obj->slot_count = 1;
    obj->data_bytes = payload_size;
    obj->element_size = sizeof(uint64_t);
    obj->cleanup = cleanupFn;
    // 5. Store the pointer safely
    memcpy(obj->data, &ptr, payload_size);

    return obj;
}

bool vm_get_native_array_view(const VMObject* object, VMScalarType element_type,
    VMNativeArrayView* view) {
    if (view) memset(view, 0, sizeof(*view));
    const size_t element_size = vm_scalar_size(element_type);
    if (!object || !view || object->type != T_SHARED_PTR || !element_size ||
        !vm_host_supports_native_scalar(element_type)) return false;

    const ObjRaw* raw = (const ObjRaw*)object;
    if (raw->kind != OBJ_BLOB || raw->element_size != element_size ||
        raw->slot_count > SIZE_MAX / element_size ||
        raw->data_bytes != raw->slot_count * element_size ||
        raw->data_bytes > SIZE_MAX - offsetof(ObjRaw, data) ||
        raw->header.allocation_capacity < offsetof(ObjRaw, data) + raw->data_bytes ||
        ((uintptr_t)raw->data % element_size) != 0) return false;

    view->data = raw->data;
    view->length = raw->slot_count;
    view->byte_length = raw->data_bytes;
    view->element_size = element_size;
    view->element_type = element_type;
    return true;
}

const char* vm_get_string_arg(VMContext* vm, int arg_index) {
    // 1. Get the current frame's Reference Base Pointer
    uint32_t bp = vm->call_stack[vm->frame_pointer].ref_bp;

    // 2. Access the stack at BP + index
    // Note: Add bounds checking here in a real app
    VMObject* obj = vm->refStack.ref_stack[bp + arg_index];

    // 3. Verify it's actually a string
    if (obj == NULL) return NULL;

    // Assuming you have an Enum or Kind check
    // if (obj->kind != OBJ_STRING) return NULL; 

    // 4. Cast and return data
    ObjString* strObj = (ObjString*)obj;
    return strObj->data; // Or strObj->data depending on your struct
}

void reportRuntimeErrorBridge(const char* msg);
void reportRuntimeError(VMContext* vm, const char* msg) {
    char diagnostic[1024];
    size_t offset = 0;
    const SourceMapEntry* sourceLocation = NULL;
    if (vm && vm->program && vm->ip && vm->program->base &&
        vm->ip >= vm->program->base &&
        (size_t)(vm->ip - vm->program->base) < vm->program->bytecode_count) {
        offset = (size_t)(vm->ip - vm->program->base);
    }

    if (vm && vm->program && vm->program->source_map_count) {
        size_t low = 0, high = vm->program->source_map_count;
        while (low < high) {
            size_t middle = low + (high - low) / 2;
            if (vm->program->source_map[middle].bytecode_offset <= offset)
                low = middle + 1;
            else
                high = middle;
        }
        if (low > 0) sourceLocation = &vm->program->source_map[low - 1];
    }

    if (vm) {
        const int function_id = vm->current_frame ? vm->current_frame->function_id : -1;
        const char* function_name = "<unknown>";
        if (vm->program && function_id >= 0 &&
            function_id < vm->program->function_name_count &&
            vm->program->function_names[function_id])
            function_name = vm->program->function_names[function_id];
        snprintf(diagnostic, sizeof(diagnostic),
            "%s:%d:%d: %s (frame=%d, function=%s, function_id=%d, bytecode_offset=%zu)",
            sourceLocation && sourceLocation->filename ? sourceLocation->filename :
                (vm->program && vm->program->source_filename ? vm->program->source_filename : "<script>"),
            sourceLocation ? sourceLocation->line : 0,
            sourceLocation ? sourceLocation->column : 0,
            msg ? msg : "Unknown runtime error", vm->frame_pointer,
            function_name, function_id, offset);
        size_t used = strlen(diagnostic);
        for (int frameIndex = vm->frame_pointer - 1; frameIndex > 0 && used < sizeof(diagnostic) - 1;
            --frameIndex) {
            CallFrame* frame = &vm->call_stack[frameIndex];
            const char* callerName = "<unknown>";
            if (vm->program && frame->function_id >= 0 &&
                frame->function_id < vm->program->function_name_count &&
                vm->program->function_names[frame->function_id])
                callerName = vm->program->function_names[frame->function_id];
            int written = snprintf(diagnostic + used, sizeof(diagnostic) - used,
                "\n  called from %s [frame=%d]", callerName, frameIndex);
            if (written < 0) break;
            if ((size_t)written >= sizeof(diagnostic) - used) {
                used = sizeof(diagnostic) - 1;
                break;
            }
            used += (size_t)written;
        }
    } else {
        snprintf(diagnostic, sizeof(diagnostic), "%s",
            msg ? msg : "Unknown runtime error");
    }

    printf("Runtime error: %s\n", diagnostic);
    reportRuntimeErrorBridge(diagnostic);
    if (vm) vm->had_runtime_error = true;
}

// --- Helpers for Opcode Macros ---
#define GP(x) gp[x]
#define RP(x) rp[x]

// --- Math (Primitives -> GP) ---
#define IMPLEMENT_MATH_OP(opcde, op, type) case opcde:{\
    GP(GET_A(inst)).type = GP(GET_B(inst)).type op GP(GET_C(inst)).type;break;\
}

#define IMPLEMENT_COMP_OP(opcde, op, type) case opcde:{\
    GP(GET_A(inst)).i32 = GP(GET_B(inst)).type op GP(GET_C(inst)).type;break;\
}

// Helper to get array slot address safely using A=Array, B=Index
// Slot-based addressing: always 8 bytes per slot
static inline uint64_t* get_array_element_addr(VMContext* vm, int32 inst, PrimReg* gp, RefReg* rp) {
    // 1. Get Array Object from Register A
    ObjRaw* arr = (ObjRaw*)RP(GET_A(inst));
    if (!arr) { reportRuntimeError(vm, "Null Pointer Access"); return NULL; }

    // 2. Get Index Value from Register B
    int index = GP(GET_B(inst)).i32;

    // 3. Bounds Check
    if (index < 0 || index >= arr->slot_count) {
        reportRuntimeError(vm, "Array Index Out of Bounds");
        return NULL;
    }

    // 4. Return Address (Slot-based: index * 8 bytes per slot)
    if (arr->element_size == 0) arr->element_size = sizeof(uint64_t);
    return (uint64_t*)(arr->data + (size_t)index * arr->element_size);
}

static int fn_load_ref_local(VMContext* vm, int32 inst) {
    uint32_t idx = vm->call_stack[vm->frame_pointer].ref_bp + GET_D(inst);
    vm->RP[GET_A(inst)] = vm->refStack.ref_stack[idx];
    return 1;
}

#define IMPLEMENT_COND_JUMP(opcode, OP, type) case opcode:{ \
    if (GP(GET_A(inst)).type OP GP(GET_B(inst)).type) { \
        ip += (int8_t)GET_C(inst); continue;\
    } } break;

#define CASE_INC_GLOBAL_INT(opcode, op) case opcode:{ \
uint32_t idx = GET_D(inst); \
uint64_t* globals = active_prim_globals(vm); \
if (!globals || idx >= active_prim_global_size(vm)) { VM_SYNC_STATE(); reportRuntimeError(vm, "Global Access Out of Bounds"); goto vm_exit; }\
int32_t value = (int32_t)globals[idx]; \
value op GP(GET_A(inst)).i32; \
globals[idx] = (uint64_t)(int64_t)value; \
} break;

#define CASE_INC_GLOBAL_FLOAT(opcode, op) case opcode:{ \
uint32_t idx = GET_D(inst); \
uint64_t* globals = active_prim_globals(vm); \
if (!globals || idx >= active_prim_global_size(vm)) { VM_SYNC_STATE(); reportRuntimeError(vm, "Global Access Out of Bounds"); goto vm_exit; }\
float value = 0.0f; \
memcpy(&value, &globals[idx], sizeof(float)); \
value op GP(GET_A(inst)).f32; \
uint64_t bits = 0; \
memcpy(&bits, &value, sizeof(float)); \
globals[idx] = bits; \
} break;

#define CASE_INC_LOCAL_INT(opcode, op) case opcode: { \
uint32_t idx = vm->current_frame->prim_bp + GET_D(inst); \
if (idx >= vm->primStack.capacity) { VM_SYNC_STATE(); reportRuntimeError(vm, "Stack Overflow (local increment)"); goto vm_exit; }\
int32_t value = (int32_t)vm->primStack.stack[idx]; \
value op GP(GET_A(inst)).i32; \
vm->primStack.stack[idx] = (uint64_t)(int64_t)value; \
} break;

#define CASE_INC_LOCAL_FLOAT(opcode, op) case opcode: { \
uint32_t idx = vm->current_frame->prim_bp + GET_D(inst); \
if (idx >= vm->primStack.capacity) { VM_SYNC_STATE(); reportRuntimeError(vm, "Stack Overflow (local increment)"); goto vm_exit; }\
float value = 0.0f; \
memcpy(&value, &vm->primStack.stack[idx], sizeof(float)); \
value op GP(GET_A(inst)).f32; \
uint64_t bits = 0; \
memcpy(&bits, &value, sizeof(float)); \
vm->primStack.stack[idx] = bits; \
} break;


#define GET_TAGS_PTR(raw) ((uint8_t*)((raw)->data) + (raw)->data_bytes)

static void vm_activate_program(VMContext* vm, VMProgram* program)
{
    if (!vm || !program) return;
    vm->program = program;
    vm->base = program->base;
    vm->code_start = program->code_start;
    vm->setup_ip = program->setup_ip;
    vm->update_ip = program->update_ip;
    vm->fixed_update_ip = program->fixed_update_ip;
    vm->trigger_enter_ip = program->trigger_enter_ip;
    vm->trigger_stay_ip = program->trigger_stay_ip;
    vm->trigger_exit_ip = program->trigger_exit_ip;
    vm->network_remote_ip = program->network_remote_ip;
    vm->constants = program->constants;
}

static array* active_constants(VMContext* vm)
{
    // vm_activate_program mirrors this immutable pool on every program switch.
    return &vm->constants;
}

static State* active_state(State* fallback, VMContext* vm)
{
    return (vm && vm->program && vm->program->owner) ? vm->program->owner : fallback;
}

static uint64_t* active_prim_globals(VMContext* vm)
{
    return (vm && vm->program && vm->program->prim_globals) ? vm->program->prim_globals : (vm ? vm->prim_globals : NULL);
}

static VMObject** active_ref_globals(VMContext* vm)
{
    return (vm && vm->program && vm->program->ref_globals) ? vm->program->ref_globals : (vm ? vm->ref_globals : NULL);
}

static size_t active_prim_global_size(VMContext* vm)
{
    return (vm && vm->program && vm->program->prim_globals) ? vm->program->prim_global_size : (vm ? vm->global_size : 0);
}

static size_t active_ref_global_size(VMContext* vm)
{
    return (vm && vm->program && vm->program->ref_globals) ? vm->program->ref_global_size : (vm ? vm->global_size : 0);
}

static bool copySourceMap(SourceMapEntry** destination, size_t* destinationCount,
    const SourceMapEntry* source, size_t sourceCount)
{
    *destination = NULL;
    *destinationCount = 0;
    if (!sourceCount) return true;
    SourceMapEntry* copy = (SourceMapEntry*)calloc(sourceCount, sizeof(SourceMapEntry));
    if (!copy) return false;
    for (size_t i = 0; i < sourceCount; ++i) {
        copy[i] = source[i];
        copy[i].filename = NULL;
        if (source[i].filename) {
            size_t length = strlen(source[i].filename) + 1;
            copy[i].filename = (char*)malloc(length);
            if (!copy[i].filename) {
                for (size_t j = 0; j < i; ++j) free(copy[j].filename);
                free(copy);
                return false;
            }
            memcpy(copy[i].filename, source[i].filename, length);
        }
    }
    *destination = copy;
    *destinationCount = sourceCount;
    return true;
}

static void freeProgramDiagnostics(VMProgram* program)
{
    if (!program) return;
    free(program->source_filename);
    program->source_filename = NULL;
    for (int i = 0; i < program->function_name_count; ++i) {
        free(program->function_names[i]);
        program->function_names[i] = NULL;
    }
    program->function_name_count = 0;
    for (size_t i = 0; i < program->source_map_count; ++i)
        free(program->source_map[i].filename);
    free(program->source_map);
    program->source_map = NULL;
    program->source_map_count = 0;
}

static void vm_scan_program(State* state, VMProgram* program)
{
    memset(program, 0, sizeof(*program));
    program->owner = state;
    if (state && state->source_filename) {
        size_t length = strlen(state->source_filename) + 1;
        program->source_filename = (char*)malloc(length);
        if (program->source_filename) memcpy(program->source_filename, state->source_filename, length);
    }
    if (state) {
        program->function_name_count = state->function_name_count;
        if (program->function_name_count > MAX_FUNCTIONS)
            program->function_name_count = MAX_FUNCTIONS;
        for (int i = 0; i < program->function_name_count; ++i) {
            const char* name = state->function_names[i];
            if (!name) continue;
            size_t length = strlen(name) + 1;
            program->function_names[i] = (char*)malloc(length);
            if (program->function_names[i])
                memcpy(program->function_names[i], name, length);
        }
    }
    if (state && !copySourceMap(&program->source_map, &program->source_map_count,
            state->source_map, state->source_map_count)) {
        program->source_map_count = 0;
        program->source_map = NULL;
    }
    program->base = (int32_t*)state->bytecode.data;
    program->bytecode_count = (size_t)array_size(&state->bytecode);
    program->register_count = state->register_count ? state->register_count : MAX_REGISTERS;
    program->native_target_count = (size_t)state->cFunctions.count;
    if (program->native_target_count > 0) {
        program->native_targets = (NativeCallTarget*)calloc(
            program->native_target_count, sizeof(NativeCallTarget));
        if (!program->native_targets) {
            free(program->prim_globals);
            free(program->ref_globals);
            program->prim_globals = NULL;
            program->ref_globals = NULL;
            program->native_target_count = 0;
        } else {
            CFunction* functions = (CFunction*)state->cFunctions.data;
            for (size_t i = 0; i < program->native_target_count; ++i)
            {
                program->native_targets[i].function = functions[i];
                program->native_targets[i].signature =
                    (i < (size_t)state->cFunctionSignatures.count)
                    ? *(const NativeSignature**)array_get(&state->cFunctionSignatures, (int)i)
                    : NULL;
            }
        }
    }
    program->constants = array_create(sizeof(Value));
    program->owns_constants = true;
    program->prim_global_size = 1024;
    program->ref_global_size = 1024;
    program->prim_globals = (uint64_t*)calloc(program->prim_global_size, sizeof(uint64_t));
    program->ref_globals = (VMObject**)calloc(program->ref_global_size, sizeof(VMObject*));

    int func_count = 0;
    int32_t* scanner = program->base;

    while (true)
    {
        int32_t inst = *scanner;
        OPCode op = GET_OP(inst);
        if (op == OP_DEF_K_INT || op == OP_DEF_K_FLOAT) {
            scanner++; // Advance to Data
            int32_t data = *scanner;

            Value v;
            if (op == OP_DEF_K_INT) {
                v.type.baseType = T_INT;
                v.v.i32 = data;
            }
            else {
                v.type.baseType = T_FLOAT;
                v.v.bits = (uint64_t)data;
            }
            array_append(&program->constants, &v);
            scanner++; // Advance past data
        }
        else if (op == OP_DEF_K_CHAR) {
            Value v;
            v.type.baseType = T_CHAR;
            v.v.c = (char)GET_E(inst);
            array_append(&program->constants, &v);
            scanner++;
        }
        else if (op == OP_DEF_K_BOOL) {
            Value v;
            v.type.baseType = T_BOOL;
            v.v.b = (bool)GET_E(inst);
            array_append(&program->constants, &v);
            scanner++;
        }
        else if (op == OP_DEF_K_STR) {
            scanner++; // Move to Length
            int len = *scanner;
            scanner++; // Move to Data Start

            char* str = malloc(len + 1);
            memcpy(str, scanner, len);
            str[len] = '\0';

            Value v;
            v.type.baseType = T_STRING;
            v.v.p = str;
            array_append(&program->constants, &v);

            int words = (len + 3) / 4;
            scanner += words;
        }
        else if (op == OP_DEF_SETUP || op == OP_DEF_UPDATE || op == OP_DEF_FIXED_UPDATE ||
            op == OP_DEF_TRIGGER_ENTER || op == OP_DEF_TRIGGER_STAY || op == OP_DEF_TRIGGER_EXIT ||
            op == OP_DEF_NETWORK_REMOTE || op == OP_FUNC_DEF) {
            int addr = GET_E(inst);
            if (func_count >= 0 && func_count < MAX_FUNCTIONS)
                program->func_table[func_count++] = addr;
            if (op == OP_DEF_UPDATE) program->update_ip = program->base + addr;
            else if (op == OP_DEF_FIXED_UPDATE) program->fixed_update_ip = program->base + addr;
            else if (op == OP_DEF_TRIGGER_ENTER) program->trigger_enter_ip = program->base + addr;
            else if (op == OP_DEF_TRIGGER_STAY) program->trigger_stay_ip = program->base + addr;
            else if (op == OP_DEF_TRIGGER_EXIT) program->trigger_exit_ip = program->base + addr;
            else if (op == OP_DEF_NETWORK_REMOTE) program->network_remote_ip = program->base + addr;
            else if (op == OP_DEF_SETUP) program->setup_ip = program->base + addr;

            scanner++;
        }
        else {
            break;
        }
    }

    program->func_count = func_count;
    program->code_start = scanner;
}

VMProgram* vm_clone_program_instance(VMProgram* source, VMContext* sharedVM)
{
    if (!source || !sharedVM) return NULL;
    VMProgram* instance = (VMProgram*)calloc(1, sizeof(VMProgram));
    if (!instance) return NULL;
    instance->owner = source->owner;
    if (source->source_filename) {
        size_t length = strlen(source->source_filename) + 1;
        instance->source_filename = (char*)malloc(length);
        if (instance->source_filename) memcpy(instance->source_filename, source->source_filename, length);
    }
    instance->function_name_count = source->function_name_count;
    if (instance->function_name_count > MAX_FUNCTIONS)
        instance->function_name_count = MAX_FUNCTIONS;
    for (int i = 0; i < instance->function_name_count; ++i) {
        const char* name = source->function_names[i];
        if (!name) continue;
        size_t length = strlen(name) + 1;
        instance->function_names[i] = (char*)malloc(length);
        if (instance->function_names[i])
            memcpy(instance->function_names[i], name, length);
    }
    if (!copySourceMap(&instance->source_map, &instance->source_map_count,
            source->source_map, source->source_map_count)) {
        free(instance->source_filename);
        for (int i = 0; i < instance->function_name_count; ++i)
            free(instance->function_names[i]);
        free(instance);
        return NULL;
    }
    instance->base = source->base;
    instance->code_start = source->code_start;
    instance->bytecode_count = source->bytecode_count;
    instance->register_count = source->register_count;
    instance->native_target_count = source->native_target_count;
    if (instance->native_target_count > 0) {
        instance->native_targets = (NativeCallTarget*)malloc(
            instance->native_target_count * sizeof(NativeCallTarget));
        if (!instance->native_targets) {
            freeProgramDiagnostics(instance);
            free(instance->prim_globals);
            free(instance->ref_globals);
            free(instance);
            return NULL;
        }
        memcpy(instance->native_targets, source->native_targets,
            instance->native_target_count * sizeof(NativeCallTarget));
    }
    memcpy(instance->func_table, source->func_table, sizeof(instance->func_table));
    instance->func_count = source->func_count;
    instance->setup_ip = source->setup_ip;
    instance->update_ip = source->update_ip;
    instance->fixed_update_ip = source->fixed_update_ip;
    instance->trigger_enter_ip = source->trigger_enter_ip;
    instance->trigger_stay_ip = source->trigger_stay_ip;
    instance->trigger_exit_ip = source->trigger_exit_ip;
    instance->network_remote_ip = source->network_remote_ip;
    instance->constants = source->constants;
    instance->owns_constants = false;
    instance->prim_global_size = source->prim_global_size;
    instance->ref_global_size = source->ref_global_size;
    instance->prim_globals = (uint64_t*)calloc(instance->prim_global_size, sizeof(uint64_t));
    instance->ref_globals = (VMObject**)calloc(instance->ref_global_size, sizeof(VMObject*));
    if (!instance->prim_globals || !instance->ref_globals) {
        freeProgramDiagnostics(instance);
        free(instance->prim_globals);
        free(instance->ref_globals);
        free(instance);
        return NULL;
    }
    array_append(&sharedVM->programs, &instance);
    return instance;
}

VMProgram* vm_attach_program(State* state, VMContext* sharedVM)
{
    if (!state || !sharedVM) return NULL;

    VMProgram* program = (VMProgram*)calloc(1, sizeof(VMProgram));
    vm_scan_program(state, program);
    array_append(&sharedVM->programs, &program);
    state->vm = sharedVM;
    state->program = program;
    return program;
}

void vm_init(State* state){
    VMContext* vm = (VMContext*)calloc(1, sizeof(VMContext));
    // 1MB Primitive Stack
    vm->primStack.capacity = 1024 * 128;
    vm->primStack.stack = (uint64_t*)calloc(vm->primStack.capacity, sizeof(uint64_t));

    // 1MB Reference Stack
    vm->refStack.ref_cap = 1024 * 128;
    vm->refStack.ref_stack = (VMObject**)calloc(vm->refStack.ref_cap, sizeof(VMObject*));
    vm->owned_ref_slots = (uint8_t*)calloc(vm->refStack.ref_cap, sizeof(uint8_t));

    // Globals
    vm->global_size = 1024;
    vm->prim_globals = (uint64_t*)calloc(vm->global_size, sizeof(uint64_t));
    vm->ref_globals = (VMObject**)calloc(vm->global_size, sizeof(VMObject*));

    vm->programs = array_create(sizeof(VMProgram*));

    state->vm = vm;
    VMProgram* program = vm_attach_program(state, vm);
    vm_activate_program(vm, program);
    vm->ip = vm->code_start;
}

// At the top of execute_instructions:
#define VM_SYNC_STATE() do { \
    vm->ip = ip; \
    memcpy(vm->GP, gp, register_count * sizeof(*gp)); \
    memcpy(vm->RP, rp, register_count * sizeof(*rp)); \
} while (0)

#define VM_RELOAD_REGISTERS() do { \
    memcpy(gp, vm->GP, register_count * sizeof(*gp)); \
    memcpy(rp, vm->RP, register_count * sizeof(*rp)); \
} while (0)

static void unwind_owned_reference_slots(VMContext* vm) {
    if (!vm || !vm->owned_ref_slots || !vm->refStack.ref_stack) return;
    for (size_t i = 0; i < vm->refStack.ref_cap; ++i) {
        if (!vm->owned_ref_slots[i]) continue;
        vm->owned_ref_slots[i] = 0;
        VMObject* owned = vm->refStack.ref_stack[i];
        vm->refStack.ref_stack[i] = NULL;
        if (owned && owned->type != T_STRING)
            ownership_destroy(vm, owned);
    }
    ownership_destroy_unclaimed(vm);
}

#define VM_RUNTIME_ERROR(msg) do { \
    VM_SYNC_STATE(); \
    reportRuntimeError(vm, msg); \
    goto vm_exit; \
} while(0)

#define VM_DEBUG_CHECK(expr, msg) do { if (expr) { VM_RUNTIME_ERROR(msg); } } while (0)

#define LOCAL_CONSTANT_JUMP(opcode, comparison) case opcode: { \
    uint32_t idx = vm->current_frame->prim_bp + GET_A(inst); \
    array* constants = active_constants(vm); \
    VM_DEBUG_CHECK(idx >= vm->primStack.capacity || GET_B(inst) >= constants->count, \
        "Local comparison operand out of bounds"); \
    if ((int32_t)vm->primStack.stack[idx] comparison \
        ((Value*)constants->data)[GET_B(inst)].v.i32) { \
        ip += (int8_t)GET_C(inst); continue; \
    } \
    break; \
}

#define RETURN_LOCAL_MATH(opcode, operation, type) case opcode: { \
    uint32_t bp = vm->current_frame->prim_bp; \
    uint32_t left = bp + GET_A(inst), right = bp + GET_B(inst); \
    if (left >= vm->primStack.capacity || right >= vm->primStack.capacity) { \
        VM_RUNTIME_ERROR("Return operand out of bounds"); \
    } \
    PrimReg l, r; \
    l.bits = vm->primStack.stack[left]; \
    r.bits = vm->primStack.stack[right]; \
    GP(0).type = l.type operation r.type; \
    goto vm_return_prim; \
}

#define FOR_STEP_JUMP(opcode, comparison) case opcode: { \
    VM_DEBUG_CHECK(vm->program && (size_t)(ip - vm->program->base) + 2 >= vm->program->bytecode_count, \
        "Truncated loop instruction"); \
    uint32_t idx = vm->current_frame->prim_bp + GET_A(inst); \
    VM_DEBUG_CHECK(idx >= vm->primStack.capacity, "Loop operand out of bounds"); \
    int32_t value = (int32_t)vm->primStack.stack[idx]; \
    value += (int8_t)GET_B(inst); \
    vm->primStack.stack[idx] = (uint64_t)(int64_t)value; \
    if (value comparison ip[1]) ip += ip[2]; \
    else ip += 3; \
    continue; \
}

#define FOR_STEP1_JUMP(opcode, comparison) case opcode: { \
    VM_DEBUG_CHECK(vm->program && (size_t)(ip - vm->program->base) + 2 >= vm->program->bytecode_count, \
        "Truncated loop instruction"); \
    uint32_t idx = vm->current_frame->prim_bp + GET_A(inst); \
    VM_DEBUG_CHECK(idx >= vm->primStack.capacity, "Loop operand out of bounds"); \
    int32_t value = (int32_t)vm->primStack.stack[idx] + 1; \
    vm->primStack.stack[idx] = (uint64_t)(int64_t)value; \
    if (value comparison ip[1]) ip += ip[2]; \
    else ip += 3; \
    continue; \
}

void execute_instructions(State* state, bool startInDebugMode) {
    VMContext* vm = state->vm;
    // Keep dispatch state in CPU locals. Publish it at observable boundaries.
    int32_t* ip = vm->ip;
    vm->current_frame = &vm->call_stack[vm->frame_pointer];
    if (!ip) return;
    uint16_t register_count = vm->current_frame->register_count;
    PrimReg* gp = vm->current_frame->GP;
    RefReg* rp = vm->current_frame->RP;
    VM_RELOAD_REGISTERS();
    while (1) {

        if (vm->program && (ip < vm->program->base ||
            (size_t)(ip - vm->program->base) >= vm->program->bytecode_count)) {
            VM_RUNTIME_ERROR("Instruction pointer outside bytecode");
        }
        int32_t inst = *ip;
        OPCode op = GET_OP(inst);
        vm->instruction_count++;

        #ifndef NDEBUG
        // The debugger is intentionally cold in normal runtime execution.
        // This keeps the interpreter's hot loop free of a callback and branch
        // when no debugger is attached.
        if (vm->debugMode != DEBUG_RUNNING) {
            int off = (int)(ip - vm->base);
            VM_SYNC_STATE();
            vm->debugMode = debug_hook(state, vm, off);
            ip = vm->ip;
            VM_RELOAD_REGISTERS();
            if (vm->debugMode == DEBUG_QUIT) break;
        }
        #endif

        switch (op)
        {
        case OP_NOP:
            break;
        LOCAL_CONSTANT_JUMP(OP_JEQ_LOCAL_K, ==);
        LOCAL_CONSTANT_JUMP(OP_JNE_LOCAL_K, !=);
        LOCAL_CONSTANT_JUMP(OP_JLT_LOCAL_K, <);
        LOCAL_CONSTANT_JUMP(OP_JLE_LOCAL_K, <=);
        LOCAL_CONSTANT_JUMP(OP_JGT_LOCAL_K, >);
        LOCAL_CONSTANT_JUMP(OP_JGE_LOCAL_K, >=);
        FOR_STEP_JUMP(OP_FOR_STEP_LT_I32, <);
        FOR_STEP_JUMP(OP_FOR_STEP_LE_I32, <=);
        FOR_STEP_JUMP(OP_FOR_STEP_GT_I32, >);
        FOR_STEP_JUMP(OP_FOR_STEP_GE_I32, >=);
        FOR_STEP1_JUMP(OP_FOR_STEP1_LT_I32, <);
        RETURN_LOCAL_MATH(OP_RETURN_ADD_LOCAL, +, i32);
        RETURN_LOCAL_MATH(OP_RETURN_SUB_LOCAL, -, i32);
        RETURN_LOCAL_MATH(OP_RETURN_MUL_LOCAL, *, i32);
        RETURN_LOCAL_MATH(OP_RETURN_ADDf_LOCAL, +, f32);
        RETURN_LOCAL_MATH(OP_RETURN_SUBf_LOCAL, -, f32);
        RETURN_LOCAL_MATH(OP_RETURN_MULf_LOCAL, *, f32);
        case OP_RETURN_PRIM:
        vm_return_prim:
        {
            PrimReg retValGP = GP(0);

            // If return_ip is NULL, we were called from C (vm_call_direct)
            if (vm->call_stack[vm->frame_pointer].return_ip == NULL) {
                if (vm->frame_pointer > 0) vm->frame_pointer--;
                GP(0) = retValGP;
                RP(0) = NULL;
                goto vm_exit;
            }

            vm->primStack.sp = vm->current_frame->prim_bp;
            vm->refStack.ref_sp = vm->current_frame->ref_bp;
            ip = vm->call_stack[vm->frame_pointer].return_ip;
            if (vm->call_stack[vm->frame_pointer].return_program &&
                vm->call_stack[vm->frame_pointer].return_program != vm->program)
                vm_activate_program(vm, vm->call_stack[vm->frame_pointer].return_program);

            vm->frame_pointer--;
            vm->current_frame = &vm->call_stack[vm->frame_pointer];
            gp = vm->current_frame->GP;
            rp = vm->current_frame->RP;
            register_count = vm->current_frame->register_count;

            GP(0) = retValGP;
            RP(0) = NULL;
            continue;
        } break;
        case OP_RETURN:
        {
            PrimReg retValGP = GP(0);
            RefReg  retValRP = RP(0);

            // If return_ip is NULL, we were called from C (vm_call_direct)
            if (vm->call_stack[vm->frame_pointer].return_ip == NULL) {
                if (vm->frame_pointer > 0) vm->frame_pointer--;
                // Restore result registers for C side (optional)
                GP(0) = retValGP;
                RP(0) = retValRP;
                goto vm_exit; // EXIT INTERPRETER LOOP
            }

            // Standard Return
            vm->primStack.sp = vm->current_frame->prim_bp;
            vm->refStack.ref_sp = vm->current_frame->ref_bp;
            ip = vm->call_stack[vm->frame_pointer].return_ip;
            if (vm->call_stack[vm->frame_pointer].return_program &&
                vm->call_stack[vm->frame_pointer].return_program != vm->program)
                vm_activate_program(vm, vm->call_stack[vm->frame_pointer].return_program);

            vm->frame_pointer--;
            vm->current_frame = &vm->call_stack[vm->frame_pointer];
            gp = vm->current_frame->GP;
            rp = vm->current_frame->RP;
            register_count = vm->current_frame->register_count;

            GP(0) = retValGP;
            RP(0) = retValRP;
            continue;
        } break;
        case OP_CALL_C:
        {
            vm->native_call_count++;
            int prim_offset = GET_A(inst); // Operand A: Primitive Stack Offset
            int func_idx = GET_B(inst); // Operand B: Function Index
            int ref_offset = GET_C(inst); // Operand C: Reference Stack Offset

            CFunction f32 = NULL;
            if (vm->program && func_idx >= 0 && (size_t)func_idx < vm->program->native_target_count)
                f32 = vm->program->native_targets[func_idx].function;
            else {
                State* callState = active_state(state, vm);
                if (func_idx >= 0 && func_idx < (int)callState->cFunctions.count)
                    f32 = ((CFunction*)callState->cFunctions.data)[func_idx];
            }
            if (!f32) { VM_SYNC_STATE(); reportRuntimeError(vm, "Invalid C-Function index"); goto vm_exit; }
            const NativeSignature* native_signature =
                (vm->program && func_idx >= 0 && (size_t)func_idx < vm->program->native_target_count)
                ? vm->program->native_targets[func_idx].signature : NULL;
            ApiCallFrame frame;

            // 1. Calculate Inputs based on INDEPENDENT offsets
            uint32_t current_prim_bp = vm->current_frame->prim_bp;
            uint32_t current_ref_bp = vm->current_frame->ref_bp;

            // Check Stack Bounds
            if ((uint64_t)current_prim_bp + prim_offset >= vm->primStack.capacity) { VM_SYNC_STATE(); reportRuntimeError(vm, "Stack Overflow (Primitives)"); goto vm_exit; }
            if ((uint64_t)current_ref_bp + ref_offset >= vm->refStack.ref_cap) { VM_SYNC_STATE(); reportRuntimeError(vm, "Stack Overflow (References)"); goto vm_exit; }

            frame.prim_args = &vm->primStack.stack[current_prim_bp + prim_offset];
            frame.ref_args = vm->refStack.ref_stack + (current_ref_bp + ref_offset);
            frame.vm = vm;
            // 2. Setup Return Pointers (Assuming R0 is the standard return register)
            frame.return_prim = &vm->GP[0].bits;
            frame.return_ref = (VMObject**)&vm->RP[0];

            if (native_signature && native_signature->fast_function) {
                native_signature->fast_function(frame.prim_args,
                    frame.ref_args, frame.return_prim, frame.return_ref, vm);
                VM_RELOAD_REGISTERS();
                break;
            }

            VM_SYNC_STATE();
            f32(&frame);
            ip = vm->ip;
            VM_RELOAD_REGISTERS();
        }break;
        case OP_CALL_C32:
        {
            VM_DEBUG_CHECK(vm->program &&
                (size_t)(ip - vm->program->base) + 1 >= vm->program->bytecode_count,
                "Truncated 32-bit native call");
            vm->native_call_count++;
            // 1. Read Stack Offsets from Current Instruction
            // Note: We packed RefOffset into 'B' in the assembler
            int prim_offset = GET_A(inst);
            int ref_offset = GET_B(inst);

            // 2. Advance IP to read the next 32-bit word (The Function Index)
            ip++;
            int func_idx = *ip;

            CFunction f32 = NULL;
            if (vm->program && func_idx >= 0 && (size_t)func_idx < vm->program->native_target_count)
                f32 = vm->program->native_targets[func_idx].function;
            else {
                State* callState = active_state(state, vm);
                if (func_idx >= 0 && func_idx < (int)callState->cFunctions.count)
                    f32 = ((CFunction*)callState->cFunctions.data)[func_idx];
            }
            if (!f32) { VM_SYNC_STATE(); reportRuntimeError(vm, "Invalid C-Function index (32-bit)"); goto vm_exit; }
            const NativeSignature* native_signature =
                (vm->program && func_idx >= 0 && (size_t)func_idx < vm->program->native_target_count)
                ? vm->program->native_targets[func_idx].signature : NULL;
            ApiCallFrame frame;

            uint32_t current_prim_bp = vm->current_frame->prim_bp;
            uint32_t current_ref_bp = vm->current_frame->ref_bp;

            if ((uint64_t)current_prim_bp + prim_offset >= vm->primStack.capacity) { VM_SYNC_STATE(); reportRuntimeError(vm, "Stack Overflow (P)"); goto vm_exit; }
            if ((uint64_t)current_ref_bp + ref_offset >= vm->refStack.ref_cap) { VM_SYNC_STATE(); reportRuntimeError(vm, "Stack Overflow (R)"); goto vm_exit; }

            // 5. Setup Frame
            frame.prim_args = &vm->primStack.stack[current_prim_bp + prim_offset];
            frame.ref_args = vm->refStack.ref_stack + (current_ref_bp + ref_offset);
            frame.vm = vm;

            frame.return_prim = &vm->GP[0].bits;
            frame.return_ref = (VMObject**)&vm->RP[0];

            if (native_signature && native_signature->fast_function) {
                native_signature->fast_function(frame.prim_args,
                    frame.ref_args, frame.return_prim, frame.return_ref, vm);
                VM_RELOAD_REGISTERS();
                break;
            }

            // 6. Execute
            VM_SYNC_STATE();
            f32(&frame);
            ip = vm->ip;
            VM_RELOAD_REGISTERS();
        } break;
        case OP_CALL_C_TYPED:
        {
            vm->native_call_count++;
            const int base = GET_A(inst);
            const int func_idx = GET_B(inst);
            const NativeSignature* sig = (vm->program && func_idx >= 0 &&
                (size_t)func_idx < vm->program->native_target_count)
                ? vm->program->native_targets[func_idx].signature : NULL;
            if (!sig || !sig->fast_function || base < 0 ||
                base + sig->param_count > register_count) {
                VM_RUNTIME_ERROR("Invalid typed native call");
            }
            sig->fast_function((const uint64_t*)&gp[base],
                (VMObject* const*)&rp[base], &gp[0].bits, (VMObject**)&rp[0], vm);
        } break;
        case OP_CALL:
        {
            vm->script_call_count++;
            if (vm->frame_pointer >= 1023) goto vm_exit;

            int prim_offset = GET_A(inst);
            int func_id = GET_B(inst); // <--- This is now the ID
            int ref_offset = GET_C(inst);
            if (func_id < 0 || func_id >= MAX_FUNCTIONS) {
                VM_RUNTIME_ERROR("Invalid function ID");
            }
            int real_address = vm->program ? vm->program->func_table[func_id] : vm->func_table[func_id];

            // Safety: If address is 0 (and it's not ID 0), it might be invalid
            if (real_address == 0 && func_id != 0) {
                VM_SYNC_STATE(); reportRuntimeError(vm, "Calling undefined function"); goto vm_exit;
            }

            // 2. Standard Frame Setup
            vm->frame_pointer++;
            CallFrame* calleeFrame = &vm->call_stack[vm->frame_pointer];
            calleeFrame->function_id = func_id;
            calleeFrame->register_count = vm->program ? vm->program->register_count : MAX_REGISTERS;
            if (calleeFrame->register_count > MAX_REGISTERS) calleeFrame->register_count = MAX_REGISTERS;

            uint32_t old_prim_bp = vm->call_stack[vm->frame_pointer - 1].prim_bp;
            uint32_t old_ref_bp = vm->call_stack[vm->frame_pointer - 1].ref_bp;

            vm->call_stack[vm->frame_pointer].prim_bp = old_prim_bp + prim_offset;
            vm->call_stack[vm->frame_pointer].ref_bp = old_ref_bp + ref_offset;
            vm->call_stack[vm->frame_pointer].return_ip = ip + 1;
            vm->call_stack[vm->frame_pointer].return_program = vm->program;

            ip = vm->base + real_address;

            vm->current_frame = calleeFrame;
            gp = vm->current_frame->GP;
            rp = vm->current_frame->RP;
            register_count = vm->current_frame->register_count;
            // Inactive windows may contain references whose owners left scope.
            #ifndef NDEBUG
            memset(rp, 0, register_count * sizeof(*rp));
            #endif
            continue;
        }
        case OP_CALL32:
        {
            VM_DEBUG_CHECK(vm->program &&
                (size_t)(ip - vm->program->base) + 1 >= vm->program->bytecode_count,
                "Truncated 32-bit function call");
            vm->script_call_count++;
            if (vm->frame_pointer >= 1023) goto vm_exit;

            // 1. Read Operands from Current Instruction
            int prim_offset = GET_A(inst);
            int ref_offset = GET_B(inst); // Note: We packed Ref into B in assembler

            // 2. Advance IP to read the next 32-bit word (The Function ID)
            ip++;
            int func_id = *ip;
            if (func_id < 0 || func_id >= MAX_FUNCTIONS) {
                VM_RUNTIME_ERROR("Invalid 32-bit function ID");
            }

            // 3. Lookup Address
            int real_address = vm->program ? vm->program->func_table[func_id] : vm->func_table[func_id];

            // 4. Standard Frame Setup (Same as OP_CALL)
            vm->frame_pointer++;
            CallFrame* calleeFrame32 = &vm->call_stack[vm->frame_pointer];
            calleeFrame32->function_id = func_id;
            calleeFrame32->register_count = vm->program ? vm->program->register_count : MAX_REGISTERS;
            if (calleeFrame32->register_count > MAX_REGISTERS) calleeFrame32->register_count = MAX_REGISTERS;

            uint32_t old_prim_bp = vm->call_stack[vm->frame_pointer - 1].prim_bp;
            uint32_t old_ref_bp = vm->call_stack[vm->frame_pointer - 1].ref_bp;

            vm->call_stack[vm->frame_pointer].prim_bp = old_prim_bp + prim_offset;
            vm->call_stack[vm->frame_pointer].ref_bp = old_ref_bp + ref_offset;
            vm->call_stack[vm->frame_pointer].return_ip = ip + 1;
            vm->call_stack[vm->frame_pointer].return_program = vm->program;

            vm->current_frame = calleeFrame32;
            gp = vm->current_frame->GP;
            rp = vm->current_frame->RP;
            register_count = vm->current_frame->register_count;
            // Inactive windows may contain references whose owners left scope.
            #ifndef NDEBUG
            memset(rp, 0, register_count * sizeof(*rp));
            #endif
            // 5. Jump
            ip = vm->base + real_address;
            continue;
        }
        case OP_JUMP_IF_FALSE: {
            // Check if the value in Register A is 0 (False)
            if (GP(GET_A(inst)).i32 == 0) {
                ip += (int16_t)GET_D(inst);
                continue;
            }
        }break;
        case OP_JUMP_IF_TRUE: {
            // Check if the value in Register A is non-zero (True)
            if (GP(GET_A(inst)).i32 != 0)
            {
                int offset = (int16_t)GET_D(inst);
                ip += offset;
                continue;
            }
        } break;
        case OP_JUMP: ip += (int16_t)GET_D(inst); continue;
        case OP_CAST_I2F:GP(GET_A(inst)).f32 = (float)GP(GET_D(inst)).i32; break;
        case OP_CAST_F2I:GP(GET_A(inst)).i32 = (int)GP(GET_D(inst)).f32; break;
        case OP_SQRTf: GP(GET_A(inst)).f32 = sqrtf(fmaxf(0.0f, GP(GET_B(inst)).f32)); break;
        case OP_SINf: GP(GET_A(inst)).f32 = sinf(GP(GET_B(inst)).f32); break;
        case OP_COSf: GP(GET_A(inst)).f32 = cosf(GP(GET_B(inst)).f32); break;
        case OP_ABSf: GP(GET_A(inst)).f32 = fabsf(GP(GET_B(inst)).f32); break;
        case OP_ATAN2f: GP(GET_A(inst)).f32 = atan2f(GP(GET_B(inst)).f32, GP(GET_C(inst)).f32); break;
        case OP_MINf: GP(GET_A(inst)).f32 = fminf(GP(GET_B(inst)).f32, GP(GET_C(inst)).f32); break;
        case OP_MAXf: GP(GET_A(inst)).f32 = fmaxf(GP(GET_B(inst)).f32, GP(GET_C(inst)).f32); break;
        case OP_ALOAD_P: {
            // Instruction: A = Dest, B = Array, C = Index

            // 1. Get Array Object
            ObjRaw* arr = (ObjRaw*)RP(GET_B(inst));
            if (!arr) { VM_SYNC_STATE(); reportRuntimeError(vm, "Null Array Access"); goto vm_exit; }

            // 2. Get Index (from GP)
            int index = GP(GET_C(inst)).i32;
            if (index < 0 || index >= arr->slot_count) { VM_SYNC_STATE(); reportRuntimeError(vm, "Array Index Out of Bounds"); goto vm_exit; }

            // 3. Read Pointer
            // Assuming OBJ_POINTERS data is void* array
            void** ptrs = (void**)arr->data;

            // 4. WRITE TO RP (Critical Fix)
            RP(GET_A(inst)) = (VMObject*)ptrs[index];

        } break;
        case OP_MOVE: {// Move copies BOTH GP and RP to be safe in hybrid context
            GP(GET_A(inst)) = GP(GET_D(inst));
            RP(GET_A(inst)) = RP(GET_D(inst));
        }break;
        case OP_GET_THIS_FIELDp: {
            // Implicitly reads from Ref Stack Index 0 ('this')
            // Operand A: Dest Reg
            // Operand B: Field Offset

            // 1. Get 'this' directly
            ObjRaw* thisObj = (ObjRaw*)vm->refStack.ref_stack[vm->current_frame->ref_bp + 0];

            if (!thisObj) goto vm_exit;

            // 2. Get Field
            int slot = GET_B(inst);
            RP(GET_A(inst)) = ((void**)thisObj->data)[slot];
        } break;
        case OP_CONCAT:
        {
            // Concatenates Strings in RP[B] and RP[C], result to RP[A]
            ObjString* s1 = (ObjString*)RP(GET_B(inst));
            ObjString* s2 = (ObjString*)RP(GET_C(inst));

            // Safety checks skipped for brevity, assumed safe strings
            size_t len1 = s1 ? s1->length : 0;
            size_t len2 = s2 ? s2->length : 0;

            size_t new_len = len1 + len2;
            // Create temp buffer or handle alloc
            char* buf = malloc(new_len + 1);
            if (!buf) { VM_SYNC_STATE(); reportRuntimeError(vm, "Out of memory in string concat"); goto vm_exit; }
            if (len1) memcpy(buf, s1->data, len1);
            if (len2) memcpy(buf + len1, s2->data, len2);
            buf[new_len] = '\0';
            VM_SYNC_STATE();
            RP(GET_A(inst)) = intern_string(vm, buf, new_len);
            free(buf);
        }break;
        case OP_GET_FIELDp:
        {
            ObjRaw* raw = (ObjRaw*)RP(GET_B(inst)); // A=Dest, B=Obj, C=Offset
            if (!raw) { VM_SYNC_STATE(); reportRuntimeError(vm, "Null Ref"); goto vm_exit; }
            int slot_offset = GET_C(inst);
            if (slot_offset < 0 || slot_offset >= raw->slot_count) goto vm_exit;
            RP(GET_A(inst)) = ((void**)raw->data)[slot_offset];
        }break;
        case OP_LOADp_LOCAL:
        {
            uint32_t idx = vm->current_frame->ref_bp + GET_D(inst);
            if (idx >= vm->refStack.ref_cap) goto vm_exit;
            RP(GET_A(inst)) = vm->refStack.ref_stack[idx];
        } break;
        case OP_STOREp_LOCAL: {
            uint32_t idx = vm->current_frame->ref_bp + GET_D(inst);
            if (idx >= vm->refStack.ref_cap) goto vm_exit;
            vm->refStack.ref_stack[idx] = (VMObject*)RP(GET_A(inst)); // Copy pointer
        }break;
        case OP_CLEARp_LOCAL: {
            uint32_t idx = vm->current_frame->ref_bp + GET_D(inst);
            if (idx >= vm->refStack.ref_cap) goto vm_exit;
            vm->refStack.ref_stack[idx] = NULL;
        }break;
        case OP_DESTROYp_LOCAL: {
            uint32_t idx = vm->current_frame->ref_bp + GET_D(inst);
            if (idx >= vm->refStack.ref_cap) goto vm_exit;
            VMObject* owned = vm->refStack.ref_stack[idx];
            vm->refStack.ref_stack[idx] = NULL;
            vm->owned_ref_slots[idx] = 0;
            // Destruction mutates only the heap and this reference slot; it
            // does not observe or change the interpreter's cached registers.
            ownership_destroy(vm, owned);
        }break;
        case OP_MARK_OWNED_LOCAL: {
            uint32_t idx = vm->current_frame->ref_bp + GET_D(inst);
            if (idx >= vm->refStack.ref_cap) goto vm_exit;
            vm->owned_ref_slots[idx] = 1;
            ownership_claim(vm, vm->refStack.ref_stack[idx]);
        } break;
        case OP_MARK_UNCLAIMED_REF:
            ownership_unclaim(vm, RP(GET_A(inst)));
            break;
        case OP_DESTROY_UNCLAIMED:
            VM_SYNC_STATE();
            ownership_destroy_unclaimed(vm);
            ip = vm->ip;
            VM_RELOAD_REGISTERS();
            break;
        case OP_DESTROY_UNCLAIMED_KEEP_REF: {
            VMObject* keep = RP(GET_A(inst));
            VM_SYNC_STATE();
            ownership_destroy_unclaimed_except(vm, keep);
            ip = vm->ip;
            VM_RELOAD_REGISTERS();
        } break;
        case OP_CLONE_REF: {
            VMObject* source = (VMObject*)RP(GET_B(inst));
            VM_SYNC_STATE();
            VMObject* copy = ownership_clone(vm, source);
            ip = vm->ip;
            VM_RELOAD_REGISTERS();
            if (source && !copy) VM_RUNTIME_ERROR("Value is not copyable");
            RP(GET_A(inst)) = copy;
        }break;
        case OP_LOAD_FIELDf:
        case OP_LOAD_FIELDi:
        {
            // Instruction Layout: 
            // A = Destination Register (GP)
            // B = Object Reference Source (RP)
            // C = Field Offset (Immediate Index, 0-based slots)
            ObjRaw* raw = (ObjRaw*)RP(GET_B(inst));
            if (!raw) { VM_SYNC_STATE(); reportRuntimeError(vm, "Null Reference Exception: Attempted to read field 'int' from null."); goto vm_exit; }
            int slot_offset = GET_C(inst);
            if (slot_offset < 0 || slot_offset >= (int)raw->slot_count) {
                VM_SYNC_STATE(); reportRuntimeError(vm, "Field Access Out of Bounds");
                goto vm_exit;
            }

            uint64_t* storage = (uint64_t*)raw->data;
            GP(GET_A(inst)).bits = (int64_t)storage[slot_offset];
        }break;
        case OP_STORE_FIELDf:
        {
            ObjRaw* raw = (ObjRaw*)RP(GET_A(inst));
            if (!raw)  VM_RUNTIME_ERROR("Null Reference: Cannot write to field of null object.");
            
            int slot = GET_B(inst);
            if (slot < 0 || slot >= raw->slot_count) goto vm_exit;;
            uint64_t* storage = (uint64_t*)raw->data;
            GP(GET_C(inst)).bits &= 0xFFFFFFFF; // Optional safety
            storage[slot] = (uint64_t)GP(GET_C(inst)).bits;
            GET_TAGS_PTR(raw)[slot] = 0;
        }break;
        case OP_STORE_FIELDi: {
            ObjRaw* raw = (ObjRaw*)RP(GET_A(inst));
            if (!raw) goto vm_exit;

            int slot = GET_B(inst);
            if (slot < 0 || slot >= raw->slot_count) goto vm_exit;
            uint64_t* storage = (uint64_t*)raw->data;
            storage[slot] = (uint64_t)GP(GET_C(inst)).bits;
            GET_TAGS_PTR(raw)[slot] = 0;
        }break;
        case OP_STORE_FIELDb: {
            ObjRaw* raw = (ObjRaw*)RP(GET_A(inst));
            if (!raw) goto vm_exit;
            int slot = GET_B(inst);
            if (slot < 0 || slot >= raw->slot_count) goto vm_exit;
            uint64_t* storage = (uint64_t*)raw->data;
            storage[slot] = (uint64_t)GP(GET_C(inst)).i32;
            GET_TAGS_PTR(raw)[slot] = 0;
        }break;
        case OP_STORE_FIELDp:
        {
            ObjRaw* raw = (ObjRaw*)RP(GET_A(inst));
            if (!raw) { VM_SYNC_STATE(); reportRuntimeError(vm, "Null Ref"); goto vm_exit; }
            int slot = GET_B(inst);

            // Bounds check using SLOTS
            if (slot < 0 || slot >= raw->slot_count) { VM_SYNC_STATE(); reportRuntimeError(vm, "Field Ptr Write OOB"); goto vm_exit; }

            // Pointer arithmetic: Treat data as array of void*
            VMObject** field = &((VMObject**)raw->data)[slot];
            VMObject* incoming = (VMObject*)RP(GET_C(inst));
            if (*field && *field != incoming && (*field)->type != T_STRING) {
                VM_SYNC_STATE();
                ownership_destroy(vm, *field);
                ip = vm->ip;
                VM_RELOAD_REGISTERS();
            }
            *field = incoming;
            ownership_claim(vm, incoming);
            GET_TAGS_PTR(raw)[slot] = 1;
        }break;
        case OP_STORE_FIELDp_BORROW:
        {
            ObjRaw* raw = (ObjRaw*)RP(GET_A(inst));
            if (!raw) { VM_SYNC_STATE(); reportRuntimeError(vm, "Null Ref"); goto vm_exit; }
            int slot = GET_B(inst);
            if (slot < 0 || slot >= raw->slot_count) { VM_SYNC_STATE(); reportRuntimeError(vm, "Field Ptr Write OOB"); goto vm_exit; }
            VMObject** field = &((VMObject**)raw->data)[slot];
            if (*field && *field != (VMObject*)RP(GET_C(inst)) && GET_TAGS_PTR(raw)[slot] && (*field)->type != T_STRING) {
                VM_SYNC_STATE();
                ownership_destroy(vm, *field);
                ip = vm->ip;
                VM_RELOAD_REGISTERS();
            }
            *field = (VMObject*)RP(GET_C(inst));
            GET_TAGS_PTR(raw)[slot] = 0;
        }break;
        case OP_LOADf_GLOBAL:
        case OP_LOADi_GLOBAL:
        case OP_LOADb_GLOBAL: {
            uint32_t idx = GET_D(inst);
            uint64_t* globals = active_prim_globals(vm);
            if (!globals || idx >= active_prim_global_size(vm)) { VM_SYNC_STATE(); reportRuntimeError(vm, "Global Access Out of Bounds"); goto vm_exit; }
            GP(GET_A(inst)).bits = globals[idx];
        } break;
        case OP_LOAD8_TYPED_GLOBAL: {
            uint32_t idx = GET_D(inst);
            uint64_t* globals = active_prim_globals(vm);
            if (!globals || idx >= active_prim_global_size(vm)) goto vm_exit;
            GP(GET_A(inst)).bits = (uint64_t)(int64_t)(int8_t)globals[idx];
        } break;
        case OP_LOAD16_TYPED_GLOBAL: {
            uint32_t idx = GET_D(inst);
            uint64_t* globals = active_prim_globals(vm);
            if (!globals || idx >= active_prim_global_size(vm)) goto vm_exit;
            GP(GET_A(inst)).bits = (uint64_t)(int64_t)(int16_t)globals[idx];
        } break;
        case OP_LOADp_GLOBAL: {
            uint32_t idx = GET_D(inst);
            VMObject** globals = active_ref_globals(vm);
            if (!globals || idx >= active_ref_global_size(vm)) { VM_SYNC_STATE(); reportRuntimeError(vm, "Global Access Out of Bounds"); goto vm_exit; }
            RP(GET_A(inst)) = globals[idx];
        } break;
        case OP_STOREp_GLOBAL:
        {
            uint32_t idx = GET_D(inst);
            VMObject** globals = active_ref_globals(vm);
            if (!globals || idx >= active_ref_global_size(vm)) goto vm_exit;
            globals[idx] = (VMObject*)RP(GET_A(inst));
            ownership_claim(vm, globals[idx]);
        }break;
        case OP_STORE32_GLOBAL:
        case OP_STORE8_GLOBAL: {
            uint32_t idx = GET_D(inst);
            uint64_t* globals = active_prim_globals(vm);
            if (!globals || idx >= active_prim_global_size(vm)) goto vm_exit;
            globals[idx] = GP(GET_A(inst)).bits;
        }break;
        case OP_STORE8_TYPED_GLOBAL: {
            uint32_t idx = GET_D(inst);
            uint64_t* globals = active_prim_globals(vm);
            if (!globals || idx >= active_prim_global_size(vm)) goto vm_exit;
            globals[idx] = (uint64_t)(int64_t)(int8_t)GP(GET_A(inst)).bits;
        }break;
        case OP_STORE16_TYPED_GLOBAL: {
            uint32_t idx = GET_D(inst);
            uint64_t* globals = active_prim_globals(vm);
            if (!globals || idx >= active_prim_global_size(vm)) goto vm_exit;
            globals[idx] = (uint64_t)(int64_t)(int16_t)GP(GET_A(inst)).bits;
        }break;
        case OP_LOADf_LOCAL:
        case OP_LOAD32_LOCAL:
        {
            uint32_t idx = vm->current_frame->prim_bp + GET_D(inst);
            GP(GET_A(inst)).bits = vm->primStack.stack[idx];
        }break;
        case OP_LOAD8_LOCAL:
        {
            uint32_t idx = vm->current_frame->prim_bp + GET_D(inst);
            GP(GET_A(inst)).bits = (uint64_t)(uint8_t)vm->primStack.stack[idx];
        }break;
        case OP_LOAD8_TYPED_LOCAL:
        {
            uint32_t idx = vm->current_frame->prim_bp + GET_D(inst);
            GP(GET_A(inst)).bits = (uint64_t)(int64_t)(int8_t)vm->primStack.stack[idx];
        }break;
        case OP_LOAD16_TYPED_LOCAL:
        {
            uint32_t idx = vm->current_frame->prim_bp + GET_D(inst);
            GP(GET_A(inst)).bits = (uint64_t)(int64_t)(int16_t)vm->primStack.stack[idx];
        }break;
        case OP_STORE32_LOCAL_K: {
            uint32_t idx = vm->current_frame->prim_bp + GET_D(inst);
            array* constants = active_constants(vm);
            Value val = ((Value*)constants->data)[GET_A(inst)];
            vm->primStack.stack[idx] = val.v.bits;
            break;
        }
        case OP_STORE32_GLOBAL_K: {
            uint32_t idx = GET_D(inst);
            uint64_t* globals = active_prim_globals(vm);
            if (!globals || idx >= active_prim_global_size(vm)) { VM_SYNC_STATE(); reportRuntimeError(vm, "Global Access Out of Bounds"); goto vm_exit; }
            array* constants = active_constants(vm);
            globals[idx] = ((Value*)constants->data)[GET_A(inst)].v.bits;
            break;
        }
        case OP_STORE32_LOCAL:
        {
            uint32_t idx = vm->current_frame->prim_bp + GET_D(inst);
            if (idx >= vm->primStack.capacity) goto vm_exit;
            int v = (int)GP(GET_A(inst)).bits;
            vm->primStack.stack[idx] = GP(GET_A(inst)).bits; // Copy raw bits
        }break;
        case OP_STORE8_TYPED_LOCAL:
        {
            uint32_t idx = vm->current_frame->prim_bp + GET_D(inst);
            if (idx >= vm->primStack.capacity) goto vm_exit;
            vm->primStack.stack[idx] = (uint64_t)(int64_t)(int8_t)GP(GET_A(inst)).bits;
        }break;
        case OP_STORE16_TYPED_LOCAL:
        {
            uint32_t idx = vm->current_frame->prim_bp + GET_D(inst);
            if (idx >= vm->primStack.capacity) goto vm_exit;
            vm->primStack.stack[idx] = (uint64_t)(int64_t)(int16_t)GP(GET_A(inst)).bits;
        }break;
        case OP_LOAD_NULL: {
            // A = Destination Register (RP)
            // We set it to NULL
            int a = GET_A(inst);
            RP(a) = NULL;
        }break;
        case OP_LOAD_Ki:GP(GET_A(inst)).i32 = GET_D(inst); break;
        case OP_LOAD_STORE_LOCAL: {
            uint32_t bp = vm->current_frame->prim_bp;
            uint32_t src = bp + GET_B(inst), dst = bp + GET_C(inst);
            if (src >= vm->primStack.capacity || dst >= vm->primStack.capacity) {
                VM_RUNTIME_ERROR("Local copy operand out of bounds");
            }
            GP(GET_A(inst)).bits = vm->primStack.stack[src];
            vm->primStack.stack[dst] = GP(GET_A(inst)).bits;
            break;
        }
        case OP_LOADKi_STORE_LOCAL: {
            uint32_t dst = vm->current_frame->prim_bp + GET_B(inst);
            if (dst >= vm->primStack.capacity) {
                VM_RUNTIME_ERROR("Local store operand out of bounds");
            }
            GP(GET_A(inst)).i32 = (int8_t)GET_C(inst);
            vm->primStack.stack[dst] = GP(GET_A(inst)).bits;
            break;
        }
        case OP_LOAD_K: {
            int constIdx = GET_D(inst);
            array* constants = active_constants(vm);
            if ((unsigned)constIdx >= (unsigned)constants->count) {
                VM_RUNTIME_ERROR("Constant index out of bounds");
            }
            Value* valPtr = &((Value*)constants->data)[constIdx];

            if (valPtr->type.baseType == T_STRING) {
                // Intern string from VM memory
                VM_SYNC_STATE();
                RP(GET_A(inst)) = intern_string(vm, (char*)valPtr->v.p, strlen((char*)valPtr->v.p));
            }
            else {
                if (valPtr->type.baseType == T_FLOAT)
                    GP(GET_A(inst)).f32 = valPtr->v.f32;
                else
                    GP(GET_A(inst)).i32 = valPtr->v.i32;
            }
        } break;
        case OP_ALLOCATE:
        {
            int slots = GET_D(inst); // This is now a Slot Count
            if (slots <= 0) slots = 1;

            // Calculate Size: Slots (8 bytes) + Tags (1 byte)
            size_t data_bytes = slots * sizeof(uint64_t);
            size_t tag_bytes = slots * sizeof(uint8_t);

            // Allocation updates VM heap metadata only; avoid publishing and
            // reloading every register around this common operation.
            ObjRaw* obj = (ObjRaw*)ownership_allocate(vm, sizeof(ObjRaw) + data_bytes + tag_bytes, T_SHARED_PTR);
            obj->slot_count = slots;
            obj->data_bytes = data_bytes;
            obj->element_size = sizeof(uint64_t);
            obj->kind = OBJ_INSTANCE;

            memset(obj->data, 0, data_bytes + tag_bytes);
            RP(GET_A(inst)) = (void*)obj;
        }break;
        case OP_ALLOCATE_PACKED_OBJECT:
        {
            size_t data_bytes = GET_B(inst);
            size_t field_count = GET_C(inst);
            size_t tag_bytes = field_count * sizeof(uint8_t);
            // Keep the packed path consistent with OP_ALLOCATE above.
            ObjRaw* obj = (ObjRaw*)ownership_allocate(vm,
                sizeof(ObjRaw) + data_bytes + tag_bytes, T_SHARED_PTR);
            obj->slot_count = field_count;
            obj->data_bytes = data_bytes;
            obj->element_size = 1;
            obj->kind = OBJ_INSTANCE;
            memset(obj->data, 0, data_bytes + tag_bytes);
            RP(GET_A(inst)) = (void*)obj;
        }break;
        case OP_LOAD_PACKED_I8:
        case OP_LOAD_PACKED_I16:
        case OP_LOAD_PACKED_I32:
        case OP_LOAD_PACKED_F32:
        {
            ObjRaw* raw = (ObjRaw*)RP(GET_B(inst));
            size_t offset = GET_C(inst);
            size_t width = (op == OP_LOAD_PACKED_I8) ? 1 : (op == OP_LOAD_PACKED_I16) ? 2 : 4;
            if (!raw || offset + width > raw->data_bytes) goto vm_exit;
            uint64_t bits = 0;
            memcpy(&bits, raw->data + offset, width);
            if (op == OP_LOAD_PACKED_I8) GP(GET_A(inst)).bits = (uint64_t)(int64_t)(int8_t)bits;
            else if (op == OP_LOAD_PACKED_I16) GP(GET_A(inst)).bits = (uint64_t)(int64_t)(int16_t)bits;
            else GP(GET_A(inst)).bits = bits;
        }break;
        case OP_STORE_PACKED_I8:
        case OP_STORE_PACKED_I16:
        case OP_STORE_PACKED_I32:
        case OP_STORE_PACKED_F32:
        {
            ObjRaw* raw = (ObjRaw*)RP(GET_A(inst));
            size_t offset = GET_B(inst);
            size_t width = (op == OP_STORE_PACKED_I8) ? 1 : (op == OP_STORE_PACKED_I16) ? 2 : 4;
            if (!raw || offset + width > raw->data_bytes) goto vm_exit;
            memcpy(raw->data + offset, &GP(GET_C(inst)).bits, width);
        }break;
        case OP_ALLOCATE_ARRAY_PTR:
        {
            int sizeReg = GET_B(inst);
            int count = GP(sizeReg).i32;

            if (count < 0) {
                VM_SYNC_STATE(); reportRuntimeError(vm, "Negative array size"); goto vm_exit;
            }

            size_t bytes = count * sizeof(void*);
            VM_SYNC_STATE();
            ObjRaw* obj = (ObjRaw*)ownership_allocate(vm, sizeof(ObjRaw) + bytes, T_SHARED_PTR);
            obj->kind = OBJ_POINTERS;
            obj->slot_count = count;
            obj->data_bytes = bytes;
            obj->element_size = sizeof(uint64_t);
            memset(obj->data, 0, bytes);

            RP(GET_A(inst)) = (void*)obj;
        }break;
        case OP_NEWARRAY: {
            // Operands: A = Dest Register, D = Size Register
            // CodeGen: emitAD_IR(state, OP_NEWARRAY, destRegister, sizeReg);
            // D contains the Register Index where the size is stored.
            int sizeRegIndex = GET_D(inst); // Opcode uses is_AD format
            int count = GP(sizeRegIndex).i32;

            if (count < 0) { VM_SYNC_STATE(); reportRuntimeError(vm, "Negative array size"); goto vm_exit; }

            // Slot-based: each slot is 8 bytes (uint64_t)
            size_t bytes = count * sizeof(uint64_t);
            VM_SYNC_STATE();
            ObjRaw* obj = (ObjRaw*)ownership_allocate(vm, sizeof(ObjRaw) + bytes, T_SHARED_PTR);
            obj->kind = OBJ_BLOB; // Primitive arrays contain no owned references.
            obj->slot_count = count;
            obj->data_bytes = bytes;
            obj->element_size = sizeof(uint64_t);
            memset(obj->data, 0, bytes);

            RP(GET_A(inst)) = (VMObject*)obj;
        }break;
        case OP_NEWARRAY_TYPED: {
            int count = GP(GET_B(inst)).i32;
            size_t elementSize = (size_t)GET_C(inst);
            if (count < 0 || (elementSize != 1 && elementSize != 2 && elementSize != 4)) goto vm_exit;
            size_t bytes = (size_t)count * elementSize;
            size_t tagBytes = (size_t)count;
            ObjRaw* obj = (ObjRaw*)ownership_allocate(vm, sizeof(ObjRaw) + bytes + tagBytes, T_SHARED_PTR);
            obj->kind = OBJ_BLOB;
            obj->slot_count = (size_t)count;
            obj->data_bytes = bytes;
            obj->element_size = elementSize;
            memset(obj->data, 0, bytes + tagBytes);
            RP(GET_A(inst)) = (VMObject*)obj;
        }break;
        case OP_ASTORE_P: {
            // CodeGen: A=Array, B=Index, C=Value
            // Slot-based storage: always 8-byte slots
            VM_SYNC_STATE();
            uint64_t* storage = (uint64_t*)get_array_element_addr(vm, inst, gp, rp);
            if (!storage) goto vm_exit;
            VMObject* previous = (VMObject*)(uintptr_t)*storage;
            VMObject* incoming = (VMObject*)RP(GET_C(inst));
            if (previous && previous != incoming && previous->type != T_STRING) {
                ownership_destroy(vm, previous);
                ip = vm->ip;
                VM_RELOAD_REGISTERS();
            }
            *storage = (uint64_t)RP(GET_C(inst)); // Value is in C (Reference Stack)
            ownership_claim(vm, incoming);
        }break;
        case OP_ASTORE_F: {
            // CodeGen: A=Array, B=Index, C=Value
            // Slot-based storage: floats are stored in 8-byte slots
            VM_SYNC_STATE();
            uint64_t* storage = (uint64_t*)get_array_element_addr(vm, inst, gp, rp);
            if (!storage) goto vm_exit;
            *(float*)storage = 0;
            *(float*)storage = GP(GET_C(inst)).f32; // Value is in C
        } break;
        case OP_ASTORE_I: {
            // CodeGen: A=Array, B=Index, C=Value
            // Slot-based storage: ints are stored in 8-byte slots
            VM_SYNC_STATE();
            uint64_t* storage = (uint64_t*)get_array_element_addr(vm, inst, gp, rp);
            if (!storage) goto vm_exit;
            *storage = (uint64_t)GP(GET_C(inst)).i32; // Value is in C, zero-extended to 64-bit
        } break;
        case OP_ASTORE_I8:
        case OP_ASTORE_I16:
        case OP_ASTORE_F32: {
            ObjRaw* arr = (ObjRaw*)RP(GET_A(inst));
            if (!arr) goto vm_exit;
            int index = GP(GET_B(inst)).i32;
            size_t width = GET_OP(inst) == OP_ASTORE_I8 ? 1u :
                (GET_OP(inst) == OP_ASTORE_I16 ? 2u : 4u);
            if (index < 0 || index >= arr->slot_count || arr->element_size != width) goto vm_exit;
            uint8_t* dst = arr->data + (size_t)index * width;
            if (width == 1) *(int8_t*)dst = (int8_t)GP(GET_C(inst)).bits;
            else if (width == 2) *(int16_t*)dst = (int16_t)GP(GET_C(inst)).bits;
            else memcpy(dst, &GP(GET_C(inst)).f32, sizeof(float));
        } break;
        case OP_ALOAD_I:
        {
            // Load uses a different format usually: A=Dest, B=Array, C=Index
            // Slot-based: always 8-byte slots (uint64_t)

            ObjRaw* arr = (ObjRaw*)RP(GET_B(inst));
            if (!arr) goto vm_exit;

            int index = GP(GET_C(inst)).i32;
            if (index < 0 || index >= arr->slot_count) goto vm_exit;

            uint64_t* storage = (uint64_t*)arr->data;
            GP(GET_A(inst)).i32 = (int32_t)storage[index];  // Load from slot, cast to int
        } break;
        case OP_ALOAD_I8:
        case OP_ALOAD_I16:
        case OP_ALOAD_F32: {
            ObjRaw* arr = (ObjRaw*)RP(GET_B(inst));
            if (!arr) goto vm_exit;
            int index = GP(GET_C(inst)).i32;
            size_t width = GET_OP(inst) == OP_ALOAD_I8 ? 1u :
                (GET_OP(inst) == OP_ALOAD_I16 ? 2u : 4u);
            if (index < 0 || index >= arr->slot_count || arr->element_size != width) goto vm_exit;
            uint8_t* src = arr->data + (size_t)index * width;
            if (width == 1)
                GP(GET_A(inst)).bits = (uint64_t)(int64_t)*(int8_t*)src;
            else if (width == 2)
                GP(GET_A(inst)).bits = (uint64_t)(int64_t)*(int16_t*)src;
            else
                memcpy(&GP(GET_A(inst)).f32, src, sizeof(float));
        } break;
        case OP_ALOAD_I_LOCALS:
        case OP_ALOAD_F_LOCALS: {
            uint32_t ref = vm->current_frame->ref_bp + GET_B(inst);
            uint32_t idx = vm->current_frame->prim_bp + GET_C(inst);
            if (ref >= vm->refStack.ref_cap || idx >= vm->primStack.capacity) {
                VM_RUNTIME_ERROR("Array local operand out of bounds");
            }
            ObjRaw* arr = (ObjRaw*)vm->refStack.ref_stack[ref];
            if (!arr) { VM_RUNTIME_ERROR("Null Array Access"); }
            int index = (int32_t)vm->primStack.stack[idx];
            if (index < 0 || index >= arr->slot_count) {
                VM_RUNTIME_ERROR("Array Index Out of Bounds");
            }
            if (GET_OP(inst) == OP_ALOAD_F_LOCALS && arr->element_size == sizeof(float)) {
                uint8_t* src = arr->data + (size_t)index * sizeof(float);
                memcpy(&GP(GET_A(inst)).f32, src, sizeof(float));
            }
            else {
                // Legacy int arrays and old slot-based float arrays use 8-byte slots.
                uint64_t* storage = (uint64_t*)arr->data;
                if (GET_OP(inst) == OP_ALOAD_F_LOCALS)
                    GP(GET_A(inst)).f32 = *(float*)&storage[index];
                else
                    GP(GET_A(inst)).i32 = (int32_t)storage[index];
            }
            break;
        }
        case OP_ALOAD_F: {
            ObjRaw* arr = (ObjRaw*)RP(GET_B(inst));
            if (!arr) goto vm_exit;
            int index = GP(GET_C(inst)).i32;
            if (index < 0 || index >= arr->slot_count) goto vm_exit;
            uint64_t* storage = (uint64_t*)arr->data;
            GP(GET_A(inst)).f32 = *(float*)&storage[index];
        } break;
        case OP_NEG: GP(GET_A(inst)).i32 = -GP(GET_B(inst)).i32; break;
        case OP_NEGf: GP(GET_A(inst)).f32 = -GP(GET_B(inst)).f32; break;
        case OP_NOT: GP(GET_A(inst)).i32 = GP(GET_B(inst)).i32 ? 0 : 1; break;
        case OP_EQ_REF: {
                bool eq = (RP(GET_B(inst)) == RP(GET_C(inst)));
                GP(GET_A(inst)).i32 = eq ? 1 : 0;
            }break;
        case OP_DIV:
            if (GP(GET_C(inst)).i32 == 0) { VM_SYNC_STATE(); reportRuntimeError(vm, "Div by zero"); goto vm_exit; }
            GP(GET_A(inst)).i32 = GP(GET_B(inst)).i32 / GP(GET_C(inst)).i32;
            break;
        case OP_DIVf:
            if (GP(GET_C(inst)).f32 == 0) { VM_SYNC_STATE(); reportRuntimeError(vm, "Div by zero"); goto vm_exit; }
            GP(GET_A(inst)).f32 = GP(GET_B(inst)).f32 / GP(GET_C(inst)).f32;
            break;
            IMPLEMENT_MATH_OP(OP_ADD, +, i32);
            IMPLEMENT_MATH_OP(OP_ADDf, +, f32);
            IMPLEMENT_MATH_OP(OP_SUB, -, i32);
            IMPLEMENT_MATH_OP(OP_SUBf, -, f32);
            IMPLEMENT_MATH_OP(OP_MUL, *, i32);
            IMPLEMENT_MATH_OP(OP_MULf, *, f32);

            IMPLEMENT_COMP_OP(OP_NEf, != , f32);
            IMPLEMENT_COMP_OP(OP_NE, != , i32);
            IMPLEMENT_COMP_OP(OP_EQ, == , i32);
            IMPLEMENT_COMP_OP(OP_LT, < , i32);
            IMPLEMENT_COMP_OP(OP_LE, <= , i32);
            IMPLEMENT_COMP_OP(OP_GE, >= , i32);
            IMPLEMENT_COMP_OP(OP_GT, > , i32);
            IMPLEMENT_COMP_OP(OP_EQf, == , f32);
            IMPLEMENT_COMP_OP(OP_LTf, < , f32);
            IMPLEMENT_COMP_OP(OP_LEf, <= , f32);
            IMPLEMENT_COMP_OP(OP_GEf, >= , f32);
            IMPLEMENT_COMP_OP(OP_GTf, > , f32);
            IMPLEMENT_COND_JUMP(OP_JEQ, == , i32);
            //IMPLEMENT_COND_JUMP(OP_JEQ, == , i32);
            IMPLEMENT_COND_JUMP(OP_JNE, != , i32);
            IMPLEMENT_COND_JUMP(OP_JGE, >= , i32);
            IMPLEMENT_COND_JUMP(OP_JGT, > , i32);
            IMPLEMENT_COND_JUMP(OP_JLE, <= , i32);
            IMPLEMENT_COND_JUMP(OP_JLT, < , i32);

            IMPLEMENT_COND_JUMP(OP_JEQf, == , f32);
            IMPLEMENT_COND_JUMP(OP_JNEf, != , f32);
            IMPLEMENT_COND_JUMP(OP_JGEf, >= , f32);
            IMPLEMENT_COND_JUMP(OP_JGTf, > , f32);

            IMPLEMENT_COND_JUMP(OP_JLEf, <= , f32);
            IMPLEMENT_COND_JUMP(OP_JLTf, < , f32);

            CASE_INC_GLOBAL_INT(OP_DECi_GLOBAL, -=);
            CASE_INC_GLOBAL_INT(OP_INCi_GLOBAL, +=);
            CASE_INC_GLOBAL_FLOAT(OP_DECf_GLOBAL, -=);
            CASE_INC_GLOBAL_FLOAT(OP_INCf_GLOBAL, +=);
            CASE_INC_LOCAL_INT(OP_DECi_LOCAL, -=);
            CASE_INC_LOCAL_INT(OP_INCi_LOCAL, +=);
            CASE_INC_LOCAL_FLOAT(OP_DECf_LOCAL, -=);
            CASE_INC_LOCAL_FLOAT(OP_INCf_LOCAL, +=);
        case OP_ADDi_LOCAL_IMM: {
            uint32_t idx = vm->current_frame->prim_bp + GET_D(inst);
            if (idx >= vm->primStack.capacity) {
                VM_RUNTIME_ERROR("Stack Overflow (local increment)");
            }
            int32_t value = (int32_t)vm->primStack.stack[idx];
            value += (int8_t)GET_A(inst);
            vm->primStack.stack[idx] = (uint64_t)(int64_t)value;
            break;
        }
        case OP_ADDf_LOCAL_K: {
            uint32_t idx = vm->current_frame->prim_bp + GET_D(inst);
            array* constants = active_constants(vm);
            if (idx >= vm->primStack.capacity || GET_A(inst) >= constants->count) {
                VM_RUNTIME_ERROR("Local update operand out of bounds");
            }
            PrimReg value;
            value.bits = vm->primStack.stack[idx];
            value.f32 += ((Value*)constants->data)[GET_A(inst)].v.f32;
            vm->primStack.stack[idx] = value.bits & UINT32_MAX;
            break;
        }
        case OP_SUBf_LOCAL_K: {
            uint32_t idx = vm->current_frame->prim_bp + GET_D(inst);
            array* constants = active_constants(vm);
            if (idx >= vm->primStack.capacity || GET_A(inst) >= constants->count) {
                VM_RUNTIME_ERROR("Local update operand out of bounds");
            }
            PrimReg value;
            value.bits = vm->primStack.stack[idx];
            value.f32 -= ((Value*)constants->data)[GET_A(inst)].v.f32;
            vm->primStack.stack[idx] = value.bits & UINT32_MAX;
            break;
        }
        case OP_HALT:goto vm_exit;
        default:
            VM_RUNTIME_ERROR("Invalid or unimplemented opcode");
        }
        ip++;
    }

vm_exit:
    VM_SYNC_STATE();
    if (vm->had_runtime_error)
        unwind_owned_reference_slots(vm);
    return;
}


bool vm_call_program_direct(State* state, VMProgram* program, int32_t* targetIP, float dt) {
    VMContext* vm = state->vm;
    if (!vm || !program || !targetIP || vm->had_runtime_error) return false;
    vm_activate_program(vm, program);

    const int previous_frame = vm->frame_pointer;
    const uint32_t previous_prim_sp = vm->primStack.sp;
    const uint32_t previous_ref_sp = vm->refStack.ref_sp;
    if (previous_frame >= 1023 || previous_prim_sp >= vm->primStack.capacity ||
        previous_ref_sp >= vm->refStack.ref_cap) {
        reportRuntimeError(vm, "VM execution stack exhausted");
        return false;
    }

    // 1. Push Frame
    vm->frame_pointer++;

    // Capture the current Stack Pointers as the Base Pointers for the new frame
    vm->call_stack[vm->frame_pointer].prim_bp = vm->primStack.sp;
    vm->call_stack[vm->frame_pointer].ref_bp = vm->refStack.ref_sp;
    vm->call_stack[vm->frame_pointer].return_ip = NULL; // Flag to return to C
    vm->call_stack[vm->frame_pointer].return_program = program;
    vm->call_stack[vm->frame_pointer].register_count = program->register_count;
    vm->call_stack[vm->frame_pointer].function_id = -1;
    for (int functionId = 0; functionId < program->func_count; ++functionId) {
        if (targetIP == program->base + program->func_table[functionId]) {
            vm->call_stack[vm->frame_pointer].function_id = functionId;
            break;
        }
    }

    // 2. Push DT (Primitive Stack)
    uint64_t dtBits;
    memcpy(&dtBits, &dt, sizeof(float));
    vm->primStack.stack[vm->primStack.sp++] = dtBits;

    // 3. Set IP
    vm->ip = targetIP;

    // 4. Run
    execute_instructions(state, vm);
    const bool debugQuit = vm->debugMode == DEBUG_QUIT;
    // Always restore the exact caller state, including nested/re-entrant calls.
    vm->primStack.sp = previous_prim_sp;
    vm->refStack.ref_sp = previous_ref_sp;
    vm->frame_pointer = previous_frame;
    vm->current_frame = &vm->call_stack[previous_frame];
    return !debugQuit && !vm->had_runtime_error;
}

bool vm_call_direct(State* state, int32_t* targetIP, float dt) {
    return vm_call_program_direct(state, state ? state->program : NULL, targetIP, dt);
}

void vm_run_global(State* state, bool startInDebugMode) {
    VMContext* vm = state->vm;
    if (!vm || vm->had_runtime_error) return;
    vm_activate_program(vm, state->program);
    vm->debugMode = startInDebugMode ? DEBUG_STEPPING : DEBUG_RUNNING;

    // Start after the headers
    vm->ip = vm->code_start;
    vm->frame_pointer = 0;
    vm->call_stack[0].function_id = -1;

    vm->call_stack[0].register_count = vm->program->register_count;
    execute_instructions(state, vm);
}

void vm_free(State* state) {
    VMContext* vm = state->vm;
    // Teardown
    for (int p = 0; p < vm->programs.count; p++) {
        VMProgram* program = *(VMProgram**)array_get(&vm->programs, p);
        if (!program) continue;
        if (program->owns_constants) {
            for (int i = 0; i < program->constants.count; i++) {
                Value* v = (Value*)array_get(&program->constants, i);
                if (v->type.baseType == T_STRING && v->v.p) {
                    free(v->v.p);
                    v->v.p = NULL;
                }
            }
            array_destroy(&program->constants);
        }
        freeProgramDiagnostics(program);
        free(program->prim_globals);
        free(program->ref_globals);
        free(program->native_targets);
        free(program);
    }
    array_destroy(&vm->programs);

    free(vm->primStack.stack);
    free(vm->refStack.ref_stack);
    free(vm->owned_ref_slots);
    free(vm->prim_globals);
    free(vm->ref_globals);

    if (vm->classes) {
        for (int i = 0; i < vm->class_count; i++) {
            if (vm->classes[i].ptr_offsets) {
                free(vm->classes[i].ptr_offsets);
            }
        }
        free(vm->classes);
    }

    ownership_destroy_all(vm);

    // 4. Free String Intern Table
    if (vm->strings) free(vm->strings);
    free(vm);
}
