#ifndef SYMBOL_TABLE_H
#define SYMBOL_TABLE_H

#include "types.h" // Includes TypeID, array, State, etc.

// --- Symbol Structures ---
// This enum explicitly defines what a symbol represents, resolving ambiguity.
typedef enum {
    SYM_VARIABLE,
    SYM_PARAMETER,
    SYM_TYPE_DEFINITION,
    SYM_FUNCTION,
    SYM_CFUNCTION,
    SYM_CONSTANT
} SymbolKind;

#define MAX_BORROW_PATH_DEPTH 8

typedef enum {
    VALUE_INITIALIZED = 0,
    VALUE_UNINITIALIZED,
    VALUE_MOVED,
    VALUE_MAYBE_UNAVAILABLE
} ValueState;

// Represents a single member inside a struct/class definition
typedef struct {
    const char* name;
    TypeID type;
    int offset; // The byte offset from the start of the struct's memory block
} ClassMember;

// Represents any identifier in the code: a variable, function, or type definition.
typedef struct Symbol {
    char* name;     // The identifier's name (e.g., "myVar", "Person")
    TypeID type;    // The data type of the symbol (e.g., T_INT, or a T_CLASS type)

    // The nesting depth where this symbol was declared. 0 is global.
    int scopeLevel;
	int scopeParent; // For functions, this is the index in the global function list
    SymbolKind kind; // NEW: Explicitly states what this symbol is.
    int stackIndex; // For variables, their offset in the global or local memory block
    int count;       // For arrays, the number of elements
    int ownsValue;   // Reference local owns its slot; parameters borrow by default.
    ValueState valueState; // Definite-initialization and ownership-flow state.
    int borrowedFromRefIndex; // For borrowed locals that alias a known local owner.
    uint64_t borrowedFromPath; // Hash of the member path below the root owner (0 = root).
    int borrowedFromDepth;     // Number of member edges represented by borrowedFromPath.
    uint64_t borrowedPathComponents[MAX_BORROW_PATH_DEPTH]; // Field or array-index identity at each edge.

    // A union to hold data specific to the symbol's kind.
    union {
        // For struct/class type definitions, we store a list of their members
        // and their total count in bytes.
        struct {
            array members; // An array of 'ClassMember' structs
            int totalSize;
            int packedSize;
            int packedLayout;
            int destructorIndex;
        } classDef;

        // For function definitions, you might store parameter info here
        struct {
            int cfuncIndex; // CFunction would be a typedef for your function pointer
            int paramCount; // Expected argument count (-1 = unchecked)
        } cFunction;

        // This data is ONLY valid if kind == SYM_FUNCTION
		struct {
            int start_address; // The bytecode address where the function's code begins
            IRNode* entry_ir; // Compile-owned entry label; used for bounded leaf matching.
            int paramCount;   // Number of arguments
            TypeID* paramTypes;
            int borrowedReturnParamIndex; // -1=owned return, -2=this, otherwise explicit parameter index.
            int borrowedReturnRefIndex;   // Callee-local ref slot used to validate returned provenance.
            char* borrowedReturnPath;     // Dot-separated borrowed subobject path, if any.
        } funcDef;
        Value constValue;    // constant value
    } as;
} Symbol;

// Adds a new variable symbol to the current scope.
Symbol* addSymbol(State* state, const char* name, TypeID type, int size, int count);
Symbol* addConstantInt(State* state, const char* name, int value);
Symbol* addClassSymbol(State* state, const char* structName);
// Adds a new struct/class type definition to the symbol table.
void addClassMember(Symbol* structSymbol, const char* name, TypeID type, int offset);
Symbol* addFuncSymbol(State* state, const char* name, SymbolKind kind, void* funcData);
Symbol* addTypedFuncSymbol(State* state, const char* name, CFunction funcData, const NativeSignature* signature);
Symbol* addFastTypedFuncSymbol(State* state, const char* name, NativeFastFunction funcData, const NativeSignature* signature);
Symbol* findClassSymbol(State* state, const char* structName);
int findMemberOffset(Symbol* classDef, const char* memberName);
TypeID findMemberType(Symbol* classDef, const char* memberName);
// Looks for a symbol by name, searching from the current scope outwards.
Symbol* lookupSymbolRecursive(State* state, const char* name);
Symbol* lookupSymbolLocal(State* state, const char* name);
Symbol* findMethodSymbol(State* state, const char* className, const char* methodName);
Symbol* lookupSymbolGlobal(State* state, const char* name);
// Looks specifically for a type definition (a struct/class) by name.
Symbol* findTypeSymbol(State* state, const char* name);
Symbol* findTypeSymbolTstr(State* state, const TString* name);

#endif // SYMBOL_TABLE_H
