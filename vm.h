#pragma once
#include "AST.h"
#include "types.h"
#include <stdbool.h>
#include "debugger.h"
#include "Ownership.h"

#define STACK_SIZE 256  // Maximum number of local
#define MAX_CALLS 100   // Maximum number of nested calls
#define MAX_BREAKPOINTS 16
#define MAX_REGISTERS 50
#define MAX_FUNCTIONS 256

typedef uint64_t TValue;

#define GET_NATIVE(T, idx) (*(T**)(((ObjRaw*)frame->ref_args[idx])->data))


// Helpers to read bits from uint64_t stack slots
#define GET_INT(i32)   ((int)vm_unpack_int32(frame->prim_args[i32]))
#define GET_PTR(i32)   ((uint64_t*)(frame->prim_args[i32]))
#define GET_FLOAT(i32) (vm_unpack_float32(frame->prim_args[i32]))
#define GET_BOOL(i32)  ((bool)vm_unpack_uint8(frame->prim_args[i32]))
#define RETURN_VALi(val) *frame->return_prim = vm_pack_int32((int32_t_value)(val));
#define RETURN_VALp(val) (uint64_t*)frame->return_prim = val;
#define RETURN_VALf(val) *frame->return_prim = vm_pack_float32((float32)(val));


// Helpers to get Reference objects
// Note: For methods, ref_args[0] is always 'this'
#define GET_REF(i32)   (frame->ref_args[i32])
//#define GET_STRING(i32) (((ObjString*)frame->ref_args[i32])->data)
#define RETURN_REF(val) *frame->return_ref = (VMObject*)val;

#define GET_STRING(frame, idx) (((ObjString*)frame->ref_args[idx])->data)

#define RETURN_NATIVE(T, ptr, dtor) frame->return_ref[0] = (VMObject*)create_native_handle(frame->vm, (void*)ptr, dtor);

#define GET_NATIVE_FROM_SLOT(T, data) (*(T**)(data))
#define BIND_GLOBAL(name, func) addFuncSymbol(state, #name, SYM_CFUNCTION, func)

#define BEGIN_BIND_CLASS(T) \
    void bind_##T(State* state) { \
        Symbol* cls = addClassSymbol(state, #T); \
        const char* __className = #T;

#define BIND_CONSTRUCTOR(func) \
    do { \
        char fullName[128]; \
        snprintf(fullName, sizeof(fullName), "%s.create", __className); \
        addFuncSymbol(state, fullName, SYM_CFUNCTION, func); \
    } while(0)

#define BIND_CONSTRUCTOR(func) \
    do { \
        char fullName[128]; \
        snprintf(fullName, sizeof(fullName), "%s.create", __className); \
        addFuncSymbol(state, fullName, SYM_CFUNCTION, func); \
    } while(0)

#define BIND_METHOD(name, func) \
    do { \
        char fullName[128]; \
        snprintf(fullName, sizeof(fullName), "%s.%s", __className, #name); \
        addFuncSymbol(state, fullName, SYM_CFUNCTION, func); \
    } while(0);

#define BIND_FAST_METHOD0(name, func, retType, retRef) \
    do { \
        char fullName[128]; \
        snprintf(fullName, sizeof(fullName), "%s.%s", __className, #name); \
        static const NativeSignature sig = { NULL, 0, retType, retRef, func }; \
        addFastTypedFuncSymbol(state, fullName, func, &sig); \
    } while(0)

// Register a one-argument typed method whose argument is a managed reference
// (for example, a primitive array). The callback can validate/extract its
// backing bytes with vm_get_native_array_view without copying the elements.
#define BIND_FAST_METHOD1_REF(name, func, retType, retRef) \
    do { \
        char fullName[128]; \
        snprintf(fullName, sizeof(fullName), "%s.%s", __className, #name); \
        static const NativeParamType params[] = {{ VM_SCALAR_NONE, 1 }}; \
        static const NativeSignature sig = { params, 1, retType, retRef, func }; \
        addFastTypedFuncSymbol(state, fullName, func, &sig); \
    } while(0)

#define BIND_FAST_METHOD1(name, func, retType, retRef, t0) \
    do { \
        char fullName[128]; \
        snprintf(fullName, sizeof(fullName), "%s.%s", __className, #name); \
        static const NativeParamType params[] = {{ t0, 0 }}; \
        static const NativeSignature sig = { params, 1, retType, retRef, func }; \
        addFastTypedFuncSymbol(state, fullName, func, &sig); \
    } while(0)

#define BIND_FAST_METHOD2(name, func, retType, retRef, t0, t1) \
    do { \
        char fullName[128]; \
        snprintf(fullName, sizeof(fullName), "%s.%s", __className, #name); \
        static const NativeParamType params[] = {{ t0, 0 }, { t1, 0 }}; \
        static const NativeSignature sig = { params, 2, retType, retRef, func }; \
        addFastTypedFuncSymbol(state, fullName, func, &sig); \
    } while(0)

#define BIND_FAST_METHOD3(name, func, retType, retRef, t0, t1, t2) \
    do { \
        char fullName[128]; \
        snprintf(fullName, sizeof(fullName), "%s.%s", __className, #name); \
        static const NativeParamType params[] = {{ t0, 0 }, { t1, 0 }, { t2, 0 }}; \
        static const NativeSignature sig = { params, 3, retType, retRef, func }; \
        addFastTypedFuncSymbol(state, fullName, func, &sig); \
    } while(0)

#define BIND_FAST_METHOD4(name, func, retType, retRef, t0, t1, t2, t3) \
    do { \
        char fullName[128]; \
        snprintf(fullName, sizeof(fullName), "%s.%s", __className, #name); \
        static const NativeParamType params[] = {{ t0, 0 }, { t1, 0 }, { t2, 0 }, { t3, 0 }}; \
        static const NativeSignature sig = { params, 4, retType, retRef, func }; \
        addFastTypedFuncSymbol(state, fullName, func, &sig); \
    } while(0)

#define BIND_FAST_METHOD5(name, func, retType, retRef, t0, t1, t2, t3, t4) \
    do { \
        char fullName[128]; \
        snprintf(fullName, sizeof(fullName), "%s.%s", __className, #name); \
        static const NativeParamType params[] = {{ t0, 0 }, { t1, 0 }, { t2, 0 }, { t3, 0 }, { t4, 0 }}; \
        static const NativeSignature sig = { params, 5, retType, retRef, func }; \
        addFastTypedFuncSymbol(state, fullName, func, &sig); \
    } while(0)


#define BIND_FIELD(fieldName, type) \
    do { \
        char fullName[128]; \
        snprintf(fullName, sizeof(fullName), "%s.%s", __className, #fieldName); \
        registerField(state, fullName, type); \
    } while(0)

#define BIND_CONST(name, value) \
    do { \
        char fullName[128]; \
        snprintf(fullName, sizeof(fullName), "%s.%s", __className, #name); \
        addConstantInt(state, fullName, value); \
    } while(0)


#define END_BIND_CLASS() }

#define RETURN_NATIVE(T, ptr, dtor) \
    frame->return_ref[0] = (VMObject*)create_native_handle(frame->vm, (void*)ptr, dtor);

#define BIND_GLOBAL_CONST(name, value) \
    addConstantInt(state, #name, value)


// ──────────────────────────────────────────────────────────────
// 1. Core Definitions (Statically Typed)
// ──────────────────────────────────────────────────────────────

// Defines the memory layout of an object type
typedef struct {
    char* name;         // For debug
    size_t size;        // Total size in bytes
    int* ptr_offsets;   // Array of byte-offsets where pointers exist
    int ptr_count;      // How many pointers
} ClassDescriptor;

typedef struct {
    const void* data;
    size_t length;
    size_t byte_length;
    size_t element_size;
    VMScalarType element_type;
} VMNativeArrayView;

// A Primitive Register (64-bit raw storage for Int/Float)
typedef union {
    int64_t  i64;
    int32_t  i32;
    double   f64;
    float    f32;
    uint64_t bits;
} PrimReg;

// A Reference Register (Always a pointer)
typedef void* RefReg;

//typedef void* (*CFunction)(void*);
typedef int32_t int32;
typedef struct PimitiveStack {
    uint64_t* stack;
    uint32_t  sp;     // Stack Pointer (index)
    size_t    capacity;
} PrimitiveStack;

typedef struct ReferenceStack {
    VMObject** ref_stack;
    uint32_t   ref_sp;     // Stack Pointer (index)
    size_t     ref_cap;
} ReferenceStack;

typedef struct VMProgram VMProgram;
typedef struct {
    CFunction function;
    const NativeSignature* signature;
} NativeCallTarget;

typedef struct CallFrame{
    int32_t function_id;
    uint32_t prim_bp;      // Base pointer for Primitive Stack
    uint32_t ref_bp;       // Base pointer for Ref Stack
    int32_t* return_ip;    // Return instruction pointer
    VMProgram* return_program;
    PrimReg  GP[MAX_REGISTERS];  // General Purpose (Primitives)
    RefReg   RP[MAX_REGISTERS];  // Reference Pointers (Objects)
    uint16_t register_count;
} CallFrame;

struct VMProgram {
    State* owner;
    char* source_filename;
    char* function_names[MAX_FUNCTIONS];
    int function_name_count;
    SourceMapEntry* source_map;
    size_t source_map_count;
    int32_t* base;
    int32_t* code_start;
    size_t bytecode_count;
    uint16_t register_count;
    NativeCallTarget* native_targets;
    size_t native_target_count;
    int32_t func_table[MAX_FUNCTIONS];
    int func_count;
    int32_t* setup_ip;
    int32_t* update_ip;
    int32_t* fixed_update_ip;
    int32_t* trigger_enter_ip;
    int32_t* trigger_stay_ip;
    int32_t* trigger_exit_ip;
    int32_t* network_remote_ip;
    array constants;
    uint64_t* prim_globals;
    VMObject** ref_globals;
    size_t prim_global_size;
    size_t ref_global_size;
    bool owns_constants;
};

// VM Context with DUAL STACKS
typedef struct VMContext VMContext;

struct VMContext {
    // --- 1. Registers (Split Banks) ---
    PrimReg  GP[MAX_REGISTERS];  // General Purpose (Primitives)
    RefReg   RP[MAX_REGISTERS];  // Reference Pointers (Objects)

    // --- 2. Primitive Stack (Data) ---
	PrimitiveStack primStack;

    // --- 3. Reference Stack (Pointers) ---
	ReferenceStack refStack;
    // Bit per reference-stack slot marking compiler-owned locals for error unwinding.
    uint8_t* owned_ref_slots;

    // --- 4. Globals (Split) ---
    uint64_t* prim_globals;
    VMObject** ref_globals;
    size_t     global_size;

    // --- 5. Call Frames ---
    CallFrame call_stack[1024],
        *current_frame;

    int frame_pointer;

    // --- 6. Execution State ---
    VMProgram* program;
    array programs;
    int32_t* ip, *base, *code_start;

    // --- 7. Object ownership ---
    VMObject* objects;    // Registry for cleanup and diagnostics.
    ObjString** strings;  // Interned string table
    uint32_t string_capacity;
    uint32_t string_count;

    ClassDescriptor* classes;
    int class_count;

    size_t object_bytes_allocated;

    DebugMode debugMode;
    int breakpoints[16];
    int numBreakpoints;

    // Maps Function ID (0..255) -> Bytecode Index (Absolute Address)
    int32_t func_table[MAX_FUNCTIONS];
    int func_count;
    // Cached Entry Points (Native Pointers to Bytecode)
    int32_t* setup_ip;
    int32_t* update_ip;
    int32_t* fixed_update_ip;
    int32_t* trigger_enter_ip;
    int32_t* trigger_stay_ip;
    int32_t* trigger_exit_ip;
    int32_t* network_remote_ip;

    array constants;      // Array of deserialized constants
    bool had_runtime_error; // run time error
    uint64_t instruction_count;
    uint64_t script_call_count;
    uint64_t native_call_count;
};

ObjRaw* create_native_handle(VMContext* vm, void* ptr, CDtor cleanupFn);
const char* vm_get_string_arg(VMContext* vm, int arg_index);
bool vm_get_native_array_view(const VMObject* object, VMScalarType element_type,
    VMNativeArrayView* view);

void vm_init(State* state);
VMProgram* vm_attach_program(State* state, VMContext* sharedVM);
VMProgram* vm_clone_program_instance(VMProgram* source, VMContext* sharedVM);
bool vm_call_direct(State* state, int32_t* targetIP, float dt);
bool vm_call_program_direct(State* state, VMProgram* program, int32_t* targetIP, float dt);
void vm_run_global(State* state, bool startInDebugMode);
void vm_free(State* state);
