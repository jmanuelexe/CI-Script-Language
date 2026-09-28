#include "types.h"
#include "stdlib.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

void arena_init(Arena* arena, size_t default_block_size) {
    arena->head = NULL;
    arena->default_block_size = default_block_size ? default_block_size : 64 * 1024;
}

void* arena_alloc(Arena* arena, size_t size, size_t alignment) {
    if (alignment < sizeof(void*)) alignment = sizeof(void*);
    ArenaBlock* block = arena->head;
    size_t aligned = block ? (block->used + alignment - 1) & ~(alignment - 1) : 0;
    if (!block || aligned + size > block->capacity) {
        size_t capacity = arena->default_block_size;
        if (capacity < size + alignment) capacity = size + alignment;
        block = (ArenaBlock*)malloc(sizeof(ArenaBlock) + capacity);
        if (!block) return NULL;
        block->next = arena->head;
        block->used = 0;
        block->capacity = capacity;
        arena->head = block;
        aligned = 0;
    }
    void* result = block->data + aligned;
    block->used = aligned + size;
    memset(result, 0, size);
    return result;
}

void arena_reset(Arena* arena) {
    for (ArenaBlock* block = arena->head; block; block = block->next) block->used = 0;
}

void arena_destroy(Arena* arena) {
    ArenaBlock* block = arena->head;
    while (block) {
        ArenaBlock* next = block->next;
        free(block);
        block = next;
    }
    arena->head = NULL;
}

// Helper to create a string from a TString, needed for error messages or other logic
// This is a simplified version. In a real scenario, you might have a more robust utility.
char* makeStrFromTstr(const TString* tstr) {
    if (!tstr || !tstr->begin || !tstr->end || tstr->begin > tstr->end) {
        return NULL;
    }
    size_t len = tstr->end - tstr->begin;
    char* str = (char*)malloc(len + 1);
    if (!str) {
        perror("Failed to allocate memory for string");
        return NULL;
    }
    memcpy(str, tstr->begin, len);
    str[len] = '\0';
    return str;
}

// Finds a string in the string pool or adds it if not found.
char* findString(array* strings, const char* tstr, int len) 
{
    // Search for the string in the existing pool
    for (int i32 = 0; i32 < array_size(strings); ++i32) {
        char* existing_str = *(char**)array_get(strings, i32);
        // Check if lengths match first for a quick exit
        if (strlen(existing_str) == len && strncmp(existing_str, tstr, len) == 0) {
            return existing_str; // Found it, return the existing pointer
        }
    }
	// If not found, intern the string
	return 0;
}

// This process is called "string interning".
char* internString(CompilerDef* compiler, const TString* tstr)
{
    int len = (int)(tstr->end - tstr->begin);

    if (!tstr || !tstr->begin || !tstr->end || tstr->begin > tstr->end) {
        return NULL;
    }
	char* existing_str = findString(&compiler->strings, tstr->begin, len);
	if (existing_str) {
		return existing_str; // If found, return the existing string
	}
    char* new_str = makeStrFromTstr(tstr);
    array_append(&compiler->strings, &new_str);
    return new_str;
}

char* internStr(CompilerDef* compiler, const char* str){
	int len = (int)strlen(str);
    if (!str) return NULL;
	char* existing_str = findString(&compiler->strings, str, len);
	if (existing_str) {
		return existing_str; // If found, return the existing string
	}
    // Not found, so create a new one and add it to the pool
    char* new_str = _strdup(str);
    
    // Add the new string's pointer to the pool
    array_append(&compiler->strings, &new_str);

    return new_str;
}

// In Compiler.c or Parser.c

void reportError(CompilerDef* compiler, Loc loc, const char* format, ...) {
    compiler->error++;

    // 1. Build the complete error message (Prefix + Actual Error)
    char full_msg[1024];
    int len = 0;

    if (loc.filename) {
        len = snprintf(full_msg, sizeof(full_msg), "%s(%d,%d): error: ",
            loc.filename, loc.pos.line, loc.pos.col);
    }
    else {
        len = snprintf(full_msg, sizeof(full_msg), "error: ");
    }

    // Append the actual error details safely
    va_list args;
    va_start(args, format);
    vsnprintf(full_msg + len, sizeof(full_msg) - len, format, args);
    va_end(args);

    // 2. Send the FULL message to the C++ callback (if it exists)
    int line = loc.filename ? loc.pos.line : 0;
    if (compiler->onError) {
        compiler->onError(loc, full_msg, compiler->errorUserData);
    }

    // 3. Also print to stderr so you still see it in the console during debugging
    fprintf(stderr, "%s\n", full_msg);

    // 4. Handle the "Too many errors" limit
    if (compiler->error >= 10) {
        const char* stop_msg = "Too many errors, stopping compilation.";
        if (compiler->onError) {
            compiler->onError(loc, stop_msg, compiler->errorUserData);
        }
        fprintf(stderr, "%s\n", stop_msg);
    }
}

