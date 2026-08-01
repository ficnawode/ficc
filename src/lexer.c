#include "lexer.h"
#include "util/vec.h"
#include <stdio.h>
#include <string.h>

typedef struct LexerCtx LexerCtx;
struct LexerCtx
{
    const char *file;
    const char *src;
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

static bool isident_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool isident_char(char c)
{
    return isident_start(c) || (c >= '0' && c <= '9');
}

static TokenKind keyword_kind(const char *s)
{
    if (strcmp(s, "int") == 0)
        return TOK_KW_INT;
    if (strcmp(s, "void") == 0)
        return TOK_KW_VOID;
    if (strcmp(s, "return") == 0)
        return TOK_KW_RETURN;
    return TOK_IDENT;
}

static void lexer_init(LexerCtx *ctx, const char *file, const char *src, Arena *arena)
{
    ctx->file = file;
    ctx->src = src;
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
    Loc loc = {ctx->file, ctx->line, ctx->col};
    return loc;
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

static void lexer_push(LexerCtx *ctx, Token t)
{
    Token *p = arena_alloc(ctx->arena, sizeof(Token), sizeof(void *));
    *p = t;
    vec_push(ctx->tokens, p);
}

static bool lex_whitespace(LexerCtx *ctx)
{
    if (*ctx->p != ' ' && *ctx->p != '\t' && *ctx->p != '\r' && *ctx->p != '\n')
        return false;

    while (*ctx->p == ' ' || *ctx->p == '\t' || *ctx->p == '\r' || *ctx->p == '\n')
        lexer_advance(ctx);
    return true;
}

static bool lex_number(LexerCtx *ctx)
{
    if (*ctx->p < '0' || *ctx->p > '9')
        return false;

    Loc loc = lexer_loc(ctx);
    u64 val = 0;

    while (*ctx->p >= '0' && *ctx->p <= '9')
    {
        u64 digit = (u64) (*ctx->p - '0');

        /* Overflow detection */
        if (val > ((u64) INT64_MAX - digit) / 10)
        {
            lexer_error(ctx, "integer literal overflow");
            /* Consume the rest of the digits so we don't re-error,
               but leave val at max so downstream has a defined value. */
            val = (u64) INT64_MAX;
            while (*ctx->p >= '0' && *ctx->p <= '9')
                lexer_advance(ctx);
            break;
        }
        val = val * 10 + digit;
        lexer_advance(ctx);
    }

    lexer_push(ctx, (Token) {.kind = TOK_INT_LIT, .loc = loc, .payload.int_val = (i64) val});
    return true;
}

static bool lex_ident(LexerCtx *ctx)
{
    if (!isident_start(*ctx->p))
        return false;

    Loc loc = lexer_loc(ctx);
    const char *start = ctx->p;

    while (isident_char(*ctx->p))
        lexer_advance(ctx);

    size_t len = (size_t) (ctx->p - start);
    char *buf = arena_alloc(ctx->arena, len + 1, 1);
    for (size_t i = 0; i < len; i++)
        buf[i] = start[i];
    buf[len] = '\0';

    lexer_push(ctx, (Token) {.kind = keyword_kind(buf), .loc = loc, .payload.str = buf});
    return true;
}

static bool lex_comment(LexerCtx *ctx)
{
    if (ctx->p[0] != '/' || ctx->p[1] != '*')
        return false;

    lexer_advance(ctx); /* consume '/' */
    lexer_advance(ctx); /* consume '*' */

    while (*ctx->p)
    {
        if (ctx->p[0] == '*' && ctx->p[1] == '/')
        {
            lexer_advance(ctx); /* consume '*' */
            lexer_advance(ctx); /* consume '/' */
            return true;
        }
        lexer_advance(ctx);
    }

    lexer_error(ctx, "unterminated comment");
    return true; /* consumed opening, so don't re-error as unknown char */
}

static bool lex_punct(LexerCtx *ctx)
{
    TokenKind kind;
    switch (*ctx->p)
    {
        case '(':
            kind = TOK_LPAREN;
            break;
        case ')':
            kind = TOK_RPAREN;
            break;
        case '{':
            kind = TOK_LBRACE;
            break;
        case '}':
            kind = TOK_RBRACE;
            break;
        case ';':
            kind = TOK_SEMI;
            break;
        case '+':
            kind = TOK_PLUS;
            break;
        case '-':
            kind = TOK_MINUS;
            break;
        case '*':
            kind = TOK_STAR;
            break;
        case '/':
            kind = TOK_SLASH;
            break;
        case '%':
            kind = TOK_PERCENT;
            break;
        case '=':
            kind = TOK_ASSIGN;
            break;
        case ',':
            kind = TOK_COMMA;
            break;
        default:
            return false;
    }

    Loc loc = lexer_loc(ctx);
    lexer_advance(ctx);
    lexer_push(ctx, (Token) {.kind = kind, .loc = loc});
    return true;
}

Token *lex(const char *file, const char *src, Arena *arena, u64 *out_count)
{
    LexerCtx ctx;
    lexer_init(&ctx, file, src, arena);

    while (*ctx.p)
    {
        if (lex_whitespace(&ctx))
            continue;
        if (lex_comment(&ctx))
            continue;
        if (lex_number(&ctx))
            continue;
        if (lex_ident(&ctx))
            continue;
        if (lex_punct(&ctx))
            continue;

        lexer_error(&ctx, "unknown character");
        lexer_advance(&ctx);
    }

    /* EOF token */
    lexer_push(&ctx, (Token) {.kind = TOK_EOF, .loc = lexer_loc(&ctx)});

    size_t count = vec_size(ctx.tokens);
    Token *arr = arena_alloc(arena, count * sizeof(Token), sizeof(void *));
    for (size_t i = 0; i < count; i++)
    {
        Token *t = (Token *) vec_get(ctx.tokens, i);
        arr[i] = *t;
    }

    *out_count = (u64) count;
    if (ctx.error_count > 0)
        return NULL;
    return arr;
}
