#include "lexer.h"
#include "pp_lex.h"
#include "util/bytebuf.h"
#include "util/intern.h"
#include "util/vec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Decimal/hex→IEEE conversions come from glibc (C11 §6.4.4.2p7). */
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

typedef struct
{
    const char *name;
    TokenKind kind;
} Keyword;

static const Keyword KEYWORDS[] = {
    {"_Alignas", TOK_KW_ALIGNAS},
    {"_Alignof", TOK_KW_ALIGNOF},
    {"_Bool", TOK_KW_BOOL},
    {"_Generic", TOK_KW_GENERIC},
    {"_Static_assert", TOK_KW_STATIC_ASSERT},
    {"_Noreturn", TOK_KW_NORETURN},
    {"auto", TOK_KW_AUTO},
    {"register", TOK_KW_REGISTER},
    {"__const", TOK_KW_CONST},
    {"__const__", TOK_KW_CONST},
    {"__extension__", TOK_KW_EXTENSION},
    {"__inline", TOK_KW_INLINE},
    {"__inline__", TOK_KW_INLINE},
    {"__restrict", TOK_KW_RESTRICT},
    {"__restrict__", TOK_KW_RESTRICT},
    {"__signed", TOK_KW_SIGNED},
    {"__signed__", TOK_KW_SIGNED},
    {"__volatile", TOK_KW_VOLATILE},
    {"__volatile__", TOK_KW_VOLATILE},
    {"break", TOK_KW_BREAK},
    {"case", TOK_KW_CASE},
    {"char", TOK_KW_CHAR},
    {"const", TOK_KW_CONST},
    {"continue", TOK_KW_CONTINUE},
    {"default", TOK_KW_DEFAULT},
    {"do", TOK_KW_DO},
    {"double", TOK_KW_DOUBLE},
    {"else", TOK_KW_ELSE},
    {"enum", TOK_KW_ENUM},
    {"extern", TOK_KW_EXTERN},
    {"float", TOK_KW_FLOAT},
    {"for", TOK_KW_FOR},
    {"goto", TOK_KW_GOTO},
    {"if", TOK_KW_IF},
    {"inline", TOK_KW_INLINE},
    {"int", TOK_KW_INT},
    {"long", TOK_KW_LONG},
    {"return", TOK_KW_RETURN},
    {"restrict", TOK_KW_RESTRICT},
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
    {"volatile", TOK_KW_VOLATILE},
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

/* Decode one escape sequence; the leading backslash is already consumed.
   Shared by the legacy string lexer, the finalizer's string decoder, and the
   pp destringizer for `_Pragma` (C11 §6.10.9). Returns e.g. 'n' -> '\n'.
   Numeric escapes accumulate greedily and can return values > 0xFF, which
   callers reject. Unknown escapes keep the character. */
int lex_escape_byte(const char **pp, const char *end)
{
    static const char simple_codes[] = "abfnrtv\\'\"?";
    static const char simple_vals[] = {'\a', '\b', '\f', '\n', '\r', '\t',
                                       '\v', '\\', '\'', '"',  '?'};

    const char *p = *pp;
    if (end == NULL)
    {
        end = p + strlen(p);
    }
    if (p >= end)
    {
        return 0;
    }

    char c = *p;
    const char *hit = strchr(simple_codes, c);
    if (hit)
    {
        *pp = p + 1;
        return simple_vals[hit - simple_codes];
    }

    if (c >= '0' && c <= '7')
    {
        int val = 0;
        for (int i = 0; i < 3 && p < end && *p >= '0' && *p <= '7'; i++)
        {
            if (val <= 0xFF)
            {
                val = val * 8 + (*p - '0');
            }
            p++;
        }
        *pp = p;
        return val;
    }

    if (c == 'x' || c == 'X')
    {
        p++;
        if (p >= end || !is_hex_digit(*p))
        {
            *pp = p;
            return -1;
        }
        int val = 0;
        while (p < end && is_hex_digit(*p))
        {
            if (val <= 0xFF)
            {
                val = val * 16 + hex_digit_value(*p);
            }
            p++;
        }
        *pp = p;
        return val;
    }

    *pp = p + 1;
    return c;
}

typedef struct FinalizeCtx FinalizeCtx;

struct FinalizeCtx
{
    Arena *arena;
    Vec *tokens;
    InternPool *files;
    u32 error_count;
};

static void finalize_error(FinalizeCtx *ctx, Loc loc, const char *msg)
{
    fprintf(stderr, "%s:%u:%u: [lex] error: %s\n", loc.file, loc.line, loc.col, msg);
    ctx->error_count++;
}

/* Appends token, merging it into the previous token when both are adjacent
   string literals (C11 §5.1.1.2 phase 6). */
static void finalize_push_token(FinalizeCtx *ctx, Token token)
{
    if (token.kind == TOK_STRING_LIT && vec_size(ctx->tokens) > 0)
    {
        Token *prev = vec_last(ctx->tokens);
        if (prev->kind == TOK_STRING_LIT)
        {
            u32 prev_len = prev->str_len;
            u32 add_len = token.str_len;
            char *buf = arena_alloc(ctx->arena, (size_t) prev_len + add_len + 1, 1);
            memcpy(buf, prev->payload.str, prev_len);
            memcpy(buf + prev_len, token.payload.str, add_len);
            buf[prev_len + add_len] = '\0';
            prev->payload.str = buf;
            prev->str_len = prev_len + add_len;
            return;
        }
    }

    Token *slot = arena_alloc(ctx->arena, sizeof(Token), _Alignof(Token));
    *slot = token;
    if (slot->loc.file)
    {
        /* The soup's file strings live in the pp arena, which the driver frees
           right after finalize; intern them so later diagnostics stay valid. */
        slot->loc.file = intern(ctx->files, slot->loc.file);
    }
    vec_push(ctx->tokens, slot);
}

static void finalize_ident(FinalizeCtx *ctx, const PpToken *tok)
{
    char *buf = arena_alloc(ctx->arena, tok->len + 1, 1);
    memcpy(buf, tok->spell, tok->len);
    buf[tok->len] = '\0';
    finalize_push_token(ctx,
                        (Token) {.kind = keyword_kind(buf), .loc = tok->loc, .payload.str = buf});
}

/* Maps a pp punctuator id to the parser's token kind. `#`/`##` have no
   TokenKind and must never reach the finalizer (C11 §6.4.6). */
static void finalize_punct(FinalizeCtx *ctx, const PpToken *tok)
{
    TokenKind kind;
    switch (tok->punct)
    {
        case PP_PUNCT_NONE:
        case PP_PUNCT_HASH:
        case PP_PUNCT_HASHHASH:
            finalize_error(ctx, tok->loc, "stray '#' in program");
            return;
        case PP_PUNCT_LBRACKET:
            kind = TOK_LBRACKET;
            break;
        case PP_PUNCT_RBRACKET:
            kind = TOK_RBRACKET;
            break;
        case PP_PUNCT_LPAREN:
            kind = TOK_LPAREN;
            break;
        case PP_PUNCT_RPAREN:
            kind = TOK_RPAREN;
            break;
        case PP_PUNCT_LBRACE:
            kind = TOK_LBRACE;
            break;
        case PP_PUNCT_RBRACE:
            kind = TOK_RBRACE;
            break;
        case PP_PUNCT_DOT:
            kind = TOK_DOT;
            break;
        case PP_PUNCT_ELLIPSIS:
            kind = TOK_ELLIPSIS;
            break;
        case PP_PUNCT_ARROW:
            kind = TOK_ARROW;
            break;
        case PP_PUNCT_PLUS:
            kind = TOK_PLUS;
            break;
        case PP_PUNCT_PLUS_PLUS:
            kind = TOK_PLUS_PLUS;
            break;
        case PP_PUNCT_PLUS_ASSIGN:
            kind = TOK_PLUS_ASSIGN;
            break;
        case PP_PUNCT_MINUS:
            kind = TOK_MINUS;
            break;
        case PP_PUNCT_MINUS_MINUS:
            kind = TOK_MINUS_MINUS;
            break;
        case PP_PUNCT_MINUS_ASSIGN:
            kind = TOK_MINUS_ASSIGN;
            break;
        case PP_PUNCT_STAR:
            kind = TOK_STAR;
            break;
        case PP_PUNCT_STAR_ASSIGN:
            kind = TOK_STAR_ASSIGN;
            break;
        case PP_PUNCT_SLASH:
            kind = TOK_SLASH;
            break;
        case PP_PUNCT_SLASH_ASSIGN:
            kind = TOK_SLASH_ASSIGN;
            break;
        case PP_PUNCT_PERCENT:
            kind = TOK_PERCENT;
            break;
        case PP_PUNCT_PERCENT_ASSIGN:
            kind = TOK_PERCENT_ASSIGN;
            break;
        case PP_PUNCT_TILDE:
            kind = TOK_TILDE;
            break;
        case PP_PUNCT_BANG:
            kind = TOK_NOT;
            break;
        case PP_PUNCT_NE:
            kind = TOK_NE;
            break;
        case PP_PUNCT_ASSIGN:
            kind = TOK_ASSIGN;
            break;
        case PP_PUNCT_EQ:
            kind = TOK_EQ;
            break;
        case PP_PUNCT_LT:
            kind = TOK_LT;
            break;
        case PP_PUNCT_GT:
            kind = TOK_GT;
            break;
        case PP_PUNCT_LE:
            kind = TOK_LE;
            break;
        case PP_PUNCT_GE:
            kind = TOK_GE;
            break;
        case PP_PUNCT_SHL:
            kind = TOK_SHL;
            break;
        case PP_PUNCT_SHR:
            kind = TOK_SHR;
            break;
        case PP_PUNCT_SHL_ASSIGN:
            kind = TOK_SHL_ASSIGN;
            break;
        case PP_PUNCT_SHR_ASSIGN:
            kind = TOK_SHR_ASSIGN;
            break;
        case PP_PUNCT_AMP:
            kind = TOK_BW_AND;
            break;
        case PP_PUNCT_AMP_ASSIGN:
            kind = TOK_BW_AND_ASSIGN;
            break;
        case PP_PUNCT_ANDAND:
            kind = TOK_LOG_AND;
            break;
        case PP_PUNCT_CARET:
            kind = TOK_BW_XOR;
            break;
        case PP_PUNCT_CARET_ASSIGN:
            kind = TOK_BW_XOR_ASSIGN;
            break;
        case PP_PUNCT_PIPE:
            kind = TOK_BW_OR;
            break;
        case PP_PUNCT_PIPE_ASSIGN:
            kind = TOK_BW_OR_ASSIGN;
            break;
        case PP_PUNCT_OROR:
            kind = TOK_LOG_OR;
            break;
        case PP_PUNCT_QUESTION:
            kind = TOK_QUESTION;
            break;
        case PP_PUNCT_COLON:
            kind = TOK_COLON;
            break;
        case PP_PUNCT_SEMI:
            kind = TOK_SEMI;
            break;
        case PP_PUNCT_COMMA:
            kind = TOK_COMMA;
            break;
    }
    finalize_push_token(ctx, (Token) {.kind = kind, .loc = tok->loc});
}

/* Decodes one escape at *pp, advancing it; end bounds the spelling. Numeric
   escapes accumulate greedily and can return values > 0xFF. */
static int finalize_escape(const char **pp, const char *end, FinalizeCtx *ctx, Loc loc)
{
    int val = lex_escape_byte(pp, end);
    if (val >= 0)
    {
        return val;
    }
    finalize_error(ctx, loc, "hexadecimal escape sequence with no digits");
    return 0;
}

static void finalize_string(FinalizeCtx *ctx, const PpToken *tok)
{
    if (tok->spell[0] != '"')
    {
        finalize_error(ctx, tok->loc, "wide/UTF string not supported");
        return;
    }

    const char *p = tok->spell + 1;
    const char *end = tok->spell + tok->len - 1;

    ByteBuf buf;
    bytebuf_init(&buf, ctx->arena);
    while (p < end)
    {
        if (*p == '\\')
        {
            p++;
            bytebuf_append(&buf, (u8) finalize_escape(&p, end, ctx, tok->loc));
        }
        else
        {
            bytebuf_append(&buf, (u8) *p);
            p++;
        }
    }

    size_t content_len = bytebuf_len(&buf);
    bytebuf_append(&buf, '\0');
    finalize_push_token(ctx, (Token) {.kind = TOK_STRING_LIT,
                                      .loc = tok->loc,
                                      .payload.str = (const char *) bytebuf_data(&buf),
                                      .str_len = (u32) content_len});
}

static void finalize_char(FinalizeCtx *ctx, const PpToken *tok)
{
    if (tok->spell[0] != '\'')
    {
        finalize_error(ctx, tok->loc, "wide/UTF character constant not supported");
        return;
    }

    const char *p = tok->spell + 1;
    const char *end = tok->spell + tok->len - 1;
    if (p >= end)
    {
        finalize_error(ctx, tok->loc, "empty character constant");
        return;
    }

    int val;
    if (*p == '\\')
    {
        p++;
        val = finalize_escape(&p, end, ctx, tok->loc);
    }
    else
    {
        val = (u8) *p;
        p++;
    }

    if (p != end)
    {
        finalize_error(ctx, tok->loc, "multi-character character constant");
        return;
    }
    if (val > 0xFF)
    {
        finalize_error(ctx, tok->loc, "character constant exceeds bounds of type char");
        return;
    }
    finalize_push_token(ctx,
                        (Token) {.kind = TOK_CHAR_LIT, .loc = tok->loc, .payload.int_val = val});
}

static bool pp_number_is_float(const char *s, u32 len)
{
    bool is_hex = len >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
    for (u32 i = is_hex ? 2 : 0; i < len; i++)
    {
        if (s[i] == '.')
        {
            return true;
        }
        if (is_hex && (s[i] == 'p' || s[i] == 'P'))
        {
            return true;
        }
        if (!is_hex && (s[i] == 'e' || s[i] == 'E' || s[i] == 'f' || s[i] == 'F'))
        {
            return true;
        }
    }
    return false;
}

/* §6.4.4.2 floating constant; glibc converts, the whole spelling must parse. */
static void finalize_float(FinalizeCtx *ctx, const PpToken *tok)
{
    const char *s = tok->spell;
    u32 len = tok->len;

    FloatKind kind = FK_DOUBLE;
    u32 digits = len;
    if (len > 0 && (s[len - 1] == 'f' || s[len - 1] == 'F'))
    {
        kind = FK_FLOAT;
        digits = len - 1;
    }
    else if (len > 0 && (s[len - 1] == 'l' || s[len - 1] == 'L'))
    {
        kind = FK_LONG;
        digits = len - 1;
    }

    char *buf = arena_alloc(ctx->arena, digits + 1, 1);
    memcpy(buf, s, digits);
    buf[digits] = '\0';

    /* strtod accepts a hex-float without its mandatory p exponent; reject (C11 §6.4.4.2p6). */
    if (digits >= 2 && buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X') &&
        !(strchr(buf, 'p') || strchr(buf, 'P')))
    {
        finalize_error(ctx, tok->loc,
                       "hexadecimal floating constant requires a binary exponent (p)");
        return;
    }

    /* strtof/strtod/strtold give the exact host value; floats keep their bits. */
    char *endptr;
    u64 pat = 0;
    long double ld = 0.0L;
    if (kind == FK_FLOAT)
    {
        float f = strtof(buf, &endptr);
        memcpy(&pat, &f, sizeof f);
    }
    else if (kind == FK_LONG)
    {
        ld = strtold(buf, &endptr);
    }
    else
    {
        double d = strtod(buf, &endptr);
        memcpy(&pat, &d, sizeof d);
    }
    if (endptr != buf + digits)
    {
        finalize_error(ctx, tok->loc, "invalid floating literal");
        return;
    }

    Token token = {.kind = TOK_FLOAT_LIT, .loc = tok->loc, .float_kind = kind};
    if (kind == FK_LONG)
    {
        token.payload.ld_val = ld;
    }
    else
    {
        token.payload.float_pat = pat;
    }
    finalize_push_token(ctx, token);
}

static void finalize_number(FinalizeCtx *ctx, const PpToken *tok)
{
    const char *s = tok->spell;
    u32 len = tok->len;
    if (pp_number_is_float(s, len))
    {
        finalize_float(ctx, tok);
        return;
    }

    u64 base = 10;
    bool is_hex = false;
    bool is_octal = false;
    if (len >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
    {
        base = 16;
        is_hex = true;
    }
    else if (len >= 2 && s[0] == '0')
    {
        /* C11 §6.4.4.1: a leading 0 introduces an octal constant. */
        base = 8;
        is_octal = true;
    }

    u32 i = is_hex ? 2 : 0;
    u64 val = 0;
    bool overflow = false;
    while (i < len)
    {
        /* hex_digit_value returns >= base for any character outside the radix. */
        u64 digit = hex_digit_value(s[i]);
        if (digit >= base)
        {
            break;
        }
        if (!overflow && val > ((u64) UINT64_MAX - digit) / base)
        {
            overflow = true;
        }
        else if (!overflow)
        {
            val = val * base + digit;
        }
        i++;
    }

    bool is_unsigned = false;
    IntSuffix length = SUFFIX_NONE;
    if (i < len && (s[i] == 'u' || s[i] == 'U'))
    {
        is_unsigned = true;
        i++;
    }
    if (i < len && (s[i] == 'l' || s[i] == 'L'))
    {
        length = SUFFIX_L;
        i++;
        if (i < len && (s[i] == 'l' || s[i] == 'L'))
        {
            length = SUFFIX_LL;
            i++;
        }
    }
    if (!is_unsigned && i < len && (s[i] == 'u' || s[i] == 'U'))
    {
        is_unsigned = true;
        i++;
    }

    if (i < len)
    {
        finalize_error(ctx, tok->loc, "invalid suffix on integer literal");
        return;
    }

    /* C11 §6.4.4.1: hex/octal ("non-decimal") literals may take an unsigned
       type when the value exceeds the widest signed type; decimal literals are
       signed-only, and an explicit u/U suffix widens the range to UINT64_MAX. */
    bool is_non_decimal = is_hex || is_octal;
    if (overflow)
    {
        finalize_error(ctx, tok->loc, "integer literal overflow");
        return;
    }
    if (!is_unsigned && !is_non_decimal && val > (u64) INT64_MAX)
    {
        finalize_error(ctx, tok->loc, "integer literal overflow");
        return;
    }
    if (!is_unsigned && is_non_decimal && val > (u64) INT64_MAX)
    {
        is_unsigned = true;
    }

    finalize_push_token(ctx, (Token) {.kind = TOK_INT_LIT,
                                      .loc = tok->loc,
                                      .payload = {.int_val = (i64) val},
                                      .int_suffix = {.is_unsigned = is_unsigned,
                                                     .length = length,
                                                     .is_hex = is_non_decimal}});
}

static void finalize_pp_token(FinalizeCtx *ctx, const PpToken *tok)
{
    switch (tok->kind)
    {
        case TOK_PP_EOF:
        case TOK_PP_TRIVIA_WS:
        case TOK_PP_TRIVIA_NL:
        case TOK_PP_TRIVIA_COMMENT:
            break;
        case TOK_PP_IDENT:
            finalize_ident(ctx, tok);
            break;
        case TOK_PP_NUMBER:
            finalize_number(ctx, tok);
            break;
        case TOK_PP_PUNCT:
            finalize_punct(ctx, tok);
            break;
        case TOK_PP_STRING:
            finalize_string(ctx, tok);
            break;
        case TOK_PP_CHAR:
            finalize_char(ctx, tok);
            break;
        case TOK_PP_OTHER:
            finalize_error(ctx, tok->loc, "unknown character");
            break;
        case TOK_PP_HEADER_NAME:
        case TOK_PP_PARAM:
            finalize_error(ctx, tok->loc, "unexpected preprocessing token");
            break;
    }
}

LexResult lex_finalize(const Vec *soup, Arena *arena)
{
    FinalizeCtx ctx;
    ctx.arena = arena;
    ctx.tokens = vec_new(arena);
    ctx.files = intern_pool_new(arena);
    ctx.error_count = 0;

    for (size_t i = 0; i < vec_size(soup); i++)
    {
        finalize_pp_token(&ctx, vec_get(soup, i));
    }

    Loc eof_loc = {.file = NULL, .line = 1, .col = 1};
    if (vec_size(soup) > 0)
    {
        const PpToken *first = vec_get(soup, 0);
        const PpToken *last = vec_get(soup, vec_size(soup) - 1);
        eof_loc = (Loc) {.file = first->loc.file, .line = last->loc.line, .col = last->loc.col};
    }
    finalize_push_token(&ctx, (Token) {.kind = TOK_EOF, .loc = eof_loc});

    size_t count = vec_size(ctx.tokens);
    Token *arr = arena_alloc(arena, count * sizeof(Token), _Alignof(Token));
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
