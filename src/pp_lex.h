#ifndef FICC_PP_LEX_H
#define FICC_PP_LEX_H

#include "lexer.h"
#include "util/arena.h"
#include "util/types.h"
#include "util/vec.h"

/* C11 §6.4 */
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

/* C11 §6.4.6 */
#define PP_PUNCTS(X)                                                                               \
    X(PP_PUNCT_HASH, "#")                                                                          \
    X(PP_PUNCT_HASHHASH, "##")                                                                     \
    X(PP_PUNCT_LBRACKET, "[")                                                                      \
    X(PP_PUNCT_RBRACKET, "]")                                                                      \
    X(PP_PUNCT_LPAREN, "(")                                                                        \
    X(PP_PUNCT_RPAREN, ")")                                                                        \
    X(PP_PUNCT_LBRACE, "{")                                                                        \
    X(PP_PUNCT_RBRACE, "}")                                                                        \
    X(PP_PUNCT_DOT, ".")                                                                           \
    X(PP_PUNCT_ELLIPSIS, "...")                                                                    \
    X(PP_PUNCT_ARROW, "->")                                                                        \
    X(PP_PUNCT_PLUS, "+")                                                                          \
    X(PP_PUNCT_PLUS_PLUS, "++")                                                                    \
    X(PP_PUNCT_PLUS_ASSIGN, "+=")                                                                  \
    X(PP_PUNCT_MINUS, "-")                                                                         \
    X(PP_PUNCT_MINUS_MINUS, "--")                                                                  \
    X(PP_PUNCT_MINUS_ASSIGN, "-=")                                                                 \
    X(PP_PUNCT_STAR, "*")                                                                          \
    X(PP_PUNCT_STAR_ASSIGN, "*=")                                                                  \
    X(PP_PUNCT_SLASH, "/")                                                                         \
    X(PP_PUNCT_SLASH_ASSIGN, "/=")                                                                 \
    X(PP_PUNCT_PERCENT, "%")                                                                       \
    X(PP_PUNCT_PERCENT_ASSIGN, "%=")                                                               \
    X(PP_PUNCT_TILDE, "~")                                                                         \
    X(PP_PUNCT_BANG, "!")                                                                          \
    X(PP_PUNCT_NE, "!=")                                                                           \
    X(PP_PUNCT_ASSIGN, "=")                                                                        \
    X(PP_PUNCT_EQ, "==")                                                                           \
    X(PP_PUNCT_LT, "<")                                                                            \
    X(PP_PUNCT_GT, ">")                                                                            \
    X(PP_PUNCT_LE, "<=")                                                                           \
    X(PP_PUNCT_GE, ">=")                                                                           \
    X(PP_PUNCT_SHL, "<<")                                                                          \
    X(PP_PUNCT_SHR, ">>")                                                                          \
    X(PP_PUNCT_SHL_ASSIGN, "<<=")                                                                  \
    X(PP_PUNCT_SHR_ASSIGN, ">>=")                                                                  \
    X(PP_PUNCT_AMP, "&")                                                                           \
    X(PP_PUNCT_AMP_ASSIGN, "&=")                                                                   \
    X(PP_PUNCT_ANDAND, "&&")                                                                       \
    X(PP_PUNCT_CARET, "^")                                                                         \
    X(PP_PUNCT_CARET_ASSIGN, "^=")                                                                 \
    X(PP_PUNCT_PIPE, "|")                                                                          \
    X(PP_PUNCT_PIPE_ASSIGN, "|=")                                                                  \
    X(PP_PUNCT_OROR, "||")                                                                         \
    X(PP_PUNCT_QUESTION, "?")                                                                      \
    X(PP_PUNCT_COLON, ":")                                                                         \
    X(PP_PUNCT_SEMI, ";")                                                                          \
    X(PP_PUNCT_COMMA, ",")

typedef enum
{
    PP_PUNCT_NONE,
#define PP_PUNCT_ENUM_ENTRY(K, S) K,
    PP_PUNCTS(PP_PUNCT_ENUM_ENTRY)
#undef PP_PUNCT_ENUM_ENTRY
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

char *pp_prepare(const char *file, const char *src, Arena *arena);

Vec *pp_lex(const char *file, const char *src, Arena *arena);

bool pp_concat_is_punct(const char *a, size_t alen, const char *b, size_t blen);

const char *pp_kind_name(PpKind kind);

#endif
