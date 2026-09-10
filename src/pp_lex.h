#ifndef FICC_PP_LEX_H
#define FICC_PP_LEX_H

#include "lexer.h"
#include "util/arena.h"
#include "util/types.h"

/* Preprocessing-token kinds (C11 §6.4). Append only. */
#define PP_KINDS(X)                                                                                \
    X(TOK_PP_EOF)                                                                                  \
    X(TOK_PP_TRIVIA_WS)                                                                            \
    X(TOK_PP_TRIVIA_NL)                                                                            \
    X(TOK_PP_TRIVIA_COMMENT)                                                                       \
    X(TOK_PP_IDENT)                                                                                \
    X(TOK_PP_NUMBER)                                                                               \
    X(TOK_PP_STRING)                                                                               \
    X(TOK_PP_CHAR)                                                                                 \
    X(TOK_PP_PUNCT)                                                                                \
    X(TOK_PP_OTHER)                                                                                \
    X(TOK_PP_HEADER_NAME)                                                                          \
    X(TOK_PP_PARAM)

typedef enum
{
#define PP_ENUM_ENTRY(K) K,
    PP_KINDS(PP_ENUM_ENTRY)
#undef PP_ENUM_ENTRY
} PpKind;

typedef struct PpToken
{
    PpKind kind;
    Loc loc;
    const char *spell;
    u32 len;
    bool has_newline;
    u32 param_idx;
} PpToken;

/* Translation phases 1-2: trigraph replacement and backslash-newline
   splicing. Returns a newly arena-allocated NUL-terminated buffer, or NULL
   after reporting a diagnostic. */
char *pp_prepare(const char *file, const char *src, Arena *arena);

#endif
