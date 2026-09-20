#include "pp_lex.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const char *pp_kind_name(PpKind kind)
{
    switch (kind)
    {
#define CASE(K)                                                                                    \
    case K:                                                                                        \
        return #K;
        PP_KINDS(CASE)
#undef CASE
    }
    return "TOK_PP_UNKNOWN";
}

static char pp_trigraph(char c)
{
    switch (c)
    {
        case '=':
            return '#';
        case '/':
            return '\\';
        case '\'':
            return '^';
        case '(':
            return '[';
        case ')':
            return ']';
        case '!':
            return '|';
        case '<':
            return '{';
        case '>':
            return '}';
        case '-':
            return '~';
        default:
            return '\0';
    }
}

/* Width of a trigraph starting at src[i]: 0, or 3 with its replacement in
 *out. */
static u32 pp_trigraph_width(const char *src, size_t i, size_t n, char *out)
{
    if (src[i] != '?' || i + 2 >= n || src[i + 1] != '?')
    {
        return 0;
    }
    char replaced = pp_trigraph(src[i + 2]);
    if (replaced == '\0')
    {
        return 0;
    }
    *out = replaced;
    return 3;
}

static void pp_prepare_error(const char *file, Loc loc, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "%s:%u:%u: [pp] error: ", file, loc.line, loc.col);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

char *pp_prepare(const char *file, const char *src, Arena *arena)
{
    size_t n = strlen(src);
    char *out = arena_alloc(arena, n + 1, 1);
    size_t i = 0;
    size_t o = 0;
    u32 line = 1;
    u32 col = 1;

    while (i < n)
    {
        Loc loc = {.file = file, .line = line, .col = col};
        char c = src[i];
        u32 width = pp_trigraph_width(src, i, n, &c);
        if (width == 0)
        {
            width = 1;
        }

        if (c == '\\' && i + width >= n)
        {
            pp_prepare_error(file, loc, "backslash at end of file");
            return NULL;
        }
        if (c == '\\' && src[i + width] == '\n')
        {
            i += width + 1;
            line++;
            col = 1;
            continue;
        }

        out[o++] = c;
        if (c == '\n')
        {
            line++;
            col = 1;
        }
        else
        {
            col += width;
        }
        i += width;
    }
    out[o] = '\0';
    return out;
}

typedef struct SoupScannerCtx SoupScannerCtx;

struct SoupScannerCtx
{
    const char *file;
    Arena *arena;
    Vec *tokens;
    const char *p;
    u32 line;
    u32 col;
    u32 error_count;
};

static bool is_hspace(char c)
{
    return c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\r';
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool is_ident_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_ident_char(char c)
{
    return is_ident_start(c) || is_digit(c);
}

/* True when p begins a (possibly prefixed) string or char literal; prefix_len
   excludes the opening quote, quote receives it. */
static bool pp_literal_start(const char *p, u32 *prefix_len, char *quote)
{
    if (*p == '"' || *p == '\'')
    {
        *prefix_len = 0;
        *quote = *p;
        return true;
    }
    if (p[0] == 'u' && p[1] == '8' && (p[2] == '"' || p[2] == '\''))
    {
        *prefix_len = 2;
        *quote = p[2];
        return true;
    }
    if ((p[0] == 'L' || p[0] == 'u' || p[0] == 'U') && (p[1] == '"' || p[1] == '\''))
    {
        *prefix_len = 1;
        *quote = p[1];
        return true;
    }
    return false;
}

static bool pp_number_start(char c, char next)
{
    return is_digit(c) || (c == '.' && is_digit(next));
}

typedef struct PpPunctSpelling PpPunctSpelling;
struct PpPunctSpelling
{
    const char *spelling;
    PpPunct punct;
};

/* Every input spelling: the primary puncts from PP_PUNCTS plus the digraphs
   (C11 §6.4.6.3), which map to their primary's id. Order is irrelevant —
   pp_punct_match takes the longest match. */
static const PpPunctSpelling PP_PUNCT_TABLE[] = {
#define PP_TABLE_ENTRY(KIND, SPELLING) {SPELLING, KIND},
    PP_PUNCTS(PP_TABLE_ENTRY)
#undef PP_TABLE_ENTRY
        {"%:%:", PP_PUNCT_HASHHASH},
    {"<:", PP_PUNCT_LBRACKET},
    {":>", PP_PUNCT_RBRACKET},
    {"<%", PP_PUNCT_LBRACE},
    {"%>", PP_PUNCT_RBRACE},
    {"%:", PP_PUNCT_HASH},
};

static u32 pp_punct_match(const char *p, PpPunct *out)
{
    u32 best_len = 0;
    for (size_t i = 0; i < sizeof(PP_PUNCT_TABLE) / sizeof(PP_PUNCT_TABLE[0]); i++)
    {
        size_t len = strlen(PP_PUNCT_TABLE[i].spelling);
        if (len > best_len && strncmp(p, PP_PUNCT_TABLE[i].spelling, len) == 0)
        {
            *out = PP_PUNCT_TABLE[i].punct;
            best_len = (u32) len;
        }
    }
    return best_len;
}

/* Joining a then b reads as one punctuator when the concatenation itself is
   one, or when max-munch would chew part of `a` across the seam (`a` a strict
   prefix of a punctuator whose remainder `b` starts with, e.g. `<` `<<`). This
   is the -E token-separation test. */
bool pp_concat_is_punct(const char *a, size_t alen, const char *b, size_t blen)
{
    if (alen + blen > 4)
    {
        return false;
    }
    char buf[5];
    memcpy(buf, a, alen);
    memcpy(buf + alen, b, blen);
    buf[alen + blen] = '\0';
    for (size_t i = 0; i < sizeof(PP_PUNCT_TABLE) / sizeof(PP_PUNCT_TABLE[0]); i++)
    {
        const char *spelling = PP_PUNCT_TABLE[i].spelling;
        if (strcmp(spelling, buf) == 0)
        {
            return true;
        }
        size_t plen = strlen(spelling);
        if (plen > alen && strncmp(spelling, a, alen) == 0)
        {
            size_t rem = plen - alen;
            if (blen >= rem && strncmp(b, spelling + alen, rem) == 0)
            {
                return true;
            }
        }
    }
    return false;
}

static void scanner_init(SoupScannerCtx *ctx, const char *file, const char *src, Arena *arena)
{
    ctx->file = file;
    ctx->arena = arena;
    ctx->tokens = vec_new(arena);
    ctx->p = src;
    ctx->line = 1;
    ctx->col = 1;
    ctx->error_count = 0;
}

static void scanner_error(SoupScannerCtx *ctx, const char *msg)
{
    fprintf(stderr, "%s:%u:%u: [pp_lex] error: %s\n", ctx->file, ctx->line, ctx->col, msg);
    ctx->error_count++;
}

static Loc scanner_loc(const SoupScannerCtx *ctx)
{
    return (Loc) {.file = ctx->file, .line = ctx->line, .col = ctx->col};
}

static void scanner_advance(SoupScannerCtx *ctx)
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

static void scanner_advance_n(SoupScannerCtx *ctx, u32 n)
{
    for (u32 i = 0; i < n; i++)
    {
        scanner_advance(ctx);
    }
}

static PpToken pp_token(PpKind kind, const char *spell, u32 len, Loc loc, bool has_newline)
{
    return (PpToken) {
        .kind = kind, .loc = loc, .spell = spell, .len = len, .has_newline = has_newline};
}

static void scanner_push(SoupScannerCtx *ctx, PpToken token)
{
    PpToken *slot = arena_alloc(ctx->arena, sizeof(PpToken), sizeof(void *));
    *slot = token;
    vec_push(ctx->tokens, slot);
}

/* Records a token covering [start, ctx->p). */
static void scanner_emit(SoupScannerCtx *ctx, PpKind kind, const char *start, Loc loc,
                         bool has_newline)
{
    scanner_push(ctx, pp_token(kind, start, (u32) (ctx->p - start), loc, has_newline));
}

static bool scan_ws(SoupScannerCtx *ctx)
{
    if (!is_hspace(*ctx->p))
    {
        return false;
    }
    Loc loc = scanner_loc(ctx);
    const char *start = ctx->p;
    while (is_hspace(*ctx->p))
    {
        scanner_advance(ctx);
    }
    scanner_emit(ctx, TOK_PP_TRIVIA_WS, start, loc, false);
    return true;
}

static bool scan_nl(SoupScannerCtx *ctx)
{
    if (*ctx->p != '\n')
    {
        return false;
    }
    Loc loc = scanner_loc(ctx);
    const char *start = ctx->p;
    scanner_advance(ctx);
    scanner_emit(ctx, TOK_PP_TRIVIA_NL, start, loc, true);
    return true;
}

static void scan_block_comment(SoupScannerCtx *ctx)
{
    Loc loc = scanner_loc(ctx);
    const char *start = ctx->p;
    bool has_newline = false;
    scanner_advance_n(ctx, 2);
    while (*ctx->p && !(ctx->p[0] == '*' && ctx->p[1] == '/'))
    {
        has_newline |= *ctx->p == '\n';
        scanner_advance(ctx);
    }
    if (*ctx->p)
    {
        scanner_advance_n(ctx, 2);
    }
    else
    {
        scanner_error(ctx, "unterminated comment");
    }
    scanner_emit(ctx, TOK_PP_TRIVIA_COMMENT, start, loc, has_newline);
}

static void scan_line_comment(SoupScannerCtx *ctx)
{
    Loc loc = scanner_loc(ctx);
    const char *start = ctx->p;
    scanner_advance_n(ctx, 2);
    while (*ctx->p && *ctx->p != '\n')
    {
        scanner_advance(ctx);
    }
    scanner_emit(ctx, TOK_PP_TRIVIA_COMMENT, start, loc, false);
}

static bool scan_comment(SoupScannerCtx *ctx)
{
    if (ctx->p[0] != '/' || (ctx->p[1] != '*' && ctx->p[1] != '/'))
    {
        return false;
    }
    if (ctx->p[1] == '*')
    {
        scan_block_comment(ctx);
    }
    else
    {
        scan_line_comment(ctx);
    }
    return true;
}

static bool scan_literal(SoupScannerCtx *ctx)
{
    u32 prefix_len;
    char quote;
    if (!pp_literal_start(ctx->p, &prefix_len, &quote))
    {
        return false;
    }

    Loc loc = scanner_loc(ctx);
    const char *start = ctx->p;
    scanner_advance_n(ctx, prefix_len + 1);
    while (*ctx->p && *ctx->p != quote && *ctx->p != '\n')
    {
        if (*ctx->p == '\\' && ctx->p[1] != '\0')
        {
            scanner_advance_n(ctx, 2);
            continue;
        }
        scanner_advance(ctx);
    }

    PpKind kind = quote == '"' ? TOK_PP_STRING : TOK_PP_CHAR;
    if (*ctx->p == quote)
    {
        scanner_advance(ctx);
    }
    else
    {
        scanner_error(ctx, kind == TOK_PP_STRING ? "unterminated string literal"
                                                 : "unterminated character constant");
    }
    scanner_emit(ctx, kind, start, loc, false);
    return true;
}

static bool scan_number(SoupScannerCtx *ctx)
{
    if (!pp_number_start(ctx->p[0], ctx->p[1]))
    {
        return false;
    }
    Loc loc = scanner_loc(ctx);
    const char *start = ctx->p;
    scanner_advance(ctx);
    while (*ctx->p)
    {
        char c = *ctx->p;
        if ((c == 'e' || c == 'E' || c == 'p' || c == 'P') &&
            (ctx->p[1] == '+' || ctx->p[1] == '-'))
        {
            scanner_advance_n(ctx, 2);
        }
        else if (is_ident_char(c) || c == '.')
        {
            scanner_advance(ctx);
        }
        else
        {
            break;
        }
    }
    scanner_emit(ctx, TOK_PP_NUMBER, start, loc, false);
    return true;
}

static bool scan_ident(SoupScannerCtx *ctx)
{
    if (!is_ident_start(*ctx->p))
    {
        return false;
    }
    Loc loc = scanner_loc(ctx);
    const char *start = ctx->p;
    while (is_ident_char(*ctx->p))
    {
        scanner_advance(ctx);
    }
    scanner_emit(ctx, TOK_PP_IDENT, start, loc, false);
    return true;
}

static bool scan_punct(SoupScannerCtx *ctx)
{
    PpPunct punct;
    u32 len = pp_punct_match(ctx->p, &punct);
    if (len == 0)
    {
        return false;
    }
    Loc loc = scanner_loc(ctx);
    const char *start = ctx->p;
    scanner_advance_n(ctx, len);
    PpToken token = pp_token(TOK_PP_PUNCT, start, len, loc, false);
    token.punct = punct;
    scanner_push(ctx, token);
    return true;
}

static void scan_other(SoupScannerCtx *ctx)
{
    Loc loc = scanner_loc(ctx);
    const char *start = ctx->p;
    scanner_advance(ctx);
    scanner_emit(ctx, TOK_PP_OTHER, start, loc, false);
}

Vec *pp_lex(const char *file, const char *src, Arena *arena)
{
    char *normalized = pp_prepare(file, src, arena);
    if (!normalized)
    {
        return NULL;
    }

    SoupScannerCtx ctx;
    scanner_init(&ctx, file, normalized, arena);

    while (*ctx.p)
    {
        /* First recognizer to match wins; literal-before-ident keeps a
           prefixed literal one token, comment-before-punct so a block comment
           isn't split into `/` and `*`. Whatever remains is a TOK_PP_OTHER. */
        if (!(scan_ws(&ctx) || scan_nl(&ctx) || scan_comment(&ctx) || scan_literal(&ctx) ||
              scan_number(&ctx) || scan_ident(&ctx) || scan_punct(&ctx)))
        {
            scan_other(&ctx);
        }
    }

    scanner_push(&ctx, pp_token(TOK_PP_EOF, ctx.p, 0, scanner_loc(&ctx), false));

    if (ctx.error_count > 0)
    {
        return NULL;
    }
    return ctx.tokens;
}