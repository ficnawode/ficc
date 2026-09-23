#ifndef FICC_LEXER_H
#define FICC_LEXER_H

#include "type.h"
#include "util/arena.h"
#include "util/types.h"
#include "util/vec.h"

/* Floating constant kinds (§6.4.4.2). */
typedef enum
{
    FK_FLOAT,
    FK_DOUBLE,
    FK_LONG, /* x87 80-bit long double */
} FloatKind;

/* Character/string literal element kind (§6.4.4.4, §6.4.5). The encoding
   prefix selects the element width: none / u8 -> char (1), L -> wchar_t (4),
   u -> char16_t (2), U -> char32_t (4). `payload.str` holds the decoded
   elements in little-endian order, NUL-terminated by one element. */
typedef enum
{
    STRK_NARROW, /* "" or u8"" */
    STRK_WIDE,   /* L""  */
    STRK_UTF16,  /* u""  */
    STRK_UTF32,  /* U""  */
} StrKind;

/* Element size in bytes for a string/char literal kind. */
u32 str_kind_elem_size(StrKind kind);

#define TOKEN_KINDS(X)                                                                             \
    X(TOK_EOF)                                                                                     \
    X(TOK_IDENT)                                                                                   \
    X(TOK_INT_LIT)                                                                                 \
    X(TOK_FLOAT_LIT)                                                                               \
    X(TOK_CHAR_LIT)                                                                                \
    X(TOK_KW_INT)                                                                                  \
    X(TOK_KW_VOID)                                                                                 \
    X(TOK_KW_CHAR)                                                                                 \
    X(TOK_KW_SHORT)                                                                                \
    X(TOK_KW_LONG)                                                                                 \
    X(TOK_KW_UNSIGNED)                                                                             \
    X(TOK_KW_SIGNED)                                                                               \
    X(TOK_KW_FLOAT)                                                                                \
    X(TOK_KW_DOUBLE)                                                                               \
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
    X(TOK_KW_VOLATILE)                                                                             \
    X(TOK_KW_RESTRICT)                                                                             \
    X(TOK_KW_TYPEDEF)                                                                              \
    X(TOK_KW_INLINE)                                                                               \
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
    X(TOK_TILDE)                                                                                   \
    X(TOK_KW_GENERIC)                                                                              \
    X(TOK_KW_EXTENSION)                                                                            \
    X(TOK_KW_REGISTER)                                                                             \
    X(TOK_KW_AUTO)                                                                                 \
    X(TOK_KW_NORETURN)

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
        u64 float_pat;      /* IEEE-754 bit pattern: low 32 = float, all 64 = double */
        long double ld_val; /* host 80-bit value (FK_LONG) */
    } payload;
    struct
    {
        bool is_unsigned : 1;
        IntSuffix length : 2;
        bool is_hex : 1;
    } int_suffix;
    FloatKind float_kind; /* valid when kind == TOK_FLOAT_LIT */
    u32 str_len;
    StrKind str_kind; /* valid when kind == TOK_STRING_LIT / TOK_CHAR_LIT */
} Token;

typedef struct LexResult LexResult;
struct LexResult
{
    Token *tokens;
    u64 count;
};

/* Translation phase 7: convert a preprocessing-token soup (trivia dropped)
   into the parser's Token[]; merges adjacent string literals. On error,
   tokens is NULL and count is 0. */
LexResult lex_finalize(const Vec *soup, Arena *arena);

const char *token_kind_name(TokenKind kind);

/* Decodes one escape sequence (the leading backslash is already consumed);
   end bounds the spelling, or is NULL for NUL-terminated input. Returns the
   byte (numeric escapes accumulate greedily and may exceed 0xFF), or -1 on a
   hex escape with no digits. Shared by the two string-value decoders. */
int lex_escape_byte(const char **pp, const char *end);

#endif
