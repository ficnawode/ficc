#ifndef FICC_LEXER_H
#define FICC_LEXER_H

#include "type.h"
#include "util/arena.h"
#include "util/types.h"
#include "util/vec.h"

#define TOKEN_KINDS(X)                                                                             \
    X(TOK_EOF)                                                                                     \
    X(TOK_IDENT)                                                                                   \
    X(TOK_INT_LIT)                                                                                 \
    X(TOK_CHAR_LIT)                                                                                \
    X(TOK_KW_INT)                                                                                  \
    X(TOK_KW_VOID)                                                                                 \
    X(TOK_KW_CHAR)                                                                                 \
    X(TOK_KW_SHORT)                                                                                \
    X(TOK_KW_LONG)                                                                                 \
    X(TOK_KW_UNSIGNED)                                                                             \
    X(TOK_KW_SIGNED)                                                                               \
    X(TOK_KW_RETURN)                                                                               \
    X(TOK_KW_IF)                                                                                   \
    X(TOK_KW_ELSE)                                                                                 \
    X(TOK_KW_WHILE)                                                                                \
    X(TOK_KW_FOR)                                                                                  \
    X(TOK_KW_DO)                                                                                   \
    X(TOK_KW_BREAK)                                                                                \
    X(TOK_KW_CONTINUE)                                                                             \
    X(TOK_KW_GOTO)                                                                                 \
    X(TOK_KW_SWITCH)                                                                               \
    X(TOK_KW_CASE)                                                                                 \
    X(TOK_KW_DEFAULT)                                                                              \
    X(TOK_KW_SIZEOF)                                                                               \
    X(TOK_KW_STRUCT)                                                                               \
    X(TOK_KW_UNION)                                                                                \
    X(TOK_KW_ENUM)                                                                                 \
    X(TOK_KW_STATIC)                                                                               \
    X(TOK_KW_EXTERN)                                                                               \
    X(TOK_KW_CONST)                                                                                \
    X(TOK_KW_TYPEDEF)                                                                              \
    X(TOK_KW_ALIGNOF)                                                                              \
    X(TOK_KW_STATIC_ASSERT)                                                                        \
    X(TOK_KW_BOOL)                                                                                 \
    X(TOK_KW_ALIGNAS)                                                                              \
    X(TOK_DOT)                                                                                     \
    X(TOK_ELLIPSIS)                                                                                \
    X(TOK_ARROW)                                                                                   \
    X(TOK_STRING_LIT)                                                                              \
    X(TOK_LPAREN)                                                                                  \
    X(TOK_RPAREN)                                                                                  \
    X(TOK_LBRACE)                                                                                  \
    X(TOK_RBRACE)                                                                                  \
    X(TOK_LBRACKET)                                                                                \
    X(TOK_RBRACKET)                                                                                \
    X(TOK_SEMI)                                                                                    \
    X(TOK_COLON)                                                                                   \
    X(TOK_COMMA)                                                                                   \
    X(TOK_PLUS)                                                                                    \
    X(TOK_PLUS_PLUS)                                                                               \
    X(TOK_MINUS)                                                                                   \
    X(TOK_MINUS_MINUS)                                                                             \
    X(TOK_STAR)                                                                                    \
    X(TOK_SLASH)                                                                                   \
    X(TOK_PERCENT)                                                                                 \
    X(TOK_ASSIGN)                                                                                  \
    X(TOK_PLUS_ASSIGN)                                                                             \
    X(TOK_MINUS_ASSIGN)                                                                            \
    X(TOK_STAR_ASSIGN)                                                                             \
    X(TOK_SLASH_ASSIGN)                                                                            \
    X(TOK_PERCENT_ASSIGN)                                                                          \
    X(TOK_SHL_ASSIGN)                                                                              \
    X(TOK_SHR_ASSIGN)                                                                              \
    X(TOK_BW_AND_ASSIGN)                                                                           \
    X(TOK_BW_OR_ASSIGN)                                                                            \
    X(TOK_BW_XOR_ASSIGN)                                                                           \
    X(TOK_EQ)                                                                                      \
    X(TOK_NE)                                                                                      \
    X(TOK_LT)                                                                                      \
    X(TOK_GT)                                                                                      \
    X(TOK_LE)                                                                                      \
    X(TOK_GE)                                                                                      \
    X(TOK_LOG_AND)                                                                                 \
    X(TOK_LOG_OR)                                                                                  \
    X(TOK_NOT)                                                                                     \
    X(TOK_QUESTION)                                                                                \
    X(TOK_BW_AND)                                                                                  \
    X(TOK_BW_OR)                                                                                   \
    X(TOK_BW_XOR)                                                                                  \
    X(TOK_SHL)                                                                                     \
    X(TOK_SHR)                                                                                     \
    X(TOK_TILDE)

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
    struct
    {
        bool is_unsigned : 1;
        IntSuffix length : 2;
        bool is_hex : 1;
    } int_suffix;
    u32 str_len;
} Token;

typedef struct LexResult LexResult;
struct LexResult
{
    Token *tokens;
    u64 count;
};

/* Lex source text into a flat array of tokens.
   On lexical error, tokens is NULL and count is 0. */
LexResult lex(const char *file, const char *src, Arena *arena);

/* Translation phase 7: convert a preprocessing-token soup (trivia dropped)
   into the parser's Token[]; merges adjacent string literals. On error,
   tokens is NULL and count is 0. */
LexResult lex_finalize(const Vec *soup, Arena *arena);

const char *token_kind_name(TokenKind kind);

/* Decodes one escape sequence (the leading backslash is already consumed);
   end bounds the spelling, or is NULL for NUL-terminated input. Returns the
   byte (numeric escapes accumulate greedily and may exceed 0xFF), or -1 on a
   hex escape with no digits. Shared by the string lexers and the pp
   destringizer for `_Pragma`. */
int lex_escape_byte(const char **pp, const char *end);

#endif
