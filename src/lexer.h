#ifndef FICC_LEXER_H
#define FICC_LEXER_H

#include "util/arena.h"
#include "util/types.h"

#define TOKEN_KINDS(X)                                                                             \
    X(TOK_EOF)                                                                                     \
    X(TOK_IDENT)                                                                                   \
    X(TOK_INT_LIT)                                                                                 \
    X(TOK_KW_INT)                                                                                  \
    X(TOK_KW_VOID)                                                                                 \
    X(TOK_KW_RETURN)                                                                               \
    X(TOK_LPAREN)                                                                                  \
    X(TOK_RPAREN)                                                                                  \
    X(TOK_LBRACE)                                                                                  \
    X(TOK_RBRACE)                                                                                  \
    X(TOK_SEMI)

typedef enum
{
#define ENUM_ENTRY(K) K,
    TOKEN_KINDS(ENUM_ENTRY)
#undef ENUM_ENTRY
} TokenKind;

typedef struct
{
    const char *file;
    u32 line;
    u32 col;
} Loc;

typedef struct
{
    TokenKind kind;
    Loc loc;
    union
    {
        i64 int_val;
        const char *str;
    } payload;
} Token;

/* Lex source text into a flat array of tokens.
   Returns the token array; writes count into *out_count.
   Returns NULL if any lexical errors were encountered. */
Token *lex(const char *file, const char *src, Arena *arena, u64 *out_count);

const char *token_kind_name(TokenKind kind);

#endif
