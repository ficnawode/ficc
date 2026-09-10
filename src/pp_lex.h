#ifndef FICC_PP_LEX_H
#define FICC_PP_LEX_H

#include "lexer.h"
#include "util/arena.h"
#include "util/types.h"
#include "util/vec.h"

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

/* Punctuator identity (C11 §6.4.6), resolved from its spelling at scan time
   so neither the pp nor the finalizer re-matches spellings. Digraphs map to
   their primary's id. PP_PUNCT_NONE is the default for non-punctuators. */
typedef enum
{
    PP_PUNCT_NONE,
    PP_PUNCT_HASH,
    PP_PUNCT_HASHHASH,
    PP_PUNCT_LBRACKET,
    PP_PUNCT_RBRACKET,
    PP_PUNCT_LPAREN,
    PP_PUNCT_RPAREN,
    PP_PUNCT_LBRACE,
    PP_PUNCT_RBRACE,
    PP_PUNCT_DOT,
    PP_PUNCT_ELLIPSIS,
    PP_PUNCT_ARROW,
    PP_PUNCT_PLUS,
    PP_PUNCT_PLUS_PLUS,
    PP_PUNCT_PLUS_ASSIGN,
    PP_PUNCT_MINUS,
    PP_PUNCT_MINUS_MINUS,
    PP_PUNCT_MINUS_ASSIGN,
    PP_PUNCT_STAR,
    PP_PUNCT_STAR_ASSIGN,
    PP_PUNCT_SLASH,
    PP_PUNCT_SLASH_ASSIGN,
    PP_PUNCT_PERCENT,
    PP_PUNCT_PERCENT_ASSIGN,
    PP_PUNCT_TILDE,
    PP_PUNCT_BANG,
    PP_PUNCT_NE,
    PP_PUNCT_ASSIGN,
    PP_PUNCT_EQ,
    PP_PUNCT_LT,
    PP_PUNCT_GT,
    PP_PUNCT_LE,
    PP_PUNCT_GE,
    PP_PUNCT_SHL,
    PP_PUNCT_SHR,
    PP_PUNCT_SHL_ASSIGN,
    PP_PUNCT_SHR_ASSIGN,
    PP_PUNCT_AMP,
    PP_PUNCT_AMP_ASSIGN,
    PP_PUNCT_ANDAND,
    PP_PUNCT_CARET,
    PP_PUNCT_CARET_ASSIGN,
    PP_PUNCT_PIPE,
    PP_PUNCT_PIPE_ASSIGN,
    PP_PUNCT_OROR,
    PP_PUNCT_QUESTION,
    PP_PUNCT_COLON,
    PP_PUNCT_SEMI,
    PP_PUNCT_COMMA,
} PpPunct;

typedef struct Hideset Hideset;

typedef struct PpToken
{
    PpKind kind;
    Loc loc;
    const char *spell;
    u32 len;
    bool has_newline;
    u32 param_idx;
    PpPunct punct;
    Hideset *hide;
} PpToken;

/* Translation phases 1-2: trigraph replacement and backslash-newline
   splicing. Returns a newly arena-allocated NUL-terminated buffer, or NULL
   after reporting a diagnostic. */
char *pp_prepare(const char *file, const char *src, Arena *arena);

/* Translation phase 3: scans normalized source into a soup of PpToken*
   (spellings slice the normalized buffer). Returns an arena-allocated Vec
   terminated by a TOK_PP_EOF token, or NULL after reporting a diagnostic. */
Vec *pp_lex(const char *file, const char *src, Arena *arena);

const char *pp_kind_name(PpKind kind);

#endif
