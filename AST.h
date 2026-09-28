#ifndef AST_H
#define AST_H

#include "array.h" // Assuming your dynamic array implementation is in this header
#include "types.h" // For TypeID and Value definitions
#include "lexer.h"

// Defines the different kinds of nodes in the AST
typedef enum {
    AST_CONST,
    AST_VAR,
    AST_BINARY_OP,
    AST_UNARY_OP,
    AST_IF,
    AST_FOR,
    AST_WHILE,
    AST_SWITCH,
    AST_CASE,
    AST_BLOCK,
    AST_MULTI_VAR_DECL,
    AST_FUNC_DEF,
    AST_FUNC_CALL,
    AST_NEW,
    AST_RETURN,
    AST_BREAK,
    AST_CONTINUE,
    AST_ARRAY_ACCESS,
    AST_CLASS_DEF,
    AST_MEMBER_ACCESS
} ASTType;

// --- Base AST Node Structure ---
typedef struct {
    ASTType type;
    Loc loc; // For error reporting, if needed
} ASTNode;


// --- Specific AST Node Structures ---

typedef struct {
    ASTNode node; // AST_CONST
    Value value;
} ASTConst;

typedef struct {
    ASTNode node;    // AST_VAR
    char* name;
    ASTNode* index;
} ASTVar;

typedef struct {
    ASTNode node; // AST_BINARY_OP
    TTOKEN op;
    ASTNode* left;
    ASTNode* right;
} ASTBinaryOp;

typedef struct {
    ASTNode node; // AST_UNARY_OP
    TTOKEN op;
    ASTNode* operand;
} ASTUnaryOp;

typedef struct {
    ASTNode node; // AST_IF
    ASTNode* condition;
    ASTNode* thenBlock;
    ASTNode* elseBlock; // Can be NULL
} ASTIfStmt;

typedef struct {
    ASTNode node; // AST_BLOCK
    array list; // array of ASTNode*
} ASTBlock;

typedef struct {
    ASTNode node; // AST_VAR_DECL
    char* name;
    TypeID varType;
    int count;
    ASTNode* init;
	ASTNode* methodNode; // If this member is a method, it points to the ASTFuncDef nodemethodNode
} ASTVarDecl;

// New helper struct
typedef struct {
    char* name; // Interned string
    Loc loc;    // Location of this specific variable
} VariableInfo;

typedef struct {
    ASTNode node;     // Base node
    array names;      // array of VariableInfo
    TypeID varType;
    ASTNode* init;    // The single initializer, (can be NULL)
} ASTMultiVarDecl;

typedef struct {
    ASTNode node; // AST_FOR
    ASTNode* init;
    ASTNode* condition;
    ASTNode* increment;
    ASTNode* body;
} ASTFor;

typedef struct {
    ASTNode node; // AST_RETURN
    ASTNode* retval;
} ASTReturn;

// Represents a member within a struct definition
typedef struct {
    ASTNode node;
    char* name;
    TypeID type;
	ASTNode* methodNode; // If this member is a method, it points to the ASTFuncDef nodemethodNode
    int count; // For array members like 'char name[10]'
} ASTStructMember;

// Represents the definition of a struct
typedef struct {
    ASTNode node; // AST_CLASS_DEF
    char* name;
    //array members; // array of ASTStructMember
    ASTBlock* members;
} ASTClassDef;

// Represents accessing a member, e.g., myVar.field
typedef struct {
    ASTNode node; // AST_MEMBER_ACCESS
    ASTNode* classNode; // The variable being accessed (e.g., myVar)
    char* memberName;
} ASTMemberAccess;

typedef struct {
    ASTNode node; // AST_FUNC_DEF
    char* name;
    ASTNode* params, // Array of ASTParam structs
		*body;       // Body is usually an AST_BLOCK
    TypeID returnType;
    char* borrowedReturnParam; // Explicit source for `borrow(param.path) T` returns.
    char* borrowedReturnPath;  // Dot-separated fields; `[]` denotes any array element.
    int start_label_id;
} ASTFuncDef;

typedef struct {
    ASTNode node; // AST_FUNC_CALL
	ASTNode* callee; // The function being called (e.g., myFunc)
	array arguments; // array of ASTNode* for arguments
} ASTFuncCall;

// In your AST header file (e.g., ast.h)

// An enum to distinguish what is being created
typedef enum {
    NEW_OBJECT,
    NEW_ARRAY
} NewNodeType;

typedef struct {
    ASTNode node;        // Base node properties (type, loc)
    NewNodeType newType; // Is it an object or an array?
    TypeID typeToCreate; // The type being instantiated (e.g., 'Player' or 'int')

    // A union to hold the different kinds of data
    union {
        array arguments;   // For NEW_OBJECT with constructor call, e.g., new Player()
        ASTNode* arraySize; // For NEW_ARRAY, e.g., new int[10]
    } as;
} ASTNewNode;

typedef struct ASTCase {
    ASTNode node; // AST_CASE
    ASTNode* value;   // NULL if this is 'default'
    ASTNode* body;    // Usually an AST_BLOCK
} ASTCase;

typedef struct ASTSwitch {
    ASTNode node; // AST_SWITCH
    ASTNode* expression,
        *defaultCase;
    array cases;      // Array of ASTCase*
} ASTSwitch;

typedef struct ASTWhile{
    ASTNode node; // AST_SWITCH
    ASTNode* condition, *body;
} ASTWhile;

typedef struct {
    ASTNode node;
    ASTNode* array; // The target (e.g., this.shape)
    ASTNode* index; // The index (e.g., i32)
} ASTArrayAccess;

// --- AST Node Creation Functions (Prototypes) ---
ASTNode* createASTConst     (CompilerDef* compiler, Loc loc, TypeIDEnum typeEnum, ...);
ASTNode* createVar          (CompilerDef* compiler, Loc loc, char* name, ASTNode* index);
ASTNode* createStructDef    (CompilerDef* compiler, Loc loc, char* name, ASTBlock* members);
ASTNode* createBinaryOp     (CompilerDef* compiler, Token op, ASTNode* left, ASTNode* right);
ASTNode* createUnaryOp      (CompilerDef* compiler, Token op, ASTNode* operand);
ASTNode* createIfStmt       (CompilerDef* compiler, Loc loc, ASTNode* condition, ASTNode* thenBlock, ASTNode* elseBlock);
ASTBlock* createBlock       (CompilerDef* compiler, Loc loc);
void addStatement           (ASTBlock* block, ASTNode* statement);
ASTNode* createForStmt      (CompilerDef* compiler, Loc loc, ASTNode* init, ASTNode* condition, ASTNode* increment, ASTNode* body);
ASTNode* createMemberAccess (CompilerDef* compiler, ASTNode* classNode, Token memberToken);
ASTNode* createFuncCall     (CompilerDef* compiler, ASTNode* callee, Token openParen, array arguments);
ASTNode* createReturnStmt   (CompilerDef* compiler, Loc loc, ASTNode* retval);
ASTNode* createFuncDef      (CompilerDef* compiler, Loc loc, char *name, TypeID returnType, ASTNode* params, ASTNode* body);
ASTNode* createNewObjectNode(CompilerDef* compiler, Token newTok, TypeID type);
ASTNode* createNewArrayNode (CompilerDef* compiler, Token newTok, TypeID type, ASTNode* size);
ASTNode* createMultiVarDecl (CompilerDef* compiler, Loc loc, array names, TypeID varType, ASTNode* init);
ASTNode* createCaseNode     (CompilerDef* compiler, Loc loc, ASTNode* caseValue, ASTNode* caseBody);
ASTNode* createSwitchStmt   (CompilerDef* compiler, Loc loc, ASTNode* switchExpr, array* cases, ASTNode* defaultCase);
ASTNode* createWhileStmt    (CompilerDef* compiler, Loc loc, ASTNode* caseValue, ASTNode* caseBody);
ASTNode* createBreakNode    (CompilerDef* compiler, Loc loc);
ASTNode* createContinueNode (CompilerDef* compiler, Loc loc);
ASTNode* createArrayAccess  (CompilerDef* compiler, Loc loc, ASTNode* arr, ASTNode* idx);

#endif // AST_H
