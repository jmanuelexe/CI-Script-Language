#include "AST.h"
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include "CodeGen.h" // For the State struct definition

// --- Utility Functions ---

// A helper function to allocate a node and add it to the tracking list
static ASTNode* allocateNode(CompilerDef *compiler, ASTType type, size_t count, Loc loc)
{
    ASTNode* node = (ASTNode*)arena_alloc(&compiler->astArena, count, sizeof(void*));
    if (!node) {
        perror("Failed to allocate AST node");
        return NULL;
    }
    node->type = type;
	node->loc = loc; // Store the location for error reporting
    // Add the new node to the list for later cleanup
    array_append(&compiler->ASTs, &node);
    return node;
}

// Provide a basic implementation of _strdup if not available (e.g., on Linux)
#ifndef _WIN32
char* _strdup(const char* s) {
    if (s == NULL) return NULL;
    size_t len = strlen(s) + 1;
    char* new_s = malloc(len);
    if (new_s == NULL) return NULL;
    return (char*)memcpy(new_s, s, len);
}
#endif


// --- AST Node Creation Functions ---
ASTNode* createASTConst(CompilerDef* compiler, Loc loc, TypeIDEnum typeEnum, ...) {
    ASTConst* node = (ASTConst*)allocateNode(compiler, AST_CONST, sizeof(ASTConst), loc);

    node->value.type.baseType = typeEnum;
    node->value.type.name = NULL;
    va_list args;
    va_start(args, typeEnum);

    switch (typeEnum) {
    case T_INT:
    case T_BOOL:
    case T_SHORT:
        node->value.v.i32 = va_arg(args, int);
        break;
    case T_CHAR:
        node->value.v.c = (char)va_arg(args, int);
        break;
    case T_FLOAT:
        node->value.v.f32 = (float)va_arg(args, double);
        break;
    case T_STRING:
        node->value.v.p = (void*)internStr(compiler, va_arg(args, char*));
        break;
    default:
        break;
    }

    va_end(args);
    return (ASTNode*)node;
}

ASTNode* createVar(CompilerDef* compiler, Loc loc, char * name, ASTNode* index) {
    ASTVar* node = (ASTVar*)allocateNode(compiler, AST_VAR, sizeof(ASTVar), loc);
    if (!node) return NULL;
    node->name = name;
    node->index = index;
    return (ASTNode*)node;
}

// In ast.c
ASTNode* createStructDef(CompilerDef* compiler, Loc loc, char* name, ASTBlock* members) {
    // Allocate memory for the new node and add it to the tracking list
    ASTClassDef* node = (ASTClassDef*)allocateNode(compiler, AST_CLASS_DEF, sizeof(ASTClassDef), loc);
    if (!node) return NULL;
    node->name = name;
    node->members = members;

    return (ASTNode*)node;
}

ASTNode* createBinaryOp(CompilerDef* compiler, Token op, ASTNode* left, ASTNode* right) {
    ASTBinaryOp* node = (ASTBinaryOp*)allocateNode(compiler, AST_BINARY_OP, sizeof(ASTBinaryOp), op.loc);
    if (!node) return NULL;

    node->op = op.type;
    node->left = left;
    node->right = right;
    return (ASTNode*)node;
}

ASTNode* createUnaryOp(CompilerDef* compiler, Token op, ASTNode* operand) {
    ASTUnaryOp* node = (ASTUnaryOp*)allocateNode(compiler, AST_UNARY_OP, sizeof(ASTUnaryOp), op.loc);
	if (!node) return NULL;
    node->op = op.type;
    node->operand = operand;
    return (ASTNode*)node;
}

ASTNode* createIfStmt(CompilerDef* compiler, Loc loc, ASTNode* condition, ASTNode* thenBlock, ASTNode* elseBlock) {
    ASTIfStmt* node = (ASTIfStmt*)allocateNode(compiler, AST_IF, sizeof(ASTIfStmt), loc);
    if (!node) return NULL;
    node->condition = condition;
    node->thenBlock = thenBlock;
    node->elseBlock = elseBlock;
    return (ASTNode*)node;
}

ASTBlock* createBlock(CompilerDef* compiler, Loc loc) {
    ASTBlock* node = (ASTBlock*)allocateNode(compiler, AST_BLOCK, sizeof(ASTBlock), loc);
    if (!node) return NULL;
    node->list = array_create(sizeof(ASTNode*));
    return node;
}

void addStatement(ASTBlock* block, ASTNode* statement) {
    if (block && statement) {
        array_append(&block->list, &statement);
    }
}

ASTNode* createForStmt(CompilerDef* compiler, Loc loc, ASTNode* init, ASTNode* condition, ASTNode* increment, ASTNode* body) {
    ASTFor* node = (ASTFor*)allocateNode(compiler, AST_FOR, sizeof(ASTFor), loc);
    if (!node) return NULL;
    node->init = init;
    node->condition = condition;
    node->increment = increment;
    node->body = body;
    return (ASTNode*)node;
}

ASTNode* createMemberAccess(CompilerDef* compiler, ASTNode* classNode, Token memberToken) {
    ASTMemberAccess* node = (ASTMemberAccess*)allocateNode(compiler, AST_MEMBER_ACCESS, sizeof(ASTMemberAccess), memberToken.loc);
    if (!node) return NULL;

    node->classNode = classNode;	
    node->memberName = internString(compiler, &memberToken.text);
    return (ASTNode*)node;
}

// In ast.c

/**
 * @brief Creates a new AST node to represent a function call.
 * @param state The current compiler state, used for memory allocation tracking.
 * @param callee The AST node representing the function to be called (e.g., an AST_VAR).
 * @param arguments A dynamic array of ASTNode* representing the arguments.
 * @return A pointer to the newly created ASTFuncCall node.
 */
ASTNode* createFuncCall(CompilerDef* compiler, ASTNode* callee, Token openParen, array arguments) {
    // Allocate memory for the new node and add it to the tracking list
    ASTFuncCall* node = (ASTFuncCall*)allocateNode(compiler, AST_FUNC_CALL, sizeof(ASTFuncCall), openParen.loc);
	if (node == NULL) {
		fprintf(stderr, "Error: Failed to allocate memory for ASTFuncCall node.\n");
		return NULL;
	}
    // The 'callee' is the expression that evaluates to a function
    node->callee = callee;
    // The 'arguments' array, already populated by the parser, is moved into the node.
    node->arguments = arguments;

    return (ASTNode*)node;
}

ASTNode* createReturnStmt(CompilerDef* compiler, Loc loc, ASTNode* retval) {
	// Allocate memory for the new node and add it to the tracking list
	ASTReturn* node = (ASTReturn*)allocateNode(compiler, AST_RETURN, sizeof(ASTReturn), loc);
	if (node == NULL) {
		fprintf(stderr, "Error: Failed to allocate memory for ASTReturn node.\n");
		return NULL;
	}
	// The 'retval' is the expression to return
	node->retval = retval;
	return (ASTNode*)node;
}

/**
 * @brief Creates a new AST node to represent a function or method definition.
 * @param state The current compiler state, used for memory allocation tracking.
 * @param name A TString representing the function's name.
 * @param params A dynamic array of ASTParam structs for the function's parameters.
 * @param body The AST node for the function's body (usually an AST_BLOCK).
 * @return A pointer to the newly created ASTFuncDef node.
 */
ASTNode* createFuncDef(CompilerDef* compiler, Loc loc, char* name, TypeID returnType, ASTNode* params, ASTNode* body) {
    // Allocate memory for the new node and add it to the tracking list
    ASTFuncDef* node = (ASTFuncDef*)allocateNode(compiler, AST_FUNC_DEF, sizeof(ASTFuncDef), loc);
    if (!node) return NULL;

    // Set the function's name, interning the string
    node->name = name;
    node->returnType = returnType;
    node->borrowedReturnParam = NULL;
    node->borrowedReturnPath = NULL;
    // The 'params' array, already populated by the parser, is moved into the node.
    node->params = params;

    // The 'body' is a pointer to the AST node representing the function's code.
    node->body = body;

    return (ASTNode*)node;
}

// Constructor for Objects: "new Player"
ASTNode* createNewObjectNode(CompilerDef* compiler, Token newTok, TypeID type) {
    ASTNewNode* node = (ASTNewNode*)allocateNode(compiler, AST_NEW, sizeof(ASTNewNode), newTok.loc);
    if (!node) return NULL;

    node->newType = NEW_OBJECT;
    node->typeToCreate = type;
    node->as.arraySize = NULL; // No size for objects

    return (ASTNode*)node;
}

// Constructor for Arrays: "new int[10]"
ASTNode* createNewArrayNode(CompilerDef* compiler, Token newTok, TypeID type, ASTNode* size) {
    ASTNewNode* node = (ASTNewNode*)allocateNode(compiler, AST_NEW, sizeof(ASTNewNode), newTok.loc);
    if (!node) return NULL;

    node->newType = NEW_ARRAY;
    node->typeToCreate = type;
    node->as.arraySize = size;

    return (ASTNode*)node;
}

ASTNode* createMultiVarDecl(CompilerDef* compiler, Loc loc, array names, TypeID varType, ASTNode* init)
{
	ASTMultiVarDecl* node = (ASTMultiVarDecl*)allocateNode(compiler, AST_MULTI_VAR_DECL, sizeof(ASTMultiVarDecl), loc);
    if (!node) return NULL;
	node->names = names;
	node->varType = varType;
	node->init = init;
	return (ASTNode*)node;
}


ASTNode* createCaseNode(CompilerDef* compiler, Loc loc, ASTNode* caseValue, ASTNode* caseBody) {
    ASTCase* caseNode = (ASTCase*)allocateNode(compiler, AST_CASE, sizeof(ASTCase),loc);
    if (!caseNode) {
        compiler->onError(LOC_NONE, "Memory allocation failed for case node.\n", compiler);
        return NULL;
    }
    caseNode->value = caseValue;
    caseNode->body = caseBody;
    //caseNode->data = caseData;
    return (ASTNode*)caseNode;
}

ASTNode* createSwitchStmt(CompilerDef* compiler, Loc loc, ASTNode *switchExpr, array *cases, ASTNode* defaultCase)
{
    ASTSwitch* node = (ASTSwitch*)allocateNode(compiler, AST_SWITCH, sizeof(ASTSwitch), loc);
	if (!node) return NULL;
	node->expression = switchExpr;
    node->cases = *cases;
	node->defaultCase = defaultCase;
	return (ASTNode*)node;
}

ASTNode* createWhileStmt(CompilerDef* compiler, Loc loc, ASTNode* condition, ASTNode* Body)
{
    ASTWhile* node = (ASTWhile*)allocateNode(compiler, AST_WHILE, sizeof(ASTWhile), loc);
	node->body = Body;
	node->condition = condition;
    return (ASTNode*)node;
}

ASTNode* createBreakNode(CompilerDef* compiler, Loc loc)
{
    ASTNode* node = allocateNode(compiler, AST_BREAK, sizeof(ASTNode), loc);
    return node;
}

ASTNode* createContinueNode(CompilerDef* compiler, Loc loc)
{
    ASTNode* node = allocateNode(compiler, AST_CONTINUE, sizeof(ASTNode), loc);
    return node;
}

ASTNode* createArrayAccess(CompilerDef* compiler, Loc loc, ASTNode* arr, ASTNode* idx) {
    ASTArrayAccess* node = (ASTArrayAccess*)allocateNode(compiler, AST_ARRAY_ACCESS, sizeof(ASTArrayAccess), loc);
    if (!node) return NULL;

    node->array = arr;
    node->index = idx;

    return (ASTNode*)node;
}
