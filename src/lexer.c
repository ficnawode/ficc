#include "lexer.h"
#include "util/bytebuf.h"
#include "util/vec.h"
#include <stdio.h>
#include <string.h>

typedef struct LexerCtx LexerCtx;
struct LexerCtx
{
    const char *file;
    Arena *arena;
    Vec *tokens;
    u32 line;
    u32 col;
    const char *p; /* current read cursor */
    u32 error_count;
};

const char *token_kind_name(TokenKind kind)
{
    switch (kind)
    {
#define CASE(K)                                                                                    \
    case K:                                                                                        \
        return #K;
        TOKEN_KINDS(CASE)
#undef CASE
    }
    return "TOK_UNKNOWN";
}

static bool is_identifier_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_identifier_char(char c)
{
    return is_identifier_start(c) || (c >= '0' && c <= '9');
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool is_hex_digit(char c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static u8 hex_digit_value(char c)
{
    if (is_digit(c))
    {
        return (u8) (c - '0');
    }
    return (u8) (c - (c >= 'a' ? 'a' : 'A') + 10);
}

static bool is_whitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

typedef struct
{
    const char *name;
    TokenKind kind;
} Keyword;

static const Keyword KEYWORDS[] = {
    {"_Alignas", TOK_KW_ALIGNAS},
    {"_Alignof", TOK_KW_ALIGNOF},
    {"_Bool", TOK_KW_BOOL},
    {"_Static_assert", TOK_KW_STATIC_ASSERT},
    {"break", TOK_KW_BREAK},
    {"case", TOK_KW_CASE},
    {"char", TOK_KW_CHAR},
    {"const", TOK_KW_CONST},
    {"continue", TOK_KW_CONTINUE},
    {"default", TOK_KW_DEFAULT},
    {"do", TOK_KW_DO},
    {"else", TOK_KW_ELSE},
    {"enum", TOK_KW_ENUM},
    {"extern", TOK_KW_EXTERN},
    {"for", TOK_KW_FOR},
    {"goto", TOK_KW_GOTO},
    {"if", TOK_KW_IF},
    {"int", TOK_KW_INT},
    {"long", TOK_KW_LONG},
    {"return", TOK_KW_RETURN},
    {"short", TOK_KW_SHORT},
    {"signed", TOK_KW_SIGNED},
    {"sizeof", TOK_KW_SIZEOF},
    {"static", TOK_KW_STATIC},
    {"struct", TOK_KW_STRUCT},
    {"switch", TOK_KW_SWITCH},
    {"typedef", TOK_KW_TYPEDEF},
    {"union", TOK_KW_UNION},
    {"unsigned", TOK_KW_UNSIGNED},
    {"void", TOK_KW_VOID},
    {"while", TOK_KW_WHILE},
};

static TokenKind keyword_kind(const char *name)
{
    for (size_t i = 0; i < sizeof(KEYWORDS) / sizeof(KEYWORDS[0]); i++)
    {
        if (strcmp(KEYWORDS[i].name, name) == 0)
        {
            return KEYWORDS[i].kind;
        }
    }
    return TOK_IDENT;
}

typedef struct
{
    const char *spelling;
    TokenKind kind;
} Punct;

/* longest first for max munch */
static const Punct PUNCTS[] = {
    {"...", TOK_ELLIPSIS},
    {"<<=", TOK_SHL_ASSIGN},
    {">>=", TOK_SHR_ASSIGN},
    {"!=", TOK_NE},
    {"%=", TOK_PERCENT_ASSIGN},
    {"&&", TOK_LOG_AND},
    {"&=", TOK_BW_AND_ASSIGN},
    {"*=", TOK_STAR_ASSIGN},
    {"++", TOK_PLUS_PLUS},
    {"+=", TOK_PLUS_ASSIGN},
    {"--", TOK_MINUS_MINUS},
    {"-=", TOK_MINUS_ASSIGN},
    {"->", TOK_ARROW},
    {"/=", TOK_SLASH_ASSIGN},
    {"<<", TOK_SHL},
    {"<=", TOK_LE},
    {"==", TOK_EQ},
    {">=", TOK_GE},
    {">>", TOK_SHR},
    {"^=", TOK_BW_XOR_ASSIGN},
    {"|=", TOK_BW_OR_ASSIGN},
    {"||", TOK_LOG_OR},
    {"!", TOK_NOT},
    {"%", TOK_PERCENT},
    {"&", TOK_BW_AND},
    {"(", TOK_LPAREN},
    {")", TOK_RPAREN},
    {"*", TOK_STAR},
    {"+", TOK_PLUS},
    {",", TOK_COMMA},
    {"-", TOK_MINUS},
    {".", TOK_DOT},
    {"/", TOK_SLASH},
    {":", TOK_COLON},
    {";", TOK_SEMI},
    {"<", TOK_LT},
    {"=", TOK_ASSIGN},
    {">", TOK_GT},
    {"?", TOK_QUESTION},
    {"[", TOK_LBRACKET},
    {"]", TOK_RBRACKET},
    {"^", TOK_BW_XOR},
    {"{", TOK_LBRACE},
    {"|", TOK_BW_OR},
    {"}", TOK_RBRACE},
    {"~", TOK_TILDE},
};

static const Punct *punct_lookup(const char *s)
{
    for (size_t i = 0; i < sizeof(PUNCTS) / sizeof(PUNCTS[0]); i++)
    {
        if (strncmp(PUNCTS[i].spelling, s, strlen(PUNCTS[i].spelling)) == 0)
        {
            return &PUNCTS[i];
        }
    }
    return NULL;
}

static void lexer_init(LexerCtx *ctx, const char *file, const char *src, Arena *arena)
{
    ctx->file = file;
    ctx->arena = arena;
    ctx->tokens = vec_new(arena);
    ctx->line = 1;
    ctx->col = 1;
    ctx->p = src;
    ctx->error_count = 0;
}

static void lexer_error(LexerCtx *ctx, const char *msg)
{
    fprintf(stderr, "%s:%u:%u: [lex] error: %s\n", ctx->file, ctx->line, ctx->col, msg);
    ctx->error_count++;
}

static Loc lexer_loc(const LexerCtx *ctx)
{
    return (Loc) {.file = ctx->file, .line = ctx->line, .col = ctx->col};
}

static void lexer_advance(LexerCtx *ctx)
{
    switch (*ctx->p)
    {
        case '\n':
            ctx->line++;
            ctx->col = 1;
            break;
        case '\t':
            ctx->col = (ctx->col + 7) & ~7U; /* tab stop alignment */
            break;
        default:
            ctx->col++;
    }
    ctx->p++;
}

static void lexer_push(LexerCtx *ctx, Token token)
{
    Token *slot = arena_alloc(ctx->arena, sizeof(Token), sizeof(void *));
    *slot = token;
    vec_push(ctx->tokens, slot);
}

static void lex_skip_whitespace(LexerCtx *ctx)
{
    while (is_whitespace(*ctx->p))
    {
        lexer_advance(ctx);
    }
}

/* Decode one escape sequence; the leading backslash is already consumed.
   Returns e.g. 'n' -> '\n'. Numeric escapes accumulate greedily and can return
   values > 0xFF, which callers reject. Unknown escapes keep the character. */
static int lex_escape(LexerCtx *ctx)
{
    static const char simple_codes[] = "abfnrtv\\'\"?";
    static const char simple_vals[] = {'\a', '\b', '\f', '\n', '\r', '\t',
                                       '\v', '\\', '\'', '"',  '?'};

    char c = *ctx->p;
    const char *hit = strchr(simple_codes, c);
    if (hit)
    {
        lexer_advance(ctx);
        return simple_vals[hit - simple_codes];
    }

    if (c >= '0' && c <= '7')
    {
        int val = 0;
        for (int i = 0; i < 3 && *ctx->p >= '0' && *ctx->p <= '7'; i++)
        {
            if (val <= 0xFF)
            {
                val = val * 8 + (*ctx->p - '0');
            }
            lexer_advance(ctx);
        }
        return val;
    }

    if (c == 'x' || c == 'X')
    {
        lexer_advance(ctx);
        if (!is_hex_digit(*ctx->p))
        {
            lexer_error(ctx, "hexadecimal escape sequence with no digits");
            return 0;
        }
        int val = 0;
        while (is_hex_digit(*ctx->p))
        {
            if (val <= 0xFF)
            {
                val = val * 16 + hex_digit_value(*ctx->p);
            }
            lexer_advance(ctx);
        }
        return val;
    }

    lexer_advance(ctx);
    return c;
}

static void lex_number(LexerCtx *ctx)
{
    Loc loc = lexer_loc(ctx);

    u64 base = 10;
    bool is_hex = false;
    if (ctx->p[0] == '0' && (ctx->p[1] == 'x' || ctx->p[1] == 'X'))
    {
        base = 16;
        is_hex = true;
        lexer_advance(ctx);
        lexer_advance(ctx);
    }

    u64 val = 0;
    bool overflow = false;
    while (base == 16 ? is_hex_digit(*ctx->p) : is_digit(*ctx->p))
    {
        u64 digit = hex_digit_value(*ctx->p);
        if (!overflow && val > ((u64) INT64_MAX - digit) / base)
        {
            lexer_error(ctx, "integer literal overflow");
            val = (u64) INT64_MAX;
            overflow = true;
        }
        else if (!overflow)
        {
            val = val * base + digit;
        }
        lexer_advance(ctx);
    }

    /* Integer suffix: u? l? l? — plus a trailing u only when no leading one
       was seen. Accepts u, ul, ull, l, ll, lu, llu (case-insensitive). */
    bool is_unsigned = false;
    IntSuffix length = SUFFIX_NONE;
    if (*ctx->p == 'u' || *ctx->p == 'U')
    {
        is_unsigned = true;
        lexer_advance(ctx);
    }
    if (*ctx->p == 'l' || *ctx->p == 'L')
    {
        length = SUFFIX_L;
        lexer_advance(ctx);
        if (*ctx->p == 'l' || *ctx->p == 'L')
        {
            length = SUFFIX_LL;
            lexer_advance(ctx);
        }
    }
    if (!is_unsigned && (*ctx->p == 'u' || *ctx->p == 'U'))
    {
        is_unsigned = true;
        lexer_advance(ctx);
    }

    if (is_identifier_start(*ctx->p))
    {
        lexer_error(ctx, "invalid suffix on integer literal");
        while (is_identifier_char(*ctx->p))
        {
            lexer_advance(ctx);
        }
    }

    lexer_push(ctx, (Token) {.kind = TOK_INT_LIT,
                             .loc = loc,
                             .payload = {.int_val = (i64) val},
                             .int_suffix = {
                                 .is_unsigned = is_unsigned, .length = length, .is_hex = is_hex}});
}

static void lex_ident(LexerCtx *ctx)
{
    Loc loc = lexer_loc(ctx);
    const char *start = ctx->p;

    while (is_identifier_char(*ctx->p))
    {
        lexer_advance(ctx);
    }

    size_t len = (size_t) (ctx->p - start);
    char *buf = arena_alloc(ctx->arena, len + 1, 1);
    memcpy(buf, start, len);
    buf[len] = '\0';

    lexer_push(ctx, (Token) {.kind = keyword_kind(buf), .loc = loc, .payload.str = buf});
}

static void lex_comment(LexerCtx *ctx)
{
    lexer_advance(ctx);
    lexer_advance(ctx);

    while (*ctx->p)
    {
        if (ctx->p[0] == '*' && ctx->p[1] == '/')
        {
            lexer_advance(ctx);
            lexer_advance(ctx);
            return;
        }
        lexer_advance(ctx);
    }

    lexer_error(ctx, "unterminated comment");
}

static void lex_line_comment(LexerCtx *ctx)
{
    lexer_advance(ctx);
    lexer_advance(ctx);

    while (*ctx->p && *ctx->p != '\n')
    {
        lexer_advance(ctx);
    }
}

static void lex_string(LexerCtx *ctx)
{
    Loc loc = lexer_loc(ctx);
    lexer_advance(ctx); /* opening quote */

    ByteBuf buf;
    bytebuf_init(&buf, ctx->arena);

    while (*ctx->p && *ctx->p != '"')
    {
        if (*ctx->p == '\\')
        {
            lexer_advance(ctx); /* backslash */
            bytebuf_append(&buf, (u8) lex_escape(ctx));
        }
        else if (*ctx->p == '\n')
        {
            lexer_error(ctx, "unterminated string literal");
            return;
        }
        else
        {
            bytebuf_append(&buf, (u8) *ctx->p);
            lexer_advance(ctx);
        }
    }

    if (*ctx->p != '"')
    {
        lexer_error(ctx, "unterminated string literal");
        return;
    }
    lexer_advance(ctx); /* closing quote */

    size_t content_len = bytebuf_len(&buf);
    bytebuf_append(&buf, '\0');

    lexer_push(ctx, (Token) {.kind = TOK_STRING_LIT,
                             .loc = loc,
                             .payload.str = (const char *) bytebuf_data(&buf),
                             .str_len = (u32) content_len});
}

static void lex_char(LexerCtx *ctx)
{
    Loc loc = lexer_loc(ctx);
    lexer_advance(ctx); /* opening ' */

    if (*ctx->p == '\'')
    {
        lexer_error(ctx, "empty character constant");
        lexer_advance(ctx); /* closing ', so the error doesn't cascade */
        return;
    }

    if (*ctx->p == '\n' || *ctx->p == '\0')
    {
        lexer_error(ctx, "unterminated character constant");
        return;
    }

    int val;
    if (*ctx->p == '\\')
    {
        lexer_advance(ctx); /* backslash */
        val = lex_escape(ctx);
    }
    else
    {
        val = (u8) *ctx->p;
        lexer_advance(ctx);
    }

    if (*ctx->p != '\'')
    {
        lexer_error(ctx, *ctx->p && *ctx->p != '\n' ? "multi-character character constant"
                                                    : "unterminated character constant");
        while (*ctx->p && *ctx->p != '\'' && *ctx->p != '\n')
        {
            lexer_advance(ctx);
        }
        if (*ctx->p == '\'')
        {
            lexer_advance(ctx);
        }
        return;
    }
    lexer_advance(ctx); /* closing ' */

    if (val > 0xFF)
    {
        lexer_error(ctx, "character constant exceeds bounds of type char");
    }

    lexer_push(ctx, (Token) {.kind = TOK_CHAR_LIT, .loc = loc, .payload.int_val = val});
}

static bool lex_punct(LexerCtx *ctx)
{
    const Punct *m = punct_lookup(ctx->p);
    if (!m)
    {
        return false;
    }

    Loc loc = lexer_loc(ctx);
    size_t len = strlen(m->spelling);
    for (size_t i = 0; i < len; i++)
    {
        lexer_advance(ctx);
    }
    lexer_push(ctx, (Token) {.kind = m->kind, .loc = loc});
    return true;
}

LexResult lex(const char *file, const char *src, Arena *arena)
{
    LexerCtx ctx;
    lexer_init(&ctx, file, src, arena);

    while (*ctx.p)
    {
        if (is_whitespace(*ctx.p))
        {
            lex_skip_whitespace(&ctx);
        }
        else if (is_digit(*ctx.p))
        {
            lex_number(&ctx);
        }
        else if (is_identifier_start(*ctx.p))
        {
            lex_ident(&ctx);
        }
        else if (*ctx.p == '"')
        {
            lex_string(&ctx);
        }
        else if (*ctx.p == '\'')
        {
            lex_char(&ctx);
        }
        else if (*ctx.p == '/' && ctx.p[1] == '*')
        {
            lex_comment(&ctx);
        }
        else if (*ctx.p == '/' && ctx.p[1] == '/')
        {
            lex_line_comment(&ctx);
        }
        else if (!lex_punct(&ctx))
        {
            lexer_error(&ctx, "unknown character");
            lexer_advance(&ctx);
        }
    }

    lexer_push(&ctx, (Token) {.kind = TOK_EOF, .loc = lexer_loc(&ctx)});

    size_t count = vec_size(ctx.tokens);
    Token *arr = arena_alloc(arena, count * sizeof(Token), sizeof(void *));
    for (size_t i = 0; i < count; i++)
    {
        Token *slot = (Token *) vec_get(ctx.tokens, i);
        arr[i] = *slot;
    }

    if (ctx.error_count > 0)
    {
        return (LexResult) {0};
    }
    return (LexResult) {.tokens = arr, .count = (u64) count};
}
