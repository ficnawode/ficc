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

static bool is_whitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static TokenKind keyword_kind(const char *s)
{
    if (strcmp(s, "int") == 0)
    {
        return TOK_KW_INT;
    }
    if (strcmp(s, "void") == 0)
    {
        return TOK_KW_VOID;
    }
    if (strcmp(s, "return") == 0)
    {
        return TOK_KW_RETURN;
    }
    if (strcmp(s, "if") == 0)
    {
        return TOK_KW_IF;
    }
    if (strcmp(s, "else") == 0)
    {
        return TOK_KW_ELSE;
    }
    if (strcmp(s, "while") == 0)
    {
        return TOK_KW_WHILE;
    }
    if (strcmp(s, "for") == 0)
    {
        return TOK_KW_FOR;
    }
    if (strcmp(s, "do") == 0)
    {
        return TOK_KW_DO;
    }
    if (strcmp(s, "break") == 0)
    {
        return TOK_KW_BREAK;
    }
    if (strcmp(s, "continue") == 0)
    {
        return TOK_KW_CONTINUE;
    }
    if (strcmp(s, "goto") == 0)
    {
        return TOK_KW_GOTO;
    }
    if (strcmp(s, "char") == 0)
    {
        return TOK_KW_CHAR;
    }
    if (strcmp(s, "short") == 0)
    {
        return TOK_KW_SHORT;
    }
    if (strcmp(s, "long") == 0)
    {
        return TOK_KW_LONG;
    }
    if (strcmp(s, "unsigned") == 0)
    {
        return TOK_KW_UNSIGNED;
    }
    if (strcmp(s, "sizeof") == 0)
    {
        return TOK_KW_SIZEOF;
    }
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

static void lexer_push(LexerCtx *ctx, Token token)
{
    Token *slot = arena_alloc(ctx->arena, sizeof(Token), sizeof(void *));
    *slot = token;
    vec_push(ctx->tokens, slot);
}

static bool lex_whitespace(LexerCtx *ctx)
{
    if (!is_whitespace(*ctx->p))
    {
        return false;
    }

    while (is_whitespace(*ctx->p))
    {
        lexer_advance(ctx);
    }
    return true;
}

static bool lex_number(LexerCtx *ctx)
{
    if (!is_digit(*ctx->p))
    {
        return false;
    }

    Loc loc = lexer_loc(ctx);
    u64 val = 0;
    bool overflow = false;
    bool is_hex = false;

    if (ctx->p[0] == '0' && (ctx->p[1] == 'x' || ctx->p[1] == 'X'))
    {
        is_hex = true;
        lexer_advance(ctx); /* 0 */
        lexer_advance(ctx); /* x/X */
        while (is_digit(*ctx->p) || (*ctx->p >= 'a' && *ctx->p <= 'f') ||
               (*ctx->p >= 'A' && *ctx->p <= 'F'))
        {
            u64 digit;
            if (is_digit(*ctx->p))
            {
                digit = (u64) (*ctx->p - '0');
            }
            else if (*ctx->p >= 'a')
            {
                digit = (u64) (*ctx->p - 'a' + 10);
            }
            else
            {
                digit = (u64) (*ctx->p - 'A' + 10);
            }
            if (!overflow && val > ((u64) INT64_MAX - digit) / 16)
            {
                lexer_error(ctx, "integer literal overflow");
                val = (u64) INT64_MAX;
                overflow = true;
            }
            else if (!overflow)
            {
                val = val * 16 + digit;
            }
            lexer_advance(ctx);
        }
    }
    else
    {
        while (is_digit(*ctx->p))
        {
            u64 digit = (u64) (*ctx->p - '0');
            if (!overflow && val > ((u64) INT64_MAX - digit) / 10)
            {
                lexer_error(ctx, "integer literal overflow");
                val = (u64) INT64_MAX;
                overflow = true;
            }
            else if (!overflow)
            {
                val = val * 10 + digit;
            }
            lexer_advance(ctx);
        }
    }

    /* Parse integer suffixes: u/U, l/L, ll/LL */
    bool is_unsigned = false;
    IntSuffix length = SUFFIX_NONE;

    if (*ctx->p == 'u' || *ctx->p == 'U')
    {
        is_unsigned = true;
        lexer_advance(ctx);
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
    }
    else if (*ctx->p == 'l' || *ctx->p == 'L')
    {
        length = SUFFIX_L;
        lexer_advance(ctx);
        if (*ctx->p == 'l' || *ctx->p == 'L')
        {
            length = SUFFIX_LL;
            lexer_advance(ctx);
        }
        if (*ctx->p == 'u' || *ctx->p == 'U')
        {
            is_unsigned = true;
            lexer_advance(ctx);
        }
    }

    if (is_identifier_start(*ctx->p))
    {
        lexer_error(ctx, "invalid suffix on integer literal");
        while (is_identifier_char(*ctx->p))
        {
            lexer_advance(ctx);
        }
    }

    Token tok = {.kind = TOK_INT_LIT, .loc = loc, .payload.int_val = (i64) val};
    tok.int_suffix.is_unsigned = is_unsigned;
    tok.int_suffix.length = length;
    tok.int_suffix.is_hex = is_hex;
    lexer_push(ctx, tok);
    return true;
}

static bool lex_ident(LexerCtx *ctx)
{
    if (!is_identifier_start(*ctx->p))
    {
        return false;
    }

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
    return true;
}

static bool lex_comment(LexerCtx *ctx)
{
    if (ctx->p[0] != '/' || ctx->p[1] != '*')
    {
        return false;
    }

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

static bool lex_string(LexerCtx *ctx)
{
    if (*ctx->p != '"')
    {
        return false;
    }

    Loc loc = lexer_loc(ctx);
    lexer_advance(ctx); /* consume opening " */

    /* Accumulate characters into a scratch buffer (arena-backed). */
    size_t cap = 64;
    char *buf = arena_alloc(ctx->arena, cap, 1);
    size_t len = 0;

    while (*ctx->p && *ctx->p != '"')
    {
        if (*ctx->p == '\\')
        {
            lexer_advance(ctx); /* consume backslash */
            char esc = *ctx->p;
            char ch;
            switch (esc)
            {
                case 'n':
                    ch = '\n';
                    break;
                case 't':
                    ch = '\t';
                    break;
                case '\\':
                    ch = '\\';
                    break;
                case '"':
                    ch = '"';
                    break;
                case '0':
                    ch = '\0';
                    break;
                default:
                    ch = esc; /* unknown escape: keep the character */
                    break;
            }
            if (len + 1 >= cap)
            {
                cap *= 2;
                char *new_buf = arena_alloc(ctx->arena, cap, 1);
                memcpy(new_buf, buf, len);
                buf = new_buf;
            }
            buf[len++] = ch;
            lexer_advance(ctx);
        }
        else if (*ctx->p == '\n')
        {
            lexer_error(ctx, "unterminated string literal");
            return true;
        }
        else
        {
            if (len + 1 >= cap)
            {
                cap *= 2;
                char *new_buf = arena_alloc(ctx->arena, cap, 1);
                memcpy(new_buf, buf, len);
                buf = new_buf;
            }
            buf[len] = *ctx->p;
            len++;
            lexer_advance(ctx);
        }
    }

    if (*ctx->p != '"')
    {
        lexer_error(ctx, "unterminated string literal");
        return true;
    }
    lexer_advance(ctx); /* consume closing " */

    /* NUL-terminate the string data */
    if (len >= cap)
    {
        cap = len + 1;
        char *new_buf = arena_alloc(ctx->arena, cap, 1);
        memcpy(new_buf, buf, len);
        buf = new_buf;
    }
    buf[len] = '\0';

    Token tok = {.kind = TOK_STRING_LIT, .loc = loc, .payload.str = buf, .str_len = (u32) len};
    lexer_push(ctx, tok);
    return true;
}

static bool lex_punct(LexerCtx *ctx)
{
    Loc loc = lexer_loc(ctx);
    char c = ctx->p[0];
    char n = ctx->p[1];
    TokenKind kind;
    u32 advance = 1;

    switch (c)
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
        case '[':
            kind = TOK_LBRACKET;
            break;
        case ']':
            kind = TOK_RBRACKET;
            break;
        case ';':
            kind = TOK_SEMI;
            break;
        case ':':
            kind = TOK_COLON;
            break;
        case ',':
            kind = TOK_COMMA;
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
            if (n == '=')
            {
                kind = TOK_EQ;
                advance = 2;
            }
            else
            {
                kind = TOK_ASSIGN;
            }
            break;
        case '!':
            if (n == '=')
            {
                kind = TOK_NE;
                advance = 2;
            }
            else
            {
                kind = TOK_NOT;
            }
            break;
        case '?':
            kind = TOK_QUESTION;
            break;
        case '<':
            if (n == '=')
            {
                kind = TOK_LE;
                advance = 2;
            }
            else if (n == '<')
            {
                kind = TOK_SHL;
                advance = 2;
            }
            else
            {
                kind = TOK_LT;
            }
            break;
        case '>':
            if (n == '=')
            {
                kind = TOK_GE;
                advance = 2;
            }
            else if (n == '>')
            {
                kind = TOK_SHR;
                advance = 2;
            }
            else
            {
                kind = TOK_GT;
            }
            break;
        case '&':
            if (n == '&')
            {
                kind = TOK_LOG_AND;
                advance = 2;
            }
            else
            {
                kind = TOK_BW_AND;
            }
            break;
        case '|':
            if (n == '|')
            {
                kind = TOK_LOG_OR;
                advance = 2;
            }
            else
            {
                kind = TOK_BW_OR;
            }
            break;
        case '^':
            kind = TOK_BW_XOR;
            break;
        case '~':
            kind = TOK_TILDE;
            break;
        default:
            return false;
    }

    for (u32 i = 0; i < advance; i++)
    {
        lexer_advance(ctx);
    }
    lexer_push(ctx, (Token) {.kind = kind, .loc = loc});
    return true;
}

LexResult lex(const char *file, const char *src, Arena *arena)
{
    LexerCtx ctx;
    lexer_init(&ctx, file, src, arena);

    while (*ctx.p)
    {
        if (lex_whitespace(&ctx))
        {
            continue;
        }
        if (lex_comment(&ctx))
        {
            continue;
        }
        if (lex_string(&ctx))
        {
            continue;
        }
        if (lex_number(&ctx))
        {
            continue;
        }
        if (lex_ident(&ctx))
        {
            continue;
        }
        if (lex_punct(&ctx))
        {
            continue;
        }

        lexer_error(&ctx, "unknown character");
        lexer_advance(&ctx);
    }

    /* EOF token */
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
