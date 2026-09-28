#include "SymbolTable.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "AST.h"

static int isRefType(TypeID type) {
    return (type.baseType == T_STRING ||
        type.baseType == T_CLASS ||
        type.baseType == T_SHARED_PTR ||
        type.baseType == T_PTR);
}

/**
 * @brief Looks for a symbol by name, starting from the innermost scope and searching outwards.
 * @param state The current compiler state.
 * @param name The name of the symbol to find.
 * @return A pointer to the found symbol, or NULL if not found.
 */
Symbol* lookupSymbolRecursive(State* state, const char* name) {
    CompilerDef* compiler = state->compiler;
    // Iterate backwards through the unified symbol list.
    // This naturally finds the innermost declaration first.
    for (int i32 = compiler->symbols.count - 1; i32 >= 0; i32--) {
        Symbol* s = (Symbol*)array_get(&compiler->symbols, i32);
        if (strcmp(s->name, name) == 0) {
            // Found a symbol with the right name. Since we are searching backwards,
            // this is guaranteed to be the one in the nearest visible scope.
            return s;
        }
    }
    return NULL; // Not found
}
/**
 * @brief Looks for a symbol by name, starting from the innermost *local* scope
 * and searching outwards, but *stopping* at the global scope.
 * @param state The current compiler state.
 * @param name The name of the symbol to find.
 * @return A pointer to the found symbol, or NULL if not found in any local scope.
 */
Symbol* lookupSymbolLocal(State* state, const char* name) {
    CompilerDef* compiler = state->compiler;

    // Iterate backwards through the symbol list, from innermost to outermost.
    for (int i32 = compiler->symbols.count - 1; i32 >= 0; i32--) {
        Symbol* s = (Symbol*)array_get(&compiler->symbols, i32);

        if (s->scopeParent == state->compiler->currentScopeParent &&
            s->scopeLevel > 0 && strcmp(s->name, name) == 0) 
        {
            return s;
        }        
    }

    return NULL; // Not found in any local scope
}

Symbol* findMethodSymbol(State* state, const char* className, const char* methodName) {
    CompilerDef* compiler = state->compiler;
    char mangled[256];
    snprintf(mangled, 256, "%s.%s", className, methodName);
    // Iterate through all symbols to find the method definition.
    for (int i32 = 0; i32 < compiler->symbols.count; i32++) {
        Symbol* s = (Symbol*)array_get(&compiler->symbols, i32);
        if ((s->kind == SYM_FUNCTION || s->kind == SYM_CFUNCTION) && strcmp(s->name, mangled) == 0) {
            return s;
        }
    }
    return NULL;
}

/**
 * @brief Looks for a symbol by name in the *global scope only*.
 * @param state The current compiler state.
 * @param name The name of the symbol to find.
 * @return A pointer to the found symbol, or NULL if not found.
 */
Symbol* lookupSymbolGlobal(State* state, const char* name) {
    CompilerDef* compiler = state->compiler;

    // Iterate through all symbols.
    for (int i32 = 0; i32 < compiler->symbols.count; i32++) {
        Symbol* s = (Symbol*)array_get(&compiler->symbols, i32);

        // We only care about symbols in the global scope (scope 0).
        if (s->scopeLevel == 0) {
            if (strcmp(s->name, name) == 0) {
                // Found it.
                return s;
            }
        }
    }
    return NULL; // Not found in global scope
}
/**
 * @brief Looks specifically for a type definition (a class/struct) by name.
 * @param state The current compiler state.
 * @param name The name of the type to find.
 * @return A pointer to the symbol representing the type, or NULL if not found.
 */
Symbol* findTypeSymbol(State* state, const char* name) {
    CompilerDef* compiler = state->compiler;
    // Iterate through all symbols to find a type definition.
    for (int i32 = 0; i32 < compiler->symbols.count; i32++) {
        Symbol* s = (Symbol*)array_get(&compiler->symbols, i32);
        if (s->type.baseType == T_CLASS && strcmp(s->name, name) == 0) {
            return s;
        }
    }
    return NULL;
}

Symbol* findTypeSymbolTstr(State* state, const TString* name) {
    CompilerDef* compiler = state->compiler;
	int len = (int)(name->end - name->begin);
    // Iterate through all symbols to find a type definition.
    for (int i32 = 0; i32 < compiler->symbols.count; i32++) {
        Symbol* s = (Symbol*)array_get(&compiler->symbols, i32);
        if (s->type.baseType == T_CLASS && strncmp(s->name, name->begin, len) == 0) {
            return s;
        }
    }
    return NULL;
}

Symbol* findSymbol(State* state, const char* name) {
	CompilerDef* compiler = state->compiler;
	// Iterate through all symbols to find a variable or function.
	for (int i32 = 0; i32 < compiler->symbols.count; i32++) {
		Symbol* s = (Symbol*)array_get(&compiler->symbols, i32);
		//if (s->scopeLevel < compiler->currentScopeLevel) {
		//	break; // We've left the current scope, so no need to check further.
		//}
		if (strcmp(s->name, name) == 0) {
			return s; // Found the symbol
		}
	}
	return NULL; // Not found
}

/**
 * @brief Adds a new struct/class type definition to the symbol table.
 * @param state The current compiler state.
 * @param structName The name of the new type.
 * @param astMembers An array of ASTStructMember nodes from the parser.
 * @return A pointer to the symbol representing the new type, or NULL on failure.
 */
Symbol* addClassSymbol(State* state, const char* structName) 
{
    if (findTypeSymbol(state, structName) != NULL)return NULL;

    Symbol clase;
    clase.name = internStr(state->compiler, structName);
    clase.type.baseType = T_CLASS;
    clase.type.name = clase.name;
    clase.scopeLevel = state->compiler->currentScopeLevel;
    clase.as.classDef.members = array_create(sizeof(ClassMember));
    clase.as.classDef.totalSize = 0;
    clase.as.classDef.packedSize = 0;
    clase.as.classDef.packedLayout = 0;
    clase.kind = SYM_TYPE_DEFINITION;
    clase.scopeParent = state->compiler->currentScopeParent;
	clase.stackIndex = 0;
	clase.as.classDef.destructorIndex = -1;
    array_append(&state->compiler->symbols, &clase);
    return (Symbol*)array_get(&state->compiler->symbols, state->compiler->symbols.count - 1);
}

void addClassMember(Symbol* structSymbol, const char* name, TypeID type, int offset) {
    // --- Safety Check ---
    // Ensure we are actually adding a member to a valid struct definition symbol.
    if (!structSymbol || structSymbol->kind != SYM_TYPE_DEFINITION || structSymbol->type.baseType != T_CLASS) {
        fprintf(stderr, "Compiler Error: Attempted to add a member to a non-struct symbol.\n");
        return;
    }

    ClassMember newMember;

    // 2. Populate its fields with the provided data.
    newMember.name = name;         // The name is an interned string.
    newMember.type = type;         // The data type of the member.
    newMember.offset = offset;     // Its byte offset in the struct's memory layout.

    // --- Add to List ---
    // 3. Append the newly created member to the struct's list of members.
    //    This list is stored within the symbol's `as.classDef` union field.
    array_append(&structSymbol->as.classDef.members, &newMember);
}

/**
 * @brief Adds a new variable or function symbol to the unified symbol table.
 * It checks for redeclarations at the current scope level.
 * @param state The current compiler state.
 * @param name The name of the symbol.
 * @param type The TypeID of the symbol.
 * @param count For arrays, the number of elements. For scalars, 1.
 * @return A pointer to the newly created symbol, or NULL on failure.
 */
Symbol* addSymbol(State* state, const char* name, TypeID type, int size, int count) {
    Symbol newSymbol = { 0 };

    // Check for redeclaration in the *current* scope only.
    for (int i32 = state->compiler->symbols.count - 1; i32 >= 0; i32--) {
        Symbol* s = (Symbol*)array_get(&state->compiler->symbols, i32);
        if (s->scopeLevel < state->compiler->currentScopeLevel) {
            break; // We've left the current scope, so no need to check further.
        }
        // If we find a symbol with the same name in the current scope, we have a redeclaration.
        if (
            s->scopeParent == state->compiler->currentScopeParent &&
            s->scopeLevel == state->compiler->currentScopeLevel &&
            strcmp(s->name, name) == 0
            ) {
			return NULL; //return ERROR_REDECLARATION;
        }
    }
    newSymbol.name = internStr(state->compiler, name);
    newSymbol.type = type;
    newSymbol.count = count;
    newSymbol.kind = SYM_VARIABLE;
    newSymbol.scopeLevel = state->compiler->currentScopeLevel;
    newSymbol.ownsValue = isRefType(type) && type.baseType != T_STRING && !type.borrowed &&
        newSymbol.scopeLevel > 0;
    newSymbol.borrowedFromRefIndex = -1;
    newSymbol.borrowedFromPath = 0;
    newSymbol.borrowedFromDepth = 0;
    memset(newSymbol.borrowedPathComponents, 0, sizeof(newSymbol.borrowedPathComponents));
    newSymbol.scopeParent = state->compiler->currentScopeParent;

    if (newSymbol.scopeLevel == 0) {
        // --- Global Scope ---
        if (isRefType(type)) {
            newSymbol.stackIndex = state->compiler->nextGlobalRefIndex;
            state->compiler->nextGlobalRefIndex += count; // Reserve slots for arrays
        }
        else {
            newSymbol.stackIndex = state->compiler->nextGlobalPrimIndex;
            state->compiler->nextGlobalPrimIndex += count;
        }
    }
    else {
        // --- Local Scope ---
        if (isRefType(type)) {
            newSymbol.stackIndex = state->compiler->nextRefIndex;
            state->compiler->nextRefIndex += count;
        }
        else {
            newSymbol.stackIndex = state->compiler->nextPrimIndex;
            state->compiler->nextPrimIndex += count;
        }
    }

    array_append(&state->compiler->symbols, &newSymbol);
    return (Symbol*)array_get(&state->compiler->symbols, state->compiler->symbols.count - 1);
}

int internStrIndex(CompilerDef* compiler, const char* str) {
    int len = (int)strlen(str);
    if (!str) return -1;

    // Search existing
    for (int i = 0; i < array_size(&compiler->strings); i++) {
        char* s = *(char**)array_get(&compiler->strings, i);
        if (strcmp(s, str) == 0)
            return i;
    }

    // Add new
    char* new_str = _strdup(str);
    array_append(&compiler->strings, &new_str);
    return array_size(&compiler->strings) - 1;
}


/**
 * @brief Adds a new function symbol to the symbol table.
 * This is a generic function that can handle both native C functions
 * and user-defined script functions.
 * @param state The current compiler state.
 * @param name The name of the function.
 * @param kind The kind of function (SYM_CFUNCTION or SYM_FUNCTION).
 * @param funcData A pointer to the function's data (either a CFunction pointer or an integer address).
 * @return A pointer to the newly created symbol, or NULL on failure.
 */
Symbol* addFuncSymbol(State* state, const char* name, SymbolKind kind, void* funcData) {
    CompilerDef* compiler = state->compiler;

    Symbol newSymbol;
    char * nameSTR = internStr(state->compiler, name);
    newSymbol.name = nameSTR;
    newSymbol.scopeLevel = compiler->currentScopeLevel;
    newSymbol.kind = kind;
    newSymbol.stackIndex = 0;
    newSymbol.scopeParent = state->compiler->currentScopeParent;

    if (kind == SYM_CFUNCTION) {
        newSymbol.type.baseType = T_CFUNCTION;
        // The funcData is a CFunction pointer. We add it to a list and store the index.       
        array_append(&state->cFunctions, &funcData);
		const NativeSignature* signature = NULL;
		array_append(&state->cFunctionSignatures, &signature);

        newSymbol.as.cFunction.cfuncIndex = state->cFunctions.count - 1;
        newSymbol.as.cFunction.paramCount = -1; // -1 = unchecked by default
    }
    else if (kind == SYM_FUNCTION) {
        newSymbol.type.baseType = T_FUNCTION;
        newSymbol.as.funcDef.borrowedReturnParamIndex = -1;
        newSymbol.as.funcDef.borrowedReturnRefIndex = -1;
        newSymbol.as.funcDef.borrowedReturnPath = NULL;
    }

    array_append(&compiler->symbols, &newSymbol);
    return (Symbol*)array_get(&compiler->symbols, compiler->symbols.count - 1);
}

Symbol* findClassSymbol(State* state, const char* structName) {
	if (!state || !state->compiler || !structName) return NULL;
	CompilerDef* compiler = state->compiler;
	for (int i32 = 0; i32 < compiler->symbols.count; i32++) {
		Symbol* s = (Symbol*)array_get(&compiler->symbols, i32);
		if (s->type.baseType == T_CLASS && strcmp(s->name, structName) == 0) {
			return s;
		}
	}
	return NULL; // Not found
}

/**
 * @brief Finds the byte offset of a member within a struct definition.
 * @param classDef A pointer to the Symbol that defines the struct/class.
 * @param memberName The name of the member to find.
 * @return The integer byte offset of the member, or -1 if not found.
 */
int findMemberOffset(Symbol* classDef, const char* memberName) {
    if (!classDef || classDef->type.baseType != T_CLASS) {
        //fprintf(stderr, "Error: Attempted to find member on a non-struct symbol.\n");
        return -2; // Not a struct definition
    }

    // The 'as.classDef.members' array holds the list of ClassMember instances.
    for (int i32 = 0; i32 < array_size(&classDef->as.classDef.members); ++i32) {
        ClassMember* member = (ClassMember*)array_get(&classDef->as.classDef.members, i32);
        if (strcmp(member->name, memberName) == 0) {
            // Found the member, return its pre-calculated offset.
            return member->offset;
        }
    }
    // Member was not found in the struct definition
    return -1;
}

/**
 * @brief Finds the TypeID of a member within a struct definition.
 * @param classDef A pointer to the Symbol that defines the struct/class.
 * @param memberName The name of the member to find.
 * @return The TypeID of the member, or a T_UNKNOWNTYPE type if not found.
 */
TypeID findMemberType(Symbol* classDef, const char* memberName) {
    if (!classDef || classDef->type.baseType != T_CLASS) {
        return (TypeID) { .baseType = T_UNKNOWNTYPE};
    }

    for (int i32 = 0; i32 < array_size(&classDef->as.classDef.members); ++i32) {
        ClassMember* member = (ClassMember*)array_get(&classDef->as.classDef.members, i32);
        if (strcmp(member->name, memberName) == 0) {
            return member->type; // Found it
        }
    }

    return (TypeID) { .baseType = T_UNKNOWNTYPE}; // Not found
}

Symbol* addConstantInt(State* state, const char* name, int value) {
    // 1. Create the value wrapper
    Value val;
    val.type.baseType = T_INT;
    val.v.i32 = value;

    // 2. Add to symbol table
    // We pass 0 for size/stackIndex because constants don't take up stack memory!
    Symbol* sym = addSymbol(state, name, val.type, 0, 0);

    if (sym) {
        sym->kind = SYM_CONSTANT;
        sym->as.constValue = val;
    }
    return sym;
}

Symbol* addTypedFuncSymbol(State* state, const char* name, CFunction funcData,
    const NativeSignature* signature) {
    Symbol* symbol = addFuncSymbol(state, name, SYM_CFUNCTION, (void*)funcData);
    if (symbol && state->cFunctionSignatures.count > 0) {
        const NativeSignature** entry = (const NativeSignature**)array_get(
            &state->cFunctionSignatures, state->cFunctionSignatures.count - 1);
        *entry = signature;
    }
    return symbol;
}

Symbol* addFastTypedFuncSymbol(State* state, const char* name, NativeFastFunction funcData,
    const NativeSignature* signature) {
    Symbol* symbol = addFuncSymbol(state, name, SYM_CFUNCTION, (void*)funcData);
    if (symbol && state->cFunctionSignatures.count > 0) {
        const NativeSignature** entry = (const NativeSignature**)array_get(
            &state->cFunctionSignatures, state->cFunctionSignatures.count - 1);
        *entry = signature;
    }
    return symbol;
}
