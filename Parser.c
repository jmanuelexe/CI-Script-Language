#include "Parser.h"
#include "Lexer.h"
#include "SymbolTable.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ===================================================================
// MEMORY & ARRAY HELPERS
// ===================================================================

#define ARRAY_NEW(type) array_new_impl(sizeof(type))

static array* array_new_impl(int elem_size) {
    array* a = (array*)malloc(sizeof(array));
    if (!a) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    *a = array_create(elem_size);
    return a;
}

#define ARRAY_FREE(arr_ptr) do { \
    if (arr_ptr) { array_destroy(arr_ptr); free(arr_ptr); } \
} while(0)

// ===================================================================
// TYPE DEFINITIONS & FORWARD DECLARATIONS
// ===================================================================

typedef enum {
    PREC_NONE = 0,
    PREC_ASSIGNMENT = 1,  // = += -=
    PREC_OR = 2,          // ||
    PREC_AND = 3,         // &&
    PREC_EQUALITY = 4,    // == !=
    PREC_COMPARISON = 5,  // < > <= >=
    PREC_TERM = 6,        // + -
    PREC_FACTOR = 8,      // * /  <--- Changed from 7 to 8 (Gap created)
    PREC_UNARY = 9,       // ! - ++ --
    PREC_CALL = 10,       // . () []
    PREC_PRIMARY = 11
} Precedence;

typedef ASTNode* (*PrefixFn)(Parser*);
typedef ASTNode* (*InfixFn)(Parser*, ASTNode*);

typedef struct {
    PrefixFn   prefix;
    InfixFn    infix;
    Precedence prec;
} Rule;

static ASTNode* statement(Parser* p);
static ASTNode* expression(Parser* p);
static ASTNode* grouping(Parser* p);
static ASTNode* unary(Parser* p);
static ASTNode* binary(Parser* p, ASTNode* left);
static ASTNode* call(Parser* p, ASTNode* left);
static ASTNode* subscript(Parser* p, ASTNode* left);
static ASTNode* member(Parser* p, ASTNode* left);
static ASTNode* parseNew(Parser* p);
static ASTNode* primary(Parser* p);
static ASTNode* postfix(Parser* p, ASTNode* left);
static TypeID   parseType(Parser* p);
static void advance(Parser* p);
static int check(Parser* p, TTOKEN t);
static int match(Parser* p, TTOKEN t);
static void consume(Parser* p, TTOKEN t, const char* msg);

static void parseEnum(Parser* p) {
    Loc enumLoc = p->token.loc;
    consume(p, TK_ENUM, "enum");
    if (!check(p, TK_IDENTIFIER)) {
        reportError(p->state->compiler, p->token.loc, "Expected enum name.");
        return;
    }
    char* enumName = internString(p->state->compiler, &p->token.text);
    advance(p);
    consume(p, TK_OPENCURLYBRACKET, "{");

    int nextValue = 0;
    while (!check(p, TK_CLOSECURLYBRACKET) && !check(p, TK_EOF)) {
        if (!check(p, TK_IDENTIFIER)) {
            reportError(p->state->compiler, p->token.loc, "Expected enum member name.");
            advance(p);
            continue;
        }
        char* memberName = internString(p->state->compiler, &p->token.text);
        advance(p);
        if (match(p, TK_ASSIGN)) {
            int sign = match(p, TK_MINUS) ? -1 : 1;
            if (!check(p, TK_INTEGER)) {
                reportError(p->state->compiler, p->token.loc,
                    "Enum values must be integer constants.");
            } else {
                char buffer[32];
                size_t length = (size_t)(p->token.text.end - p->token.text.begin);
                if (length >= sizeof(buffer)) length = sizeof(buffer) - 1;
                memcpy(buffer, p->token.text.begin, length);
                buffer[length] = '\0';
                nextValue = sign * (int)strtol(buffer, NULL, 0);
                advance(p);
            }
        }
        char qualified[256];
        snprintf(qualified, sizeof(qualified), "%s.%s", enumName, memberName);
        addConstantInt(p->state, qualified, nextValue++);
        if (!match(p, TK_COMMA)) break;
    }
    consume(p, TK_CLOSECURLYBRACKET, "}");
    match(p, TK_SEMICOLON);
    (void)enumLoc;
}

// ===================================================================
// FILE I/O & IMPORT HELPERS
// ===================================================================

char* readFile(const char* path) {
    FILE* file = fopen(path, "rb");
    if (!file) return NULL;

    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);

    char* buffer = (char*)malloc(length + 1);
    if (!buffer) {
        fclose(file);
        return NULL;
    }

    fread(buffer, 1, length, file);
    buffer[length] = '\0';

    fclose(file);
    return buffer;
}

static int isFileImported(State* state, const char* path) {
    array* imported = &state->compiler->importedFiles;
    for (int i32 = 0; i32 < array_size(imported); ++i32) {
        char* existing = *(char**)array_get(imported, i32);
        if (strcmp(existing, path) == 0) return 1;
    }
    return 0;
}

// ===================================================================
// PARSE RULES TABLE
// ===================================================================

static const Rule rules[] = {
    [TK_THIS] = { primary,    NULL,       PREC_NONE },
    [TK_NULL] = { primary,    NULL,       PREC_NONE },
    [TK_IDENTIFIER] = { primary,    NULL,       PREC_NONE },
    [TK_TRUE] = { primary,    NULL,       PREC_NONE },
    [TK_FALSE] = { primary,    NULL,       PREC_NONE },
    [TK_INTEGER] = { primary,    NULL,       PREC_NONE },
    [TK_FLOATS] = { primary,    NULL,       PREC_NONE },
    [TK_STRING] = { primary,    NULL,       PREC_NONE },
    [TK_OPENPARENTHESIS] = { grouping,   call,       PREC_CALL },
    [TK_OPENBRACKET] = { NULL,       subscript,  PREC_CALL },
    [TK_DOT] = { NULL,       member,     PREC_CALL },
    [TK_PLUS] = { unary,      binary,     PREC_TERM },
    [TK_MINUS] = { unary,      binary,     PREC_TERM },
    [TK_NOT] = { unary,      NULL,       PREC_UNARY },
    [TK_MOVE] = { unary,     NULL,       PREC_UNARY },
    [TK_MULT] = { NULL,       binary,     PREC_FACTOR },
    [TK_DIV] = { NULL,       binary,     PREC_FACTOR },
    [TK_INC] = { unary,      postfix,    PREC_UNARY },
    [TK_DEC] = { unary,      postfix,    PREC_UNARY },
    [TK_LT] = { NULL,       binary,     PREC_COMPARISON },
    [TK_GT] = { NULL,       binary,     PREC_COMPARISON },
    [TK_LE] = { NULL,       binary,     PREC_COMPARISON },
    [TK_GE] = { NULL,       binary,     PREC_COMPARISON },
    [TK_EQ] = { NULL,       binary,     PREC_EQUALITY },
    [TK_NE] = { NULL,       binary,     PREC_EQUALITY },
    [TK_AND] = { NULL,       binary,     PREC_AND },
    [TK_OR] = { NULL,       binary,     PREC_OR },
    [TK_ASSIGN] = { NULL,       binary,     PREC_ASSIGNMENT },
    [TK_ADD_ASSIGN] = { NULL,       binary,     PREC_ASSIGNMENT },
    [TK_SUB_ASSIGN] = { NULL,       binary,     PREC_ASSIGNMENT },
    [TK_MULT_ASSIGN] = { NULL,      binary,     PREC_ASSIGNMENT },
    [TK_DIVEQ] = { NULL,            binary,     PREC_ASSIGNMENT },
    [TK_NEW] = { parseNew,   NULL,       PREC_NONE },
    [TK_EOF] = { NULL,       NULL,       PREC_NONE }
};

static const Rule* getRule(TTOKEN t) {
    if (t < 0 || t >= sizeof(rules) / sizeof(rules[0])) return NULL;
    return (rules[t].prefix || rules[t].infix) ? &rules[t] : NULL;
}

// ===================================================================
// TOKEN MANIPULATION
// ===================================================================

static void advance(Parser* p) {
    p->token = lex_getNextToken(p->lexer);
}

static int check(Parser* p, TTOKEN t) {
    return p->token.type == t;
}

static int match(Parser* p, TTOKEN t) {
    if (check(p, t)) {
        advance(p);
        return 1;
    }
    return 0;
}

static void consume(Parser* p, TTOKEN t, const char* msg) {
    if (!match(p, t)) {
        reportError(p->state->compiler, p->token.loc, "Expected %s, got '%.*s'", msg,
            (int)(p->token.text.end - p->token.text.begin), p->token.text.begin);
        advance(p);
    }
}

// ===================================================================
// EXPRESSION PARSING
// ===================================================================

static int isValueStart(TTOKEN type) {
    return (type == TK_INTEGER ||
        type == TK_FLOATS ||
        type == TK_STRING ||
        type == TK_IDENTIFIER ||
        type == TK_TRUE ||
        type == TK_FALSE ||
        type == TK_NEW ||
        type == TK_MINUS ||
        type == TK_NOT ||
        type == TK_OPENPARENTHESIS);
}

static ASTNode* parsePrecedence(Parser* p, Precedence prec) {
    const Rule* rule = getRule(p->token.type);
    if (!rule || !rule->prefix) {
        int size = (int)(p->token.text.end - p->token.text.begin);
        reportError(p->state->compiler, p->token.loc, "Expected expression but found '%.*s'", size, p->token.text.begin);
        advance(p);
        return NULL;
    }

    ASTNode* left = rule->prefix(p);

    while (1) {
        const Rule* r = getRule(p->token.type);
        if (!r || r->prec <= prec) break;
        left = r->infix(p, left);
    }
    return left;
}

static ASTNode* expression(Parser* p) {
    return parsePrecedence(p, PREC_NONE);
}

static ASTNode* primary(Parser* p) {
    Token t = p->token;
    advance(p);
    char* name;

    switch (t.type) {
    case TK_NULL:
        return createASTConst(p->state->compiler, t.loc, T_NIL, 0);
    case TK_THIS:
        name = internStr(p->state->compiler, "this");
        return createVar(p->state->compiler, t.loc, name, NULL);
    case TK_IDENTIFIER:
        name = internString(p->state->compiler, &t.text);
        return createVar(p->state->compiler, t.loc, name, NULL);
    case TK_STRING:
        name = internString(p->state->compiler, &t.text);
        return createASTConst(p->state->compiler, t.loc, T_STRING, name);
    case TK_TRUE:
        return createASTConst(p->state->compiler, t.loc, T_BOOL, 1);
    case TK_FALSE:
        return createASTConst(p->state->compiler, t.loc, T_BOOL, 0);
    case TK_INTEGER: {
        char buf[32];
        size_t n = t.text.end - t.text.begin;
        if (n >= 32) {
			reportError(p->state->compiler, t.loc, "Integer literal too long.");
            n = 31;
        }
        memcpy(buf, t.text.begin, n);
        buf[n] = '\0';
        return createASTConst(p->state->compiler, t.loc, T_INT, (int)strtol(buf, NULL, 0));
    }
    case TK_FLOATS: {
        char buf[32];
        size_t n = t.text.end - t.text.begin;
        if (n >= 32) n = 31;
        memcpy(buf, t.text.begin, n);
        buf[n] = '\0';
        return createASTConst(p->state->compiler, t.loc, T_FLOAT, strtof(buf, NULL));
    }
    default:
        reportError(p->state->compiler, t.loc, "Unexpected token in primary expression.");
        return NULL;
    }
}

static ASTNode* unary(Parser* p) {
    Token op = p->token;
    advance(p);
    return createUnaryOp(p->state->compiler, op, parsePrecedence(p, PREC_UNARY));
}

static ASTNode* binary(Parser* p, ASTNode* left) {
    Token op = p->token;
    advance(p);

    const Rule* r = getRule(op.type);
    int nextPrec = r->prec + 1;
    // Right-associative assignment operators
    if (op.type == TK_ASSIGN || op.type == TK_ADD_ASSIGN || op.type == TK_SUB_ASSIGN) {
        nextPrec = r->prec;
    }

    ASTNode* right = parsePrecedence(p, nextPrec);
    return createBinaryOp(p->state->compiler, op, left, right);
}

static ASTNode* postfix(Parser* p, ASTNode* left) {
    // advance already happened in parsePrecedence loop for the operator? 
    // No, parsePrecedence calls infix(p, left). Current token is INC/DEC.
    Token op = p->token;
    advance(p);
    return createUnaryOp(p->state->compiler, op, left); // Using UnaryOp structure for postfix usually works if backend distinguishes
}

static ASTNode* grouping(Parser* p) {
    consume(p, TK_OPENPARENTHESIS, "(");
    ASTNode* e = expression(p);
    consume(p, TK_CLOSEPARENTHESIS, ")");
    return e;
}

static ASTNode* call(Parser* p, ASTNode* left) {
    Token openParen = p->token;
    array* args = ARRAY_NEW(ASTNode*);
    consume(p, TK_OPENPARENTHESIS, "(");

    if (!check(p, TK_CLOSEPARENTHESIS)) {
        do {
            ASTNode* arg = expression(p);
            array_append(args, &arg);
        } while (match(p, TK_COMMA));
    }
    consume(p, TK_CLOSEPARENTHESIS, ")");

    ASTNode* node = createFuncCall(p->state->compiler, left, openParen, *args);
    free(args);
    return node;
}

static ASTNode* subscript(Parser* p, ASTNode* left) {
    consume(p, TK_OPENBRACKET, "[");
    ASTNode* idx = expression(p);
    consume(p, TK_CLOSEBRACKET, "]");
    return createArrayAccess(p->state->compiler, left->loc, left, idx);
}

static ASTNode* member(Parser* p, ASTNode* left) {
    consume(p, TK_DOT, ".");
    Token name = p->token;
    consume(p, TK_IDENTIFIER, "member name");
    return createMemberAccess(p->state->compiler, left, name);
}

static TypeID parseType(Parser* p) {
    TypeID t = { .baseType = T_CLASS };

    // Borrow is a type modifier. Keep it attached to the full type, including
    // array types, so ownership analysis sees borrowed locals, fields and params.
    if (match(p, TK_BORROW)) t.borrowed = 1;

    // Check for 'const' (optional modifier)
    if (match(p, TK_CONST)) {
        // We might want to store isConst in TypeID, currently unused in this struct
    }

    switch (p->token.type) {
	case TK_ASSIGN: // Handle inferred type (e.g., x: = 5;)
		t.baseType = T_AUTO;
        return t;
	case TK_AUTO:
		t.baseType = T_AUTO;
		advance(p);
		return t;
    case TK_BOOL:   t.baseType = T_BOOL; break;
    case TK_INT:    t.baseType = T_INT; break;
    case TK_FLOAT:  t.baseType = T_FLOAT; break;
    case TK_CHAR:   t.baseType = T_CHAR; break;
    case TK_STRING: t.baseType = T_STRING; break;
    default:        t.name = internString(p->state->compiler, &p->token.text); break;
    }
    advance(p);

    // Handle Array Syntax (e.g., int[])
    while (check(p, TK_OPENBRACKET)) {
        Token next = lex_peek(p->lexer);
        if (next.type == TK_CLOSEBRACKET) {
            advance(p); // eat [
            advance(p); // eat ]
            t.elementType = t.baseType;
            t.baseType = T_PTR; // Treating arrays as pointers
        }
        else {
            break; // Found [ expression ], stop here.
        }
    }
    return t;
}

static ASTNode* parseNew(Parser* p) {
    Token newLoc = p->token;
    advance(p); // consume 'new'
    TypeID type = parseType(p);

    // Check for Array Initialization (e.g., new int[10])
    if (match(p, TK_OPENBRACKET)) {
        ASTNode* size = expression(p);
        consume(p, TK_CLOSEBRACKET, "]");
        return createNewArrayNode(p->state->compiler, newLoc, type, size);
    }

    // Normal Object Creation
    return createNewObjectNode(p->state->compiler, newLoc, type);
}

// ===================================================================
// STATEMENT PARSING
// ===================================================================

static ASTNode* varDeclaration(Parser* p) {
    Loc startLoc = p->token.loc;
    char* name = NULL;

    array* varlist = ARRAY_NEW(VariableInfo);
    do {
        name = internString(p->state->compiler, &p->token.text);
        VariableInfo varInfo = { .name = name, .loc = p->token.loc };
        array_append(varlist, &varInfo);
        advance(p);
    } while (match(p, TK_COMMA));
    
    if(!match(p, TK_COLON)) {
        if (match(p, TK_OPENPARENTHESIS)) {
            reportError(p->state->compiler, p->token.loc, "Expected ':' after variable, Did you mean 'fun %s' or '%s:type' ?.", name, name);
        }
        else {
            reportError(p->state->compiler, p->token.loc, "Expected ':' after variable declaration '%s'.", name);
            //advance(p);
        }
	}
    
    TypeID type = parseType(p);
    ASTNode* init = NULL;

    if (match(p, TK_ASSIGN)) {
        init = expression(p);
    }
    else if (isValueStart(p->token.type)) {
        reportError(p->state->compiler, p->token.loc, "Missing '=' before initializer. Did you mean ': %s = ...'?",
            type.name ? type.name : "type");
        init = expression(p);
    }

    ASTNode* decl = createMultiVarDecl(p->state->compiler, startLoc, *varlist, type, init);
    free(varlist);
    return decl;
}

static ASTNode* block(Parser* p) {
    Loc startLoc = p->token.loc;
    ASTBlock* b = createBlock(p->state->compiler, startLoc);

    consume(p, TK_OPENCURLYBRACKET, "{");
    p->state->compiler->currentScopeLevel++;

    while (!check(p, TK_CLOSECURLYBRACKET) && p->token.type != TK_EOF) {
        ASTNode* stmt = statement(p);
        if (stmt) addStatement(b, stmt);
        else advance(p);
    }

    consume(p, TK_CLOSECURLYBRACKET, "}");
    p->state->compiler->currentScopeLevel--;
    return (ASTNode*)b;
}

static ASTNode* forStmt(Parser* p) {
    Loc forLoc = p->token.loc;
    consume(p, TK_OPENPARENTHESIS, "(");

    ASTNode* init = NULL;
    if (!check(p, TK_SEMICOLON)) {
        Token peek = lex_peek(p->lexer);
        if (peek.type == TK_COLON || peek.type == TK_COMMA)
            init = varDeclaration(p);
        else
            init = expression(p);
    }
    consume(p, TK_SEMICOLON, ";");

    ASTNode* cond = check(p, TK_SEMICOLON) ? NULL : expression(p);
    consume(p, TK_SEMICOLON, ";");

    ASTNode* inc = check(p, TK_CLOSEPARENTHESIS) ? NULL : expression(p);
    consume(p, TK_CLOSEPARENTHESIS, ")");

    ASTNode* body = statement(p);
    return createForStmt(p->state->compiler, forLoc, init, cond, inc, body);
}

static ASTNode* ifStmt(Parser* p) {
    Loc ifLoc = p->token.loc;
    consume(p, TK_OPENPARENTHESIS, "(");
    ASTNode* cond = expression(p);
    consume(p, TK_CLOSEPARENTHESIS, ")");

    ASTNode* then = statement(p);
    ASTNode* elseb = match(p, TK_ELSE) ? statement(p) : NULL;

    return createIfStmt(p->state->compiler, ifLoc, cond, then, elseb);
}

static ASTNode* whileStmt(Parser* p) {
    Loc wLoc = p->token.loc;
    consume(p, TK_OPENPARENTHESIS, "(");
    ASTNode* cond = expression(p);
    consume(p, TK_CLOSEPARENTHESIS, ")");
    return createWhileStmt(p->state->compiler, wLoc, cond, statement(p));
}

static ASTNode* switchStmt(Parser* p) {
    Loc swLoc = p->token.loc;
    consume(p, TK_OPENPARENTHESIS, "(");
    ASTNode* expr = expression(p);
    consume(p, TK_CLOSEPARENTHESIS, ")");
    consume(p, TK_OPENCURLYBRACKET, "{");

    array* cases = ARRAY_NEW(ASTNode*);
    ASTNode* defaultCase = NULL;
    ASTBlock* currentBlock = NULL;

    while (!check(p, TK_CLOSECURLYBRACKET) && p->token.type != TK_EOF) {
        if (match(p, TK_CASE)) {
            Loc caseLoc = p->token.loc;
            ASTNode* val = expression(p);
            consume(p, TK_COLON, ":");
            currentBlock = createBlock(p->state->compiler, caseLoc);
            ASTNode* caseNode = createCaseNode(p->state->compiler, caseLoc, val, (ASTNode*)currentBlock);
            array_append(cases, &caseNode);
        }
        else if (match(p, TK_DEFAULT)) {
            Loc defLoc = p->token.loc;
            consume(p, TK_COLON, ":");
            currentBlock = createBlock(p->state->compiler, defLoc);
            defaultCase = (ASTNode*)currentBlock;
        }
        else {
            if (currentBlock) addStatement(currentBlock, statement(p));
            else {
                reportError(p->state->compiler, p->token.loc, "Statements in switch must be inside case/default");
                advance(p);
            }
        }
    }
    consume(p, TK_CLOSECURLYBRACKET, "}");
    ASTNode* node = createSwitchStmt(p->state->compiler, swLoc, expr, cases, defaultCase);
    free(cases);
    return node;
}

static ASTNode* parseParams(Parser* p, int *count)
{
    *count = 0;
    if (check(p, TK_CLOSEPARENTHESIS)) return NULL;

    ASTBlock* block = createBlock(p->state->compiler, p->token.loc);

    while (!check(p, TK_CLOSEPARENTHESIS) && p->token.type != TK_EOF)
    {
        ASTNode* decl = varDeclaration(p);
        *count += ((ASTMultiVarDecl*)decl)->names.count;
        addStatement(block, decl);

        if (match(p, TK_SEMICOLON))
        {
            if (check(p, TK_CLOSEPARENTHESIS)) break;
            continue;
        }

        if (!check(p, TK_CLOSEPARENTHESIS)) 
        {
            reportError(p->state->compiler, decl->loc, "Expected ';' between parameter groups of different types.");
        }
    }
    return (ASTNode*)block;
}

static ASTNode* funcDef(Parser* p, char* className) 
{
    Loc funcLoc = p->token.loc;
    Token nameTok = p->token;
    advance(p); // consume name
    int count = 0;

    consume(p, TK_OPENPARENTHESIS, "(");
    ASTNode* params = parseParams(p, &count);
    consume(p, TK_CLOSEPARENTHESIS, ")");

    TypeID ret = { T_VOID, NULL };
    char* borrowedReturnParam = NULL;
    char* borrowedReturnPath = NULL;
    if (match(p, TK_COLON)) {
        if (check(p, TK_BORROW) && lex_peek(p->lexer).type == TK_OPENPARENTHESIS) {
            advance(p); // borrow
            consume(p, TK_OPENPARENTHESIS, "(");
            Token source = p->token;
            if (source.type != TK_IDENTIFIER && source.type != TK_THIS) {
                reportError(p->state->compiler, source.loc,
                    "Expected a parameter name in borrowed return annotation");
            }
            else {
                borrowedReturnParam = source.type == TK_THIS
                    ? internStr(p->state->compiler, "this")
                    : internString(p->state->compiler, &source.text);
                advance(p);
            }

            // A return may be tied to a subobject of the named owner, for
            // example borrow(holder.item) or borrow(items[]). The [] marker
            // means any element because the current analysis is index-agnostic.
            char path[1024] = { 0 };
            size_t pathLength = 0;
            int pathOverflow = 0;
            while (check(p, TK_DOT) || check(p, TK_OPENBRACKET)) {
                if (match(p, TK_DOT)) {
                    if (p->token.type != TK_IDENTIFIER) {
                        reportError(p->state->compiler, p->token.loc,
                            "Expected a field name in borrowed return path");
                        break;
                    }
                    size_t componentLength = (size_t)(p->token.text.end - p->token.text.begin);
                    size_t separatorLength = pathLength ? 1 : 0;
                    if (pathLength + separatorLength + componentLength >= sizeof(path)) {
                        pathOverflow = 1;
                        advance(p);
                        continue;
                    }
                    if (separatorLength) path[pathLength++] = '.';
                    memcpy(path + pathLength, p->token.text.begin, componentLength);
                    pathLength += componentLength;
                    path[pathLength] = '\0';
                    advance(p);
                }
                else {
                    Loc arrayLoc = p->token.loc;
                    advance(p); // [
                    if (!check(p, TK_CLOSEBRACKET)) {
                        reportError(p->state->compiler, arrayLoc,
                            "Borrowed return array path must use [] without an index");
                        while (p->token.type != TK_CLOSEBRACKET && p->token.type != TK_EOF)
                            advance(p);
                    }
                    consume(p, TK_CLOSEBRACKET, "]");
                    const char* marker = pathLength ? ".[]" : "[]";
                    size_t markerLength = strlen(marker);
                    if (pathLength + markerLength >= sizeof(path)) pathOverflow = 1;
                    else {
                        memcpy(path + pathLength, marker, markerLength + 1);
                        pathLength += markerLength;
                    }
                }
            }
            if (pathOverflow) {
                reportError(p->state->compiler, source.loc,
                    "Borrowed return path is too long");
            }
            else if (pathLength) {
                borrowedReturnPath = internStr(p->state->compiler, path);
            }
            consume(p, TK_CLOSEPARENTHESIS, ")");
            ret = parseType(p);
            ret.borrowed = 1;
        }
        else {
            ret = parseType(p);
        }
    }

    ASTNode* body = block(p);

    char* fullName;
    if (className) {
        char mangled[128];
        char* simpleName = internString(p->state->compiler, &nameTok.text);
        snprintf(mangled, sizeof(mangled), "%s.%s", className, simpleName);
        fullName = internStr(p->state->compiler, mangled);
    }
    else {
        fullName = internString(p->state->compiler, &nameTok.text);
    }

    ASTFuncDef* function = (ASTFuncDef*)createFuncDef(
        p->state->compiler, funcLoc, fullName, ret, params, body);
    if (function) {
        function->borrowedReturnParam = borrowedReturnParam;
        function->borrowedReturnPath = borrowedReturnPath;
    }
    return (ASTNode*)function;
}

static ASTNode* parseImport(Parser* p) {
    Loc importLoc = p->token.loc;

    if (p->token.type != TK_STRING) {
        reportError(p->state->compiler, p->token.loc, "Expected string file path after 'import'");
        advance(p);
        consume(p, TK_SEMICOLON, ";");
        return NULL;
    }

    char* filename = internString(p->state->compiler, &p->token.text);
    advance(p);
    consume(p, TK_SEMICOLON, ";");

    // Prevent recursive or duplicate imports
    if (isFileImported(p->state, filename)) {
        return NULL;
    }

    array_append(&p->state->compiler->importedFiles, &filename);

    char* source = readFile(filename);
    if (!source) {
        reportError(p->state->compiler, importLoc, "Could not open imported file '%s'", filename);
        return NULL;
    }

    // Spin up a Sub-Parser with shared state
    lexer* importLexer = lex_create(source, filename);
    Parser importParser = { .lexer = importLexer, .state = p->state };

    advance(&importParser); // Prime the pump

    ASTBlock* fileBlock = createBlock(p->state->compiler, importLoc);
    while (importParser.token.type != TK_EOF) {
        if (p->state->compiler->error) break;
        ASTNode* stmt = statement(&importParser);
        if (stmt) addStatement((ASTBlock*)fileBlock, stmt);
    }

    lex_dispose(&importLexer);
    free(source);

    return (ASTNode*)fileBlock;
}

static ASTNode* statement(Parser* p) {
    if (p->state->compiler->error) {
        advance(p);
        return NULL;
    }

    Loc stmtLoc = p->token.loc;

    switch (p->token.type) {
    case TK_ENUM:   parseEnum(p); return NULL;
    case TK_IF:     advance(p); return ifStmt(p);
    case TK_FOR:    advance(p); return forStmt(p);
    case TK_WHILE:  advance(p); return whileStmt(p);
    case TK_SWITCH: advance(p); return switchStmt(p);
    case TK_BREAK:
        advance(p);
        consume(p, TK_SEMICOLON, ";");
        return createBreakNode(p->state->compiler, stmtLoc);
    case TK_CONTINUE:
        advance(p);
        consume(p, TK_SEMICOLON, ";");
        return createContinueNode(p->state->compiler, stmtLoc);
    case TK_IMPORT: advance(p); return parseImport(p);
    case TK_RETURN: {
        advance(p);
        ASTNode* val = check(p, TK_SEMICOLON) ? NULL : expression(p);
        consume(p, TK_SEMICOLON, ";");
        return createReturnStmt(p->state->compiler, stmtLoc, val);
    }
    case TK_FUN:
        advance(p);
        return funcDef(p, NULL);

    case TK_CLASS: {
        advance(p);
        char* name = internString(p->state->compiler, &p->token.text);
        advance(p);
        consume(p, TK_OPENCURLYBRACKET, "{");
        ASTBlock* members = createBlock(p->state->compiler, stmtLoc);

        while (!check(p, TK_CLOSECURLYBRACKET) && p->token.type != TK_EOF) {
            if (p->token.type == TK_FUN) {
                advance(p);
                addStatement((ASTBlock*)members, funcDef(p, name));
            }
            else {
                addStatement((ASTBlock*)members, varDeclaration(p));
                consume(p, TK_SEMICOLON, ";");
            }
        }
        consume(p, TK_CLOSECURLYBRACKET, "}");
        match(p, TK_SEMICOLON);
        return createStructDef(p->state->compiler, stmtLoc, name, members);
    }
    case TK_OPENCURLYBRACKET: return block(p);
    case TK_IDENTIFIER: {
        Token next = lex_peek(p->lexer);
        if (next.type == TK_COLON || next.type == TK_COMMA) {
            ASTNode* node = varDeclaration(p);
            consume(p, TK_SEMICOLON, ";");
            return node;
        }
        else if (next.type == TK_IDENTIFIER) {
            reportError(p->state->compiler, p->token.loc,
                "Unexpected identifier '%.*s' at start of statement. "
                "Is this a misspelled keyword?",
                (int)(p->token.text.end - p->token.text.begin), p->token.text.begin);
            return NULL;
        }
        // Fallthrough to expression statement
    }
    default: {
        ASTNode* e = expression(p);
        consume(p, TK_SEMICOLON, ";");
        return e;
    }
    }
}

// ===================================================================
// PUBLIC API
// ===================================================================

ASTNode* parse(const char* source, const char* filename, State* state) {
    Parser p = { .lexer = lex_create(source, filename), .state = state };
    if (!p.lexer) return NULL;

    ASTBlock* program = createBlock(state->compiler, (Loc) { { 1, 1 }, NULL });
    advance(&p);

    while (p.token.type != TK_EOF) {
        if (state->compiler->error) break;
        ASTNode* stmt = statement(&p);
        if (stmt) addStatement(program, stmt);
    }

    lex_dispose(&p.lexer);
    return (ASTNode*)program;
}
