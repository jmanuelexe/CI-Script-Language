#include "lexer.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h> 

// --- Internal Defines & Types ---
#define TK_SKIP -1  // Internal signal to skip whitespace/comments

typedef Token(*LexHandler)(lexer* self);

// --- Forward Declarations ---
Token lex_getNumber(lexer* self);
Token lex_getIdentifier(lexer* self);
Token lex_parseLiteral(lexer* self, char delimiter, TTOKEN type);
Token lex_parseSpecial(lexer* self);
char lex_getChar(lexer* self);
char lex_peekChar(lexer* self);
int lex_matchChar(lexer* self, char expected);
void lex_skipUntil(lexer* self, const char* delimiter);

// --- Keyword Tables ---
struct Reserved { const char* name; TTOKEN type; };

static const struct Reserved row2[] = { 
    { "do", TK_DO }, 
    { "if", TK_IF }, 
    { "or", TK_OR }, 
    { "to", TK_TO }, 
    { 0 } 
};
static const struct Reserved row3[] = {
    { "new", TK_NEW }, 
    { "and", TK_AND }, 
    { "end", TK_END }, 
    { "fun", TK_FUN }, 
    { "for", TK_FOR }, 
    { "not", TK_NOT }, 
    { "int", TK_INT }, 
    { 0 } 
};
static const struct Reserved row4[] = {
    { "bool", TK_BOOL}, 
    { "move", TK_MOVE },
    { "case", TK_CASE }, 
    { "char", TK_CHAR }, 
    { "byte", TK_CHAR }, 
    { "else", TK_ELSE }, 
	{ "enum", TK_ENUM },
    { "goto", TK_GOTO }, 
    { "null", TK_NULL },
    { "then", TK_THEN }, 
    { "this", TK_THIS }, 
    { "true", TK_TRUE }, 
    { "auto", TK_AUTO },
    { "int8", TK_CHAR },
    { "int8_t", TK_CHAR },
    { 0 } 
};
static const struct Reserved row5[] = {
    { "begin", TK_BEGIN }, 
    { "break", TK_BREAK }, 
    { "const", TK_CONST }, 
    { "class", TK_CLASS }, 
    { "endif", TK_ENDIF }, 
    { "float", TK_FLOAT }, 
    { "float32", TK_FLOAT },
    { "float32_t", TK_FLOAT },
    { "false", TK_FALSE }, 
    { "label", TK_LABEL }, 
    { "short", TK_SHORT }, 
    { "range", TK_RANGE }, 
    { "while", TK_WHILE }, 
    { "int16", TK_SHORT },
    { "int16_t", TK_SHORT },
    { 0 } 
};
static const struct Reserved row6[] = { { "borrow", TK_BORROW }, { "double", TK_DOUBLE }, { "endfor", TK_ENDFOR }, { "import", TK_IMPORT }, { "return", TK_RETURN }, { "string", TK_STRING }, { "struct", TK_STRUCT }, { "switch", TK_SWITCH }, { "int32", TK_INT }, { "int32_t", TK_INT }, { 0 } };
static const struct Reserved row7[] = { { "default", TK_DEFAULT }, { 0 } };
static const struct Reserved row8[] = { { "continue", TK_CONTINUE }, { "endwhile", TK_ENDWHILE }, { 0 } };

const struct Reserved* Keywords[] = { NULL, NULL, row2, row3, row4, row5, row6, row7, row8 };

// --- Dispatch Table Storage ---
static LexHandler lex_dispatch[256] = { 0 };
static int handlers_initialized = 0;

// --- Handler Functions ---

Token handle_eof(lexer* self) {
    Token t = { 0 };
    t.type = TK_EOF;
    t.text.begin = t.text.end = NULL;
    return t;
}

Token handle_error(lexer* self) {
    Token t = { 0 };
    t.loc = self->loc;
    t.text.begin = self->input;
    printf("Lexing Error: Unexpected character '%c' at Line %d, Col %d\n",
        *self->input, self->loc.pos.line, self->loc.pos.col);
    lex_getChar(self); // consume bad char
    t.text.end = self->input;
    t.type = TK_ERROR;
    return t;
}

Token handle_whitespace(lexer* self) {
    lex_getChar(self); // consume
    Token t = { 0 };
    t.type = TK_SKIP;
    return t;
}

Token handle_digit(lexer* self) {
    return lex_getNumber(self);
}

Token handle_alpha(lexer* self) {
    return lex_getIdentifier(self);
}

Token handle_string(lexer* self) {
    lex_getChar(self); // consume opening "
    return lex_parseLiteral(self, '"', TK_STRING);
}

Token handle_char(lexer* self) {
    lex_getChar(self); // consume opening '
    return lex_parseLiteral(self, '\'', TK_CHARACTER);
}

Token handle_special(lexer* self) {
    return lex_parseSpecial(self);
}

Token handle_slash(lexer* self) {
    char next = *(self->input + 1);

    if (next == '/') {
        // Single line comment
        lex_getChar(self); lex_getChar(self); // consume //
        lex_skipUntil(self, "\n");
        Token t = { 0 }; t.type = TK_SKIP;
        return t;
    }
    else if (next == '*') {
        // Block comment
        lex_getChar(self); lex_getChar(self); // consume /*
        lex_skipUntil(self, "*/");
        // consume closing */ if we didn't hit EOF
        if (*self->input) { lex_getChar(self); lex_getChar(self); }
        Token t = { 0 }; t.type = TK_SKIP;
        return t;
    }

    // It's a division operator
    return lex_parseSpecial(self);
}

// --- Initialization Logic ---

void lex_init_handlers() {
    if (handlers_initialized) return;

    // 1. Default all to error
    for (int i32 = 0; i32 < 256; i32++) lex_dispatch[i32] = handle_error;

    // 2. Whitespace
    lex_dispatch[' '] = handle_whitespace;
    lex_dispatch['\t'] = handle_whitespace;
    lex_dispatch['\n'] = handle_whitespace;
    lex_dispatch['\r'] = handle_whitespace;

    // 3. Digits
    for (int i32 = '0'; i32 <= '9'; i32++) lex_dispatch[i32] = handle_digit;

    // 4. Identifiers (Alpha + Underscore)
    for (int i32 = 'a'; i32 <= 'z'; i32++) lex_dispatch[i32] = handle_alpha;
    for (int i32 = 'A'; i32 <= 'Z'; i32++) lex_dispatch[i32] = handle_alpha;
    lex_dispatch['_'] = handle_alpha;

    // 5. Special Chars (Operators)
    const char* specials = "(){}[].;,:=!<>+-*^&|%";
    while (*specials) {
        lex_dispatch[(unsigned char)*specials] = handle_special;
        specials++;
    }

    // 6. Specific Handlers
    lex_dispatch['"'] = handle_string;
    lex_dispatch['\''] = handle_char;
    lex_dispatch['/'] = handle_slash;
    lex_dispatch[0] = handle_eof;

    handlers_initialized = 1;
}

// --- Lexer Lifecycle ---

void lex_init(lexer* self, const char* input, const char* filename) {
    self->input = input;
    self->loc.pos.col = 1;
    self->loc.pos.line = 1;
    self->loc.filename = filename ? filename : "script";
}

lexer* lex_create(const char* input, const char* filename) {
    lex_init_handlers(); // Ensure dispatch table is ready

    lexer* self = (lexer*)malloc(sizeof(lexer));
    if (!self) { perror("malloc failed for lexer"); return 0; }
    lex_init(self, input, filename);
    return self;
}

void lex_dispose(lexer** self) {
    if (*self) {
        free(*self);
        *self = NULL;
    }
}

void lex_setSource(lexer* self, const char* source) {
    self->input = source;
    self->loc.pos.col = 1;
    self->loc.pos.line = 1;
}

// --- Helper Functions ---

char lex_getChar(lexer* self) {
    if (!self->input || *self->input == '\0') return '\0';

    char ch = *self->input;
    if (ch == '\n') {
        self->loc.pos.line++;
        self->loc.pos.col = 1;
    }
    else if (ch == '\t') {
        self->loc.pos.col += 4;
    }
    else {
        self->loc.pos.col++;
    }
    return *self->input++;
}

char lex_peekChar(lexer* self) {
    return (self->input) ? *self->input : '\0';
}

int lex_matchChar(lexer* self, char expected) {
    if (lex_peekChar(self) == expected) {
        lex_getChar(self);
        return 1;
    }
    return 0;
}

void lex_skipUntil(lexer* self, const char* delimiter) {
    size_t len = strlen(delimiter);
    while (*self->input != '\0') {
        if (strncmp(self->input, delimiter, len) == 0) return;
        lex_getChar(self);
    }
}

TTOKEN lex_isKeyWord(lexer* self, const Token* token) {
    int length = (int)(token->text.end - token->text.begin);
    if (length >= 2 && length <= 8) {
        const struct Reserved* prw = Keywords[length];
        if (prw) {
            for (; prw->name; ++prw) {
                if (strncmp(prw->name, token->text.begin, length) == 0)
                    return prw->type;
            }
        }
    }
    return TK_IDENTIFIER;
}

// --- Parsing Implementations ---

Token lex_getIdentifier(lexer* self) {
    Token token;
    token.text.begin = self->input;
    token.loc = self->loc;

    while (self->input && (isalnum((unsigned char)*self->input) || *self->input == '_'))
        lex_getChar(self);

    token.text.end = self->input;
    token.type = lex_isKeyWord(self, &token);
    return token;
}

Token lex_getNumber(lexer* self) {
    Token token = { 0 };
    int isFloat = 0;
    token.text.begin = self->input;
    token.loc = self->loc;

    while (self->input && *self->input != '\0') {
        if (*self->input == '.') {
            if (isFloat) break;
            isFloat = 1;
        }
        else if (!isdigit((unsigned char)*self->input)) {
            break;
        }
        lex_getChar(self);
    }

    token.text.end = self->input;
    token.type = isFloat ? TK_FLOATS : TK_INTEGER;
    return token;
}

Token lex_parseLiteral(lexer* self, char delimiter, TTOKEN type) {
    Token token = { 0 };
    token.text.begin = self->input; // content start
    token.loc = self->loc;

    char ch;
    while ((ch = lex_peekChar(self)) != delimiter && ch != '\n' && ch != '\0') {
        if (ch == '\\') lex_getChar(self); // skip escape
        lex_getChar(self);
    }

    token.text.end = self->input;

    if (lex_matchChar(self, delimiter)) {
        token.type = type;
        if (type == TK_CHARACTER) {
            size_t len = token.text.end - token.text.begin;
            // Basic validation for char 'x' or '\n'
            if (len == 0 || (len > 1 && *token.text.begin != '\\') || len > 2) {
                token.type = TK_ERROR;
                printf("Error: Malformed char literal Line %d\n", token.loc.pos.line);
            }
        }
    }
    else {
        token.type = TK_ERROR;
        printf("Error: Unterminated literal Line %d\n", token.loc.pos.line);
    }
    return token;
}

Token lex_parseSpecial(lexer* self) {
    Token token = { 0 };
    token.loc = self->loc;
    token.text.begin = self->input;

    char ch = lex_getChar(self);

    switch (ch) {
    case '.': token.type = TK_DOT; break;
    case ',': token.type = TK_COMMA; break;
    case ';': token.type = TK_SEMICOLON; break;
    case ':': token.type = TK_COLON; break;
    case '{': token.type = TK_OPENCURLYBRACKET; break;
    case '}': token.type = TK_CLOSECURLYBRACKET; break;
    case '[': token.type = TK_OPENBRACKET; break;
    case ']': token.type = TK_CLOSEBRACKET; break;
    case '(': token.type = TK_OPENPARENTHESIS; break;
    case ')': token.type = TK_CLOSEPARENTHESIS; break;

    case '/': token.type = lex_matchChar(self, '=') ? TK_DIVEQ : TK_DIV; break;
    case '=': token.type = lex_matchChar(self, '=') ? TK_EQ : TK_ASSIGN; break;
    case '!': token.type = lex_matchChar(self, '=') ? TK_NE : TK_NOT; break;
    case '*': token.type = lex_matchChar(self, '=') ? TK_MULT_ASSIGN : TK_MULT; break; // Assumed TK_MULT_ASSIGN exists, else remove
    case '^': token.type = TK_POWER; break;

    case '>': token.type = lex_matchChar(self, '=') ? TK_GE : TK_GT; break;
    case '<': token.type = lex_matchChar(self, '=') ? TK_LE : TK_LT; break;

    case '+':
        if (lex_matchChar(self, '=')) token.type = TK_ADD_ASSIGN;
        else if (lex_matchChar(self, '+')) token.type = TK_INC;
        else token.type = TK_PLUS;
        break;

    case '-':
        if (lex_matchChar(self, '=')) token.type = TK_SUB_ASSIGN;
        else if (lex_matchChar(self, '-')) token.type = TK_DEC;
        else if (lex_matchChar(self, '>')) token.type = TK_ARROW;
        else token.type = TK_MINUS;
        break;

    case '|': token.type = lex_matchChar(self, '|') ? TK_OR : TK_BITOR; break;
    case '&': token.type = lex_matchChar(self, '&') ? TK_AND : TK_BITAND; break;

    default: token.type = (TTOKEN)ch; break;
    }

    token.text.end = self->input;
    return token;
}

// --- Main Token Retrieval ---

Token lex_peek(lexer* self) {
    lexer saved = *self;
    Token t = lex_getNextToken(self);
    *self = saved;
    return t;
}

Token lex_getNextToken(lexer* self) {
    Token t;
    while (1) {
        // Cast to unsigned char to prevent negative indexing
        unsigned char index = (unsigned char)(self->input ? *self->input : 0);

        // O(1) Dispatch
        t = lex_dispatch[index](self);

        // Loop if we hit whitespace or comments
        if (t.type != TK_SKIP) {
            return t;
        }
    }
}
