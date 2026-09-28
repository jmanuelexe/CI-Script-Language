#include "CodeGen.h"
#include <stdbool.h> 
#include <stdarg.h> 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "SymbolTable.h"
#include "Emitter.h"
#include "Parser.h"
#include "vm.h"

#define BORROW_UNKNOWN_REF_INDEX (-2)

// --- Forward Declarations ---
void generateStatement(ASTNode* stmtNode, State* state);
void generateClass(State* state, ASTClassDef* classNode);
static void evaluateAndCheckArg(State* s, ASTNode* argNode, int destReg, Symbol* funcSym, int argIndex);
void generateExpression(State* state, ASTNode* exprNode, int destRegister);
TypeID getExpressionResultType(State* state, ASTNode* node);
void generateFunction(State* state, ASTFuncDef* funcNode, int classSymbol);
bool emitGetGlobal(State* state, TypeIDEnum type, int destRegister, short localOffset);
static void setVariable(State* s, ASTVar* var, int src, TypeID srcType);
static void emitOwnedLocalCleanup(State* state, int firstSymbol);

static bool statementDefinitelyReturns(ASTNode* node)
{
    if (!node) return false;
    if (node->type == AST_RETURN) return true;
    if (node->type == AST_BLOCK) {
        ASTBlock* block = (ASTBlock*)node;
        for (int i = 0; i < array_size(&block->list); ++i) {
            ASTNode* statement = *(ASTNode**)array_get(&block->list, i);
            if (statementDefinitelyReturns(statement)) return true;
        }
        return false;
    }
    if (node->type == AST_IF) {
        ASTIfStmt* branch = (ASTIfStmt*)node;
        return branch->elseBlock &&
            statementDefinitelyReturns(branch->thenBlock) &&
            statementDefinitelyReturns(branch->elseBlock);
    }
    return false;
}

static bool constantNumber(ASTNode* node, double* value)
{
    if (!node || node->type != AST_CONST) return false;
    Value constant = ((ASTConst*)node)->value;
    switch (constant.type.baseType) {
    case T_BOOL: case T_CHAR: case T_SHORT: case T_INT:
        *value = (double)constant.v.i32;
        return true;
    case T_FLOAT:
        *value = (double)constant.v.f32;
        return true;
    case T_DOUBLE:
        *value = constant.v.f64;
        return true;
    default:
        return false;
    }
}

static bool constantCondition(ASTNode* node, bool* value)
{
    double left = 0, right = 0;
    if (constantNumber(node, &left)) {
        *value = left != 0.0;
        return true;
    }
    if (!node) return false;
    if (node->type == AST_UNARY_OP) {
        ASTUnaryOp* unary = (ASTUnaryOp*)node;
        bool operand;
        if (unary->op == TK_NOT && constantCondition(unary->operand, &operand)) {
            *value = !operand;
            return true;
        }
        return false;
    }
    if (node->type != AST_BINARY_OP) return false;
    ASTBinaryOp* binary = (ASTBinaryOp*)node;
    if (!constantNumber(binary->left, &left) || !constantNumber(binary->right, &right))
        return false;
    switch (binary->op) {
    case TK_EQ: *value = left == right; return true;
    case TK_NE: *value = left != right; return true;
    case TK_LT: *value = left < right; return true;
    case TK_LE: *value = left <= right; return true;
    case TK_GT: *value = left > right; return true;
    case TK_GE: *value = left >= right; return true;
    default: return false;
    }
}

typedef struct {
    int count;
    unsigned char* valueState;
    int* borrowedFromRefIndex;
    uint64_t* borrowedFromPath;
    int* borrowedFromDepth;
    uint64_t* borrowedPathComponents;
} FlowSnapshot;

static FlowSnapshot snapshotFlowState(State* state)
{
    FlowSnapshot snapshot = { 0 };
    snapshot.count = state->compiler->symbols.count;
    if (snapshot.count <= 0) return snapshot;

    snapshot.valueState = (unsigned char*)malloc((size_t)snapshot.count);
    snapshot.borrowedFromRefIndex = (int*)malloc(sizeof(int) * (size_t)snapshot.count);
    snapshot.borrowedFromPath = (uint64_t*)malloc(sizeof(uint64_t) * (size_t)snapshot.count);
    snapshot.borrowedFromDepth = (int*)malloc(sizeof(int) * (size_t)snapshot.count);
    snapshot.borrowedPathComponents = (uint64_t*)calloc((size_t)snapshot.count * MAX_BORROW_PATH_DEPTH,
        sizeof(uint64_t));
    if (!snapshot.valueState || !snapshot.borrowedFromRefIndex || !snapshot.borrowedFromPath ||
        !snapshot.borrowedFromDepth || !snapshot.borrowedPathComponents) {
        reportError(state->compiler, LOC_NONE, "Out of memory while snapshotting ownership flow state");
        free(snapshot.valueState);
        free(snapshot.borrowedFromRefIndex);
        free(snapshot.borrowedFromPath);
        free(snapshot.borrowedFromDepth);
        free(snapshot.borrowedPathComponents);
        snapshot.valueState = NULL;
        snapshot.borrowedFromRefIndex = NULL;
        snapshot.borrowedFromPath = NULL;
        snapshot.borrowedFromDepth = NULL;
        snapshot.borrowedPathComponents = NULL;
        snapshot.count = 0;
        return snapshot;
    }

    for (int i = 0; i < snapshot.count; ++i) {
        Symbol* symbol = (Symbol*)array_get(&state->compiler->symbols, i);
        snapshot.valueState[i] = symbol ? (unsigned char)symbol->valueState : VALUE_INITIALIZED;
        snapshot.borrowedFromRefIndex[i] = symbol ? symbol->borrowedFromRefIndex : -1;
        snapshot.borrowedFromPath[i] = symbol ? symbol->borrowedFromPath : 0;
        snapshot.borrowedFromDepth[i] = symbol ? symbol->borrowedFromDepth : 0;
        if (symbol) memcpy(&snapshot.borrowedPathComponents[i * MAX_BORROW_PATH_DEPTH],
            symbol->borrowedPathComponents, sizeof(symbol->borrowedPathComponents));
    }
    return snapshot;
}

static void restoreFlowState(State* state, const FlowSnapshot* snapshot)
{
    if (!snapshot) return;
    int limit = snapshot->count < state->compiler->symbols.count ? snapshot->count : state->compiler->symbols.count;
    for (int i = 0; i < limit; ++i) {
        Symbol* symbol = (Symbol*)array_get(&state->compiler->symbols, i);
        if (!symbol) continue;
        symbol->valueState = snapshot->valueState
            ? (ValueState)snapshot->valueState[i] : VALUE_INITIALIZED;
        symbol->borrowedFromRefIndex = snapshot->borrowedFromRefIndex ? snapshot->borrowedFromRefIndex[i] : -1;
        symbol->borrowedFromPath = snapshot->borrowedFromPath ? snapshot->borrowedFromPath[i] : 0;
        symbol->borrowedFromDepth = snapshot->borrowedFromDepth ? snapshot->borrowedFromDepth[i] : 0;
        if (snapshot->borrowedPathComponents)
            memcpy(symbol->borrowedPathComponents,
                &snapshot->borrowedPathComponents[i * MAX_BORROW_PATH_DEPTH],
                sizeof(symbol->borrowedPathComponents));
    }
}

static void mergeFlowStateAfterBranch(State* state, const FlowSnapshot* left, const FlowSnapshot* right, int count)
{
    int limit = count < state->compiler->symbols.count ? count : state->compiler->symbols.count;
    for (int i = 0; i < limit; ++i) {
        Symbol* symbol = (Symbol*)array_get(&state->compiler->symbols, i);
        if (!symbol) continue;
        ValueState leftState = left && left->valueState
            ? (ValueState)left->valueState[i] : VALUE_INITIALIZED;
        ValueState rightState = right && right->valueState
            ? (ValueState)right->valueState[i] : VALUE_INITIALIZED;
        symbol->valueState = leftState == rightState ? leftState : VALUE_MAYBE_UNAVAILABLE;
        if (symbol->type.borrowed) {
            int leftBorrow = left && left->borrowedFromRefIndex ? left->borrowedFromRefIndex[i] : -1;
            int rightBorrow = right && right->borrowedFromRefIndex ? right->borrowedFromRefIndex[i] : -1;
            if (leftBorrow >= 0 && rightBorrow >= 0 && leftBorrow == rightBorrow &&
                left->borrowedFromPath[i] == right->borrowedFromPath[i] &&
                left->borrowedFromDepth[i] == right->borrowedFromDepth[i]) {
                symbol->borrowedFromRefIndex = leftBorrow;
                symbol->borrowedFromPath = left->borrowedFromPath[i];
                symbol->borrowedFromDepth = left->borrowedFromDepth[i];
                memcpy(symbol->borrowedPathComponents,
                    &left->borrowedPathComponents[i * MAX_BORROW_PATH_DEPTH],
                    sizeof(symbol->borrowedPathComponents));
            } else {
                symbol->borrowedFromRefIndex =
                    (leftBorrow != -1 || rightBorrow != -1)
                    ? BORROW_UNKNOWN_REF_INDEX : -1;
                symbol->borrowedFromPath = 0;
                symbol->borrowedFromDepth = 0;
                memset(symbol->borrowedPathComponents, 0, sizeof(symbol->borrowedPathComponents));
            }
        }
    }
}

static void freeFlowSnapshot(FlowSnapshot* snapshot)
{
    if (!snapshot) return;
    free(snapshot->valueState);
    free(snapshot->borrowedFromRefIndex);
    free(snapshot->borrowedFromPath);
    free(snapshot->borrowedFromDepth);
    free(snapshot->borrowedPathComponents);
    snapshot->valueState = NULL;
    snapshot->borrowedFromRefIndex = NULL;
    snapshot->borrowedFromPath = NULL;
    snapshot->borrowedFromDepth = NULL;
    snapshot->borrowedPathComponents = NULL;
    snapshot->count = 0;
}
int emitGetLocal(State* state, TypeIDEnum type, int destRegister, short localOffset);
int emitSetLocal(State* state, TypeIDEnum type, int destRegister, short localOffset);
int getTypeSize(State* state, TypeID type); // Assumed to be defined elsewhere
int emitConditionAndJump(State* state, ASTNode* condition, bool* isOptimized);

#define R0			  0
static const struct {
    TTOKEN token;
    OPCode opi;
    OPCode opf;
} kBinOps[] = {
    { TK_PLUS,  OP_ADD, OP_ADDf},
    { TK_MINUS, OP_SUB, OP_SUBf},
    { TK_MULT,  OP_MUL, OP_MULf},
    { TK_DIV,   OP_DIV, OP_DIVf},
    { TK_LT,    OP_LT,  OP_LTf},
    { TK_GT,    OP_GT,  OP_GTf},
    { TK_LE,    OP_LE,  OP_LEf},
    { TK_GE,    OP_GE,  OP_GEf},
    { TK_EQ,    OP_EQ,  OP_EQf},
    { TK_NE,    OP_NE,  OP_NEf},
};

#define BIN_OPS_COUNT (sizeof(kBinOps)/sizeof(kBinOps[0]))

static OPCode getBinOp(TTOKEN tk, bool isFloat) {
    for (int i32 = 0; i32 < BIN_OPS_COUNT; ++i32)
        if (kBinOps[i32].token == tk)
            if (isFloat) return kBinOps[i32].opf;
            else return kBinOps[i32].opi;

    return OP_NOT;
}

// Inverse comparison ? jump when condition is FALSE
static const struct { TTOKEN tk; OPCode op; } kInverseJumps[] = {
    { TK_LT, OP_JGE},
    { TK_LE, OP_JGT},
    { TK_GT, OP_JLE},
    { TK_GE, OP_JLT},
    { TK_EQ, OP_JNE},
    { TK_NE, OP_JEQ},
};

#define INVERSE_JUMPS_COUNT (sizeof(kInverseJumps)/sizeof(kInverseJumps[0]))

static OPCode getInverseJump(TTOKEN tk) {
    for (int i32 = 0; i32 < INVERSE_JUMPS_COUNT; ++i32)
        if (kInverseJumps[i32].tk == tk)
            return kInverseJumps[i32].op;
    return OP_NOT;
}

const char* typeEnumToString(TypeID* t) {
    switch (t->baseType) {
    case T_INT: return "int";
    case T_BOOL: return "bool";
    case T_FLOAT: return "float";
    case T_DOUBLE: return "double";
    case T_CHAR: return "char";
    case T_SHORT: return "short";
    case T_STRING: return "string";
    case T_PTR: return "ptr";
    case T_SHARED_PTR: return "shared_ptr";
    case T_CSTRING: return "cstring";
    case T_VOID: return "void";
    case T_CLASS: return t->name ? t->name : "<unnamed class>";
    default: return "unknown";
    }
}

// RAII register guard (optional, but beautiful)
typedef struct { State* s; int old; } RegGuard;
#define REG_GUARD(s, n) for (RegGuard _g = {s, s->compiler->nextRegisterIndex}; _g.s; \
                          _g.s->compiler->nextRegisterIndex = _g.old, _g.s = NULL)
// Allocate one register
static int allocReg(State* s) {
    int r = s->compiler->nextRegisterIndex++;
    if (r >= MAX_REGISTERS) reportError(s->compiler, LOC_NONE, "Register overflow");
    else if (s->register_count < r + 1) s->register_count = (uint16_t)(r + 1);
    return r;
}

// Helper to determine if a type belongs on the Ref Stack
bool isRefType(TypeID type) {
    return (type.baseType == T_STRING ||
        type.baseType == T_CLASS ||
        type.baseType == T_SHARED_PTR ||
        type.baseType == T_PTR); // <--- Add this!
}

// Returns 1 if values are equal, 0 otherwise
int values_equal(const Value* v1, const Value* v2)
{
    if (v1 == v2) return 1;
    if (v1->type.baseType != v2->type.baseType) return 0;

    if (v1->type.baseType == T_CLASS) {
        if (v1->type.name == v2->type.name) return 1;
        if (!v1->type.name || !v2->type.name) return 0;
        if (strcmp(v1->type.name, v2->type.name) != 0) return 0;
    }

    switch (v1->type.baseType) {
    case T_INT:
    case T_SHORT:
    case T_BOOL: return v1->v.i32 == v2->v.i32;
    case T_CHAR:return v1->v.c == v2->v.c;
    case T_FLOAT:return v1->v.f32 == v2->v.f32;
    case T_STRING:
        return (v1->v.p == v2->v.p) || (v1->v.p && v2->v.p && strcmp((char*)v1->v.p, (char*)v2->v.p) == 0);
    case T_VOID:return 1;
    default: return 0;
    }
}

// Add this before emitSetLocal/emitSetGlobal
int getTypeSizeEnum(TypeIDEnum t) {
    const VMScalarType scalar = vm_scalar_for_type(t);
    if (scalar != VM_SCALAR_NONE)
        return (int)vm_scalar_size(scalar);

    switch (t) {
    case T_STRING:
    case T_CLASS:
    case T_SHARED_PTR:
    case T_PTR:     return 8; // Assuming 64-bit pointers
    default:        return 0;
    }
}

// Returns index of a constant in the pool, or adds it if not found.
int addConstant(State* state, Value value) {
    int count = array_size(&state->Constants);
    if (count > 0) {
        Value* last = (Value*)array_get(&state->Constants, count - 1);
        if (values_equal(last, &value)) return count - 1;
    }

    // Linear search
    for (int i32 = 0; i32 < count; ++i32) {
        if (values_equal((Value*)array_get(&state->Constants, i32), &value)) {
            return i32;
        }
    }

    if (count > UINT16_MAX) {
        reportError(state->compiler, LOC_NONE, "Constant pool exceeded maximum size.\n");
        return 0;
    }
    array_append(&state->Constants, &value);
    return count;
}

int loadConstant(State* s, Value val) {
    int reg = allocReg(s);
    emitConstant_IR(s, val, reg);
    return reg;
}

void emitCastIfNeeded(State* state, TypeID destType, TypeID srcType, short srcRegister)
{
    if (destType.baseType == srcType.baseType) return; // Fast exit

    if (destType.baseType == T_FLOAT && srcType.baseType == T_INT) {
        emitAD_IR(state, OP_CAST_I2F, srcRegister, srcRegister);
    }
    else if (destType.baseType == T_INT && srcType.baseType == T_FLOAT) {
        emitAD_IR(state, OP_CAST_F2I, srcRegister, srcRegister);
    }
}

#include "vm.h"

// Frees 'count' bytes (simple LIFO deallocation)
static inline void freeRegisters(State* state, int count) {
    state->compiler->nextRegisterIndex -= count;
}

static const int kTypeSizes[] = {
    0,          // T_UNKNOWNTYPE (0)
    0,          // T_VOID
    0,          // T_NULL
    1,          // T_BOOL
    1,          // T_CHAR
    2,          // T_SHORT
    4,          // T_INT
    4,          // T_FLOAT
    8,          // T_DOUBLE
    sizeof(void*),// T_STRING (Pointer size, assuming 32-bit VM or compressed ptrs. Change to 8 if 64-bit)
    sizeof(void*),// T_PTR
    sizeof(void*),// T_CFUNCTION
    0,          // T_FUNCTION
    0           // T_CLASS (Variable size)
};

int getTypeSize(State* state, TypeID type) {
    if (type.baseType == T_CLASS) {
        Symbol* classDef = findClassSymbol(state, type.name);
        if (classDef) return classDef->as.classDef.totalSize; // Already calculated in slots
        return 0;
    }
    // Primitives (Int, Float, Bool) and Pointers (String, Class Ref) 
    // all occupy 1 slot in a 64-bit VM.
    return 1;
    /*
    if (type.baseType < T_CLASS) {
        return kTypeSizes[type.baseType];
    }

    if (type.baseType == T_CLASS) {
        Symbol* classDef = findClassSymbol(state, type.name);
        if (classDef) return classDef->as.classDef.totalSize;
        reportError(state, LOC_NONE, "Unknown class type '%s'.\n", type.name);
        return 0;
    }
    return sizeof(void*); // T_STRING, T_PTR fallback
    */
}

TypeID getClassMemberType(State* state, TypeID classType, const char* memberName) {
    if (classType.baseType != T_CLASS || classType.name == NULL) {
        return (TypeID) { T_VOID, NULL };
    }

    Symbol* classDef = findClassSymbol(state, classType.name);

    if (classDef) {
        for (int i32 = 0; i32 < array_size(&classDef->as.classDef.members); ++i32) {
            ClassMember* member = (ClassMember*)array_get(&classDef->as.classDef.members, i32);
            if (strcmp(member->name, memberName) == 0) {
                return member->type;
            }
        }
        reportError(state->compiler, LOC_NONE, "Member '%s' not found in class '%s'.\n", memberName, classType.name);
        return (TypeID) { T_VOID, NULL };
    }

    reportError(state->compiler, LOC_NONE, "Class type '%s' not found.\n", classType.name);
    return (TypeID) { T_VOID, NULL };
}

static TypeID functionCallResultType(State* state, Symbol* function) {
    if (!function) return (TypeID){ .baseType = T_VOID };
    if (function->kind != SYM_CFUNCTION)
        return function->type;

    int index = function->as.cFunction.cfuncIndex;
    if (index < 0 || index >= state->cFunctionSignatures.count)
        return function->type;
    const NativeSignature* signature = *(const NativeSignature**)array_get(
        &state->cFunctionSignatures, index);
    if (!signature) return function->type;

    TypeID result = { .baseType = T_UNKNOWNTYPE };
    if (signature->return_is_reference) {
        result.baseType = T_PTR;
        return result;
    }
    switch (signature->return_type) {
    case VM_SCALAR_NONE: result.baseType = T_VOID; break;
    case VM_SCALAR_I8: case VM_SCALAR_U8: result.baseType = T_CHAR; break;
    case VM_SCALAR_I16: case VM_SCALAR_U16: result.baseType = T_SHORT; break;
    case VM_SCALAR_I32: case VM_SCALAR_U32: result.baseType = T_INT; break;
    case VM_SCALAR_F32: result.baseType = T_FLOAT; break;
    case VM_SCALAR_F64: result.baseType = T_DOUBLE; break;
    default: break; // 64-bit integers do not have an exact language scalar yet.
    }
    return result;
}

TypeID getExpressionResultType(State* state, ASTNode* exprNode) {
    if (!exprNode) return (TypeID) { .baseType = T_VOID };

    switch (exprNode->type) {
    case AST_CONST: return ((ASTConst*)exprNode)->value.type;
    case AST_VAR: {
        ASTVar* varNode = (ASTVar*)exprNode;
        Symbol* localSym = lookupSymbolLocal(state, varNode->name);
        if (localSym) return localSym->type;

        if (state->compiler->currentClass) {
            int offset = findMemberOffset(state->compiler->currentClass, varNode->name);
            if (offset >= 0) return findMemberType(((Symbol*)state->compiler->currentClass), varNode->name);
        }
        Symbol* globalSym = lookupSymbolGlobal(state, varNode->name);
        return globalSym ? globalSym->type : (TypeID) { T_VOID };
    }
    case AST_BINARY_OP: {
        ASTBinaryOp* binOp = (ASTBinaryOp*)exprNode;

        switch (binOp->op) {
        case TK_EQ:case TK_NE:
        case TK_LT:case TK_GT:
        case TK_LE:case TK_GE:
        case TK_NOT:case TK_AND:case TK_OR:return (TypeID) { .baseType = T_INT };
        default:
            break;
        }
        TypeID leftType = getExpressionResultType(state, binOp->left);
        // String concat check
        if (binOp->op == TK_PLUS && leftType.baseType == T_STRING) return (TypeID) { T_STRING };

        TypeID rightType = getExpressionResultType(state, binOp->right);
        if (rightType.baseType == T_STRING && binOp->op == TK_PLUS) return (TypeID) { T_STRING };

        if (leftType.baseType == T_FLOAT || rightType.baseType == T_FLOAT) return (TypeID) { T_FLOAT };
        return (TypeID) { T_INT };
    }
    case AST_UNARY_OP: {
        ASTUnaryOp* unOp = (ASTUnaryOp*)exprNode;
        return getExpressionResultType(state, unOp->operand);
    }
    case AST_MEMBER_ACCESS: {
        ASTMemberAccess* memberNode = (ASTMemberAccess*)exprNode;
        if (memberNode->classNode->type == AST_VAR) {
            char qualified[256];
            snprintf(qualified, sizeof(qualified), "%s.%s",
                ((ASTVar*)memberNode->classNode)->name, memberNode->memberName);
            Symbol* constant = lookupSymbolGlobal(state, qualified);
            if (constant && constant->kind == SYM_CONSTANT)
                return constant->type;
        }
        TypeID classType = getExpressionResultType(state, memberNode->classNode);
        if (classType.baseType != T_CLASS) {
            return (TypeID) { .baseType = T_VOID };
        }
        Symbol* classDef = findClassSymbol(state, classType.name);
        return findMemberType(classDef, memberNode->memberName);
    }
    case AST_NEW: {
        ASTNewNode* newNode = (ASTNewNode*)exprNode;
        // If it is an array, it is ALWAYS a Pointer/Reference
        if (newNode->newType == NEW_ARRAY) {
            return (TypeID) { T_PTR, newNode->typeToCreate.name,
                newNode->typeToCreate.baseType, 0 };
        }

        Symbol* classSym = findClassSymbol(state, newNode->typeToCreate.name);
        if (classSym) {
            return classSym->type;
        }
        return (TypeID) { .baseType = T_VOID };
    }
    case AST_FUNC_CALL: {
        ASTFuncCall* call = (ASTFuncCall*)exprNode;
        // Constructor syntax is represented as a call whose callee is AST_NEW.
        // Its value is the newly allocated class, not the constructor's void return.
        if (call->callee->type == AST_NEW)
            return getExpressionResultType(state, call->callee);
        // Handle method calls (obj.method)
        if (call->callee->type == AST_MEMBER_ACCESS) {
            ASTMemberAccess* m = (ASTMemberAccess*)call->callee;
            TypeID objType = getExpressionResultType(state, m->classNode);
            Symbol* sym = findMethodSymbol(state, objType.name, m->memberName);//lookupSymbolGlobal(state, mangled);
            return functionCallResultType(state, sym);
        }

        // Handle standard function calls
        if (call->callee->type == AST_VAR) {
            ASTVar* var = (ASTVar*)call->callee;
            Symbol* sym = lookupSymbolRecursive(state, var->name);
            return functionCallResultType(state, sym);
        }
        return (TypeID) { .baseType = T_INT }; // Fallback
    }
    case AST_ARRAY_ACCESS: {
        ASTArrayAccess* access = (ASTArrayAccess*)exprNode;
        TypeID arrayType = getExpressionResultType(state, access->array);

        // If your type system tracks array types (e.g. "Texture[]"), 
        // we need to return the element type ("Texture").

        // Assuming T_PTR or T_SHARED_PTR represents an array:
        if (arrayType.baseType == T_PTR || arrayType.baseType == T_SHARED_PTR) {
            // We need to look up the variable to see what it actually holds.
            // If 'arrayType' has a name like "Texture", then it's a "Texture[]".
            // So the result is type "Texture".

            // NOTE: This depends on how you store types. 
            // If TypeID just says T_PTR, we might need to look at the Symbol.
            // But usually, you store the class name in 'arrayType.name'.

            return (TypeID) {
                arrayType.elementType ? arrayType.elementType : T_CLASS,
                arrayType.name,
                0,
                arrayType.borrowed
            };
        }

        // Fallback for primitive arrays if you distinguish them
        return (TypeID) { T_INT, NULL };
    }
    default:
        return (TypeID) { .baseType = T_VOID };
    }
}

void generateNew(State* state, ASTNode* exprNode, int destRegister)
{
    ASTNewNode* newNode = (ASTNewNode*)exprNode;

    // 1. Single Object Allocation
    if (newNode->newType == NEW_OBJECT) {
        Symbol* classSym = findClassSymbol(state, newNode->typeToCreate.name);
        if (classSym && classSym->as.classDef.packedLayout &&
            classSym->as.classDef.packedSize <= 255 &&
            array_size(&classSym->as.classDef.members) <= 255) {
            emitABC_IR(state, OP_ALLOCATE_PACKED_OBJECT, destRegister,
                classSym->as.classDef.packedSize,
                array_size(&classSym->as.classDef.members));
        } else {
            short count = (short)getTypeSize(state, newNode->typeToCreate);
            emitAD_IR(state, OP_ALLOCATE, destRegister, count);
        }
        return;
    }

    // 2. Array Allocation
    if (newNode->newType == NEW_ARRAY && newNode->as.arraySize) {
        int sizeReg = allocReg(state);
        generateExpression(state, newNode->as.arraySize, sizeReg);

        // We must ensure "Array[]" is treated as a Pointer Array.
        // If typeToCreate is T_CLASS, it's a pointer array.
        // If it's T_STRING, T_PTR, etc., it's a pointer array.
        bool isPointerArray = false;
        if (newNode->typeToCreate.baseType == T_CLASS ||
            newNode->typeToCreate.baseType == T_STRING ||
            newNode->typeToCreate.baseType == T_PTR ||
            newNode->typeToCreate.baseType == T_SHARED_PTR) {
            isPointerArray = true;
        }

        if (isPointerArray) {
            // Allocate 64-bit slots (8 bytes)
            emitABC_IR(state, OP_ALLOCATE_ARRAY_PTR, destRegister, sizeReg, 0);
        }
        else {
            if (newNode->typeToCreate.baseType == T_CHAR)
                emitABC_IR(state, OP_NEWARRAY_TYPED, destRegister, sizeReg, 1);
            else if (newNode->typeToCreate.baseType == T_SHORT)
                emitABC_IR(state, OP_NEWARRAY_TYPED, destRegister, sizeReg, 2);
            else if (newNode->typeToCreate.baseType == T_FLOAT)
                emitABC_IR(state, OP_NEWARRAY_TYPED, destRegister, sizeReg, 4);
            else
                emitAD_IR(state, OP_NEWARRAY, destRegister, (short)sizeReg);
        }

        freeRegisters(state, 1);
    }
}

// =====================================================================
// Fast direct array access helpers (eliminates array_get calls)
// =====================================================================
static inline ASTNode** args_data(ASTFuncCall* call) { return (ASTNode**)call->arguments.data; }
static inline ASTNode** block_stmts(ASTBlock* block) { return (ASTNode**)block->list.data; }
static inline Token** decl_names(ASTMultiVarDecl* decl) { return (Token**)decl->names.data; }
static bool packedField(Symbol* classSym, int offset, TypeIDEnum type);
static OPCode getPackedLoadOp(TypeIDEnum type);
static OPCode getPackedStoreOp(TypeIDEnum type);
// =====================================================================

#define MAX_CFUNC_ARGS 64
#define MAX_REGS 16

// Returns the correct SET opcode based on type
OPCode getSetFieldOp(TypeIDEnum type) {
    switch (type) {
    case T_FLOAT: return OP_STORE_FIELDf;
    case T_INT:   return OP_STORE_FIELDi;
    case T_BOOL:  return OP_STORE_FIELDi; // or OP_SET_FIELDb
    case T_STRING:
    case T_CLASS:
    case T_SHARED_PTR:
    case T_PTR:   return OP_STORE_FIELDp;
    default:      return OP_STORE_FIELDi;
    }
}

// Returns the correct GET opcode based on type
OPCode getGetFieldOp(TypeIDEnum type) {
    switch (type) {
    case T_FLOAT: return OP_LOAD_FIELDf;
    case T_INT:   return OP_LOAD_FIELDi;
    case T_BOOL:  return OP_LOAD_FIELDi;
    case T_STRING:
    case T_CLASS:
    case T_SHARED_PTR:
    case T_PTR:   return OP_GET_FIELDp;
    default:      return OP_LOAD_FIELDi;
    }
}

int emitSetGlobal(State* s, TypeIDEnum type, int reg, short offset)
{
    switch (type)
    {
    case T_STRING:
    case T_CLASS:
    case T_SHARED_PTR:
    case T_PTR: emitAD_IR(s, OP_STOREp_GLOBAL, reg, offset); break;
    case T_CHAR: emitAD_IR(s, OP_STORE8_TYPED_GLOBAL, reg, offset); break;
    case T_SHORT: emitAD_IR(s, OP_STORE16_TYPED_GLOBAL, reg, offset); break;
    default: emitAD_IR(s, OP_STORE32_GLOBAL, reg, offset); break;
    }

    return true;
}

int emitSetLocal(State* s, TypeIDEnum type, int reg, short index)
{
    switch (type) {
    case T_STRING:
    case T_CLASS:
    case T_SHARED_PTR:
    case T_PTR:emitAD_IR(s, OP_STOREp_LOCAL, reg, index); break;
    case T_CHAR:
        emitAD_IR(s, OP_STORE8_TYPED_LOCAL, reg, index);
        break;
    case T_SHORT:
        emitAD_IR(s, OP_STORE16_TYPED_LOCAL, reg, index);
        break;
    default:
        emitAD_IR(s, OP_STORE32_LOCAL, reg, index);
        break;
    }
    return true;
}

int emitGetLocal(State* state, TypeIDEnum type, int reg, short index)
{
    switch (type)
    {
    case T_STRING:
    case T_CLASS:
    case T_SHARED_PTR:
    case T_PTR:emitAD_IR(state, OP_LOADp_LOCAL, reg, index); break;
    case T_INT:emitAD_IR(state, OP_LOAD32_LOCAL, reg, index); break;
    case T_FLOAT:emitAD_IR(state, OP_LOADf_LOCAL, reg, index); break;
    case T_BOOL: emitAD_IR(state, OP_LOAD8_LOCAL, reg, index); break;
    case T_CHAR: emitAD_IR(state, OP_LOAD8_TYPED_LOCAL, reg, index); break;
    case T_SHORT: emitAD_IR(state, OP_LOAD16_TYPED_LOCAL, reg, index); break;
    default:
        // Optional: Print error here to catch silent fails in future
        reportError(state->compiler, LOC_NONE, "Unhandled type %d in emitGetLocal\n", type);
        return false;
    }
    return true;
}

bool emitGetGlobal(State* state, TypeIDEnum type, int destRegister, short localOffset)
{
    switch (type)
    {
    case T_BOOL: emitAD_IR(state, OP_LOADb_GLOBAL, destRegister, localOffset); return true;
    case T_CHAR: emitAD_IR(state, OP_LOAD8_TYPED_GLOBAL, destRegister, localOffset); return true;
    case T_SHORT: emitAD_IR(state, OP_LOAD16_TYPED_GLOBAL, destRegister, localOffset); return true;
    case T_INT: emitAD_IR(state, OP_LOADi_GLOBAL, destRegister, localOffset); return true;
    case T_FLOAT: emitAD_IR(state, OP_LOADf_GLOBAL, destRegister, localOffset); return true;
    case T_STRING: //emitAD_IR(state, OP_LOADs_GLOBAL, destRegister, localOffset); return true;
    case T_CLASS:
    case T_SHARED_PTR:
    case T_PTR: emitAD_IR(state, OP_LOADp_GLOBAL, destRegister, localOffset); return true;
    default:
        // Optional: Print error here to catch silent fails in future
        reportError(state->compiler, LOC_NONE, "Unhandled type %d in emitGetGlobal\n", type);
        return false;
    }
}

// Tries to load the 'this' pointer into a register if the variable name is a member.
// Returns the register index containing 'this', or -1 if not found.
// Fills 'memberOffset' and 'memberType'.
int tryLoadThisForMember(State* s, char* varName, int* memberOffset, TypeID* memberType) {
    if (!s->compiler->currentClass) return -1;

    *memberOffset = findMemberOffset(s->compiler->currentClass, varName);
    if (*memberOffset < 0) return -1;

    Symbol* thisSym = lookupSymbolLocal(s, "this");
    if (!thisSym) return -1; // Should theoretically not happen inside a method

    int thisReg = allocReg(s);
    emitGetLocal(s, thisSym->type.baseType, thisReg, (short)thisSym->stackIndex);

    *memberType = findMemberType(s->compiler->currentClass, varName);
    return thisReg;
}

static void getVariable(State* s, char* name, Loc loc, int dest) {
    // 1. Try Local
    Symbol* sym = lookupSymbolLocal(s, name);
    if (sym) {
            if (sym->valueState != VALUE_INITIALIZED) {
                const char* reason = sym->valueState == VALUE_MOVED ? "moved value" :
                    sym->valueState == VALUE_UNINITIALIZED ? "uninitialized value" :
                    "value that is not initialized on every control-flow path";
                reportError(s->compiler, loc, "Use of %s: %s", reason, name);
                emitAD_IR(s, OP_LOAD_NULL, dest, 0);
                return;
            }
        if (sym->kind == SYM_CONSTANT) {
            emitConstant_IR(s, sym->as.constValue, dest);
        }
        else {
            emitGetLocal(s, sym->type.baseType, dest, (short)sym->stackIndex);
        }
        return;
    }

    // 2. Try Class Member (Implicit 'this')
    int offset;
    TypeID type;
    int thisReg = tryLoadThisForMember(s, name, &offset, &type);

    if (thisReg != -1) {
        OPCode op = packedField((Symbol*)s->compiler->currentClass, offset, type.baseType)
            ? getPackedLoadOp(type.baseType) : getGetFieldOp(type.baseType);
        emitABC_IR(s, op, dest, thisReg, offset);
        freeRegisters(s, 1); // Free thisReg
        return;
    }

    // 3. Try Global
    sym = lookupSymbolGlobal(s, name);
    if (sym) {
        if (sym->kind == SYM_CONSTANT) emitConstant_IR(s, sym->as.constValue, dest);
        else emitGetGlobal(s, sym->type.baseType, dest, (short)sym->stackIndex);
        return;
    }

    reportError(s->compiler, loc, "Undefined variable: %s", name);
}

static void setVariable(State* s, ASTVar* var, int src, TypeID srcType) {
    // 1. Try Local
    Symbol* sym = lookupSymbolLocal(s, var->name);
    if (sym) {
        emitCastIfNeeded(s, sym->type, srcType, (short)src);
        emitSetLocal(s, sym->type.baseType, src, (short)sym->stackIndex);
        sym->valueState = VALUE_INITIALIZED;
        return;
    }

    // 2. Try Class Member (Implicit 'this')
    int offset;
    TypeID memberType;
    int thisReg = tryLoadThisForMember(s, var->name, &offset, &memberType);

    if (thisReg != -1) {
        emitCastIfNeeded(s, memberType, srcType, (short)src);
        OPCode op = packedField((Symbol*)s->compiler->currentClass, offset, memberType.baseType)
            ? getPackedStoreOp(memberType.baseType) : getSetFieldOp(memberType.baseType);
        emitABC_IR(s, op, thisReg, (short)offset, (short)src);
        freeRegisters(s, 1);
        return;
    }

    // 3. Try Global
    sym = lookupSymbolGlobal(s, var->name);
    if (sym) {
        emitSetGlobal(s, sym->type.baseType, src, (short)sym->stackIndex);
        return;
    }
    reportError(s->compiler, var->node.loc, "Undefined variable: '%s'", var->name);
}

// Pushes evaluated registers to the stack frame
// Returns the total number of bytes advanced on the stack
int emitPushArguments(State* s, ASTNode** args, int nargs, int* regs, int startOffset) {
    int currentOffset = startOffset;

    for (int i32 = 0; i32 < nargs; ++i32) {
        TypeID t = getExpressionResultType(s, args[i32]);

        // Use the centralized size logic
        int size = getTypeSize(s, t);

        // Optional: Alignment logic can go here if needed
        // currentOffset = (currentOffset + 3) & ~3; 

        emitSetLocal(s, t.baseType, regs[i32], (short)currentOffset);
        currentOffset += size;
    }
    return currentOffset - startOffset;
}

static bool emitMathIntrinsic(State* s, Symbol* funcSym, array* arguments, int destReg, Loc loc)
{
    if (!funcSym || funcSym->kind != SYM_CFUNCTION || !funcSym->name) return false;

    OPCode op = OP_NOP;
    int expected = 0;
    if      (strcmp(funcSym->name, "Math.sqrt") == 0)  { op = OP_SQRTf; expected = 1; }
    else if (strcmp(funcSym->name, "Math.sin") == 0)   { op = OP_SINf; expected = 1; }
    else if (strcmp(funcSym->name, "Math.cos") == 0)   { op = OP_COSf; expected = 1; }
    else if (strcmp(funcSym->name, "Math.abs") == 0)   { op = OP_ABSf; expected = 1; }
    else if (strcmp(funcSym->name, "Math.atan2") == 0) { op = OP_ATAN2f; expected = 2; }
    else if (strcmp(funcSym->name, "Math.min") == 0)   { op = OP_MINf; expected = 2; }
    else if (strcmp(funcSym->name, "Math.max") == 0)   { op = OP_MAXf; expected = 2; }
    else if (strcmp(funcSym->name, "Math.clamp") != 0) return false;
    else expected = 3;

    const int nargs = array_size(arguments);
    if (nargs != expected) {
        reportError(s->compiler, loc, "Intrinsic '%s' expects %d arguments, but got %d.",
            funcSym->name, expected, nargs);
        return true;
    }

    ASTNode** args = (ASTNode**)arguments->data;
    const int base = s->compiler->nextRegisterIndex;
    s->compiler->nextRegisterIndex += nargs;
    if (s->compiler->nextRegisterIndex > MAX_REGISTERS) {
        reportError(s->compiler, loc, "Register overflow in math intrinsic");
        return true;
    }
    for (int i = 0; i < nargs; ++i) generateExpression(s, args[i], base + i);

    if (expected == 1) emitABC_IR(s, op, destReg, base, 0);
    else if (expected == 2) emitABC_IR(s, op, destReg, base, base + 1);
    else {
        int temp = allocReg(s);
        emitABC_IR(s, OP_MAXf, temp, base, base + 1);
        emitABC_IR(s, OP_MINf, destReg, temp, base + 2);
        freeRegisters(s, 1);
    }
    freeRegisters(s, nargs);
    return true;
}

// Inline a proven leaf expression after evaluating all arguments exactly once.
// Matching emitted IR avoids assuming that source syntax implies purity: any
// preceding statement, conversion, call, or control flow makes the match fail.
static bool emitLeafArithmeticCall(State* s, Symbol* function, int base, int nargs, int dest) {
    if (function->kind != SYM_FUNCTION || nargs != 2 || function->as.funcDef.paramCount != 2)
        return false;
    IRNode* entry = function->as.funcDef.entry_ir;
    IRNode* left = entry ? entry->next : NULL;
    IRNode* right = left ? left->next : NULL;
    IRNode* math = right ? right->next : NULL;
    IRNode* ret = math ? math->next : NULL;
    if (!ret || left->data.type != IR_INSTRUCTION || right->data.type != IR_INSTRUCTION ||
        math->data.type != IR_INSTRUCTION || ret->data.type != IR_INSTRUCTION ||
        (ret->data.op != OP_RETURN && ret->data.op != OP_RETURN_PRIM) || math->data.a != 0 ||
        left->data.a == right->data.a || math->data.b != left->data.a ||
        math->data.c != right->data.a || left->data.c < 0 || left->data.c > 1 ||
        right->data.c < 0 || right->data.c > 1) return false;
    TypeIDEnum type = function->type.baseType;
    if (function->as.funcDef.paramTypes[0].baseType != type ||
        function->as.funcDef.paramTypes[1].baseType != type) return false;
    bool integer = type == T_INT && left->data.op == OP_LOAD32_LOCAL &&
        right->data.op == OP_LOAD32_LOCAL &&
        (math->data.op == OP_ADD || math->data.op == OP_SUB || math->data.op == OP_MUL);
    bool floating = type == T_FLOAT && left->data.op == OP_LOADf_LOCAL &&
        right->data.op == OP_LOADf_LOCAL &&
        (math->data.op == OP_ADDf || math->data.op == OP_SUBf || math->data.op == OP_MULf);
    if (!integer && !floating) return false;
    emitABC_IR(s, math->data.op, dest, base + left->data.c, base + right->data.c);
    return true;
}

static bool expressionTransfersOwnership(State* state, ASTNode* expression) {
    if (!expression) return false;
    if (expression->type == AST_NEW) return true;
    if (expression->type == AST_FUNC_CALL)
        return !getExpressionResultType(state, expression).borrowed;
    if (expression->type == AST_UNARY_OP)
        return ((ASTUnaryOp*)expression)->op == TK_MOVE;
    return false;
}

// Emit a full-expression heap scan only if evaluation can create an unclaimed
// owned result. Primitive-only expressions cannot leave such a temporary.
static bool expressionMayCreateUnclaimed(State* state, ASTNode* expression) {
    if (!expression) return false;
    switch (expression->type) {
    case AST_NEW:
        return true;
    case AST_FUNC_CALL: {
        ASTFuncCall* call = (ASTFuncCall*)expression;
        TypeID result = getExpressionResultType(state, expression);
        if (isRefType(result) && !result.borrowed) return true;
        if (expressionMayCreateUnclaimed(state, call->callee)) return true;
        for (int i = 0; i < array_size(&call->arguments); ++i) {
            if (expressionMayCreateUnclaimed(state,
                *(ASTNode**)array_get(&call->arguments, i))) return true;
        }
        return false;
    }
    case AST_BINARY_OP: {
        ASTBinaryOp* binary = (ASTBinaryOp*)expression;
        return expressionMayCreateUnclaimed(state, binary->left) ||
            expressionMayCreateUnclaimed(state, binary->right);
    }
    case AST_UNARY_OP:
        return expressionMayCreateUnclaimed(state, ((ASTUnaryOp*)expression)->operand);
    case AST_VAR:
        return expressionMayCreateUnclaimed(state, ((ASTVar*)expression)->index);
    case AST_MEMBER_ACCESS:
        return expressionMayCreateUnclaimed(state,
            ((ASTMemberAccess*)expression)->classNode);
    case AST_ARRAY_ACCESS: {
        ASTArrayAccess* access = (ASTArrayAccess*)expression;
        return expressionMayCreateUnclaimed(state, access->array) ||
            expressionMayCreateUnclaimed(state, access->index);
    }
    default:
        return false;
    }
}

typedef struct {
    int rootRefIndex;
    uint64_t path;
    int depth;
    uint64_t components[MAX_BORROW_PATH_DEPTH];
} BorrowOrigin;

static Symbol* functionSymbolForCall(State* state, ASTFuncCall* call)
{
    if (!state || !call || !call->callee) return NULL;
    if (call->callee->type == AST_VAR) {
        const char* name = ((ASTVar*)call->callee)->name;
        Symbol* function = lookupSymbolLocal(state, (char*)name);
        if (!function) function = lookupSymbolGlobal(state, name);
        return function && function->kind == SYM_FUNCTION ? function : NULL;
    }
    if (call->callee->type == AST_MEMBER_ACCESS) {
        ASTMemberAccess* member = (ASTMemberAccess*)call->callee;
        TypeID receiverType = getExpressionResultType(state, member->classNode);
        if (receiverType.baseType != T_CLASS) return NULL;
        Symbol* function = findMethodSymbol(state, receiverType.name, member->memberName);
        return function && function->kind == SYM_FUNCTION ? function : NULL;
    }
    return NULL;
}

static bool sameNamedType(TypeID expected, TypeID actual)
{
    if (expected.baseType != actual.baseType) return false;
    if (expected.baseType == T_CLASS) {
        if (!expected.name || !actual.name) return expected.name == actual.name;
        if (strcmp(expected.name, actual.name) != 0) return false;
    }
    if ((expected.baseType == T_PTR || expected.baseType == T_SHARED_PTR) &&
        ((expected.name == NULL) != (actual.name == NULL) ||
         (expected.name && strcmp(expected.name, actual.name) != 0))) return false;
    if ((expected.baseType == T_PTR || expected.baseType == T_SHARED_PTR) &&
        expected.elementType != actual.elementType) return false;
    return true;
}

static bool assignmentTypeCompatible(TypeID expected, TypeID actual)
{
    if (sameNamedType(expected, actual)) return true;
    return expected.baseType == T_FLOAT && actual.baseType == T_INT;
}

static void reportIncompatibleValue(State* state, Loc loc, const char* context,
    TypeID expected, TypeID actual)
{
    if (assignmentTypeCompatible(expected, actual)) return;
    reportError(state->compiler, loc, "%s: expected %s, got %s.", context,
        typeEnumToString(&expected), typeEnumToString(&actual));
}

static uint64_t appendBorrowPath(uint64_t path, const char* memberName)
{
    uint64_t hash = path ? path : UINT64_C(1469598103934665603);
    const unsigned char* p = (const unsigned char*)memberName;
    while (p && *p) {
        hash ^= (uint64_t)*p++;
        hash *= UINT64_C(1099511628211);
    }
    hash ^= UINT64_C(0xff);
    hash *= UINT64_C(1099511628211);
    return hash;
}

#define BORROW_COMPONENT_KIND_MASK UINT64_C(0xc000000000000000)
#define BORROW_ARRAY_INDEX_KIND   UINT64_C(0x8000000000000000)
#define BORROW_ARRAY_ANY_COMPONENT UINT64_C(0xc000000000000000)

static uint64_t borrowFieldComponentToken(const char* fieldName)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    const unsigned char* p = (const unsigned char*)fieldName;
    while (p && *p) {
        hash ^= (uint64_t)*p++;
        hash *= UINT64_C(1099511628211);
    }
    hash &= ~BORROW_COMPONENT_KIND_MASK;
    return hash ? hash : 1;
}

static bool borrowPathComponentMatches(uint64_t left, uint64_t right)
{
    if (left == right) return true;
    if (left == BORROW_ARRAY_ANY_COMPONENT)
        return (right & BORROW_COMPONENT_KIND_MASK) == BORROW_ARRAY_INDEX_KIND;
    if (right == BORROW_ARRAY_ANY_COMPONENT)
        return (left & BORROW_COMPONENT_KIND_MASK) == BORROW_ARRAY_INDEX_KIND;
    return false;
}

static bool borrowPathComponentsMatch(const uint64_t* left, const uint64_t* right,
    int count)
{
    for (int i = 0; i < count; ++i) {
        if (!borrowPathComponentMatches(left[i], right[i])) return false;
    }
    return true;
}

static bool borrowOriginsMatch(BorrowOrigin actual, BorrowOrigin expected)
{
    return actual.rootRefIndex == expected.rootRefIndex &&
        actual.depth == expected.depth &&
        borrowPathComponentsMatch(actual.components, expected.components, actual.depth);
}

static bool constantArrayIndex(ASTNode* expression, int32_t* index)
{
    if (!expression || expression->type != AST_CONST) return false;
    ASTConst* constant = (ASTConst*)expression;
    if (constant->value.type.baseType != T_INT) return false;
    *index = constant->value.v.i32;
    return true;
}

static BorrowOrigin appendBorrowReturnPath(State* state, BorrowOrigin origin,
    const char* path, Loc loc)
{
    if (!path || !*path || origin.rootRefIndex == -1) return origin;
    const char* component = path;
    while (*component) {
        const char* end = strchr(component, '.');
        size_t length = end ? (size_t)(end - component) : strlen(component);
        if (length == 0 || length >= 1024 || origin.depth >= MAX_BORROW_PATH_DEPTH) {
            reportError(state->compiler, loc,
                "Borrow path exceeds the supported depth of %d components",
                MAX_BORROW_PATH_DEPTH);
            origin.rootRefIndex = -1;
            return origin;
        }
        char part[1024];
        memcpy(part, component, length);
        part[length] = '\0';
        origin.path = appendBorrowPath(origin.path, part);
        origin.components[origin.depth++] = strcmp(part, "[]") == 0
            ? BORROW_ARRAY_ANY_COMPONENT : borrowFieldComponentToken(part);
        if (!end) break;
        component = end + 1;
    }
    return origin;
}

static BorrowOrigin borrowedSourceOrigin(State* state, ASTNode* expression)
{
    BorrowOrigin none = { -1, 0, 0, { 0 } };
    if (!state || !expression) return none;
    if (expression->type == AST_FUNC_CALL) {
        ASTFuncCall* call = (ASTFuncCall*)expression;
        Symbol* function = functionSymbolForCall(state, call);
        if (!function || !function->type.borrowed) return none;
        int sourceIndex = function->as.funcDef.borrowedReturnParamIndex;
        if (sourceIndex == -2 && call->callee->type == AST_MEMBER_ACCESS) {
            BorrowOrigin origin = borrowedSourceOrigin(state,
                ((ASTMemberAccess*)call->callee)->classNode);
            return appendBorrowReturnPath(state, origin,
                function->as.funcDef.borrowedReturnPath, expression->loc);
        }
        if (sourceIndex < 0 || sourceIndex >= array_size(&call->arguments)) return none;
        ASTNode* source = *(ASTNode**)array_get(&call->arguments, sourceIndex);
        BorrowOrigin origin = borrowedSourceOrigin(state, source);
        return appendBorrowReturnPath(state, origin,
            function->as.funcDef.borrowedReturnPath, expression->loc);
    }
    if (expression->type == AST_ARRAY_ACCESS) {
        ASTArrayAccess* access = (ASTArrayAccess*)expression;
        TypeID elementType = getExpressionResultType(state, expression);
        if (!isRefType(elementType)) return none;
        BorrowOrigin origin = borrowedSourceOrigin(state, access->array);
        if (origin.rootRefIndex == -1) return none;
        if (origin.depth >= MAX_BORROW_PATH_DEPTH) {
            reportError(state->compiler, expression->loc,
                "Borrow path exceeds the supported depth of %d components",
                MAX_BORROW_PATH_DEPTH);
            return none;
        }
        int32_t constantIndex = 0;
        char indexPath[32];
        uint64_t component;
        if (constantArrayIndex(access->index, &constantIndex)) {
            snprintf(indexPath, sizeof(indexPath), "[%d]", constantIndex);
            component = BORROW_ARRAY_INDEX_KIND | (uint32_t)constantIndex;
        }
        else {
            // Dynamic indices conservatively alias every array element.
            strcpy(indexPath, "[]");
            component = BORROW_ARRAY_ANY_COMPONENT;
        }
        origin.path = appendBorrowPath(origin.path, indexPath);
        origin.components[origin.depth] = component;
        origin.depth++;
        return origin;
    }
    if (expression->type == AST_MEMBER_ACCESS) {
        ASTMemberAccess* member = (ASTMemberAccess*)expression;
        TypeID memberType = getExpressionResultType(state, expression);
        if (!isRefType(memberType)) return none;
        BorrowOrigin origin = borrowedSourceOrigin(state, member->classNode);
        if (origin.rootRefIndex == -1) return none;
        if (origin.depth >= MAX_BORROW_PATH_DEPTH) {
            reportError(state->compiler, expression->loc,
                "Borrow path exceeds the supported depth of %d components",
                MAX_BORROW_PATH_DEPTH);
            return none;
        }
        origin.path = appendBorrowPath(origin.path, member->memberName);
        origin.components[origin.depth] = borrowFieldComponentToken(member->memberName);
        origin.depth++;
        return origin;
    }
    if (expression->type != AST_VAR) return none;
    Symbol* owner = lookupSymbolLocal(state, ((ASTVar*)expression)->name);
    if (!owner || !isRefType(owner->type) || owner->valueState != VALUE_INITIALIZED) return none;
    // Parameters (including `this`) are borrowed from the caller. Their exact
    // owner is unknown here, but their local ref slot is a useful provenance
    // root for rejecting escapes from this function.
    if (owner->kind == SYM_PARAMETER && !owner->ownsValue) {
        BorrowOrigin origin = { owner->stackIndex, 0, 0, { 0 } };
        return origin;
    }
    if (owner->type.borrowed) {
        BorrowOrigin origin = { owner->borrowedFromRefIndex, owner->borrowedFromPath,
            owner->borrowedFromDepth, { 0 } };
        memcpy(origin.components, owner->borrowedPathComponents, sizeof(origin.components));
        return origin;
    }
    if (!owner->ownsValue) return none;
    BorrowOrigin origin = { owner->stackIndex, 0, 0, { 0 } };
    return origin;
}

// Like borrowedSourceOrigin, but for a storage destination: an owned local
// remains the same storage root even if the current iteration has moved it.
static BorrowOrigin borrowedStorageOrigin(State* state, ASTNode* expression)
{
    BorrowOrigin none = { -1, 0, 0, { 0 } };
    if (!state || !expression) return none;
    if (expression->type == AST_VAR) {
        Symbol* symbol = lookupSymbolLocal(state, ((ASTVar*)expression)->name);
        if (!symbol || !isRefType(symbol->type)) return none;
        if (symbol->type.borrowed) {
            BorrowOrigin origin = { symbol->borrowedFromRefIndex, symbol->borrowedFromPath,
                symbol->borrowedFromDepth, { 0 } };
            memcpy(origin.components, symbol->borrowedPathComponents, sizeof(origin.components));
            return origin;
        }
        if (symbol->ownsValue || symbol->kind == SYM_PARAMETER)
            return (BorrowOrigin){ symbol->stackIndex, 0, 0, { 0 } };
        return none;
    }
    if (expression->type == AST_MEMBER_ACCESS) {
        ASTMemberAccess* member = (ASTMemberAccess*)expression;
        if (!isRefType(getExpressionResultType(state, expression))) return none;
        BorrowOrigin origin = borrowedStorageOrigin(state, member->classNode);
        if (origin.rootRefIndex == -1 || origin.depth >= MAX_BORROW_PATH_DEPTH) return none;
        origin.path = appendBorrowPath(origin.path, member->memberName);
        origin.components[origin.depth++] = borrowFieldComponentToken(member->memberName);
        return origin;
    }
    if (expression->type == AST_ARRAY_ACCESS) {
        ASTArrayAccess* access = (ASTArrayAccess*)expression;
        if (!isRefType(getExpressionResultType(state, expression))) return none;
        BorrowOrigin origin = borrowedStorageOrigin(state, access->array);
        if (origin.rootRefIndex == -1 || origin.depth >= MAX_BORROW_PATH_DEPTH) return none;
        int32_t index = 0;
        char indexPath[32];
        uint64_t component;
        if (constantArrayIndex(access->index, &index)) {
            snprintf(indexPath, sizeof(indexPath), "[%d]", index);
            component = BORROW_ARRAY_INDEX_KIND | (uint32_t)index;
        }
        else {
            strcpy(indexPath, "[]");
            component = BORROW_ARRAY_ANY_COMPONENT;
        }
        origin.path = appendBorrowPath(origin.path, indexPath);
        origin.components[origin.depth++] = component;
        return origin;
    }
    return none;
}

static int borrowedSourceRefIndex(State* state, ASTNode* expression)
{
    return borrowedSourceOrigin(state, expression).rootRefIndex;
}

static bool hasLiveBorrowOfRefIndex(State* state, int refIndex, int firstSymbol)
{
    if (!state || refIndex < 0) return false;
    int start = firstSymbol < 0 ? 0 : firstSymbol;
    for (int i = state->compiler->symbols.count - 1; i >= start; --i) {
        Symbol* symbol = (Symbol*)array_get(&state->compiler->symbols, i);
        if (symbol && symbol->scopeParent == state->compiler->currentScopeParent &&
            symbol->scopeLevel > 0 && symbol->kind == SYM_VARIABLE &&
            symbol->type.borrowed &&
            (symbol->borrowedFromRefIndex == refIndex ||
             symbol->borrowedFromRefIndex == BORROW_UNKNOWN_REF_INDEX)) {
            return true;
        }
    }
    return false;
}

static bool hasLiveBorrowOfStorage(State* state, BorrowOrigin origin, int firstSymbol)
{
    if (!state || origin.rootRefIndex == -1) return false;
    int start = firstSymbol < 0 ? 0 : firstSymbol;
    for (int i = state->compiler->symbols.count - 1; i >= start; --i) {
        Symbol* symbol = (Symbol*)array_get(&state->compiler->symbols, i);
        if (symbol && symbol->scopeParent == state->compiler->currentScopeParent &&
            symbol->scopeLevel > 0 && symbol->kind == SYM_VARIABLE && symbol->type.borrowed &&
            (origin.rootRefIndex == BORROW_UNKNOWN_REF_INDEX ||
             symbol->borrowedFromRefIndex == BORROW_UNKNOWN_REF_INDEX ||
             symbol->borrowedFromRefIndex == origin.rootRefIndex) &&
            (origin.rootRefIndex == BORROW_UNKNOWN_REF_INDEX ||
             symbol->borrowedFromRefIndex == BORROW_UNKNOWN_REF_INDEX ||
             origin.depth <= symbol->borrowedFromDepth)) {
            if (origin.rootRefIndex == BORROW_UNKNOWN_REF_INDEX ||
                symbol->borrowedFromRefIndex == BORROW_UNKNOWN_REF_INDEX ||
                borrowPathComponentsMatch(origin.components,
                symbol->borrowedPathComponents, origin.depth))
                return true;
        }
    }
    return false;
}

static bool ownedStorageTarget(State* state, ASTNode* target)
{
    if (!state || !target) return false;
    if (target->type == AST_VAR) {
        Symbol* symbol = lookupSymbolLocal(state, ((ASTVar*)target)->name);
        return symbol && symbol->ownsValue && isRefType(symbol->type);
    }
    if (target->type == AST_MEMBER_ACCESS) {
        TypeID type = getExpressionResultType(state, target);
        return isRefType(type) && !type.borrowed;
    }
    if (target->type == AST_ARRAY_ACCESS) {
        ASTArrayAccess* access = (ASTArrayAccess*)target;
        TypeID arrayType = getExpressionResultType(state, access->array);
        return isRefType(getExpressionResultType(state, target)) && !arrayType.borrowed;
    }
    return false;
}

static void checkLoopCarriedMutation(State* state, ASTNode* target, Loc loc)
{
    if (!ownedStorageTarget(state, target)) return;
    BorrowOrigin origin = borrowedStorageOrigin(state, target);
    if (hasLiveBorrowOfStorage(state, origin, 0)) {
        reportError(state->compiler, loc,
            "Loop may replace storage still borrowed from the previous iteration");
    }
}

static void scanLoopCarryOperations(State* state, ASTNode* node)
{
    if (!node) return;
    switch (node->type) {
    case AST_BLOCK: {
        ASTBlock* block = (ASTBlock*)node;
        for (int i = 0; i < array_size(&block->list); ++i)
            scanLoopCarryOperations(state, *(ASTNode**)array_get(&block->list, i));
        break;
    }
    case AST_BINARY_OP: {
        ASTBinaryOp* binary = (ASTBinaryOp*)node;
        if (binary->op == TK_ASSIGN)
            checkLoopCarriedMutation(state, binary->left, binary->node.loc);
        scanLoopCarryOperations(state, binary->left);
        scanLoopCarryOperations(state, binary->right);
        break;
    }
    case AST_UNARY_OP: {
        ASTUnaryOp* unary = (ASTUnaryOp*)node;
        if (unary->op == TK_MOVE) {
            ASTNode* target = unary->operand;
            if (target && target->type == AST_VAR) {
                Symbol* symbol = lookupSymbolLocal(state, ((ASTVar*)target)->name);
                if (symbol && symbol->ownsValue && isRefType(symbol->type))
                    checkLoopCarriedMutation(state, target, unary->node.loc);
            }
        }
        scanLoopCarryOperations(state, unary->operand);
        break;
    }
    case AST_IF: {
        ASTIfStmt* branch = (ASTIfStmt*)node;
        scanLoopCarryOperations(state, branch->thenBlock);
        scanLoopCarryOperations(state, branch->elseBlock);
        break;
    }
    case AST_FOR: {
        ASTFor* loop = (ASTFor*)node;
        scanLoopCarryOperations(state, loop->init);
        scanLoopCarryOperations(state, loop->body);
        scanLoopCarryOperations(state, loop->increment);
        break;
    }
    case AST_WHILE:
        scanLoopCarryOperations(state, ((ASTWhile*)node)->body);
        break;
    case AST_FUNC_CALL: {
        ASTFuncCall* call = (ASTFuncCall*)node;
        for (int i = 0; i < array_size(&call->arguments); ++i)
            scanLoopCarryOperations(state, *(ASTNode**)array_get(&call->arguments, i));
        break;
    }
    case AST_MEMBER_ACCESS:
        scanLoopCarryOperations(state, ((ASTMemberAccess*)node)->classNode);
        break;
    case AST_ARRAY_ACCESS: {
        ASTArrayAccess* access = (ASTArrayAccess*)node;
        scanLoopCarryOperations(state, access->array);
        scanLoopCarryOperations(state, access->index);
        break;
    }
    case AST_MULTI_VAR_DECL:
        scanLoopCarryOperations(state, ((ASTMultiVarDecl*)node)->init);
        break;
    case AST_SWITCH: {
        ASTSwitch* selection = (ASTSwitch*)node;
        for (int i = 0; i < array_size(&selection->cases); ++i)
            scanLoopCarryOperations(state, (*(ASTCase**)array_get(&selection->cases, i))->body);
        scanLoopCarryOperations(state, selection->defaultCase);
        break;
    }
    case AST_RETURN:
    case AST_CONST:
    case AST_VAR:
    case AST_NEW:
    case AST_BREAK:
    case AST_CONTINUE:
    case AST_CASE:
    case AST_CLASS_DEF:
    case AST_FUNC_DEF:
        break;
    }
}

static bool borrowedFieldMayOutliveSource(State* state, ASTNode* receiver, ASTNode* source)
{
    BorrowOrigin origin = borrowedSourceOrigin(state, source);
    if (origin.rootRefIndex == -1)
        return getExpressionResultType(state, source).borrowed;
    int sourceRefIndex = borrowedSourceRefIndex(state, source);
    if (sourceRefIndex == -1) return false;
    if (sourceRefIndex == BORROW_UNKNOWN_REF_INDEX) return true;
    if (!receiver || receiver->type != AST_VAR) return true;

    Symbol* object = lookupSymbolLocal(state, ((ASTVar*)receiver)->name);
    if (!object || !object->ownsValue || !isRefType(object->type)) return true;

    Symbol* symbols = (Symbol*)state->compiler->symbols.data;
    int objectIndex = (int)(object - symbols);
    Symbol* sourceOwner = NULL;
    for (int i = 0; i < state->compiler->symbols.count; ++i) {
        Symbol* candidate = (Symbol*)array_get(&state->compiler->symbols, i);
        if (candidate && candidate->stackIndex == sourceRefIndex &&
            candidate->scopeParent == state->compiler->currentScopeParent &&
            (candidate->ownsValue || candidate->kind == SYM_PARAMETER) &&
            isRefType(candidate->type)) {
            sourceOwner = candidate;
            break;
        }
    }
    if (!sourceOwner) return true;
    // A parameter may alias storage owned by the caller. No object stored in
    // this function is proven to die before that caller-owned object escapes.
    if (sourceOwner->kind == SYM_PARAMETER) return true;
    int sourceIndex = (int)(sourceOwner - symbols);
    return objectIndex < sourceIndex;
}

static bool expressionIsBorrowedView(State* state, ASTNode* expression)
{
    TypeID type = getExpressionResultType(state, expression);
    return type.borrowed != 0;
}

static bool packedField(Symbol* classSym, int offset, TypeIDEnum type) {
    return classSym && classSym->as.classDef.packedLayout &&
        offset >= 0 && offset <= 255 &&
        (type == T_BOOL || type == T_CHAR || type == T_SHORT || type == T_INT || type == T_FLOAT);
}

static OPCode getPackedLoadOp(TypeIDEnum type) {
    if (type == T_BOOL || type == T_CHAR) return OP_LOAD_PACKED_I8;
    if (type == T_SHORT) return OP_LOAD_PACKED_I16;
    if (type == T_FLOAT) return OP_LOAD_PACKED_F32;
    return OP_LOAD_PACKED_I32;
}

static OPCode getPackedStoreOp(TypeIDEnum type) {
    if (type == T_BOOL || type == T_CHAR) return OP_STORE_PACKED_I8;
    if (type == T_SHORT) return OP_STORE_PACKED_I16;
    if (type == T_FLOAT) return OP_STORE_PACKED_F32;
    return OP_STORE_PACKED_I32;
}

void genericEmitCall(State* s, Symbol* funcSym, ASTNode* thisNode, int useThisReg, array* arguments, int destReg, Loc loc)
{
    if (!thisNode && useThisReg == -1 && emitMathIntrinsic(s, funcSym, arguments, destReg, loc)) return;
    int nargs = array_size(arguments);
    ASTNode** argsData = (ASTNode**)arguments->data;

    // 1. Argument Count Check
    if (funcSym->kind == SYM_FUNCTION && nargs != funcSym->as.funcDef.paramCount) {
        reportError(s->compiler, loc, "Function '%s' expects %d arguments, but got %d.",
            funcSym->name, funcSym->as.funcDef.paramCount, nargs);
    }

    // 2. Prepare 'this' Register
    int thisReg = -1;
    if (thisNode) {
        thisReg = allocReg(s);
        generateExpression(s, thisNode, thisReg);
    }
    else if (useThisReg != -1) {
        thisReg = useThisReg;
    }

    // 3. Evaluate Arguments (to Registers)
    // We allocate a contiguous block of registers for arguments
    int baseArgReg = s->compiler->nextRegisterIndex;
    // Optimization: Manually reserve count instead of looping allocReg to reduce overhead
    s->compiler->nextRegisterIndex += nargs;
    if (s->compiler->nextRegisterIndex > MAX_REGISTERS) reportError(s->compiler, loc, "Register overflow");

    bool simpleArguments = true;
    for (int i = 0; i < nargs; ++i) {
        if (argsData[i]->type != AST_CONST && argsData[i]->type != AST_VAR)
            simpleArguments = false;
    }
    // Leaf matching is read-only, but emission must follow argument evaluation.
    // Evaluate all script-function arguments here; native simple arguments can
    // still be emitted beside their outgoing stores below.
    if (funcSym->kind == SYM_FUNCTION) simpleArguments = false;
    for (int i32 = 0; !simpleArguments && i32 < nargs; ++i32) {
        // Use the helper to Gen Code + Type Check + Cast
        evaluateAndCheckArg(s, argsData[i32], baseArgReg + i32, funcSym, i32);
    }

    // Typed fast natives consume the evaluated register block directly.  Do
    // this before allocating/pushing the legacy slot-based argument area.
    if (funcSym->kind == SYM_CFUNCTION && !thisNode && useThisReg == -1) {
        int nativeID = funcSym->as.cFunction.cfuncIndex;
        const NativeSignature* nativeSignature = NULL;
        if (nativeID >= 0 && nativeID < s->cFunctionSignatures.count)
            nativeSignature = *(const NativeSignature**)array_get(&s->cFunctionSignatures, nativeID);
        if (nativeSignature && nativeSignature->fast_function &&
            nativeSignature->param_count == (uint16_t)nargs &&
            baseArgReg + nargs <= MAX_REGISTERS) {
            for (int i32 = 0; i32 < nargs; ++i32)
                if (simpleArguments)
                    evaluateAndCheckArg(s, argsData[i32], baseArgReg + i32, funcSym, i32);
            emitABC_IR(s, OP_CALL_C_TYPED, baseArgReg, nativeID, nargs);
            freeRegisters(s, nargs);
            return;
        }
    }

    if (!thisNode && useThisReg == -1 && emitLeafArithmeticCall(s, funcSym, baseArgReg, nargs, destReg)) {
        freeRegisters(s, nargs);
        return;
    }

    // 4. Stack Push Logic
    int startRef = s->compiler->nextRefIndex;
    int startPrim = s->compiler->nextPrimIndex;
    int curRef = startRef;
    int curPrim = startPrim;
    // Push 'this' (Ref 0)
    if (thisNode || useThisReg != -1) {
        emitSetLocal(s, T_PTR, thisReg, (short)curRef++);
    }

    // Push Args
    for (int i32 = 0; i32 < nargs; ++i32) {
        // With no nested calls or side effects, each argument can be evaluated
        // beside its store. This exposes load/store fusion without changing
        // the left-to-right evaluation order of general expressions.
        if (simpleArguments)
            evaluateAndCheckArg(s, argsData[i32], baseArgReg + i32, funcSym, i32);
        TypeID type = getExpressionResultType(s, argsData[i32]);

        // If we casted, we should strictly use the expected type for stack placement
        if (funcSym->kind == SYM_FUNCTION && i32 < funcSym->as.funcDef.paramCount) {
            type = funcSym->as.funcDef.paramTypes[i32];
        }

        if (isRefType(type)) {
            emitSetLocal(s, type.baseType, baseArgReg + i32, (short)curRef++);
        }
        else {
            emitSetLocal(s, type.baseType, baseArgReg + i32, (short)curPrim++);
        }
    }

    // 5. Emit Opcode
    int funcID = 0;
    if (funcSym->kind == SYM_CFUNCTION) {
        funcID = funcSym->as.cFunction.cfuncIndex;

        const NativeSignature* nativeSignature = NULL;
        if (funcID >= 0 && funcID < s->cFunctionSignatures.count)
            nativeSignature = *(const NativeSignature**)array_get(&s->cFunctionSignatures, funcID);
        if (funcID <= 255) 
        {
            // Standard 8-bit Native Call Format: A=Prim, B=ID, C=Ref
            emitABC_IR(s, OP_CALL_C, startPrim, funcID, startRef);
        } 
        else 
        {
            // Extended 32-bit Native Call
            // Format: Word1[OP, A=Prim, B=Ref], Word2[ID]
            IREntry ir = { 0 };
            ir.type = IR_INSTRUCTION;
            ir.op = OP_CALL_C32;
            ir.a = startPrim;
            ir.b = startRef; // We pack RefOffset into B!
            ir.c = funcID;   // We store the large ID here to assemble later
            ir.is_AD = false;

            emitNode(s, ir);
        }
    }
    else {
        funcID = funcSym->as.funcDef.start_address; // This is the ID/Label
        if (funcID <= 255) {
            emitABC_IR(s, OP_CALL, startPrim, funcSym->as.funcDef.start_address, startRef);
        }
        else {
           // Extended 32-bit call
           // Format: [OP_CALL32] [A:PrimOff] [RefOff:B] [Unused] ... [NEXT: FuncID]
           // 1. Emit the Opcode. We pack Prim into A, and Ref into B (shift registers)
           // We don't use C here.
            IREntry ir = { 0 };
            ir.type = IR_INSTRUCTION;
            ir.op = OP_CALL32;
            ir.a = startPrim;
            ir.b = startRef;
            ir.c = 0;

            // 2. Mark this instruction as having an "Immediate" 32-bit follower
            // You might need to adjust your 'emitNode' or 'assemble' logic if 
            // you don't support custom payloads in IR.

            // SIMPLER APPROACH FOR YOUR ASSEMBLER:
            // Just treat 'c' as the 32-bit value in IR, and split it in 'assemble'.
            ir.c = funcID; // Store full ID in C for now
            ir.is_AD = false; // Custom handling

            emitNode(s, ir);
        }
    }

    // 6. Cleanup & Return
    freeRegisters(s, nargs + (thisNode ? 1 : 0));

    if (destReg != 0) {
        emitAD_IR(s, OP_MOVE, destReg, 0);
    }
}

// Handle Method Calls (obj.method(...))
static void generateMethodCall(State* s, ASTFuncCall* c, ASTMemberAccess* m, int dest) {
    // 1. Determine the Type of the Object (Left of dot)
    // This works for 'player', 'tex[0]', 'getObj()', etc.
    TypeID objType = getExpressionResultType(s, m->classNode);

    if (objType.baseType != T_CLASS && objType.baseType != T_STRING) { // Strings have methods too?
        reportError(s->compiler, c->node.loc, "Attempting to call method '%s' on non-object type.", m->memberName);
        return;
    }

    // 2. Find the Method Symbol
    // We use the Type Name (e.g., "Texture") to find the method
    Symbol* funcSym = findMethodSymbol(s, objType.name, m->memberName);
    if (!funcSym) {
        reportError(s->compiler, c->node.loc, "Method '%s' not found in class '%s'.", m->memberName, objType.name);
        return;
    }

    bool isStaticCall = false;

    // Check if the left-hand side is a Variable Name (e.g. "IO" or "player")
    if (m->classNode->type == AST_VAR) {
        ASTVar* v = (ASTVar*)m->classNode;

        // Look up the symbol to see WHAT it is
        Symbol* sym = lookupSymbolLocal(s, v->name);
        if (!sym) sym = lookupSymbolGlobal(s, v->name);

        // If the symbol is the CLASS DEFINITION itself, it's a Static Call.
        if (sym && sym->kind == SYM_TYPE_DEFINITION) {
            isStaticCall = true;
        }
    }

    int thisReg = -1;
    if (isStaticCall) {
        // STATIC: Do NOT allocate a register, do NOT generate code to load 'IO'.
        // Pass -1 so 'genericEmitCall' skips pushing 'this'.
        thisReg = -1;
    }
    else { // INSTANCE: "player.update()" -> We must load the 'player' object.
        thisReg = allocReg(s);
        generateExpression(s, m->classNode, thisReg);
    }

    genericEmitCall(s, funcSym, NULL, thisReg, &c->arguments, dest, c->node.loc);

    if (thisReg != -1)
        freeRegisters(s, 1); // Free thisReg
}

void generateBinaryExp(State* s, ASTNode* n, int dest)
{
    ASTBinaryOp* b = (ASTBinaryOp*)n;

    // ---------------------------------------------------------
    // OPTIMIZATION: Identity Arithmetic (x + 0, x - 0)
    // ---------------------------------------------------------
    if ((b->op == TK_PLUS || b->op == TK_MINUS) && b->right->type == AST_CONST) {
        ASTConst* c = (ASTConst*)b->right;
        bool isZero = (c->value.type.baseType == T_INT && c->value.v.i32 == 0) ||
            (c->value.type.baseType == T_FLOAT && c->value.v.f32 == 0.0f);

        if (isZero) {
            // "x + 0" is just "x". Generate code directly to dest.
            generateExpression(s, b->left, dest);
            return;
        }
    }

    // ---------------------------------------------------------
    // Short-Circuit Logic (AND / OR)
    // ---------------------------------------------------------
    if (b->op == TK_AND || b->op == TK_OR) {
        generateExpression(s, b->left, dest);

        // AND: Jump if False. OR: Jump if True.
        int jmp = emitJump_IR(s, b->op == TK_AND ? OP_JUMP_IF_FALSE : OP_JUMP_IF_TRUE, dest);

        generateExpression(s, b->right, dest);
        emitLabel_IR(s, jmp);
        return;
    }

    // ---------------------------------------------------------
    // Standard Math / Comparison
    // ---------------------------------------------------------
    int registersToReserve = 2;
    bool burnR0 = false;

    // If the next register is 0, we must burn it so 'l' gets 1.
    if (s->compiler->nextRegisterIndex == 0) {
        burnR0 = true;
        registersToReserve++; // Need 3 slots (0, 1, 2)
    }
    REG_GUARD(s, registersToReserve) {
        if (burnR0) {
            allocReg(s); // Allocate R0 but ignore it.
        }
        int l = allocReg(s);
        int r = allocReg(s);

        generateExpression(s, b->left, l);
        generateExpression(s, b->right, r);

        TypeID leftType = getExpressionResultType(s, b->left);
        TypeID rightType = getExpressionResultType(s, b->right);

        // String Concatenation
        if (b->op == TK_PLUS && (leftType.baseType == T_STRING || rightType.baseType == T_STRING)) {
            emitABC_IR(s, OP_CONCAT, dest, l, r);
        }
        else {
            // Automatic Float Promotion
            bool isFloat = (leftType.baseType == T_FLOAT || rightType.baseType == T_FLOAT);
            if (isFloat) {
                if (leftType.baseType == T_INT)  emitAD_IR(s, OP_CAST_I2F, l, (short)l);
                if (rightType.baseType == T_INT) emitAD_IR(s, OP_CAST_I2F, r, (short)r);
            }

            OPCode op = getBinOp(b->op, isFloat);
            emitABC_IR(s, op, dest, (short)l, (short)r);
        }
    }
}

void generateIncDec(State* state, ASTUnaryOp* unOp, int destRegister) {
    ASTNode* operand = unOp->operand;
    bool isInc = (unOp->op == TK_INC);

    // 1. Optimized Local/Global Variable Path
    if (operand->type == AST_VAR) {
        ASTVar* var = (ASTVar*)operand;
        Symbol* sym = lookupSymbolLocal(state, var->name);

        if (sym && sym->type.baseType == T_INT) {
            emitAD_IR(state, OP_ADDi_LOCAL_IMM, (uint8_t)(isInc ? 1 : -1), (short)sym->stackIndex);
            return;
        }

        sym = lookupSymbolGlobal(state, var->name);
        if (sym && sym->type.baseType == T_INT) {
            int oneReg = loadConstant(state, (Value) { .type = { T_INT }, .v.i32 = 1 });
            emitAD_IR(state, isInc ? OP_INCi_GLOBAL : OP_DECi_GLOBAL, oneReg, (short)sym->stackIndex);
            freeRegisters(state, 1);
            return;
        }
    }

    // 2. Fallback: Load -> Math -> Store
    int reg = (destRegister == 0) ? allocReg(state) : destRegister;
    generateExpression(state, operand, reg);

    int oneReg = loadConstant(state, (Value) { .type = { T_INT }, .v.i32 = 1 });
    emitABC_IR(state, isInc ? OP_ADD : OP_SUB, reg, (short)reg, (short)oneReg);

    // Assign back
    if (operand->type == AST_VAR) {
        setVariable(state, (ASTVar*)operand, reg, (TypeID) { T_INT });
    }
    // Add support for obj.prop++ here if needed (AST_MEMBER_ACCESS)

    freeRegisters(state, 1); // Free oneReg
    if (destRegister == 0) freeRegisters(state, 1);
}

// Helper to handle AST_MULTI_VAR_DECL
static void generateVarDeclaration(State* s, ASTMultiVarDecl* decl)
{
    VariableInfo* vars = (VariableInfo*)decl->names.data;
    int count = array_size(&decl->names);

    // 1. Calculate the current index of the class.
    //    We do this because 'currentClass' pointer will become invalid if 'addSymbol' resizes the array.
    int currentClassIdx = -1;
    if (s->compiler->currentClass) {
        currentClassIdx = (int)((Symbol*)s->compiler->currentClass - (Symbol*)s->compiler->symbols.data);
    }

    // -------------------------------------------------------------------------
    // OPTIMIZATION: Constant Initialization (Zero-Register Path)
    // -------------------------------------------------------------------------
    if (decl->init && decl->init->type == AST_CONST) {
        ASTConst* constNode = (ASTConst*)decl->init;
        Value val = constNode->value;
        bool canOptimize = true;

        if (decl->varType.baseType == T_AUTO) {
            decl->varType = constNode->value.type;
        }

        if (decl->varType.baseType != val.type.baseType) {
            if (decl->varType.baseType == T_FLOAT && val.type.baseType == T_INT) {
                val.v.f32 = (float)val.v.i32;
                val.type.baseType = T_FLOAT;
            }
            else {
                canOptimize = false;
            }
        }
        if (canOptimize) {
            int constIdx = addConstant(s, val);
            if (constIdx >= 0 && constIdx < 256) {
                for (int i = 0; i < count; ++i) {
                    Symbol* sym = addSymbol(s, vars[i].name, decl->varType, 0, 1);

                    // --- FIX: Restore currentClass pointer ---
                    // This is critical because we are in a loop and might return immediately after.
                    if (currentClassIdx != -1) {
                        s->compiler->currentClass = (Symbol*)array_get(&s->compiler->symbols, currentClassIdx);
                    }
                    // -----------------------------------------

                    if (sym) {
                        if (sym->scopeLevel > 0) {
                            emitAD_IR(s, OP_STORE32_LOCAL_K, constIdx, (short)sym->stackIndex);
                        }
                        else {
                            emitAD_IR(s, OP_STORE32_GLOBAL_K, constIdx, (short)sym->stackIndex);
                        }
                    }
                }
                return; // Optimization Successful
            }
        }
    }

    // -------------------------------------------------------------------------
    // STANDARD PATH: Register Allocation
    // -------------------------------------------------------------------------
    int srcReg = allocReg(s);
    TypeID srcType = { T_INT, NULL };

    if (decl->init) {
        generateExpression(s, decl->init, srcReg);
        srcType = getExpressionResultType(s, decl->init);
    }
    else {
        if (isRefType(decl->varType)) {
            emitAD_IR(s, OP_LOAD_NULL, srcReg, 0);
            srcType = (TypeID){ T_PTR, NULL };
        }
        else {
            emitConstant_IR(s, (Value) { .type = { T_INT }, .v.i32 = 0 }, srcReg);
            srcType = (TypeID){ T_INT, NULL };
        }
    }
    
    // Resolve AUTO type for non-constant initializers
    if (decl->varType.baseType == T_AUTO) {
        decl->varType = srcType;
    }
    if (decl->init) {
        reportIncompatibleValue(s, decl->node.loc, "Initializer type mismatch",
            decl->varType, srcType);
    }

    for (int i = 0; i < count; ++i) {
        Symbol* sym = addSymbol(s, vars[i].name, decl->varType, 0, 1);

        // --- FIX: Restore currentClass pointer ---
        if (currentClassIdx != -1) {
            s->compiler->currentClass = (Symbol*)array_get(&s->compiler->symbols, currentClassIdx);
        }
        // -----------------------------------------

        if (sym) {
            if (!decl->init && isRefType(sym->type) && sym->type.baseType != T_STRING) {
                sym->valueState = VALUE_UNINITIALIZED;
            }
            if (sym->type.borrowed && expressionTransfersOwnership(s, decl->init)) {
                reportError(s->compiler, decl->node.loc, "Borrowed variable '%s' cannot take ownership; assign an existing owner instead", sym->name);
            }
            if (sym->type.borrowed) {
                BorrowOrigin origin = borrowedSourceOrigin(s, decl->init);
                if (isRefType(srcType) && srcType.borrowed &&
                    origin.rootRefIndex == -1) {
                    reportError(s->compiler, decl->node.loc,
                        "Cannot store a borrowed result without a tracked owner; use a named owner or borrowed view");
                }
                sym->borrowedFromRefIndex = origin.rootRefIndex;
                sym->borrowedFromPath = origin.path;
                sym->borrowedFromDepth = origin.depth;
                memcpy(sym->borrowedPathComponents, origin.components,
                    sizeof(sym->borrowedPathComponents));
            }
            if (sym->ownsValue && isRefType(sym->type) && sym->type.baseType != T_STRING &&
                (!expressionTransfersOwnership(s, decl->init) || i > 0)) {
                emitABC_IR(s, OP_CLONE_REF, srcReg, srcReg, 0);
            }
            if (sym->scopeLevel > 0) {
                emitCastIfNeeded(s, sym->type, srcType, (short)srcReg);
                emitSetLocal(s, sym->type.baseType, (short)srcReg, (short)sym->stackIndex);
                if (sym->ownsValue && isRefType(sym->type))
                    emitAD_IR(s, OP_MARK_OWNED_LOCAL, 0, (short)sym->stackIndex);
            }
            else {
                emitSetGlobal(s, sym->type.baseType, (short)srcReg, (short)sym->stackIndex);
            }
        }
    }

    if (expressionMayCreateUnclaimed(s, decl->init))
        emitAD_IR(s, OP_DESTROY_UNCLAIMED, 0, 0);
    freeRegisters(s, 1);
}

// Helper to Evaluate Argument, Check Type, and Emit Cast
static void evaluateAndCheckArg(State* s, ASTNode* argNode, int destReg, Symbol* funcSym, int argIndex) {
    // 1. Generate Code
    generateExpression(s, argNode, destReg);

    // 2. Type Checking (if function signature exists)
    if (funcSym && funcSym->kind == SYM_FUNCTION && funcSym->as.funcDef.paramTypes) {
        if (argIndex < funcSym->as.funcDef.paramCount) {
            TypeID expected = funcSym->as.funcDef.paramTypes[argIndex];
            TypeID actual = getExpressionResultType(s, argNode);

            if (!assignmentTypeCompatible(expected, actual)) {
                reportError(s->compiler, argNode->loc,
                    "Argument %d type mismatch in '%s'. Expected %s, got %s.",
                    argIndex + 1, funcSym->name,
                    typeEnumToString(&expected), typeEnumToString(&actual));
            }
            // Emit Cast Instruction
            emitCastIfNeeded(s, expected, actual, (short)destReg);
        }
    }
}

// =================================================================
// generateExpression – now clean and complete
// =================================================================

void generateExpression(State* s, ASTNode* n, int dest) {
    if (n) s->current_loc = n->loc;
    if (dest < 0 || dest >= MAX_REGISTERS) {
        reportError(s->compiler, LOC_NONE, "Register overflow");
        return;
    }
    if (s->register_count < dest + 1) s->register_count = (uint16_t)(dest + 1);
    if (!n) return;

    switch (n->type) {
    case AST_CONST: {
        ASTConst* c = (ASTConst*)n;
        if (c->value.type.baseType == T_NIL) {
            emitAD_IR(s, OP_LOAD_NULL, dest, 0);
        }
        else {
            emitConstant_IR(s, c->value, dest);
        }
        break;
    }
    case AST_NEW:generateNew(s, n, dest); break;
    case AST_VAR:getVariable(s, ((ASTVar*)n)->name, n->loc, dest); break;
    case AST_BINARY_OP:generateBinaryExp(s, n, dest); break;
    case AST_UNARY_OP: {
        ASTUnaryOp* u = (ASTUnaryOp*)n;
        if (u->op == TK_MOVE) {
            if (u->operand->type != AST_VAR) {
                reportError(s->compiler, u->node.loc, "move requires a local variable");
                break;
            }
            ASTVar* source = (ASTVar*)u->operand;
            Symbol* symbol = lookupSymbolLocal(s, source->name);
            if (!symbol || !isRefType(symbol->type) || !symbol->ownsValue) {
                reportError(s->compiler, u->node.loc, "move requires an owned local reference");
                break;
            }
            if (symbol->valueState != VALUE_INITIALIZED) {
                const char* reason = symbol->valueState == VALUE_MOVED ? "moved" :
                    symbol->valueState == VALUE_UNINITIALIZED ? "uninitialized" :
                    "not initialized on every control-flow path";
                reportError(s->compiler, u->node.loc, "Cannot move %s value: %s", reason, source->name);
                emitAD_IR(s, OP_LOAD_NULL, dest, 0);
                break;
            }
            if (hasLiveBorrowOfRefIndex(s, symbol->stackIndex, 0)) {
                reportError(s->compiler, u->node.loc, "Cannot move '%s' while a borrowed local still aliases it", source->name);
                emitAD_IR(s, OP_LOAD_NULL, dest, 0);
                break;
            }
            emitGetLocal(s, symbol->type.baseType, dest, (short)symbol->stackIndex);
            emitAD_IR(s, OP_CLEARp_LOCAL, 0, (short)symbol->stackIndex);
            symbol->valueState = VALUE_MOVED;
        }
        else if (u->op == TK_INC || u->op == TK_DEC) {
            generateIncDec(s, u, dest);
        }
        else if (u->op == TK_MINUS) {
            // Optimization: Constant folding for negative numbers
            if (u->operand->type == AST_CONST) {
                ASTConst* c = (ASTConst*)u->operand;
                Value folded = c->value;
                if (folded.type.baseType == T_INT) {
                    folded.v.i32 = -folded.v.i32;
                    emitConstant_IR(s, folded, dest);
                    return;
                }
                else if (folded.type.baseType == T_FLOAT) {
                    folded.v.f32 = -folded.v.f32;
                    emitConstant_IR(s, folded, dest);
                    return;
                }
            }
            generateExpression(s, u->operand, dest);
            TypeID t = getExpressionResultType(s, u->operand);

            if (t.baseType == T_FLOAT) {
                emitAD_IR(s, OP_NEGf, dest, (short)dest); // Use the new Float Opcode
            }
            else {
                emitAD_IR(s, OP_NEG, dest, (short)dest);       // Use existing Int Opcode
            }
        }
        else if (u->op == TK_NOT) {
            generateExpression(s, u->operand, dest);
            emitAD_IR(s, OP_NOT, dest, (short)dest);
        }
        else {
            generateExpression(s, u->operand, dest);
        }
        break;
    }
    case AST_ARRAY_ACCESS: {
        ASTArrayAccess* arrNode = (ASTArrayAccess*)n;

        if (arrNode->array->type == AST_VAR && arrNode->index->type == AST_VAR) {
            Symbol* arrayLocal = lookupSymbolLocal(s, ((ASTVar*)arrNode->array)->name);
            Symbol* indexLocal = lookupSymbolLocal(s, ((ASTVar*)arrNode->index)->name);
            if (arrayLocal && indexLocal && indexLocal->type.baseType == T_INT &&
                (arrayLocal->type.baseType == T_PTR || arrayLocal->type.baseType == T_SHARED_PTR) &&
                (arrayLocal->type.elementType == T_INT || arrayLocal->type.elementType == T_FLOAT) &&
                arrayLocal->stackIndex >= 0 && arrayLocal->stackIndex <= 255 &&
                indexLocal->stackIndex >= 0 && indexLocal->stackIndex <= 255) {
                emitABC_IR(s, arrayLocal->type.elementType == T_INT ? OP_ALOAD_I_LOCALS : OP_ALOAD_F_LOCALS,
                    dest, arrayLocal->stackIndex, indexLocal->stackIndex);
                break;
            }
        }

        int arrReg = allocReg(s);
        generateExpression(s, arrNode->array, arrReg);

        int idxReg = allocReg(s);
        generateExpression(s, arrNode->index, idxReg);

        // --- TYPE DETECTION ---
        TypeID arrayType = getExpressionResultType(s, arrNode->array);
        // Note: You need to know if the ARRAY elements are References.
        // If your type system flags T_CLASS or T_PTR arrays correctly:

        OPCode op = OP_ALOAD_I; // Default to Int

        // Logic to determine if elements are Pointers
        // If 'arrayType' is T_PTR/T_SHARED_PTR, we assume it holds objects/pointers.
        // Adjust this check based on how your TypeID structs work for arrays.
        TypeIDEnum elementType = arrayType.elementType;
        if (elementType == T_CLASS || elementType == T_STRING || elementType == T_PTR || elementType == T_SHARED_PTR) {
            // If we are accessing an array of Objects, use P-Load
            op = OP_ALOAD_P;
        }
        else if (elementType == T_FLOAT) {
            op = OP_ALOAD_F32;
        }
        else if (elementType == T_CHAR) {
            op = OP_ALOAD_I8;
        }
        else if (elementType == T_SHORT) {
            op = OP_ALOAD_I16;
        }

        emitABC_IR(s, op, dest, arrReg, idxReg);

        freeRegisters(s, 2);
        break;
    }
    case AST_MEMBER_ACCESS: {
        ASTMemberAccess* m = (ASTMemberAccess*)n;
        if (m->classNode->type == AST_VAR) {
            char qualified[256];
            snprintf(qualified, sizeof(qualified), "%s.%s",
                ((ASTVar*)m->classNode)->name, m->memberName);
            Symbol* constant = lookupSymbolGlobal(s, qualified);
            if (constant && constant->kind == SYM_CONSTANT) {
                emitConstant_IR(s, constant->as.constValue, dest);
                break;
            }
        }
        int obj = allocReg(s);
        generateExpression(s, m->classNode, obj);

        TypeID t = getExpressionResultType(s, m->classNode);
        Symbol* classDef = findClassSymbol(s, t.name);

        if (classDef) {
            int off = findMemberOffset(classDef, m->memberName);
            if (off < 0) {
                reportError(s->compiler, m->node.loc, "Member '%s' not found in class '%s'.", m->memberName, t.name ? t.name : "<unknown>");
                break;
            }
            TypeID memberType = findMemberType(classDef, m->memberName);
            OPCode op = packedField(classDef, off, memberType.baseType)
                ? getPackedLoadOp(memberType.baseType) : getGetFieldOp(memberType.baseType);
            emitABC_IR(s, op, dest, obj, off);
        }
        else {
            reportError(s->compiler, m->node.loc, "Unknown class or member.");
        }
        freeRegisters(s, 1);
        break;
    }
    case AST_FUNC_CALL: {
        ASTFuncCall* c = (ASTFuncCall*)n;
        // Check if it's a Method Call (obj.Function())
        if (c->callee->type == AST_MEMBER_ACCESS) {
            generateMethodCall(s, c, (ASTMemberAccess*)c->callee, dest);
        }
        else { // Standard Function Call (Function())
            ASTVar* objectName = (ASTVar*)c->callee;
            //if not name it might be a constructor
            if (objectName->node.type == AST_NEW) {
                ASTNewNode* newNode = (ASTNewNode*)c->callee;
                generateNew(s, c->callee, dest); // Allocate object first
                if (newNode->newType == NEW_OBJECT) {
                    Symbol* methodSym = findMethodSymbol(s, newNode->typeToCreate.name, newNode->typeToCreate.name);

                    if (!methodSym) {
                        reportError(s->compiler, c->node.loc, "Constructor for class '%s' not found.", newNode->typeToCreate.name);
                        return;
                    }
                    else {
                        genericEmitCall(s, methodSym, NULL, dest, &c->arguments, 0, c->node.loc);
                        return;
                    }
                }
            }

            Symbol* sym = lookupSymbolRecursive(s, objectName->name);
            // If symbol not found, and we are inside a class, check class methods.
            if (!sym && s->compiler->currentClass) {
                // Construct the mangled name or look it up in the class scope
                // Assuming findMethodSymbol takes (state, className, methodName)
                Symbol* methodSym = findMethodSymbol(s, ((Symbol*)s->compiler->currentClass)->name, objectName->name);

                if (methodSym) {
                    // Found it! It's a method of this class.
                    // We need to load 'this' and call it.

                    Symbol* thisSym = lookupSymbolLocal(s, "this");
                    if (thisSym) {
                        int thisReg = allocReg(s);
                        // Load 'this' pointer into a register
                        emitGetLocal(s, thisSym->type.baseType, thisReg, (short)thisSym->stackIndex);

                        // Emit call passing 'thisReg' as the instance
                        genericEmitCall(s, methodSym, NULL, thisReg, &c->arguments, dest, c->node.loc);

                        freeRegisters(s, 1); // Free thisReg
                        return;
                    }
                }
            }
            if (!sym) {
                reportError(s->compiler, ((ASTVar*)c->callee)->node.loc, "Undefined symbol function '%s'", objectName->name);
                return;
            }

            genericEmitCall(s, sym, NULL, -1, &c->arguments, dest, c->node.loc);
        }
        break;
    }

    default:
        reportError(s->compiler, n->loc, "Unsupported expression");
    }
}

// Helper to process parameters safely handling array resizing
static void processFunctionParams(State* s, int funcSymbolIndex, ASTBlock* paramBlock, int classSymbolIndex) {
    // 1. Initial Fetch of Function Symbol
    Symbol* funcSym = (Symbol*)array_get(&s->compiler->symbols, funcSymbolIndex);

    if (!paramBlock) {
        funcSym->as.funcDef.paramCount = 0;
        funcSym->as.funcDef.paramTypes = NULL;
        return;
    }

    // 2. Count Parameters
    int totalParams = 0;
    int count = array_size(&paramBlock->list);
    for (int i32 = 0; i32 < count; ++i32) {
        ASTNode* stmt = *(ASTNode**)array_get(&paramBlock->list, i32);
        if (stmt->type == AST_MULTI_VAR_DECL) {
            totalParams += array_size(&((ASTMultiVarDecl*)stmt)->names);
        }
    }

    // 3. Allocate Type Array
    TypeID* params = (TypeID*)malloc(totalParams * sizeof(TypeID));
	if (params == NULL) {
		reportError(s->compiler, LOC_NONE, "Memory allocation failed for function parameters.");
		return;
	}
    funcSym->as.funcDef.paramCount = totalParams;
    funcSym->as.funcDef.paramTypes = params;

    // 4. Register Symbols
    int pIndex = 0;
    for (int i32 = 0; i32 < count; ++i32) {
        ASTNode* stmt = *(ASTNode**)array_get(&paramBlock->list, i32);
        if (stmt->type != AST_MULTI_VAR_DECL) continue;

        ASTMultiVarDecl* decl = (ASTMultiVarDecl*)stmt;
        VariableInfo* vars = (VariableInfo*)decl->names.data;
        int varCount = array_size(&decl->names);

        for (int j = 0; j < varCount; ++j) {
            // Re-fetch dtor every iteration because addSymbol() below might resize the array
            funcSym = (Symbol*)array_get(&s->compiler->symbols, funcSymbolIndex);

            // Now safe to write to paramTypes
            int currentParamIndex = pIndex++;
            funcSym->as.funcDef.paramTypes[currentParamIndex] = decl->varType;

            // Register the parameter as a local variable.
            // This function call is what causes the realloc/move.
            addSymbol(s, vars[j].name, decl->varType, 0, 1);
            Symbol* parameter = lookupSymbolLocal(s, vars[j].name);
            if (parameter) {
                parameter->kind = SYM_PARAMETER;
                parameter->ownsValue = 0;
                funcSym = (Symbol*)array_get(&s->compiler->symbols, funcSymbolIndex);
                if (currentParamIndex == funcSym->as.funcDef.borrowedReturnParamIndex &&
                    isRefType(parameter->type)) {
                    funcSym->as.funcDef.borrowedReturnRefIndex = parameter->stackIndex;
                }
            }

            // --- FIX: Restore 'this' context ---
            if (classSymbolIndex != -1) {
                s->compiler->currentClass = (Symbol*)array_get(&s->compiler->symbols, classSymbolIndex);
            }
            // -----------------------------------
        }
    }
}

static int findBorrowedReturnParamIndex(State* state, ASTFuncDef* function,
    int classSymbol, TypeID* sourceType)
{
    if (!state || !function || !function->borrowedReturnParam) return -1;
    if (strcmp(function->borrowedReturnParam, "this") == 0) {
        if (classSymbol < 0) return -1;
        Symbol* classDef = (Symbol*)array_get(&state->compiler->symbols, classSymbol);
        *sourceType = classDef->type;
        return -2;
    }

    ASTBlock* params = (ASTBlock*)function->params;
    int parameterIndex = 0;
    if (!params) return -1;
    for (int i = 0; i < array_size(&params->list); ++i) {
        ASTNode* node = *(ASTNode**)array_get(&params->list, i);
        if (!node || node->type != AST_MULTI_VAR_DECL) continue;
        ASTMultiVarDecl* declaration = (ASTMultiVarDecl*)node;
        VariableInfo* names = (VariableInfo*)declaration->names.data;
        for (int j = 0; j < array_size(&declaration->names); ++j, ++parameterIndex) {
            if (strcmp(names[j].name, function->borrowedReturnParam) == 0) {
                *sourceType = declaration->varType;
                return parameterIndex;
            }
        }
    }
    return -1;
}

void generateFunction(State* state, ASTFuncDef* funcNode, int classSymbol) {
    int borrowedReturnParamIndex = -1;
    if (funcNode->returnType.borrowed) {
        TypeID sourceType = { .baseType = T_VOID };
        borrowedReturnParamIndex = findBorrowedReturnParamIndex(
            state, funcNode, classSymbol, &sourceType);
        if (borrowedReturnParamIndex == -1) {
            reportError(state->compiler, funcNode->node.loc,
                "Borrowed return must name a reference parameter with borrow(param.path) or borrow(this.path)");
        }
        else if (!isRefType(sourceType) ||
            (!funcNode->borrowedReturnPath &&
             !assignmentTypeCompatible(funcNode->returnType, sourceType))) {
            reportError(state->compiler, funcNode->node.loc,
                "Borrowed return source must be a reference parameter compatible with the return type");
            borrowedReturnParamIndex = -1;
        }
        if (borrowedReturnParamIndex != -1 && funcNode->borrowedReturnPath) {
            BorrowOrigin pathCheck = { 0, 0, 0, { 0 } };
            pathCheck = appendBorrowReturnPath(state, pathCheck,
                funcNode->borrowedReturnPath, funcNode->node.loc);
            if (pathCheck.rootRefIndex < 0) borrowedReturnParamIndex = -1;
        }
    }
    else if (funcNode->borrowedReturnParam) {
        reportError(state->compiler, funcNode->node.loc,
            "A borrow(param.path) return annotation requires a borrowed return type");
    }
    if (funcNode->returnType.baseType != T_VOID &&
        !statementDefinitelyReturns(funcNode->body)) {
        reportError(state->compiler, funcNode->node.loc,
            "Function '%s' does not return a value on every control-flow path",
            funcNode->name);
    }
    addFuncSymbol(state, funcNode->name, SYM_FUNCTION, NULL);

    // Store the INDEX, not the pointer
    int funcSymbolIndex = state->compiler->symbols.count - 1;

    // Update the Symbol's type to match the AST Return Type
    Symbol* funcSym = (Symbol*)array_get(&state->compiler->symbols, funcSymbolIndex);
    funcSym->type = funcNode->returnType;
    funcSym->as.funcDef.borrowedReturnParamIndex = borrowedReturnParamIndex;
    funcSym->as.funcDef.borrowedReturnRefIndex = -1;
    funcSym->as.funcDef.borrowedReturnPath = funcNode->borrowedReturnPath;

    // Setup Jump over the function body
    int jump = emitJump_IR(state, OP_JUMP, 0);

    // Label for function entry
    int functionStartLabel = createLabel(state);
    emitLabel_IR(state, functionStartLabel);

    // Update Start Address (Re-fetch pointer)
    funcSym = (Symbol*)array_get(&state->compiler->symbols, funcSymbolIndex);
    funcSym->as.funcDef.start_address = functionStartLabel;
    funcSym->as.funcDef.entry_ir = state->ir_tail;

    state->compiler->currentScopeLevel++;

    // --- Save Compiler State ---
    int oldReg = state->compiler->nextRegisterIndex;
    int oldPrim = state->compiler->nextPrimIndex;
    int oldRef = state->compiler->nextRefIndex;
    int oldScopeParent = state->compiler->currentScopeParent;
    state->compiler->currentScopeParent = state->compiler->symbols.count - 1;

    // --- Reset for New Stack Frame ---
    state->compiler->nextRegisterIndex = 0;
    state->compiler->nextPrimIndex = 0;
    state->compiler->nextRefIndex = 0;

    // --- Handle 'this' ---
    if (classSymbol != -1) {
        Symbol* classSym = (Symbol*)array_get(&state->compiler->symbols, classSymbol);
        state->compiler->currentClass = classSym;

        // addSymbol might resize the array
        addSymbol(state, "this", classSym->type, 0, 1);
        Symbol* thisParameter = lookupSymbolLocal(state, "this");
        if (thisParameter) {
            thisParameter->kind = SYM_PARAMETER;
            thisParameter->ownsValue = 0;
            funcSym = (Symbol*)array_get(&state->compiler->symbols, funcSymbolIndex);
            if (borrowedReturnParamIndex == -2)
                funcSym->as.funcDef.borrowedReturnRefIndex = thisParameter->stackIndex;
        }

        // REFRESH currentClass immediately
        state->compiler->currentClass = (Symbol*)array_get(&state->compiler->symbols, classSymbol);
    }
    else {
        state->compiler->currentClass = NULL;
    }

    // --- Handle Parameters ---
    // Pass INDEX, not pointer
    processFunctionParams(state, funcSymbolIndex, (ASTBlock*)funcNode->params, classSymbol);

    // --- REFRESH currentClass again before body generation ---
    if (classSymbol != -1) {
        state->compiler->currentClass = (Symbol*)array_get(&state->compiler->symbols, classSymbol);
    }

    // --- Generate Body ---
    generateStatement(funcNode->body, state);

    // --- Implicit Return ---
    if (funcNode->returnType.baseType != T_VOID) {
        emitConstant_IR(state, (Value) { .type = { T_INT }, .v.i32 = 0 }, 0);
    }
    else {
        // Constructors/void methods: preserve 'this' in R0 across return,
        // since callers may rely on R0 surviving the call (e.g. `new X(...)`)
        Symbol* thisSym = lookupSymbolLocal(state, "this");
        if (thisSym) {
            emitGetLocal(state, thisSym->type.baseType, 0, (short)thisSym->stackIndex);
        }
    }

    emitAD_IR(state, OP_RETURN, 0, 0);

    // --- Restore Compiler State ---
    state->compiler->currentClass = NULL;
    state->compiler->currentScopeLevel--;
    state->compiler->nextRegisterIndex = oldReg;
    state->compiler->nextPrimIndex = oldPrim;
    state->compiler->nextRefIndex = oldRef;
    state->compiler->currentScopeParent = oldScopeParent;

    emitLabel_IR(state, jump);
}

void generateAssigment(State* state, ASTBinaryOp* assignNode)
{
    ASTNode* lhs = assignNode->left;
    ASTNode* rhs = assignNode->right;

    // Typed local updates avoid loading and storing the left-hand side.
    // Keep members/globals on the normal name-resolution path.
    if (lhs->type == AST_VAR &&
        (assignNode->op == TK_ADD_ASSIGN || assignNode->op == TK_SUB_ASSIGN)) {
        Symbol* local = lookupSymbolLocal(state, ((ASTVar*)lhs)->name);
        TypeID rightType = getExpressionResultType(state, rhs);
        if (local && ((local->type.baseType == T_INT && rightType.baseType == T_INT) ||
            (local->type.baseType == T_FLOAT &&
             (rightType.baseType == T_INT || rightType.baseType == T_FLOAT)))) {
            const int slot = local->stackIndex;
            const bool floating = local->type.baseType == T_FLOAT;
            const bool subtract = assignNode->op == TK_SUB_ASSIGN;
            if (floating && rhs->type == AST_CONST && rightType.baseType == T_FLOAT) {
                int index = addConstant(state, ((ASTConst*)rhs)->value);
                if (index >= 0 && index <= 255) {
                    emitAD_IR(state, subtract ? OP_SUBf_LOCAL_K : OP_ADDf_LOCAL_K,
                        index, (short)slot);
                    return;
                }
            }
            if (!floating && rhs->type == AST_CONST) {
                int64_t delta = ((ASTConst*)rhs)->value.v.i32;
                if (subtract) delta = -delta;
                if (delta >= -128 && delta <= 127) {
                    emitAD_IR(state, OP_ADDi_LOCAL_IMM, (uint8_t)delta, (short)slot);
                    return;
                }
            }
            int reg = allocReg(state);
            generateExpression(state, rhs, reg);
            if (floating && rightType.baseType == T_INT)
                emitAD_IR(state, OP_CAST_I2F, reg, (short)reg);
            OPCode op = floating ? (subtract ? OP_DECf_LOCAL : OP_INCf_LOCAL)
                                 : (subtract ? OP_DECi_LOCAL : OP_INCi_LOCAL);
            emitAD_IR(state, op, reg, (short)slot);
            freeRegisters(state, 1);
            return;
        }
    }

    // ---------------------------------------------------------
    // Desugar compound assignments: x += y  x = x + y
    // ---------------------------------------------------------
    if (assignNode->op == TK_ADD_ASSIGN || assignNode->op == TK_SUB_ASSIGN ||
        assignNode->op == TK_MULT_ASSIGN || assignNode->op == TK_DIVEQ) {

        TTOKEN mathOp;

        switch (assignNode->op) {
        case TK_ADD_ASSIGN:  mathOp = TK_PLUS;  break;
        case TK_SUB_ASSIGN:  mathOp = TK_MINUS; break;
        case TK_MULT_ASSIGN: mathOp = TK_MULT;  break;
        case TK_DIVEQ:       mathOp = TK_DIV;   break;
        default:             mathOp = TK_PLUS;  break;
        }

        int rhsReg = allocReg(state);
        generateExpression(state, rhs, rhsReg);

        int lhsReg = allocReg(state);
        generateExpression(state, lhs, lhsReg);

        TypeID lhsType = getExpressionResultType(state, lhs);
        TypeID rhsType = getExpressionResultType(state, rhs);
        bool isFloat = (lhsType.baseType == T_FLOAT || rhsType.baseType == T_FLOAT);

        if (isFloat) {
            if (lhsType.baseType == T_INT) emitAD_IR(state, OP_CAST_I2F, lhsReg, (short)lhsReg);
            if (rhsType.baseType == T_INT) emitAD_IR(state, OP_CAST_I2F, rhsReg, (short)rhsReg);
        }

        OPCode op = getBinOp(mathOp, isFloat);
        emitABC_IR(state, op, lhsReg, (short)lhsReg, (short)rhsReg);

        if (lhs->type == AST_VAR) {
            setVariable(state, (ASTVar*)lhs, lhsReg, isFloat ? (TypeID) { T_FLOAT } : lhsType);
        }
        else if (lhs->type == AST_MEMBER_ACCESS) {
            ASTMemberAccess* memberNode = (ASTMemberAccess*)lhs;
            if (expressionIsBorrowedView(state, memberNode->classNode)) {
                reportError(state->compiler, memberNode->node.loc,
                    "Cannot mutate through a borrowed reference; mutate through the owner instead");
            }
            TypeID classType = getExpressionResultType(state, memberNode->classNode);
            Symbol* classSym = findClassSymbol(state, classType.name);
            if (classSym) {
                int offset = findMemberOffset(classSym, memberNode->memberName);
                TypeID memberType = findMemberType(classSym, memberNode->memberName);
                int objReg = allocReg(state);
                generateExpression(state, memberNode->classNode, objReg);
                OPCode storeOp = packedField(classSym, offset, memberType.baseType)
                    ? getPackedStoreOp(memberType.baseType) : getSetFieldOp(memberType.baseType);
                emitABC_IR(state, storeOp, objReg, offset, lhsReg);
                freeRegisters(state, 1);
            }
        }
        else if (lhs->type == AST_ARRAY_ACCESS) {
            ASTArrayAccess* arrNode = (ASTArrayAccess*)lhs;
            TypeID arrType = getExpressionResultType(state, arrNode->array);
            if (arrType.borrowed) {
                reportError(state->compiler, arrNode->node.loc,
                    "Cannot assign through a borrowed array; assign through the owning array instead");
            }
            int arrReg = allocReg(state);
            generateExpression(state, arrNode->array, arrReg);
            int idxReg = allocReg(state);
            generateExpression(state, arrNode->index, idxReg);
            OPCode storeOp = OP_ASTORE_I;
            TypeIDEnum elementType = arrType.elementType;
            if (elementType == T_CLASS || elementType == T_STRING || elementType == T_PTR || elementType == T_SHARED_PTR) storeOp = OP_ASTORE_P;
            else if (elementType == T_FLOAT) storeOp = OP_ASTORE_F32;
            else if (elementType == T_CHAR) storeOp = OP_ASTORE_I8;
            else if (elementType == T_SHORT) storeOp = OP_ASTORE_I16;
            else if (elementType == T_FLOAT) storeOp = OP_ASTORE_F32;
            emitABC_IR(state, storeOp, arrReg, idxReg, lhsReg);
            freeRegisters(state, 2);
        }

        freeRegisters(state, 2);
        return;
    }
    // ---------------------------------------------------------
    // Case 1: Simple Variable Assignment (x = ...)
    // ---------------------------------------------------------
    if (lhs->type == AST_VAR) {
        ASTVar* varNode = (ASTVar*)lhs;

        // x = x + constant: A is an immediate only for this dedicated opcode.
        if (rhs->type == AST_BINARY_OP) {
            ASTBinaryOp* bin = (ASTBinaryOp*)rhs;
            if (bin->op == TK_PLUS && bin->left->type == AST_VAR &&
                bin->right->type == AST_CONST &&
                strcmp(varNode->name, ((ASTVar*)bin->left)->name) == 0) {
                ASTConst* constant = (ASTConst*)bin->right;
                Symbol* local = lookupSymbolLocal(state, varNode->name);
                if (local && local->type.baseType == T_INT &&
                    local->valueState == VALUE_INITIALIZED &&
                    constant->value.type.baseType == T_INT &&
                    constant->value.v.i32 >= -128 && constant->value.v.i32 <= 127) {
                    emitAD_IR(state, OP_ADDi_LOCAL_IMM, (uint8_t)constant->value.v.i32,
                        (short)local->stackIndex);
                    return;
                }
            }
        }

        // --- OPTIMIZATION B: Constant Store (x = 5) ---
        if (rhs->type == AST_CONST) {
            ASTConst* constNode = (ASTConst*)rhs;
            int constIdx = addConstant(state, constNode->value);

            // If constant fits in 8 bits, use STORE_K
            if (constIdx < 256) {
                Symbol* sym = lookupSymbolLocal(state, varNode->name);
                if (sym) {
                    // Check if variable size allows simple 32-bit store
                    // (Assuming you have OP_STORE32_LOCAL_K)
                    if (getTypeSizeEnum(sym->type.baseType) <= 4) {
                        emitAD_IR(state, OP_STORE32_LOCAL_K, constIdx, (short)sym->stackIndex);
                        sym->valueState = VALUE_INITIALIZED;
                        return;
                    }
                }
            }
        }

        // --- Fallback: Standard Variable Assignment ---
        int tempReg = allocReg(state);
        generateExpression(state, rhs, tempReg);
        TypeID assignmentType = getExpressionResultType(state, rhs);
        Symbol* destination = lookupSymbolLocal(state, varNode->name);
        if (!destination) destination = lookupSymbolGlobal(state, varNode->name);
        if (destination && !assignmentTypeCompatible(destination->type, assignmentType)) {
            reportError(state->compiler, varNode->node.loc,
                "Cannot assign %s to variable '%s' of type %s.",
                typeEnumToString(&assignmentType), destination->name,
                typeEnumToString(&destination->type));
        }
        if (destination && destination->type.borrowed && expressionTransfersOwnership(state, rhs)) {
            reportError(state->compiler, varNode->node.loc, "Borrowed variable '%s' cannot take ownership; assign an existing owner instead", destination->name);
        }
        if (destination && destination->type.borrowed) {
            BorrowOrigin origin = borrowedSourceOrigin(state, rhs);
            if (isRefType(assignmentType) && assignmentType.borrowed &&
                origin.rootRefIndex == -1) {
                reportError(state->compiler, varNode->node.loc,
                    "Cannot store a borrowed result without a tracked owner; use a named owner or borrowed view");
            }
            if (destination->scopeLevel == 0 && origin.rootRefIndex != -1) {
                reportError(state->compiler, varNode->node.loc,
                    "Cannot store a borrow rooted in a local or parameter in a global variable");
            }
            destination->borrowedFromRefIndex = origin.rootRefIndex;
            destination->borrowedFromPath = origin.path;
            destination->borrowedFromDepth = origin.depth;
            memcpy(destination->borrowedPathComponents, origin.components,
                sizeof(destination->borrowedPathComponents));
        }
        if (destination && destination->ownsValue &&
            isRefType(assignmentType) && assignmentType.baseType != T_STRING &&
            !expressionTransfersOwnership(state, rhs))
            emitABC_IR(state, OP_CLONE_REF, tempReg, tempReg, 0);
        if (destination && destination->ownsValue && isRefType(destination->type) &&
            hasLiveBorrowOfRefIndex(state, destination->stackIndex, 0)) {
            reportError(state->compiler, varNode->node.loc,
                "Cannot replace '%s' while a borrowed local still aliases it", destination->name);
        }
        if (destination && destination->ownsValue && isRefType(destination->type))
            emitAD_IR(state, OP_DESTROYp_LOCAL, 0, (short)destination->stackIndex);
        setVariable(state, varNode, tempReg, assignmentType);
        freeRegisters(state, 1);
    }
    // ---------------------------------------------------------
    // Case 2: Member Assignment (obj.field = ...)
    // ---------------------------------------------------------
    else if (lhs->type == AST_MEMBER_ACCESS) {
        ASTMemberAccess* memberNode = (ASTMemberAccess*)lhs;
        if (expressionIsBorrowedView(state, memberNode->classNode)) {
            reportError(state->compiler, memberNode->node.loc,
                "Cannot mutate through a borrowed reference; mutate through the owner instead");
        }

        // Resolve Object Type
        TypeID classType = getExpressionResultType(state, memberNode->classNode);
        Symbol* classSym = findClassSymbol(state, classType.name);

        if (!classSym) {
            reportError(state->compiler, memberNode->node.loc, "Unknown class type.");
            return;
        }

        int offset = findMemberOffset(classSym, memberNode->memberName);
        if (offset < 0) {
            reportError(state->compiler, memberNode->node.loc, "Member '%s' not found in class '%s'.", memberNode->memberName, classType.name ? classType.name : "<unknown>");
            return;
        }
        TypeID memberType = findMemberType(classSym, memberNode->memberName);
        TypeID valueType = getExpressionResultType(state, rhs);
        if (!assignmentTypeCompatible(memberType, valueType)) {
            reportError(state->compiler, memberNode->node.loc,
                "Cannot assign %s to field '%s' of type %s.",
                typeEnumToString(&valueType), memberNode->memberName,
                typeEnumToString(&memberType));
        }
        if (!memberType.borrowed && isRefType(memberType)) {
            BorrowOrigin fieldOrigin = borrowedSourceOrigin(state, lhs);
            if (hasLiveBorrowOfStorage(state, fieldOrigin, 0)) {
                reportError(state->compiler, memberNode->node.loc,
                    "Cannot replace field '%s' while a borrowed local still aliases it",
                    memberNode->memberName);
            }
        }

        int valReg = allocReg(state);
        int objReg = allocReg(state);

        generateExpression(state, rhs, valReg);
        generateExpression(state, memberNode->classNode, objReg);
        if (memberType.borrowed && expressionTransfersOwnership(state, rhs)) {
            reportError(state->compiler, memberNode->node.loc, "Borrowed field '%s' cannot take ownership; assign an existing owner instead", memberNode->memberName);
        }
        if (memberType.borrowed && borrowedFieldMayOutliveSource(state, memberNode->classNode, rhs)) {
            reportError(state->compiler, memberNode->node.loc,
                "Borrowed field '%s' may outlive the owner it references", memberNode->memberName);
        }
        if (!memberType.borrowed &&
            isRefType(memberType) && memberType.baseType != T_STRING &&
            !expressionTransfersOwnership(state, rhs))
            emitABC_IR(state, OP_CLONE_REF, valReg, valReg, 0);

        // Select correct Opcode based on field type
        OPCode storeOp = OP_STORE_FIELDi;
        if (memberType.baseType == T_FLOAT) storeOp = OP_STORE_FIELDf;
        else if (isRefType(memberType))     storeOp = memberType.borrowed ? OP_STORE_FIELDp_BORROW : OP_STORE_FIELDp;
        else if (memberType.baseType == T_BOOL || memberType.baseType == T_CHAR)
            storeOp = OP_STORE_FIELDb;
        
        emitABC_IR(state, storeOp, objReg, offset, valReg);

        freeRegisters(state, 2);
    }
    // ---------------------------------------------------------
    // Case 3: Array Assignment (arr[i] = ...)
    // ---------------------------------------------------------
    else if (lhs->type == AST_ARRAY_ACCESS) {
        ASTArrayAccess* arrNode = (ASTArrayAccess*)lhs;

        // 1. Evaluate Value (RHS) FIRST.
        //    This prevents function calls in RHS (like LoadTexture) from clobbering 
        //    registers holding the Array/Index if we evaluated them first.
        int valReg = allocReg(state);
        generateExpression(state, rhs, valReg);

        // 2. Evaluate Array
        int arrReg = allocReg(state);
        generateExpression(state, arrNode->array, arrReg);

        // 3. Evaluate Index
        int idxReg = allocReg(state);
        generateExpression(state, arrNode->index, idxReg);

        // 4. Determine Opcode
        TypeID arrType = getExpressionResultType(state, arrNode->array);
        TypeID valueType = getExpressionResultType(state, rhs);
        TypeID elementTypeInfo = { arrType.elementType, arrType.name, T_UNKNOWNTYPE, 0 };
        if (!assignmentTypeCompatible(elementTypeInfo, valueType)) {
            reportError(state->compiler, arrNode->node.loc,
                "Cannot assign %s to array element of type %s.",
                typeEnumToString(&valueType), typeEnumToString(&elementTypeInfo));
        }
        OPCode op = OP_ASTORE_I; // Default

        TypeIDEnum elementType = arrType.elementType;
        if (elementType == T_CLASS || elementType == T_STRING || elementType == T_PTR || elementType == T_SHARED_PTR)
            op = OP_ASTORE_P;
        else if (elementType == T_FLOAT) op = OP_ASTORE_F32;
        else if (elementType == T_CHAR) op = OP_ASTORE_I8;
        else if (elementType == T_SHORT) op = OP_ASTORE_I16;
        else if (elementType == T_FLOAT) op = OP_ASTORE_F32;

        if (arrType.borrowed) {
            reportError(state->compiler, arrNode->node.loc,
                "Cannot assign through a borrowed array; assign through the owning array instead");
        }
        BorrowOrigin elementOrigin = borrowedSourceOrigin(state, lhs);
        if (hasLiveBorrowOfStorage(state, elementOrigin, 0)) {
            reportError(state->compiler, arrNode->node.loc,
                "Cannot replace an array element while a borrowed local still aliases it");
        }
        if (!arrType.borrowed &&
            (elementType == T_CLASS || elementType == T_PTR || elementType == T_SHARED_PTR) &&
            !expressionTransfersOwnership(state, rhs))
            emitABC_IR(state, OP_CLONE_REF, valReg, valReg, 0);

        // 5. Emit Store
        emitABC_IR(state, op, arrReg, idxReg, valReg);

        freeRegisters(state, 3);
    }
}

void generateFor(State* state, ASTFor* forNode)
{
    int loopSymbolStart = state->compiler->symbols.count;
    int baseRegisterCount = state->compiler->nextRegisterIndex;
    state->compiler->nextRegisterIndex = baseRegisterCount + 1; // Reserve scope?

    generateStatement(forNode->init, state);
    int continueCleanupStart = state->compiler->symbols.count;
    FlowSnapshot loopEntryState = snapshotFlowState(state);
    bool conditionValue = true;
    bool loopBodyReachable = !forNode->condition ||
        !constantCondition(forNode->condition, &conditionValue) || conditionValue;

    // A constant bound and a single integer update can share one loop opcode.
    // Read the current local on every iteration: body writes remain visible.
    int stepSlot = -1, boundIndex = -1;
    OPCode stepOp = OP_NOP;
    if (forNode->condition && forNode->condition->type == AST_BINARY_OP) {
        ASTBinaryOp* condition = (ASTBinaryOp*)forNode->condition;
        if (condition->left->type == AST_VAR && condition->right->type == AST_CONST) {
            Symbol* local = lookupSymbolLocal(state, ((ASTVar*)condition->left)->name);
            ASTConst* bound = (ASTConst*)condition->right;
            if (local && local->type.baseType == T_INT && local->stackIndex >= 0 &&
                local->stackIndex <= 255 && bound->value.type.baseType == T_INT) {
                switch (condition->op) {
                case TK_LT: stepOp = OP_FOR_STEP_LT_I32; break;
                case TK_LE: stepOp = OP_FOR_STEP_LE_I32; break;
                case TK_GT: stepOp = OP_FOR_STEP_GT_I32; break;
                case TK_GE: stepOp = OP_FOR_STEP_GE_I32; break;
                default: break;
                }
                if (stepOp != OP_NOP) {
                    stepSlot = local->stackIndex;
                    boundIndex = addConstant(state, bound->value);
                    if (boundIndex < 0) stepOp = OP_NOP;
                }
            }
        }
    }

    int loopStart = createLabel(state);
    int loopExit = createLabel(state); // 1. Create Exit Label
    int continueLabel = createLabel(state);
    int parentBreakLabel = state->compiler->currentBreakLabel;
    int parentContinueLabel = state->compiler->currentContinueLabel;
    int parentBreakCleanup = state->compiler->currentBreakCleanupSymbol;
    int parentContinueCleanup = state->compiler->currentContinueCleanupSymbol;
    state->compiler->currentBreakLabel = loopExit;
    state->compiler->currentContinueLabel = continueLabel;
    state->compiler->currentBreakCleanupSymbol = loopSymbolStart;
    state->compiler->currentContinueCleanupSymbol = continueCleanupStart;

    emitLabel_IR(state, loopStart);

    // 1. Evaluate Condition & Jump (Pass 1 as TRUE constant if no condition exists)
    bool optimized = false;
    int jumpToEnd = -1;

    if (forNode->condition) {
        jumpToEnd = emitConditionAndJump(state, forNode->condition, &optimized);
    }

    // 2. Body
    int bodyStart = createLabel(state);
    emitLabel_IR(state, bodyStart);
    generateStatement(forNode->body, state);

    // continue jumps here (before increment)
    emitLabel_IR(state, continueLabel);
    IRNode* beforeIncrement = state->ir_tail;
    generateStatement(forNode->increment, state);
    FlowSnapshot loopBodyState = snapshotFlowState(state);
    if (loopBodyReachable) {
        scanLoopCarryOperations(state, forNode->body);
        scanLoopCarryOperations(state, forNode->increment);
    }

    // 3. Jump Loop
    IRNode* increment = beforeIncrement->next;
    if (stepOp != OP_NOP && increment && increment == state->ir_tail &&
        increment->data.type == IR_INSTRUCTION && increment->data.op == OP_ADDi_LOCAL_IMM &&
        increment->data.c == stepSlot) {
        int delta = increment->data.a;
        if (stepOp == OP_FOR_STEP_LT_I32 && delta == 1) {
            stepOp = OP_FOR_STEP1_LT_I32;
        }
        increment->data.op = stepOp;
        increment->data.a = stepSlot;
        increment->data.b = boundIndex;
        increment->data.c = delta;
        increment->data.is_AD = false;
        increment->data.label_id = bodyStart;
    } else {
        emitLoopJump_IR(state, loopStart);
    }

    // 4. Patch Exit
    if (jumpToEnd != -1) {
        emitLabel_IR(state, jumpToEnd);
    }
    // 4. Place Break Exit
    emitLabel_IR(state, loopExit);
    emitOwnedLocalCleanup(state, loopSymbolStart);
    // A for loop may execute zero times, so only states true both before and
    // after an iteration are definite after the loop.
    if (loopBodyReachable) {
        mergeFlowStateAfterBranch(state, &loopEntryState, &loopBodyState,
            loopEntryState.count);
    }
    else {
        restoreFlowState(state, &loopEntryState);
    }
    freeFlowSnapshot(&loopEntryState);
    freeFlowSnapshot(&loopBodyState);

    // 5. Restore
    state->compiler->currentBreakLabel = parentBreakLabel;
    state->compiler->currentContinueLabel = parentContinueLabel;
    state->compiler->currentBreakCleanupSymbol = parentBreakCleanup;
    state->compiler->currentContinueCleanupSymbol = parentContinueCleanup;
    state->compiler->symbols.count = loopSymbolStart;
    state->compiler->nextRegisterIndex = baseRegisterCount;
}

// 1. Helper to get the DIRECT opcode (Not the inverse)
static const struct { TTOKEN tk; OPCode op; } kDirectJumps[] = {
    { TK_LT, OP_JLT}, { TK_LE, OP_JLE},
    { TK_GT, OP_JGT}, { TK_GE, OP_JGE},
    { TK_EQ, OP_JEQ}, { TK_NE, OP_JNE},
};
static OPCode getDirectJump(TTOKEN tk) {
    for (int i = 0; i < 6; ++i) if (kDirectJumps[i].tk == tk) return kDirectJumps[i].op;
    return OP_NOT;
}

int emitConditionAndJump(State* state, ASTNode* condition, bool* isOptimized) {
    *isOptimized = false;

    // STRATEGY: "Jump-Over-Jump" (Trampoline)
    // We want to jump to 'Else/End' if the condition is FALSE.
    // Since Conditional Jumps have limited range (8-bit), we invert the flow:
    //
    // 1. IF (Condition is TRUE) GOTO SkipLabel
    // 2. GOTO ElseLabel (Long Jump)
    // 3. SkipLabel:
    // 4. ... Body ...

    // --- CASE 1: Optimization for Binary Comparisons (a < b) ---
    if (condition->type == AST_BINARY_OP) {
        ASTBinaryOp* binOp = (ASTBinaryOp*)condition;

        // Get the opcode that jumps when condition is TRUE (e.g. OP_JLT)
        OPCode jumpOp = getDirectJump(binOp->op);

        if (jumpOp != OP_NOT) {
            *isOptimized = true;

            // Static int local vs. constant: compare the slot directly, without
            // loading either operand into a temporary register each iteration.
            if (binOp->left->type == AST_VAR && binOp->right->type == AST_CONST) {
                Symbol* local = lookupSymbolLocal(state, ((ASTVar*)binOp->left)->name);
                ASTConst* constant = (ASTConst*)binOp->right;
                if (local && local->type.baseType == T_INT &&
                    local->stackIndex >= 0 && local->stackIndex <= 255 &&
                    constant->value.type.baseType == T_INT) {
                    int index = addConstant(state, constant->value);
                    if (index >= 0 && index <= 255) {
                        OPCode fused = OP_JEQ_LOCAL_K;
                        switch (jumpOp) {
                        case OP_JNE: fused = OP_JNE_LOCAL_K; break;
                        case OP_JLT: fused = OP_JLT_LOCAL_K; break;
                        case OP_JLE: fused = OP_JLE_LOCAL_K; break;
                        case OP_JGT: fused = OP_JGT_LOCAL_K; break;
                        case OP_JGE: fused = OP_JGE_LOCAL_K; break;
                        default: break;
                        }
                        int skip = createLabel(state);
                        IREntry ir = { 0 };
                        ir.type = IR_INSTRUCTION;
                        ir.op = fused;
                        ir.a = local->stackIndex;
                        ir.b = index;
                        ir.label_id = skip;
                        emitNode(state, ir);
                        int end = emitJump_IR(state, OP_JUMP, 0);
                        emitLabel_IR(state, skip);
                        return end;
                    }
                }
            }

            int l = allocReg(state);
            int r = allocReg(state);
            generateExpression(state, binOp->left, l);
            generateExpression(state, binOp->right, r);
            // Both operands are now materialized in registers. Reclaim any
            // owning temporaries they produced before branching on the result.
            if (expressionMayCreateUnclaimed(state, binOp->left) ||
                expressionMayCreateUnclaimed(state, binOp->right))
                emitAD_IR(state, OP_DESTROY_UNCLAIMED, 0, 0);

            // Float Promotion
            TypeID leftType = getExpressionResultType(state, binOp->left);
            TypeID rightType = getExpressionResultType(state, binOp->right);

            if (leftType.baseType == T_FLOAT || rightType.baseType == T_FLOAT) {
                if (leftType.baseType == T_INT) emitAD_IR(state, OP_CAST_I2F, l, (short)l);
                if (rightType.baseType == T_INT) emitAD_IR(state, OP_CAST_I2F, r, (short)r);

                // Map to Float Opcodes
                switch (jumpOp) {
                case OP_JGT: jumpOp = OP_JGTf; break;
                case OP_JGE: jumpOp = OP_JGEf; break;
                case OP_JLT: jumpOp = OP_JLTf; break;
                case OP_JLE: jumpOp = OP_JLEf; break;
                case OP_JEQ: jumpOp = OP_JEQf; break;
                case OP_JNE: jumpOp = OP_JNEf; break;
                default: break;
                }
            }

            // 1. Create Label to skip the "Else Jump"
            int skipLabel = createLabel(state);

            // 2. Emit Conditional Jump to SKIP
            // Note: We construct this manually to attach the label_id
            IREntry irSkip = { 0 };
            irSkip.type = IR_INSTRUCTION;
            irSkip.op = jumpOp;
            irSkip.a = l;
            irSkip.b = r;
            irSkip.label_id = skipLabel;
            irSkip.is_AD = false; // Comparison uses ABC format
            emitNode(state, irSkip);

            // 3. Emit Unconditional Jump to DEST (Long Jump)
            // This is the instruction that actually jumps to 'Else/End'
            // We return THIS index so generateIF/While patches it.
            int longJumpIndex = emitJump_IR(state, OP_JUMP, 0);

            // 4. Place Skip Label
            emitLabel_IR(state, skipLabel);

            freeRegisters(state, 2);
            return longJumpIndex;
        }
    }

    // --- CASE 2: Standard Boolean Expression (if valid) ---
    int condReg = allocReg(state);
    generateExpression(state, condition, condReg);
    // The condition value is in a primitive register; discarded owning
    // temporaries from evaluating it can now be reclaimed on either branch.
    if (expressionMayCreateUnclaimed(state, condition))
        emitAD_IR(state, OP_DESTROY_UNCLAIMED, 0, 0);

    int skipLabel = createLabel(state);

    // 1. Emit Jump If True -> Skip
    // If 'condReg' is true, we jump over the next instruction (which goes to Else)
    IREntry irSkip = { 0 };
    irSkip.type = IR_INSTRUCTION;
    irSkip.op = OP_JUMP_IF_TRUE;
    irSkip.a = condReg;
    irSkip.label_id = skipLabel;
    irSkip.is_AD = true; // JUMP_IF_TRUE uses AD format usually
    emitNode(state, irSkip);

    // 2. Emit Unconditional Jump -> Else/End
    // If we fell through (condition was false), we jump to the end.
    int longJumpIndex = emitJump_IR(state, OP_JUMP, 0);

    // 3. Place Skip Label
    emitLabel_IR(state, skipLabel);

    freeRegisters(state, 1);
    return longJumpIndex;
}

void generateIF(State* state, ASTIfStmt* ifStmt)
{
    int startRegisterIndex = state->compiler->nextRegisterIndex;

    // 1. Evaluate Condition & Jump
    bool optimized;
    int jumpToElse = emitConditionAndJump(state, ifStmt->condition, &optimized);
    FlowSnapshot incoming = snapshotFlowState(state);

    // 2. Then Block
    generateStatement(ifStmt->thenBlock, state);
    FlowSnapshot thenState = snapshotFlowState(state);

    // 3. Jump over Else
    int jumpAfterElse = -1;
    if (ifStmt->elseBlock) {
        jumpAfterElse = emitJump_IR(state, OP_JUMP, 0);
    }
    // 4. Patch Condition Jump
    emitLabel_IR(state, jumpToElse);
    restoreFlowState(state, &incoming);

    // 5. Else Block
    if (ifStmt->elseBlock) {
        generateStatement(ifStmt->elseBlock, state);
    }
    FlowSnapshot elseState = snapshotFlowState(state);
    if (jumpAfterElse != -1) {
        emitLabel_IR(state, jumpAfterElse);
    }
    bool thenReturns = statementDefinitelyReturns(ifStmt->thenBlock);
    bool elseReturns = ifStmt->elseBlock && statementDefinitelyReturns(ifStmt->elseBlock);
    if (thenReturns && !elseReturns) {
        restoreFlowState(state, ifStmt->elseBlock ? &elseState : &incoming);
    }
    else if (elseReturns && !thenReturns) {
        restoreFlowState(state, &thenState);
    }
    else if (!thenReturns && !elseReturns) {
        mergeFlowStateAfterBranch(state, &thenState,
            ifStmt->elseBlock ? &elseState : &incoming, incoming.count);
    }
    freeFlowSnapshot(&incoming);
    freeFlowSnapshot(&thenState);
    freeFlowSnapshot(&elseState);
    state->compiler->nextRegisterIndex = startRegisterIndex;
}

void generateWhile(State* state, ASTWhile* whileNode)
{
    int loopCleanupStart = state->compiler->symbols.count;
    int loopStart = createLabel(state);
    int loopExit = createLabel(state); // 1. Create Exit Label

    // Save previous break label (for nested loops)
    int parentBreakLabel = state->compiler->currentBreakLabel;
    int parentContinueLabel = state->compiler->currentContinueLabel;
    int parentBreakCleanup = state->compiler->currentBreakCleanupSymbol;
    int parentContinueCleanup = state->compiler->currentContinueCleanupSymbol;
    state->compiler->currentBreakLabel = loopExit;
    state->compiler->currentContinueLabel = loopStart;
    state->compiler->currentBreakCleanupSymbol = loopCleanupStart;
    state->compiler->currentContinueCleanupSymbol = loopCleanupStart;
    emitLabel_IR(state, loopStart);

    // 1. Evaluate Condition & Jump
    bool optimized;
    int jumpToEnd = emitConditionAndJump(state, whileNode->condition, &optimized);
    FlowSnapshot loopEntryState = snapshotFlowState(state);
    bool conditionValue = true;
    bool loopBodyReachable = !constantCondition(whileNode->condition, &conditionValue) ||
        conditionValue;

    // 2. Body
    generateStatement(whileNode->body, state);
    FlowSnapshot loopBodyState = snapshotFlowState(state);
    if (loopBodyReachable)
        scanLoopCarryOperations(state, whileNode->body);

    // 3. Jump Loop
    emitLoopJump_IR(state, loopStart);

    // 4. Patch Exit
    emitLabel_IR(state, jumpToEnd);

    emitLabel_IR(state, loopExit);
    // The body may not run. Merge the zero-iteration path with one iteration.
    if (loopBodyReachable) {
        mergeFlowStateAfterBranch(state, &loopEntryState, &loopBodyState,
            loopEntryState.count);
    }
    else {
        restoreFlowState(state, &loopEntryState);
    }
    freeFlowSnapshot(&loopEntryState);
    freeFlowSnapshot(&loopBodyState);
    state->compiler->currentBreakLabel = parentBreakLabel;
    state->compiler->currentContinueLabel = parentContinueLabel;
    state->compiler->currentBreakCleanupSymbol = parentBreakCleanup;
    state->compiler->currentContinueCleanupSymbol = parentContinueCleanup;
}

void generateBreak(State* state) {
    if (state->compiler->currentBreakLabel == 0) {
        reportError(state->compiler, LOC_NONE, "'break' statement not within loop or switch");
        return;
    }

    // Jump directly to the Exit Label of the current loop
    emitOwnedLocalCleanup(state, state->compiler->currentBreakCleanupSymbol);
    emitLoopJump_IR(state, state->compiler->currentBreakLabel);
}

void generateSwitch(State* state, ASTSwitch* switchNode)
{
    int switchCleanupStart = state->compiler->symbols.count;
    int startRegIndex = state->compiler->nextRegisterIndex;

    // 1. Evaluate Switch Expression
    int exprReg = allocReg(state);
    generateExpression(state, switchNode->expression, exprReg);
    TypeID switchType = getExpressionResultType(state, switchNode->expression);
    if (isRefType(switchType)) {
        reportError(state->compiler, switchNode->node.loc,
            "Switch expression must have a primitive type");
    }
    else {
        if (expressionMayCreateUnclaimed(state, switchNode->expression))
            emitAD_IR(state, OP_DESTROY_UNCLAIMED, 0, 0);
    }

    int caseCount = array_size(&switchNode->cases);

    // Create Labels
    int* bodyLabels = (int*)malloc(caseCount * sizeof(int));
    if(bodyLabels != NULL) {
        for (int i = 0; i < caseCount; i++) bodyLabels[i] = createLabel(state);
    }

    int defaultLabel = createLabel(state);
    int endLabel = createLabel(state);

    // ---------------------------------------------------------
    // Phase 1: Comparison Chain (Dispatch)
    // ---------------------------------------------------------
    for (int i = 0; i < caseCount; ++i) {
        // Correctly retrieve the pointer from the array
        ASTCase* currentCase = *(ASTCase**)array_get(&switchNode->cases, i);

        int valReg = allocReg(state);
        generateExpression(state, currentCase->value, valReg);
        TypeID caseType = getExpressionResultType(state, currentCase->value);
        if (isRefType(caseType)) {
            reportError(state->compiler, currentCase->node.loc,
                "Switch case value must have a primitive type");
        }
        else {
            if (expressionMayCreateUnclaimed(state, currentCase->value))
                emitAD_IR(state, OP_DESTROY_UNCLAIMED, 0, 0);
        }

        // Logic: 
        // IF (Expr != CaseVal) GOTO NextCheck
        // GOTO BodyLabel
        // NextCheck: ...

        int nextCheckLabel = createLabel(state);

        // 1. Emit JNE (Jump if Not Equal) to Next Check
        IREntry jne = { 0 };
        jne.type = IR_INSTRUCTION;
        jne.op = OP_JNE; // Integer comparison (Use OP_JNEf for floats if needed)
        jne.a = exprReg;
        jne.b = valReg;
        jne.label_id = nextCheckLabel; // Target
        jne.is_AD = false; // Comparison Jumps are ABC (Offset in C)
        emitNode(state, jne);

        // 2. Emit JUMP to Body
        IREntry jmpBody = { 0 };
        jmpBody.type = IR_INSTRUCTION;
        jmpBody.op = OP_JUMP;
        jmpBody.label_id = bodyLabels[i]; // Target
        jmpBody.is_AD = true; // Standard Jump is AD
        emitNode(state, jmpBody);

        // 3. Place Next Check Label
        emitLabel_IR(state, nextCheckLabel);

        freeRegisters(state, 1); // Free valReg
    }

    // If no cases matched, jump to Default
    IREntry jmpDef = { 0 };
    jmpDef.type = IR_INSTRUCTION;
    jmpDef.op = OP_JUMP;
    jmpDef.label_id = switchNode->defaultCase ? defaultLabel : endLabel;
    jmpDef.is_AD = true;
    emitNode(state, jmpDef);

    // ---------------------------------------------------------
    // Phase 2: Bodies
    // ---------------------------------------------------------

    // Save old break label (for nested switches/loops)
    int parentBreakLabel = state->compiler->currentBreakLabel;
    int parentBreakCleanup = state->compiler->currentBreakCleanupSymbol;
    state->compiler->currentBreakLabel = endLabel; // 'break' goes to endLabel
    state->compiler->currentBreakCleanupSymbol = switchCleanupStart;

    for (int i = 0; i < caseCount; ++i) {
        ASTCase* currentCase = *(ASTCase**)array_get(&switchNode->cases, i);

        // Mark start of body
        emitLabel_IR(state, bodyLabels[i]);

        generateStatement(currentCase->body, state);

        // Auto-break: no fallthrough
        emitLoopJump_IR(state, endLabel);
    }

    // Default Body
    if (switchNode->defaultCase) {
        emitLabel_IR(state, defaultLabel);
        generateStatement(switchNode->defaultCase, state);
    }

    // ---------------------------------------------------------
    // Phase 3: Cleanup
    // ---------------------------------------------------------
    emitLabel_IR(state, endLabel);

    state->compiler->currentBreakLabel = parentBreakLabel;
    state->compiler->currentBreakCleanupSymbol = parentBreakCleanup;
    state->compiler->nextRegisterIndex = startRegIndex;
    free(bodyLabels);
}

void generateContinue(State* state) {
    if (state->compiler->currentContinueLabel == 0) {
        reportError(state->compiler, LOC_NONE, "'continue' statement not within loop");
        return;
    }
    emitOwnedLocalCleanup(state, state->compiler->currentContinueCleanupSymbol);
    emitLoopJump_IR(state, state->compiler->currentContinueLabel);
}

static void emitOwnedLocalCleanup(State* state, int firstSymbol) {
    for (int i = state->compiler->symbols.count - 1; i >= firstSymbol; --i) {
        Symbol* symbol = (Symbol*)array_get(&state->compiler->symbols, i);
        if (symbol->scopeParent == state->compiler->currentScopeParent &&
            symbol->scopeLevel > 0 && symbol->kind == SYM_VARIABLE &&
            symbol->ownsValue && isRefType(symbol->type)) {
            if (hasLiveBorrowOfRefIndex(state, symbol->stackIndex, 0) &&
                !hasLiveBorrowOfRefIndex(state, symbol->stackIndex, firstSymbol)) {
                reportError(state->compiler, LOC_NONE,
                    "Borrowed local outlives owned value '%s'", symbol->name);
            }
            emitAD_IR(state, OP_DESTROYp_LOCAL, 0, (short)symbol->stackIndex);
        }
    }
}

void generateStatement(ASTNode* stmtNode, State* state)
{
    if (!stmtNode) return;
    state->current_loc = stmtNode->loc;
    switch (stmtNode->type) {
    case AST_CLASS_DEF:generateClass(state, (ASTClassDef*)stmtNode); break;
    case AST_FUNC_DEF: generateFunction(state, (ASTFuncDef*)stmtNode, -1); break;
    case AST_FUNC_CALL:
        generateExpression(state, stmtNode, 0);
        if (expressionMayCreateUnclaimed(state, stmtNode))
            emitAD_IR(state, OP_DESTROY_UNCLAIMED, 0, 0);
        break;
    case AST_FOR: generateFor(state, (ASTFor*)stmtNode); break;
    case AST_MULTI_VAR_DECL:generateVarDeclaration(state, (ASTMultiVarDecl*)stmtNode); break;
    case AST_BLOCK: {
        ASTBlock* block = (ASTBlock*)stmtNode;

        // 1. Save Symbol Table State
        int previousSymbolCount = state->compiler->symbols.count;

        // 2. Enter Scope
        state->compiler->currentScopeLevel++;

        for (int i = 0; i < array_size(&block->list); ++i) {
            ASTNode* statement = *(ASTNode**)array_get(&block->list, i);
            generateStatement(statement, state);

            // Optimization: Stop if we hit a terminator (Return/Break)
            if (statementDefinitelyReturns(statement) || statement->type == AST_BREAK) {
                break;
            }
        }

        emitOwnedLocalCleanup(state, previousSymbolCount);

        // 3. Exit Scope
        state->compiler->currentScopeLevel--;
        // Clean up symbols before popping them ---
        int currentCount = state->compiler->symbols.count;
        for (int i = currentCount - 1; i >= previousSymbolCount; i--) {
            Symbol* s = (Symbol*)array_get(&state->compiler->symbols, i);

            // Free Class Member Lists
            if (s->type.baseType == T_CLASS && s->kind == SYM_TYPE_DEFINITION) {
                if (s->as.classDef.members.data) {
                    array_destroy(&s->as.classDef.members);
                }
            }
            // Free Function Parameter Lists (if you allocate arrays for them)
            else if (s->type.baseType == T_FUNCTION) {
                if (s->as.funcDef.paramTypes) {
                    free(s->as.funcDef.paramTypes);
                    s->as.funcDef.paramTypes = NULL;
                }
            }
        }
        // 4. Pop Symbols (Restore Table)
        // This effectively removes all variables declared inside this block
        // so they don't shadow outer variables anymore.
        state->compiler->symbols.count = previousSymbolCount;

        break;
    }
    case AST_BREAK: generateBreak(state); break;
    case AST_CONTINUE: generateContinue(state); break;
    case AST_IF: generateIF(state, (ASTIfStmt*)stmtNode); break;
    case AST_WHILE:generateWhile(state, (ASTWhile*)stmtNode); break;
    case AST_SWITCH:generateSwitch(state, (ASTSwitch*)stmtNode); break;
    case AST_RETURN: {
        ASTReturn* retNode = (ASTReturn*)stmtNode;
        TypeID functionReturnType = (TypeID){ T_VOID, NULL };
        if (state->compiler->currentScopeParent >= 0 &&
            state->compiler->currentScopeParent < state->compiler->symbols.count) {
            Symbol* functionSymbol = (Symbol*)array_get(&state->compiler->symbols,
                state->compiler->currentScopeParent);
            if (functionSymbol && functionSymbol->kind == SYM_FUNCTION)
                functionReturnType = functionSymbol->type;
        }
        if (retNode->retval) {
            TypeID returnType = getExpressionResultType(state, retNode->retval);
            reportIncompatibleValue(state, retNode->node.loc, "Return type mismatch",
                functionReturnType, returnType);
            if (functionReturnType.borrowed) {
                Symbol* functionSymbol = (Symbol*)array_get(&state->compiler->symbols,
                    state->compiler->currentScopeParent);
                BorrowOrigin origin = borrowedSourceOrigin(state, retNode->retval);
                BorrowOrigin expected = { -1, 0, 0, { 0 } };
                if (functionSymbol && functionSymbol->as.funcDef.borrowedReturnRefIndex >= 0) {
                    expected.rootRefIndex = functionSymbol->as.funcDef.borrowedReturnRefIndex;
                    expected = appendBorrowReturnPath(state, expected,
                        functionSymbol->as.funcDef.borrowedReturnPath, retNode->node.loc);
                }
                if (!functionSymbol || expected.rootRefIndex < 0 ||
                    !borrowOriginsMatch(origin, expected)) {
                    reportError(state->compiler, retNode->node.loc,
                        "Borrowed return must match the named parameter and subobject path in its annotation");
                }
            }
            // Assuming R0 is the return value register
            generateExpression(state, retNode->retval, R0);
            if (expressionMayCreateUnclaimed(state, retNode->retval) &&
                isRefType(functionReturnType) && !functionReturnType.borrowed) {
                // Keep the returned object alive while cleaning unrelated
                // temporaries created while evaluating the return expression.
                emitAD_IR(state, OP_DESTROY_UNCLAIMED_KEEP_REF, R0, 0);
            }
            else if (expressionMayCreateUnclaimed(state, retNode->retval)) {
                emitAD_IR(state, OP_DESTROY_UNCLAIMED, 0, 0);
            }
            if (isRefType(returnType) && retNode->retval->type == AST_VAR) {
                Symbol* returned = lookupSymbolLocal(state, ((ASTVar*)retNode->retval)->name);
                if (returned && returned->ownsValue)
                    emitAD_IR(state, OP_CLEARp_LOCAL, 0, (short)returned->stackIndex);
            }
            if (isRefType(functionReturnType) && !functionReturnType.borrowed)
                emitAD_IR(state, OP_MARK_UNCLAIMED_REF, R0, 0);
        }
        else if (functionReturnType.baseType != T_VOID) {
            reportError(state->compiler, retNode->node.loc,
                "Return type mismatch: expected %s, got void.",
                typeEnumToString(&functionReturnType));
        }
        emitOwnedLocalCleanup(state, 0);
        emitAD_IR(state, isRefType(functionReturnType) ? OP_RETURN : OP_RETURN_PRIM, 0, 0);
        break;
    }
    case AST_BINARY_OP:
    {
        ASTBinaryOp* binOp = (ASTBinaryOp*)stmtNode;
        if (binOp->op == TK_ASSIGN || binOp->op == TK_ADD_ASSIGN || binOp->op == TK_SUB_ASSIGN ||
            binOp->op == TK_MULT_ASSIGN || binOp->op == TK_DIVEQ) {
            generateAssigment(state, binOp);
        }
        else {
            generateExpression(state, stmtNode, 0);
        }
        if (expressionMayCreateUnclaimed(state, stmtNode))
            emitAD_IR(state, OP_DESTROY_UNCLAIMED, 0, 0);
        break;
    }
    case AST_UNARY_OP:
        generateExpression(state, stmtNode, 0);
        if (expressionMayCreateUnclaimed(state, stmtNode))
            emitAD_IR(state, OP_DESTROY_UNCLAIMED, 0, 0);
        break;
    default:
        break;
    }
}


void generateClass(State* state, ASTClassDef* classNode) {
    // 1. Add class symbol
    addClassSymbol(state, classNode->name);
    int classSymbolIndex = state->compiler->symbols.count - 1;

    int currentOffset = 0;
    int packedOffset = 0;
    // Host-created script instances still use the legacy 64-bit slot layout.
    // Keep packed class fields disabled until every object allocator, including
    // script-instance creation, uses the packed object ABI.
    bool packedLayout = false;

    ASTNode** members = (ASTNode**)classNode->members->list.data;
    int count = array_size(&classNode->members->list);

    // Decide the layout before assigning any offsets.  Classes containing
    // references retain the legacy slot layout; only primitive-only classes
    // may use byte-packed offsets.
    for (int i32 = 0; i32 < count; ++i32) {
        ASTNode* memberNode = members[i32];
        if (memberNode->type != AST_MULTI_VAR_DECL) continue;
        ASTMultiVarDecl* fieldDecl = (ASTMultiVarDecl*)memberNode;
        if (fieldDecl->varType.baseType == T_CLASS ||
            fieldDecl->varType.baseType == T_STRING ||
            fieldDecl->varType.baseType == T_PTR ||
            fieldDecl->varType.baseType == T_SHARED_PTR ||
            fieldDecl->varType.baseType == T_DOUBLE) {
            packedLayout = false;
            break;
        }
    }

    // =================================================================
    // PASS 1: REGISTER VARIABLES (Build the Class Layout)
    // =================================================================
    // We do this first so that ALL members are known before ANY method runs.
    for (int i32 = 0; i32 < count; ++i32) {
        ASTNode* memberNode = members[i32];

        if (memberNode->type == AST_MULTI_VAR_DECL) {
            // Re-fetch symbol to ensure pointer validity
            Symbol* classSymbol = (Symbol*)array_get(&state->compiler->symbols, classSymbolIndex);

            ASTMultiVarDecl* fieldDecl = (ASTMultiVarDecl*)memberNode;
            VariableInfo* names = (VariableInfo*)fieldDecl->names.data;
            int nameCount = array_size(&fieldDecl->names);

            for (int j = 0; j < nameCount; ++j) {
                // Register member
                addClassMember(classSymbol, names[j].name, fieldDecl->varType,
                    packedLayout ? packedOffset : currentOffset);
                currentOffset++;
                packedOffset += getTypeSizeEnum(fieldDecl->varType.baseType);
            }
        }
    }

    // Update total size (1 slot per variable)
    Symbol* sizedClassSymbol = (Symbol*)array_get(&state->compiler->symbols, classSymbolIndex);
    sizedClassSymbol->as.classDef.totalSize = currentOffset;
    sizedClassSymbol->as.classDef.packedLayout = packedLayout ? 1 : 0;
    sizedClassSymbol->as.classDef.packedSize = packedOffset;

    // =================================================================
    // PASS 2: COMPILE METHODS (Generate Bytecode)
    // =================================================================
    for (int i32 = 0; i32 < count; ++i32) {
        ASTNode* memberNode = members[i32];

        if (memberNode->type == AST_FUNC_DEF) {
            ASTFuncDef* func = (ASTFuncDef*)memberNode;

            // Compile function using the now-complete class definition
            generateFunction(state, func, classSymbolIndex);

            // Auto-detect Destructor
            if (strcmp(func->name, "free") == 0) {
                Symbol* dtor = findMethodSymbol(state, classNode->name, "free");
                if (dtor) {
                    int funcIdx = (int)(dtor - (Symbol*)state->compiler->symbols.data);
                    Symbol* finalClassSym = (Symbol*)array_get(&state->compiler->symbols, classSymbolIndex);
                    finalClassSym->as.classDef.destructorIndex = funcIdx;
                }
            }
        }
    }
}

static int findClassSymbolIndex(State* state, const char* className)
{
    if (!state || !state->compiler || !className) return -1;
    for (int i = 0; i < state->compiler->symbols.count; ++i) {
        Symbol* symbol = (Symbol*)array_get(&state->compiler->symbols, i);
        if (symbol && symbol->kind == SYM_TYPE_DEFINITION &&
            symbol->type.baseType == T_CLASS && strcmp(symbol->name, className) == 0) {
            return i;
        }
    }
    return -1;
}

static int typeOwnedClassIndex(State* state, TypeID type)
{
    if (type.borrowed) return -1;
    if (type.baseType == T_CLASS) return findClassSymbolIndex(state, type.name);
    if ((type.baseType == T_PTR || type.baseType == T_SHARED_PTR) &&
        type.elementType == T_CLASS) return findClassSymbolIndex(state, type.name);
    return -1;
}

static bool classOwnsPathTo(State* state, int currentIndex, int targetIndex, unsigned char* visiting)
{
    if (currentIndex == targetIndex) return true;
    if (currentIndex < 0 || currentIndex >= state->compiler->symbols.count) return false;
    if (visiting[currentIndex]) return false;

    visiting[currentIndex] = 1;
    Symbol* current = (Symbol*)array_get(&state->compiler->symbols, currentIndex);
    if (!current || current->kind != SYM_TYPE_DEFINITION ||
        current->type.baseType != T_CLASS) {
        visiting[currentIndex] = 0;
        return false;
    }

    for (int i = 0; i < array_size(&current->as.classDef.members); ++i) {
        ClassMember* member = (ClassMember*)array_get(&current->as.classDef.members, i);
        int childIndex = member ? typeOwnedClassIndex(state, member->type) : -1;
        if (childIndex >= 0 && classOwnsPathTo(state, childIndex, targetIndex, visiting)) {
            visiting[currentIndex] = 0;
            return true;
        }
    }

    visiting[currentIndex] = 0;
    return false;
}

static void validateOwnedClassCycles(State* state)
{
    if (!state || !state->compiler || state->compiler->error) return;

    int symbolCount = state->compiler->symbols.count;
    unsigned char* visiting = (unsigned char*)calloc((size_t)symbolCount, 1);
    if (!visiting) {
        reportError(state->compiler, LOC_NONE, "Out of memory while checking owned class cycles");
        return;
    }

    for (int classIndex = 0; classIndex < symbolCount && !state->compiler->error; ++classIndex) {
        Symbol* classSymbol = (Symbol*)array_get(&state->compiler->symbols, classIndex);
        if (!classSymbol || classSymbol->kind != SYM_TYPE_DEFINITION ||
            classSymbol->type.baseType != T_CLASS) continue;

        for (int memberIndex = 0; memberIndex < array_size(&classSymbol->as.classDef.members); ++memberIndex) {
            ClassMember* member = (ClassMember*)array_get(&classSymbol->as.classDef.members, memberIndex);
            int childIndex = member ? typeOwnedClassIndex(state, member->type) : -1;
            memset(visiting, 0, (size_t)symbolCount);
            if (childIndex >= 0 && classOwnsPathTo(state, childIndex, classIndex, visiting)) {
                reportError(state->compiler, LOC_NONE,
                    "Owned field '%s.%s' creates an ownership cycle; declare the back-link as 'borrow %s'",
                    classSymbol->name, member->name, member->type.name ? member->type.name : "Type");
                break;
            }
        }
    }

    free(visiting);
}

#include "Optimizer.h"

void generateIR(State* state, ASTNode* node) {
    if (!state || !node || state->compiler->error) return;
    // Fold constants before lowering so the generated IR never contains
    // instructions for expressions that can be resolved at compile time.
    optimize_ast(state->compiler, &node);
    if (state->compiler->error || !node) return;

    if (node->type == AST_BLOCK) {
        ASTBlock* programBlock = (ASTBlock*)node;

        // Iterate manually. 
        // This keeps Scope at 0.
        for (int i = 0; i < array_size(&programBlock->list); ++i) {
            ASTNode* statement = *(ASTNode**)array_get(&programBlock->list, i);
            generateStatement(statement, state);
        }
        validateOwnedClassCycles(state);
    }
    else {
        generateStatement(node, state);
        validateOwnedClassCycles(state);
    }

    emitAD_IR(state, OP_HALT, 0, 0);
    // Remove redundant IR instructions before labels and jumps are assembled.
    optimizeIR(state);
    assemble(state);
    // All rewrites run on IR: serialized constants and extended operands
    // are data, and must never be treated as executable opcodes.
}

enum {
    CI_BYTECODE_FILE_MAGIC = 0x43494243,
    CI_BYTECODE_FILE_VERSION = 1,
    CI_BYTECODE_OPCODE_VERSION = 2,
    CI_BYTECODE_LEGACY_OPCODE_VERSION = 1,
    CI_BYTECODE_METADATA_MAGIC = 0x43494D44,
    CI_BYTECODE_METADATA_VERSION = 1,
    CI_MAX_METADATA_STRING = 32768,
    CI_MAX_METADATA_ENTRIES = 16 * 1024 * 1024
};

static bool writeMetadataString(FILE* file, const char* text)
{
    uint32_t length = text ? (uint32_t)(strlen(text) + 1) : 0;
    if (length > CI_MAX_METADATA_STRING) return false;
    return fwrite(&length, sizeof(length), 1, file) == 1 &&
        (!length || fwrite(text, 1, length, file) == length);
}

static char* readMetadataString(FILE* file, bool* ok)
{
    uint32_t length = 0;
    if (fread(&length, sizeof(length), 1, file) != 1 ||
        length > CI_MAX_METADATA_STRING) {
        *ok = false;
        return NULL;
    }
    if (!length) return NULL;
    char* text = (char*)malloc(length);
    if (!text || fread(text, 1, length, file) != length || text[length - 1] != '\0') {
        free(text);
        *ok = false;
        return NULL;
    }
    return text;
}

void saveBytecode(State* state, const char* path) {
    if (!state || !path) return;
    FILE* f = fopen(path, "wb");
    if (!f) return;

    // File header allows a loader to reject bytecode requiring unknown opcodes.
    uint32_t fileMagic = CI_BYTECODE_FILE_MAGIC;
    uint32_t fileVersion = CI_BYTECODE_FILE_VERSION;
    uint32_t opcodeVersion = CI_BYTECODE_OPCODE_VERSION;
    if (fwrite(&fileMagic, sizeof(fileMagic), 1, f) != 1 ||
        fwrite(&fileVersion, sizeof(fileVersion), 1, f) != 1 ||
        fwrite(&opcodeVersion, sizeof(opcodeVersion), 1, f) != 1) {
        fclose(f);
        return;
    }

    // 1. Write bytecode size + data
    int bytecodeSize = array_size(&state->bytecode);
    fwrite(&bytecodeSize, sizeof(int), 1, f);
    fwrite(state->bytecode.data, 1, bytecodeSize * sizeof(int32_t), f);

    // 2. Write C-function binding table
    int ccount = array_size(&state->cFunctions);
    fwrite(&ccount, sizeof(int), 1, f);

    for (int i = 0; i < ccount; i++) {
        CFuncEntry* e = array_get(&state->cFunctions, i);

        // Write cfuncIndex
        fwrite(&i, sizeof(int), 1, f);

        // Write interned string index
        fwrite(&e->nameIndex, sizeof(int), 1, f);
    }

    // Optional, versioned diagnostics metadata follows the legacy payload.
    uint32_t magic = CI_BYTECODE_METADATA_MAGIC;
    uint32_t version = CI_BYTECODE_METADATA_VERSION;
    uint32_t functionCount = (uint32_t)state->function_name_count;
    uint32_t sourceMapCount = (uint32_t)state->source_map_count;
    bool metadataOK = functionCount <= MAX_FUNCTIONS &&
        sourceMapCount <= CI_MAX_METADATA_ENTRIES &&
        fwrite(&magic, sizeof(magic), 1, f) == 1 &&
        fwrite(&version, sizeof(version), 1, f) == 1 &&
        fwrite(&functionCount, sizeof(functionCount), 1, f) == 1 &&
        fwrite(&sourceMapCount, sizeof(sourceMapCount), 1, f) == 1 &&
        writeMetadataString(f, state->source_filename);
    for (uint32_t i = 0; metadataOK && i < functionCount; ++i)
        metadataOK = writeMetadataString(f, state->function_names[i]);
    for (uint32_t i = 0; metadataOK && i < sourceMapCount; ++i) {
        SourceMapEntry* entry = &state->source_map[i];
        metadataOK = fwrite(&entry->bytecode_offset, sizeof(entry->bytecode_offset), 1, f) == 1 &&
            fwrite(&entry->line, sizeof(entry->line), 1, f) == 1 &&
            fwrite(&entry->column, sizeof(entry->column), 1, f) == 1 &&
            writeMetadataString(f, entry->filename);
    }
    if (!metadataOK)
        fprintf(stderr, "Warning: failed to write complete bytecode diagnostics metadata to '%s'.\n", path);

    fclose(f);
}

static void clearStateDiagnostics(State* state)
{
    free(state->source_filename);
    state->source_filename = NULL;
    for (int i = 0; i < state->function_name_count; ++i)
        free(state->function_names[i]);
    free(state->function_names);
    state->function_names = NULL;
    state->function_name_count = 0;
    for (size_t i = 0; i < state->source_map_count; ++i)
        free(state->source_map[i].filename);
    free(state->source_map);
    state->source_map = NULL;
    state->source_map_count = 0;
    state->source_map_capacity = 0;
}

static bool copyDiagnosticsToProgram(State* state)
{
    VMProgram* program = state->program;
    if (!program) return true;
    free(program->source_filename);
    program->source_filename = NULL;
    for (int i = 0; i < program->function_name_count; ++i) {
        free(program->function_names[i]);
        program->function_names[i] = NULL;
    }
    for (size_t i = 0; i < program->source_map_count; ++i)
        free(program->source_map[i].filename);
    free(program->source_map);
    program->source_map = NULL;
    program->source_map_count = 0;

    if (state->source_filename) {
        size_t length = strlen(state->source_filename) + 1;
        program->source_filename = (char*)malloc(length);
        if (!program->source_filename) return false;
        memcpy(program->source_filename, state->source_filename, length);
    }
    program->function_name_count = state->function_name_count;
    for (int i = 0; i < program->function_name_count; ++i) {
        const char* name = state->function_names[i];
        if (!name) continue;
        size_t length = strlen(name) + 1;
        program->function_names[i] = (char*)malloc(length);
        if (!program->function_names[i]) return false;
        memcpy(program->function_names[i], name, length);
    }
    if (state->source_map_count) {
        program->source_map = (SourceMapEntry*)calloc(state->source_map_count,
            sizeof(SourceMapEntry));
        if (!program->source_map) return false;
        for (size_t i = 0; i < state->source_map_count; ++i) {
            program->source_map[i] = state->source_map[i];
            program->source_map[i].filename = NULL;
            const char* name = state->source_map[i].filename;
            if (name) {
                size_t length = strlen(name) + 1;
                program->source_map[i].filename = (char*)malloc(length);
                if (!program->source_map[i].filename) return false;
                memcpy(program->source_map[i].filename, name, length);
            }
            program->source_map_count++;
        }
    }
    return true;
}

static bool loadBytecodeDiagnostics(State* state, FILE* file)
{
    uint32_t magic = 0;
    size_t gotMagic = fread(&magic, sizeof(magic), 1, file);
    if (!gotMagic && feof(file)) return true; // Legacy bytecode without metadata.
    if (gotMagic != 1 || magic != CI_BYTECODE_METADATA_MAGIC) return false;

    uint32_t version = 0, functionCount = 0, sourceMapCount = 0;
    if (fread(&version, sizeof(version), 1, file) != 1 ||
        fread(&functionCount, sizeof(functionCount), 1, file) != 1 ||
        fread(&sourceMapCount, sizeof(sourceMapCount), 1, file) != 1 ||
        version != CI_BYTECODE_METADATA_VERSION || functionCount > MAX_FUNCTIONS ||
        sourceMapCount > CI_MAX_METADATA_ENTRIES)
        return false;

    bool ok = true;
    char* sourceName = readMetadataString(file, &ok);
    char** functionNames = functionCount ?
        (char**)calloc(functionCount, sizeof(char*)) : NULL;
    SourceMapEntry* sourceMap = sourceMapCount ?
        (SourceMapEntry*)calloc(sourceMapCount, sizeof(SourceMapEntry)) : NULL;
    if (!ok || (functionCount && !functionNames) || (sourceMapCount && !sourceMap))
        ok = false;

    for (uint32_t i = 0; ok && i < functionCount; ++i)
        functionNames[i] = readMetadataString(file, &ok);
    for (uint32_t i = 0; ok && i < sourceMapCount; ++i) {
        SourceMapEntry* entry = &sourceMap[i];
        ok = fread(&entry->bytecode_offset, sizeof(entry->bytecode_offset), 1, file) == 1 &&
            fread(&entry->line, sizeof(entry->line), 1, file) == 1 &&
            fread(&entry->column, sizeof(entry->column), 1, file) == 1;
        if (ok && (entry->bytecode_offset >= (uint32_t)array_size(&state->bytecode) ||
            entry->line <= 0 || entry->column < 0 ||
            (i && entry->bytecode_offset < sourceMap[i - 1].bytecode_offset)))
            ok = false;
        if (ok) entry->filename = readMetadataString(file, &ok);
    }

    if (ok) {
        clearStateDiagnostics(state);
        state->source_filename = sourceName;
        sourceName = NULL;
        state->function_names = functionNames;
        state->function_name_count = (int)functionCount;
        functionNames = NULL;
        state->source_map = sourceMap;
        state->source_map_count = state->source_map_capacity = sourceMapCount;
        sourceMap = NULL;
        ok = copyDiagnosticsToProgram(state);
    }

    free(sourceName);
    if (functionNames) {
        for (uint32_t i = 0; i < functionCount; ++i) free(functionNames[i]);
        free(functionNames);
    }
    if (sourceMap) {
        for (uint32_t i = 0; i < sourceMapCount; ++i) free(sourceMap[i].filename);
        free(sourceMap);
    }
    return ok;
}

void loadBytecode(State* state, const char* path) {
    enum { MAX_LOADED_BYTECODE_WORDS = 16 * 1024 * 1024 };
    if (!state || !path) return;
    clearStateDiagnostics(state);
    size_t pathLength = strlen(path) + 1;
    state->source_filename = (char*)malloc(pathLength);
    if (state->source_filename) memcpy(state->source_filename, path, pathLength);
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Failed to open bytecode file '%s'.\n", path);
        return;
    }

    // New files identify both their container and opcode set. Files without
    // this magic are legacy files and use the original opcode set.
    uint32_t marker = 0;
    if (fread(&marker, sizeof(marker), 1, f) != 1) {
        fprintf(stderr, "Truncated bytecode header in '%s'.\n", path);
        fclose(f);
        return;
    }
    int bytecodeSize = 0;
    if (marker == CI_BYTECODE_FILE_MAGIC) {
        uint32_t fileVersion = 0, opcodeVersion = 0;
        if (fread(&fileVersion, sizeof(fileVersion), 1, f) != 1 ||
            fread(&opcodeVersion, sizeof(opcodeVersion), 1, f) != 1) {
            fprintf(stderr, "Truncated bytecode version header in '%s'.\n", path);
            fclose(f);
            return;
        }
        if (fileVersion != CI_BYTECODE_FILE_VERSION ||
            opcodeVersion < CI_BYTECODE_LEGACY_OPCODE_VERSION ||
            opcodeVersion > CI_BYTECODE_OPCODE_VERSION) {
            fprintf(stderr,
                "Unsupported bytecode format/opcode version in '%s' (format=%u, opcodes=%u).\n",
                path, fileVersion, opcodeVersion);
            fclose(f);
            return;
        }
        if (fread(&bytecodeSize, sizeof(bytecodeSize), 1, f) != 1) {
            fprintf(stderr, "Truncated bytecode size in '%s'.\n", path);
            fclose(f);
            return;
        }
    }
    else {
        bytecodeSize = (int32_t)marker;
    }

    // 1. Read bytecode
    if (bytecodeSize <= 0 || bytecodeSize > MAX_LOADED_BYTECODE_WORDS) {
        fprintf(stderr, "Invalid bytecode size in '%s'.\n", path);
        fclose(f);
        return;
    }

    array_resize(&state->bytecode, bytecodeSize);
    if (fread(state->bytecode.data, sizeof(int32_t), (size_t)bytecodeSize, f) !=
        (size_t)bytecodeSize) {
        fprintf(stderr, "Truncated bytecode payload in '%s'.\n", path);
        state->bytecode.count = 0;
        fclose(f);
        return;
    }
    state->bytecode.count = bytecodeSize;

    // 2. Initialize VM (loads constants, func table, etc.)
    vm_init(state);

    // 3. Bind all C functions (fills state->cFunctions)
    //bindAll(state);

    // 4. Load C-binding table
    int ccount = 0;
    if (fread(&ccount, sizeof(int), 1, f) != 1 || ccount < 0 ||
        ccount > array_size(&state->cFunctions)) {
        fprintf(stderr, "Invalid native binding table in '%s'.\n", path);
        fclose(f);
        return;
    }

    for (int i = 0; i < ccount; i++) {
        int cfuncIndex;
        int nameIndex;

        if (fread(&cfuncIndex, sizeof(int), 1, f) != 1 ||
            fread(&nameIndex, sizeof(int), 1, f) != 1) {
            fprintf(stderr, "Truncated native binding table in '%s'.\n", path);
            fclose(f);
            return;
        }

        // Find runtime C function with matching nameIndex
        int total = array_size(&state->cFunctions);
        int runtimeIndex = -1;

        for (int j = 0; j < total; j++) {
            CFuncEntry* e = array_get(&state->cFunctions, j);
            if (e->nameIndex == nameIndex) {
                runtimeIndex = j;
                break;
            }
        }

        if (cfuncIndex < 0 || cfuncIndex >= total || runtimeIndex < 0) {
            fprintf(stderr, "Invalid or missing native binding in '%s'.\n", path);
            fclose(f);
            return;
        }

        // Restore mapping
        CFuncEntry* src = array_get(&state->cFunctions, runtimeIndex);
        CFuncEntry* dst = array_get(&state->cFunctions, cfuncIndex);

        dst->fn = src->fn;
        dst->nameIndex = src->nameIndex;
    }

    if (!loadBytecodeDiagnostics(state, f))
        fprintf(stderr, "Invalid or truncated diagnostics metadata in '%s'.\n", path);
    fclose(f);
}

bool compile(State* state, const char* filename)
{
    bool compiled = false;
    if (state) {
        free(state->source_filename);
        state->source_filename = NULL;
        if (filename) {
            size_t length = strlen(filename) + 1;
            state->source_filename = (char*)malloc(length);
            if (state->source_filename) memcpy(state->source_filename, filename, length);
        }
    }
    char* source = readFile(filename);
    if (!source) {
        fprintf(stderr, "Failed to load file : '%s'.\n", filename);
        return 0; // Exit if file loading fails
    }

    ASTNode* ast = parse(source, filename, state);
    if (!ast) {
        fprintf(stderr, "Failed to parse source code.\n");
        //freeState(state);
        free(source);
        return 0; // Exit if parsing fails
    }

    if (!state->compiler->error) {
        generateIR(state, (ASTNode*)ast);
    }
    if (!state->compiler->error)
        compiled = true;

    // nextRegisterIndex is restored after each function; it is not a high-water
    // mark. Preserve the maximum actually used, including the return register.
    if (state->register_count == 0) state->register_count = 1;
    freeCompiler(state->compiler);
    state->compiler = NULL;
    free(source);

    return compiled;
}

CompilerDef* createCompilerDef() {
    CompilerDef* compiler = (CompilerDef*)malloc(sizeof(CompilerDef));

    if (!compiler) return NULL;

    memset(compiler, 0, sizeof(CompilerDef));
    compiler->currentScopeLevel = 0;
    compiler->strings = array_create(sizeof(char*));
    compiler->ASTs = array_create(sizeof(ASTNode*));
    arena_init(&compiler->astArena, 64 * 1024);
    compiler->symbols = array_create(sizeof(Symbol));
    compiler->importedFiles = array_create(sizeof(char*)); // Stores char* paths
    return compiler;
}

State* createState()
{
    State* state = (State*)malloc(sizeof(State));

    if (!state) return NULL;

    memset(state, 0, sizeof(State));
    state->cFunctions = array_create(sizeof(CFunction));
    state->cFunctionSignatures = array_create(sizeof(const NativeSignature*));
    state->bytecode = array_create(sizeof(int32));
    state->Constants = array_create(sizeof(Value));
    arena_init(&state->irArena, 64 * 1024);
    state->compiler = createCompilerDef();
    state->compiler->error = 0;
    state->ir_head = state->ir_tail = NULL;
    state->next_label_id = 0;

    return state;
}

void freeASTNode(ASTNode* node)
{
    if (node == NULL) return;
    switch (node->type) {
        //case AST_FUNC_DEF:           array_destroy(&((ASTFuncDef*)node)-> >members); break;
    case AST_FUNC_CALL: array_destroy(&((ASTFuncCall*)node)->arguments); break;
    case AST_BLOCK: array_destroy(&((ASTBlock*)node)->list); break;
    case AST_MULTI_VAR_DECL: array_destroy(&((ASTMultiVarDecl*)node)->names); break;
    case AST_SWITCH: array_destroy(&((ASTSwitch*)node)->cases); break;
    default: break;
    }
    // Node storage is released in bulk by astArena.
}

void freeAllASTNodes(CompilerDef* compiler)
{
    for (int i32 = 0; i32 < array_size(&compiler->ASTs); ++i32)
    {
        ASTNode* node = *(ASTNode**)array_get(&compiler->ASTs, i32);
        freeASTNode(node);
    }
    array_destroy(&compiler->ASTs);
    arena_destroy(&compiler->astArena);
}

void freeStringArray(array* strings) {
    for (int i32 = 0; i32 < strings->count; ++i32) {
        free(*(char**)array_get(strings, i32));
    }
    array_destroy(strings);
}

void freeConstants(State* state) {
    array_destroy(&state->Constants);
}

void freeCompiler(CompilerDef* compiler) {
    if (!compiler) return;
    for (int i32 = 0; i32 < array_size(&compiler->symbols); ++i32)
    {
        Symbol* s = (Symbol*)array_get(&compiler->symbols, i32);

        // Check Class Definitions
        if (s->kind == SYM_TYPE_DEFINITION && s->type.baseType == T_CLASS)
        {
            array_destroy(&s->as.classDef.members);
        }
        // Check Functions (Use KIND, because type might be the return type)
        else if (s->kind == SYM_FUNCTION)
        {
            if (s->as.funcDef.paramTypes) {
                free(s->as.funcDef.paramTypes);
                s->as.funcDef.paramTypes = NULL;
            }
        }
    }
    array_destroy(&compiler->symbols);
    freeStringArray(&compiler->strings);
    freeAllASTNodes(compiler);
    array_destroy(&compiler->importedFiles);
    free(compiler);
}

void freeState(State* state) {
    if (!state) return;
    freeCompiler(state->compiler);
    state->compiler = NULL;
    freeConstants(state);
    array_destroy(&state->cFunctions);
    array_destroy(&state->cFunctionSignatures);
    array_destroy(&state->bytecode);
    arena_destroy(&state->irArena);
    free(state->source_filename);
    for (int i = 0; i < state->function_name_count; ++i)
        free(state->function_names[i]);
    free(state->function_names);
    for (size_t i = 0; i < state->source_map_count; ++i)
        free(state->source_map[i].filename);
    free(state->source_map);
    free(state);
}

// Helper struct for Backpatching
typedef struct {
    int instruction_index; // Index in bytecode array
    int label_id;          // Label to resolve
    int start_pc;          // PC of the jump instruction
    OPCode op;             // Opcode (to distinguish patch format)
} JumpPatch;

// Helper to calculate padding for string packing
int pad_bytes(int size) {
    int remainder = size % 4;
    return (remainder == 0) ? 0 : (4 - remainder);
}

static bool appendSourceMapEntry(State* state, uint32_t bytecodeOffset, Loc loc)
{
    if (!state || loc.pos.line <= 0) return true;
    if (state->source_map_count == state->source_map_capacity) {
        size_t capacity = state->source_map_capacity ? state->source_map_capacity * 2 : 128;
        SourceMapEntry* entries = (SourceMapEntry*)realloc(state->source_map,
            capacity * sizeof(SourceMapEntry));
        if (!entries) return false;
        state->source_map = entries;
        state->source_map_capacity = capacity;
    }
    char* filename = NULL;
    const char* sourceName = loc.filename ? loc.filename : state->source_filename;
    if (sourceName) {
        size_t length = strlen(sourceName) + 1;
        filename = (char*)malloc(length);
        if (!filename) return false;
        memcpy(filename, sourceName, length);
    }
    SourceMapEntry* entry = &state->source_map[state->source_map_count++];
    entry->bytecode_offset = bytecodeOffset;
    entry->filename = filename;
    entry->line = loc.pos.line;
    entry->column = loc.pos.col;
    return true;
}


void assemble(State* state) {
    if (!state->ir_head) return;

    for (size_t i = 0; i < state->source_map_count; ++i)
        free(state->source_map[i].filename);
    free(state->source_map);
    state->source_map = NULL;
    state->source_map_count = 0;
    state->source_map_capacity = 0;

    for (int i = 0; i < state->function_name_count; ++i)
        free(state->function_names[i]);
    free(state->function_names);
    state->function_names = (char**)calloc(MAX_FUNCTIONS, sizeof(char*));
    state->function_name_count = 0;
    if (!state->function_names) {
        reportError(state->compiler, LOC_NONE, "Out of memory while recording function names");
        return;
    }

    // -------------------------------------------------------------------------
    // STEP 0: RESET & SERIALIZE CONSTANTS
    // -------------------------------------------------------------------------
    state->bytecode.count = 0; // Clear previous bytecode

    for (int i = 0; i < state->Constants.count; i++) {
        Value* v = (Value*)array_get(&state->Constants, i);

        if (v->type.baseType == T_INT) {
            // [OP] [32-bit Value]
            int32_t head = (OP_DEF_K_INT);
            array_append(&state->bytecode, &head);
            array_append(&state->bytecode, &v->v.bits);
        }
        else if (v->type.baseType == T_FLOAT) {
            // [OP] [32-bit Value]
            int32_t head = (OP_DEF_K_FLOAT);
            array_append(&state->bytecode, &head);
            array_append(&state->bytecode, &v->v.bits);
        }
        else if (v->type.baseType == T_BOOL) {
            // [OP | Value] (Packed)
            int32_t head = (OP_DEF_K_BOOL) | ((v->v.i32 ? 1 : 0) << 8);
            array_append(&state->bytecode, &head);
        }
        else if (v->type.baseType == T_CHAR) {
            // [OP | Value] (Packed)
            int32_t head = (OP_DEF_K_CHAR) | (((int)v->v.c) << 8);
            array_append(&state->bytecode, &head);
        }
        else if (v->type.baseType == T_STRING || v->type.baseType == T_CSTRING) {
            char* str = (char*)v->v.p;
            int len = (str) ? (int)strlen(str) : 0;

            // [OP] [Length] [Packed Chars...]
            int32_t head = (OP_DEF_K_STR);
            array_append(&state->bytecode, &head);
            array_append(&state->bytecode, &len);

            // Pack string chars into int32s
            int words = (len + 3) / 4;
            for (int w = 0; w < words; w++) {
                int32_t pack = 0;
                for (int b = 0; b < 4; b++) {
                    int charIdx = w * 4 + b;
                    if (charIdx < len) {
                        pack |= ((uint8_t)str[charIdx]) << (b * 8);
                    }
                }
                array_append(&state->bytecode, &pack);
            }
        }
    }

    // -------------------------------------------------------------------------
    // STEP 1: PRE-CALCULATE FUNCTION HEADERS
    // -------------------------------------------------------------------------

    // Save the index where function definitions start (so we don't overwrite constants)
    int function_header_start_index = state->bytecode.count;

    // Arrays to map Label IDs to Real Addresses and Function Indices
    int max_labels = state->next_label_id + 1;
    int* label_addresses = (int*)malloc(max_labels * sizeof(int));
    int* label_to_func_id = (int*)malloc(max_labels * sizeof(int));

    memset(label_addresses, -1, max_labels * sizeof(int));
    memset(label_to_func_id, -1, max_labels * sizeof(int));

    int function_count = 0;
    for (int i = 0; i < state->compiler->symbols.count; i++) {
        Symbol* sym = (Symbol*)array_get(&state->compiler->symbols, i);
        if (sym->kind == SYM_FUNCTION) {
            if (state->function_name_count < MAX_FUNCTIONS && sym->name) {
                size_t length = strlen(sym->name) + 1;
                state->function_names[state->function_name_count] = (char*)malloc(length);
                if (state->function_names[state->function_name_count])
                    memcpy(state->function_names[state->function_name_count], sym->name, length);
            }
            if (state->function_name_count < MAX_FUNCTIONS)
                state->function_name_count++;
            int label_id = sym->as.funcDef.start_address;

            // Map the label ID to the sequential Function Index (0, 1, 2...)
            if (label_id > 0 && label_id < max_labels) {
                label_to_func_id[label_id] = function_count;
            }

            // Emit Placeholder Header: [OP]
            // We will patch the address later in Step 3B
            int32_t placeholder = OP_FUNC_DEF;
            array_append(&state->bytecode, &placeholder);

            function_count++;
        }
    }

    // -------------------------------------------------------------------------
    // STEP 2: EMIT CODE
    // -------------------------------------------------------------------------

    // Start tracking PC from the current point (After Constants + Headers)
    int current_pc = state->bytecode.count;

    array patches = array_create(sizeof(JumpPatch));
    IRNode* node = state->ir_head;

    while (node) {
        if (node->data.type == IR_LABEL_DEF) {
            int id = node->data.label_id;
            if (id > 0 && id < max_labels) {
                label_addresses[id] = current_pc;
            }
        }
        else if (node->data.type == IR_INSTRUCTION) {
            IREntry* ir = &node->data;
            int32_t instruction = 0;
            if (!appendSourceMapEntry(state, (uint32_t)current_pc, ir->loc)) {
                reportError(state->compiler, ir->loc,
                    "Out of memory while recording source locations");
                free(label_addresses);
                free(label_to_func_id);
                array_destroy(&patches);
                return;
            }

            if (isForStepJump(ir->op)) {
                instruction = ir->op | ((ir->a & 0xff) << 8) | ((ir->c & 0xff) << 16);
                array_append(&state->bytecode, &instruction);
                int32_t bound = ((Value*)state->Constants.data)[ir->b].v.i32;
                array_append(&state->bytecode, &bound);
                int32_t displacement = 0;
                JumpPatch patch = { current_pc + 2, ir->label_id, current_pc, ir->op };
                array_append(&patches, &patch);
                array_append(&state->bytecode, &displacement);
                current_pc += 3;
            }
            else if (ir->op == OP_CALL) {
                // Standard 8-bit Call
                // Format: [OP] [A:Prim] [B:FuncID] [C:Ref]
                int func_id = 0;
                if (ir->b > 0 && ir->b < max_labels) {
                    func_id = label_to_func_id[ir->b];
                    if (func_id == -1) func_id = 0; // Error fallback
                }

                instruction = (ir->op) | ((ir->a & 0xFF) << 8) |
                    ((func_id & 0xFF) << 16) |((ir->c & 0xFF) << 24);

                array_append(&state->bytecode, &instruction);
                current_pc++;
            }
            else if (ir->op == OP_CALL32) {
                // Extended 32-bit Call
                // Word 1: [OP] [A:Prim] [B:Ref] [Unused]
                // Word 2: [Function ID (32-bit)]

                instruction = (ir->op) |
                    ((ir->a & 0xFF) << 8) |
                    ((ir->b & 0xFF) << 16); // Note: Ref offset is in B for CALL32

                array_append(&state->bytecode, &instruction);

                // Resolve ID
                int func_id = 0;
                if (ir->c > 0 && ir->c < max_labels) {
                    func_id = label_to_func_id[ir->c];
                }
                // Append Word 2
                array_append(&state->bytecode, &func_id);

                current_pc += 2; // Occupies 2 slots
            }
            else if (ir->label_id > 0) {
                // JUMPS (Needs Backpatching)
                int target_addr = label_addresses[ir->label_id];
                int offset = 0;

                // If label is defined and behind us, we can calc offset now.
                // If it's forward (target_addr == -1), we must patch later.
                bool needs_patch = (target_addr == -1);

                if (!needs_patch) {
                    offset = target_addr - current_pc;
                }
                else {
                    JumpPatch p = { current_pc, ir->label_id, current_pc, ir->op };
                    array_append(&patches, &p);
                }

                // Emit instruction with placeholder (or final) offset
                if (isComparisonJump(ir->op)) {
                    // ABC Format: C is offset (8-bit)
                    instruction = (ir->op) |((ir->a & 0xFF) << 8) |((ir->b & 0xFF) << 16) |((offset & 0xFF) << 24);
                }
                else {
                    // AD Format: D is offset (16-bit)
                    instruction = (ir->op) |((ir->a & 0xFF) << 8) |((offset & 0xFFFF) << 16);
                }
                array_append(&state->bytecode, &instruction);
                current_pc++;
            }
            else if (ir->is_AD) {
                // Standard AD Format (e.g. OP_LOAD_K, OP_JUMP)
                instruction = (ir->op) |((ir->a & 0xFF) << 8) |((ir->c & 0xFFFF) << 16); // D is stored in ir->c usually
                array_append(&state->bytecode, &instruction);
                current_pc++;
            }
            else {
                // Standard ABC Format
                instruction = (ir->op) |((ir->a & 0xFF) << 8) |((ir->b & 0xFF) << 16) |((ir->c & 0xFF) << 24);
                array_append(&state->bytecode, &instruction);
                current_pc++;
            }
        }

        node = node->next;
    }

    // -------------------------------------------------------------------------
    // STEP 3: BACKPATCHING
    // -------------------------------------------------------------------------

    // A. Patch Jumps
    for (int i = 0; i < patches.count; i++) {
        JumpPatch* p = (JumpPatch*)array_get(&patches, i);
        int target_addr = label_addresses[p->label_id];

        if (target_addr != -1) {
            int offset = target_addr - p->start_pc;

            // Adjust index to account for Constants + Headers
            // Note: The 'instruction_index' stored in patch was essentially absolute
            // because we started appending to bytecode array directly.
            // However, p->instruction_index might be relative to where we started emitting code?
            // NO, in the loop above, 'array_append' adds to the end of the global bytecode array.
            // 'current_pc' tracks the absolute index.
            // So p->instruction_index IS the absolute index in state->bytecode.

            int32_t* code_ptr = (int32_t*)array_get(&state->bytecode, p->instruction_index);
            int32_t inst = *code_ptr;

            if (isForStepJump(p->op)) {
                *code_ptr = offset;
            }
            else if (isComparisonJump(p->op)) {
                // ABC Format: Offset in C (Top 8 bits)
                // Range check: -128 to 127
                if (offset < -128 || offset > 127) {
                    printf("Error: Short Jump overflow at index %d (Offset: %d)\n", p->instruction_index, offset);
                }
                *code_ptr = (inst & 0x00FFFFFF) | ((offset & 0xFF) << 24);
            }
            else {
                // AD Format: Offset in D (Top 16 bits)
                *code_ptr = (inst & 0x0000FFFF) | ((offset & 0xFFFF) << 16);
            }
        }
        else {
            printf("Error: Undefined Label ID %d\n", p->label_id);
        }
    }

    // B. Patch Function Headers
    // Iterate symbols again to match the order we emitted placeholders
    int func_idx = 0;
    for (int i = 0; i < state->compiler->symbols.count; i++) {
        Symbol* sym = (Symbol*)array_get(&state->compiler->symbols, i);
        if (sym->kind == SYM_FUNCTION) {
            int label_id = sym->as.funcDef.start_address;
            int real_addr = label_addresses[label_id];

            // Calculate exact index in bytecode array
            int bytecode_index = function_header_start_index + func_idx;

            int32_t* header_inst = (int32_t*)array_get(&state->bytecode, bytecode_index);

            OPCode defOp = OP_FUNC_DEF;
            if (strcmp(sym->name, "setup") == 0) defOp = OP_DEF_SETUP;
            else if (strcmp(sym->name, "update") == 0) defOp = OP_DEF_UPDATE;
            else if (strcmp(sym->name, "fixedUpdate") == 0) defOp = OP_DEF_FIXED_UPDATE;
            else if (strcmp(sym->name, "onTriggerEnter") == 0) defOp = OP_DEF_TRIGGER_ENTER;
            else if (strcmp(sym->name, "onTriggerStay") == 0) defOp = OP_DEF_TRIGGER_STAY;
            else if (strcmp(sym->name, "onTriggerExit") == 0) defOp = OP_DEF_TRIGGER_EXIT;
            else if (strcmp(sym->name, "onNetworkRemote") == 0) defOp = OP_DEF_NETWORK_REMOTE;

            // Encode: [Address:24] [Op:8]
            *header_inst = (defOp) | ((real_addr & 0xFFFFFF) << 8);

            func_idx++;
        }
    }

    // Cleanup
    state->ir_head = NULL;
    state->ir_tail = NULL;
    arena_reset(&state->irArena);
    free(label_addresses);
    free(label_to_func_id);
    array_destroy(&patches);
}
