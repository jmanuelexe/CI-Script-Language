#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <float.h>
#include "array.h" // Assuming you have an array implementation

// --- Basic Type Definitions ---
typedef int int32;
typedef unsigned char byte;

// Fixed-width language/native interop types. VM registers remain uint64_t;
// these aliases describe the value's logical width at storage boundaries.
typedef int8_t   int8;
typedef uint8_t  uint8;
typedef int16_t  int16;
typedef uint16_t uint16;
typedef int32_t  int32_t_value;
typedef uint32_t uint32;
typedef int64_t  int64;
typedef uint64_t uint64;
typedef float    float32;
typedef double   float64;

#if defined(__cplusplus)
static_assert(sizeof(int8) == 1 && sizeof(uint8) == 1, "CI 8-bit scalar ABI unavailable");
static_assert(sizeof(int16) == 2 && sizeof(uint16) == 2, "CI 16-bit scalar ABI unavailable");
static_assert(sizeof(int32_t_value) == 4 && sizeof(uint32) == 4, "CI 32-bit scalar ABI unavailable");
static_assert(sizeof(int64) == 8 && sizeof(uint64) == 8, "CI 64-bit scalar ABI unavailable");
#else
_Static_assert(sizeof(int8) == 1 && sizeof(uint8) == 1, "CI 8-bit scalar ABI unavailable");
_Static_assert(sizeof(int16) == 2 && sizeof(uint16) == 2, "CI 16-bit scalar ABI unavailable");
_Static_assert(sizeof(int32_t_value) == 4 && sizeof(uint32) == 4, "CI 32-bit scalar ABI unavailable");
_Static_assert(sizeof(int64) == 8 && sizeof(uint64) == 8, "CI 64-bit scalar ABI unavailable");
#endif

// --- TString for String Slices ---
typedef struct {
	const char* begin, * end;
}TString;


typedef enum TTOKEN
{
	TK_NULL,	TK_HALT,
	//compare operator mask = 00011000
	TK_EQ, TK_NE,	TK_LE, TK_GT,	TK_LT, TK_GE,
	TK_NEW, TK_MOVE,
	//general tokens = mask= 01111111
	TK_TAB, TK_NEWLINE, TK_IDENTIFIER, TK_COMMENT,
	//data types mask = 00111111
	TK_INTEGER, TK_FLOATS, TK_STRING, TK_TRUE, TK_FALSE,
	//TK_INC, TK_DEC,
	//loop
	TK_COMMA, TK_STRUCT, TK_ENUM, TK_DOT, TK_THIS,
	TK_INT, TK_FLOAT, TK_BOOL, TK_CHAR, TK_BYTE, TK_SHORT, TK_DOUBLE,
	//logic operators mask = 00010100
	TK_NOT, TK_AND, TK_OR,
	//Bit operators mask = 00010100
	TK_BITNOT, TK_BITAND, TK_BITOR,
	//arimethic operators = 000101000
	TK_INC, TK_DEC, TK_ASSIGN, 
	TK_ADD_ASSIGN, TK_SUB_ASSIGN, TK_MULTEQUALS, TK_DIVEQUALS,
	TK_PLUS, TK_MINUS, TK_MULT, TK_DIV, TK_MULT_ASSIGN, 
	TK_POWER,
	//grouping operator
	TK_OPENPARENTHESIS, TK_CLOSEPARENTHESIS, TK_OPENCURLYBRACKET, TK_CLOSECURLYBRACKET,
	TK_COLON, TK_SEMICOLON, TK_OPENBRACKET, TK_CLOSEBRACKET, TK_DEFAULT, TK_IMPORT,
	//special keyboard
	TK_END, TK_ENDWHILE, TK_ENDFOR, TK_ENDIF, TK_FUNCTIONCAL, TK_LABEL, TK_GOTO,
	TK_FUN, TK_RETURN, TK_IF, TK_THEN, TK_ELSE, TK_BEGIN, TK_UNTIL,
	TK_FOR, TK_TO, TK_DO, TK_WHILE, TK_CONTINUE, TK_BREAK, TK_SWITCH, TK_CASE, TK_RANGE,
	TK_ROOT, TK_CLASS, TK_CONCAT, TK_ERRORPARSE, TK_CONST, TK_BORROW,
	TK_ERROR, TK_CHARACTER, TK_DIVEQ, TK_ARROW, T_UNKNOWN, TK_EOF, TK_AUTO
} TTOKEN;


//store a token. The token text and type
typedef struct Pos {
	int col, line;
}Pos;

typedef struct Loc {
	struct Pos pos;
	const char* filename;
}Loc;

// --- Type System ---
typedef enum {
	T_UNKNOWNTYPE, 
	T_VOID, 
	T_NIL, 
	T_BOOL, 
	T_CHAR, 
	T_SHORT, 
	T_INT, 
	T_FLOAT,
	T_DOUBLE, 
	T_STRING, 
	T_CSTRING, 
	T_PTR,
	T_FUNCTION, 
	T_CFUNCTION,
	T_CLASS, 
	T_SHARED_PTR,
	T_AUTO, // For auto type inference (e.g., auto x = 5;)
} TypeIDEnum;

// Physical scalar description used by the typed storage/native ABI. This is
// intentionally separate from TypeIDEnum so legacy bytecode values remain
// stable while fixed-width types are introduced incrementally.
typedef enum {
    VM_SCALAR_NONE = 0,
    VM_SCALAR_I8,
    VM_SCALAR_U8,
    VM_SCALAR_I16,
    VM_SCALAR_U16,
    VM_SCALAR_I32,
    VM_SCALAR_U32,
    VM_SCALAR_I64,
    VM_SCALAR_U64,
    VM_SCALAR_F32,
    VM_SCALAR_F64
} VMScalarType;

static inline size_t vm_scalar_size(VMScalarType type) {
    switch (type) {
    case VM_SCALAR_I8: case VM_SCALAR_U8: return sizeof(int8);
    case VM_SCALAR_I16: case VM_SCALAR_U16: return sizeof(int16);
    case VM_SCALAR_I32: case VM_SCALAR_U32: case VM_SCALAR_F32: return sizeof(int32_t);
    case VM_SCALAR_I64: case VM_SCALAR_U64: case VM_SCALAR_F64: return sizeof(int64_t);
    default: return 0;
    }
}

static inline int vm_scalar_is_signed(VMScalarType type) {
    return type == VM_SCALAR_I8 || type == VM_SCALAR_I16 ||
           type == VM_SCALAR_I32 || type == VM_SCALAR_I64;
}

static inline int vm_scalar_is_float(VMScalarType type) {
    return type == VM_SCALAR_F32 || type == VM_SCALAR_F64;
}

// Whether this host can pass the scalar's exact CI representation to native
// code. Integer aliases above are fixed-width; C float/double require checking
// their size and binary format separately.
static inline int vm_host_supports_native_scalar(VMScalarType type) {
    switch (type) {
    case VM_SCALAR_I8: case VM_SCALAR_U8: return sizeof(int8_t) == 1;
    case VM_SCALAR_I16: case VM_SCALAR_U16: return sizeof(int16_t) == 2;
    case VM_SCALAR_I32: case VM_SCALAR_U32: return sizeof(int32_t) == 4;
    case VM_SCALAR_I64: case VM_SCALAR_U64: return sizeof(int64_t) == 8;
    case VM_SCALAR_F32:
        return sizeof(float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128;
    case VM_SCALAR_F64:
        return sizeof(double) == 8 && FLT_RADIX == 2 && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024;
    default: return type == VM_SCALAR_NONE;
    }
}

// C-layout metadata is intentionally distinct from ordinary owning classes.
// Fields are described in declaration order; count supports fixed scalar
// arrays inside a struct. The validator accepts the natural fixed-width ABI
// layout only (no compiler-specific packing or implicit bitfields).
typedef struct {
    VMScalarType scalar;
    size_t count;
    size_t offset;
} VMNativeLayoutField;

#if defined(__cplusplus)
#define VM_NATIVE_ALIGNOF(type) alignof(type)
#else
#define VM_NATIVE_ALIGNOF(type) _Alignof(type)
#endif
#define VM_NATIVE_LAYOUT_FIELD(type, member, scalar_type, elements) \
    { (scalar_type), (elements), offsetof(type, member) }

static inline int vm_native_layout_matches(const VMNativeLayoutField* fields,
    size_t field_count, size_t host_size, size_t host_alignment) {
    if ((!fields && field_count) || !host_alignment) return 0;
    size_t cursor = 0, struct_alignment = 1;
    for (size_t i = 0; i < field_count; ++i) {
        const VMNativeLayoutField* field = &fields[i];
        const size_t width = vm_scalar_size(field->scalar);
        const size_t alignment = width;
        if (!width || !field->count || !vm_host_supports_native_scalar(field->scalar) ||
            field->count > SIZE_MAX / width) return 0;
        cursor = (cursor + alignment - 1) / alignment * alignment;
        if (field->offset != cursor) return 0;
        const size_t bytes = width * field->count;
        if (cursor > SIZE_MAX - bytes) return 0;
        cursor += bytes;
        if (alignment > struct_alignment) struct_alignment = alignment;
    }
    cursor = (cursor + struct_alignment - 1) / struct_alignment * struct_alignment;
    return host_alignment == struct_alignment && host_size == cursor;
}

// Physical representation for the currently supported language types.
// Unsigned aliases will use the same register ABI once parser/type nodes are
// migrated to carry signedness explicitly.
static inline VMScalarType vm_scalar_for_type(TypeIDEnum type) {
    switch (type) {
    case T_BOOL:  return VM_SCALAR_U8;
    case T_CHAR:  return VM_SCALAR_I8;
    case T_SHORT: return VM_SCALAR_I16;
    case T_INT:   return VM_SCALAR_I32;
    case T_FLOAT: return VM_SCALAR_F32;
    case T_DOUBLE:return VM_SCALAR_F64;
    default:      return VM_SCALAR_NONE;
    }
}

typedef struct {
	TypeIDEnum baseType;
	char* name;
	TypeIDEnum elementType; // Non-zero only when baseType is an array pointer.
	uint8_t borrowed;
} TypeID;

typedef struct NativeSignature NativeSignature;
typedef struct NativeParamType NativeParamType;
typedef struct VMObject VMObject;
typedef struct VMContext VMContext;
typedef void (*NativeFastFunction)(const uint64_t* prim_args, VMObject* const* ref_args,
    uint64_t* return_prim, VMObject** return_ref, VMContext* vm);

typedef struct {
	int nameIndex;   // "GL.Perspective"
	void* fn;     // pointer to nat_gl_Perspective
	const NativeSignature* signature; // NULL for legacy frame callbacks
} CFuncEntry;

// Optional typed native-call description. Legacy ApiCallFrame callbacks do
// not need this metadata; typed registrations can use it to avoid per-call
// type discovery and conversion.
struct NativeParamType {
    VMScalarType type;
    uint8_t is_reference;
};

struct NativeSignature {
    const NativeParamType* params;
    uint16_t param_count;
    VMScalarType return_type;
    uint8_t return_is_reference;
    NativeFastFunction fast_function;
};


// --- Value Representation ---
typedef struct Value Value;

typedef struct ArenaBlock {
    struct ArenaBlock* next;
    size_t used;
    size_t capacity;
    unsigned char data[];
} ArenaBlock;

typedef struct Arena {
    ArenaBlock* head;
    size_t default_block_size;
} Arena;

void arena_init(Arena* arena, size_t default_block_size);
void* arena_alloc(Arena* arena, size_t size, size_t alignment);
void arena_reset(Arena* arena);
void arena_destroy(Arena* arena);

typedef union
{
	uint64_t i64;
	uint64_t bits;
	int32_t i32;
	double   f64;
	float f32;
	byte b;
	char c;
	void* p;	/// Pointer (for objects, strings, etc.)

}Val;

// Register conversion helpers. Values are always kept in a 64-bit register,
// but narrow integers are extended according to their signedness and floats
// retain their IEEE bit representation.
static inline uint64_t vm_pack_int8(int8 value)   { return (uint64_t)(int64_t)value; }
static inline uint64_t vm_pack_uint8(uint8 value)  { return (uint64_t)value; }
static inline uint64_t vm_pack_int16(int16 value)  { return (uint64_t)(int64_t)value; }
static inline uint64_t vm_pack_uint16(uint16 value){ return (uint64_t)value; }
static inline uint64_t vm_pack_int32(int32_t_value value) { return (uint64_t)(int64_t)value; }
static inline uint64_t vm_pack_uint32(uint32 value){ return (uint64_t)value; }
static inline uint64_t vm_pack_int64(int64 value)  { return (uint64_t)value; }
static inline uint64_t vm_pack_uint64(uint64 value){ return value; }

static inline int8   vm_unpack_int8(uint64_t bits)  { return (int8)(int64_t)bits; }
static inline uint8  vm_unpack_uint8(uint64_t bits) { return (uint8)bits; }
static inline int16  vm_unpack_int16(uint64_t bits) { return (int16)(int64_t)bits; }
static inline uint16 vm_unpack_uint16(uint64_t bits){ return (uint16)bits; }
static inline int32_t_value vm_unpack_int32(uint64_t bits) { return (int32_t_value)(int64_t)bits; }
static inline uint32 vm_unpack_uint32(uint64_t bits){ return (uint32)bits; }
static inline int64  vm_unpack_int64(uint64_t bits) { return (int64)bits; }
static inline uint64 vm_unpack_uint64(uint64_t bits){ return bits; }
static inline float32 vm_unpack_float32(uint64_t bits) {
    uint32_t raw = (uint32_t)bits;
    float32 value;
    memcpy(&value, &raw, sizeof(value));
    return value;
}
static inline float64 vm_unpack_float64(uint64_t bits) {
    float64 value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}
static inline uint64_t vm_pack_float32(float32 value) {
    uint32_t raw;
    memcpy(&raw, &value, sizeof(raw));
    return (uint64_t)raw;
}
static inline uint64_t vm_pack_float64(float64 value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

typedef struct Value
{
	Val v;
	TypeID type;
}Value;

typedef struct {
	// Inputs (Read only)
	uint64_t* prim_args;
	VMObject** ref_args;

	// Outputs (Write only)
	uint64_t* return_prim;
	VMObject** return_ref;
	VMContext* vm;
} ApiCallFrame;

// A C function pointer type for native functions
typedef void* (*CFunction)(ApiCallFrame* data_ptr);

// --- Symbol Table Structures ---
typedef enum {
	s_global = 0,
	s_local = 4,
	s_reg = 5,
}Scope;

// Forward-declare Symbol. Note: For this design to work, the Symbol struct
// (defined in SymbolTable.h) must contain an 'int scopeLevel;' member.
struct Symbol;

#define REG_SIZE 256

// This struct holds all the state for a single compilation process.
typedef void (*ErrorCallback)(Loc loc, const char* msg, void* userData);

typedef struct {
	// Arrays for managing compilation artifacts
	array strings;          // String pool for all interned strings
	array ASTs;             // "Arena" for all allocated AST nodes for later cleanup
	Arena astArena;

	// Symbol Table Management (GCC-style)
	array symbols;          // A single, unified list of all symbols.
	int currentScopeParent; // Index of the currently compiling function
	int currentScopeLevel;  // The current nesting level for scope management
	int nextLabelId, // For generating unique labels in the bytecode
		nextFuncId; // For generating unique function IDs
	// Counters for memory allocation
	int nextRegisterIndex;
	int nextPrimIndex; // For int, float, bool, char (GP)
	int nextRefIndex;  // For string, class, arrays (RP)
	int nextGlobalPrimIndex;  // For Globals
	int nextGlobalRefIndex;   // For Globals
	//int gloabalSize;        // Total size of global variables allocated
	void* currentClass;   // The current class/struct context for method definitions
	array* breakJumpList; // A pointer to the current list of jumps to patch
	//List of full file paths that have already been imported
	array importedFiles;
	int currentBreakLabel;    // ID of the label to jump to on 'break'
	int currentContinueLabel; // ID of the label to jump to on 'continue'
	int currentBreakCleanupSymbol;
	int currentContinueCleanupSymbol;
	int error;                // Error count (0 = no errors)
	ErrorCallback onError;
	void* errorUserData;
} CompilerDef;

typedef struct State State;
typedef struct IRNode IRNode;
typedef struct VMProgram VMProgram;

typedef struct {
    uint32_t bytecode_offset;
    char* filename;
    int line;
    int column;
} SourceMapEntry;

struct State {
	array bytecode;
	array Constants;
	CompilerDef* compiler;
	array cFunctions;
	array cFunctionSignatures; // NativeSignature* entries parallel to cFunctions
// intermidiate code IR
	IRNode* ir_head;
	IRNode* ir_tail;
	Arena irArena;
	int next_label_id;
	VMContext* vm;
	VMProgram* program;
    uint16_t register_count;
    char* source_filename;
    char** function_names;
    int function_name_count;
    Loc current_loc;
    SourceMapEntry* source_map;
    size_t source_map_count;
    size_t source_map_capacity;
};

static const Loc LOC_NONE = { {0, 0}, NULL };

// --- Function Prototypes for types.c ---
char* makeStrFromTstr(const TString* str);
char* internString(CompilerDef* strings, const TString* tstr);
char* internStr(CompilerDef* strings, const char* str);
void reportError(CompilerDef* compiler, Loc loc, const char* format, ...);

#define TYPE_EQUALS(t1, t2) ((t1).baseType == (t2).baseType && ((t1).baseType != T_CLASS || strcmp((t1).name, (t2).name) == 0))
// Error codes
/*
#define ERROR_NO_MEMBER 1
#define ERROR_UNDEFINED_SYMBOL 2
#define ERROR_REDECLARATION 3
#define ERROR_TYPE_MISMATCH 4
#define ERROR_INVALID_OPERATION 5
#define ERROR_NOT_A_FUNCTION 6
#define ERROR_NOT_A_STRUCT 7
#define ERROR_INDEX_OUT_OF_BOUNDS 8
#define ERROR_INVALID_ARRAY_SIZE 9
#define ERROR_DIVIDE_BY_ZERO 10
#define ERROR_NULL_POINTER 11
#define ERROR_UNDECLARED_VARIABLE 12
#define ERROR_UNSUPPORTED_OPERATION	13
#define ERROR_UNSUPPORTED_EXPRESSION 14
#define ERROR_MEMORY_ALLOCATION_FAILED 15
#define ERROR_UNSUPPORTED_STATEMENT 16
#define ERROR_INVALID_ARGUMENTS 17
#define ERROR_OUT_OF_MEMORY 18
#define ERROR_UNDEFINED_VARIABLE 19
*/
