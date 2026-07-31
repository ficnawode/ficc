#include "parser.h"
#include <stdarg.h>
#include <stdio.h>

typedef struct ParserCtx ParserCtx;
struct ParserCtx
{
    Token *tokens;
    u64 count;
    u64 pos;
    Arena *arena;
};

static Token *parser_peek(ParserCtx *p)
{
    if (p->pos < p->count)
        return &p->tokens[p->pos];
    return &p->tokens[p->count - 1]; /* EOF */
}

static Token *parser_advance(ParserCtx *p)
{
    Token *t = parser_peek(p);
    if (p->pos < p->count - 1)
        p->pos++;
    return t;
}

static void parser_error(ParserCtx *p, const char *fmt, ...)
{
    Token *t = parser_peek(p);
    fprintf(stderr, "%s:%u:%u: [parse] error: ", t->loc.file, t->loc.line, t->loc.col);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

static bool parser_expect(ParserCtx *p, TokenKind kind, const char *what)
{
    Token *t = parser_peek(p);
    if (t->kind != kind)
    {
        parser_error(p, "expected %s, got %s", what, token_kind_name(t->kind));
        return false;
    }
    parser_advance(p);
    return true;
}

static ASTNode *parse_expr(ParserCtx *p);
static ASTNode *parse_stmt(ParserCtx *p);

static Type *parse_type_specifier(ParserCtx *p)
{
    Token *t = parser_peek(p);
    if (t->kind == TOK_KW_INT)
    {
        parser_advance(p);
        return type_int();
    }
    if (t->kind == TOK_KW_VOID)
    {
        parser_advance(p);
        return type_void();
    }
    parser_error(p, "expected type specifier");
    return NULL;
}

static Vec *parse_param_list(ParserCtx *p)
{
    Vec *params = vec_new(p->arena);
    Token *t = parser_peek(p);
    if (t->kind == TOK_KW_VOID)
    {
        parser_advance(p);
        return params;
    }
    /* empty param list is also valid for `()` */
    if (t->kind == TOK_RPAREN)
        return params;

    parser_error(p, "expected ')' or 'void' in parameter list");
    return NULL;
}

static ASTNode *parse_compound_stmt(ParserCtx *p)
{
    Token *start = parser_peek(p);
    if (!parser_expect(p, TOK_LBRACE, "'{'"))
        return NULL;

    Vec *stmts = vec_new(p->arena);
    while (parser_peek(p)->kind != TOK_RBRACE)
    {
        ASTNode *stmt = parse_stmt(p);
        if (!stmt)
            return NULL;
        vec_push(stmts, stmt);
    }
    if (!parser_expect(p, TOK_RBRACE, "'}'"))
        return NULL;

    return ast_compound_stmt(p->arena, stmts, start->loc);
}

static ASTNode *parse_return_stmt(ParserCtx *p)
{
    Token *start = parser_peek(p);
    ASSERT(start->kind == TOK_KW_RETURN);
    parser_advance(p); /* consume 'return' */

    ASTNode *expr = NULL;
    if (parser_peek(p)->kind != TOK_SEMI)
    {
        expr = parse_expr(p);
        if (!expr)
            return NULL;
    }
    if (!parser_expect(p, TOK_SEMI, "';'"))
        return NULL;

    return ast_return_stmt(p->arena, expr, start->loc);
}

static ASTNode *parse_stmt(ParserCtx *p)
{
    Token *t = parser_peek(p);
    switch (t->kind)
    {
        case TOK_KW_RETURN:
            return parse_return_stmt(p);
        default:
            parser_error(p, "unexpected token %s", token_kind_name(t->kind));
            return NULL;
    }
}

static ASTNode *parse_expr(ParserCtx *p)
{
    Token *t = parser_peek(p);
    if (t->kind == TOK_INT_LIT)
    {
        parser_advance(p);
        return ast_int_literal(p->arena, t->payload.int_val, t->loc);
    }
    parser_error(p, "expected expression");
    return NULL;
}

static ASTNode *parse_func_def(ParserCtx *p)
{
    Token *start = parser_peek(p);
    Type *ret_type = parse_type_specifier(p);
    if (!ret_type)
        return NULL;

    Token *name_tok = parser_peek(p);
    if (name_tok->kind != TOK_IDENT)
    {
        parser_error(p, "expected function name");
        return NULL;
    }
    parser_advance(p);

    if (!parser_expect(p, TOK_LPAREN, "'('"))
        return NULL;

    Vec *params = parse_param_list(p);
    if (!params)
        return NULL;

    if (!parser_expect(p, TOK_RPAREN, "')'"))
        return NULL;

    ASTNode *body = parse_compound_stmt(p);
    if (!body)
        return NULL;

    return ast_func_def(p->arena, ret_type, name_tok->payload.str, params, body, start->loc);
}

ASTNode *parse(Token *tokens, u64 count, Arena *arena)
{
    ASSERT(count > 0);
    ParserCtx p = {tokens, count, 0, arena};
    ASTNode *node = parse_func_def(&p);
    if (!node)
        return NULL;

    Token *eof = parser_peek(&p);
    if (eof->kind != TOK_EOF)
    {
        parser_error(&p, "expected EOF, got %s", token_kind_name(eof->kind));
        return NULL;
    }
    return node;
}
