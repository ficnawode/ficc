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

static ASTNode *parse_param(ParserCtx *p)
{
    Token *start = parser_peek(p);
    Type *type = parse_type_specifier(p);
    if (!type)
        return NULL;

    Token *name_tok = parser_peek(p);
    if (name_tok->kind != TOK_IDENT)
    {
        parser_error(p, "expected parameter name");
        return NULL;
    }
    parser_advance(p);

    return ast_var_decl(p->arena, type, name_tok->payload.str, NULL, start->loc);
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

    ASTNode *first = parse_param(p);
    if (!first)
        return NULL;
    vec_push(params, first);

    while (parser_peek(p)->kind == TOK_COMMA)
    {
        parser_advance(p); /* consume ',' */
        ASTNode *next = parse_param(p);
        if (!next)
            return NULL;
        vec_push(params, next);
    }

    return params;
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

static ASTNode *parse_var_decl(ParserCtx *p)
{
    Token *start = parser_peek(p);
    ASSERT(start->kind == TOK_KW_INT);
    Type *type = parse_type_specifier(p);
    if (!type)
        return NULL;

    Token *name_tok = parser_peek(p);
    if (name_tok->kind != TOK_IDENT)
    {
        parser_error(p, "expected variable name");
        return NULL;
    }
    parser_advance(p);

    ASTNode *init = NULL;
    if (parser_peek(p)->kind == TOK_ASSIGN)
    {
        parser_advance(p); /* consume '=' */
        init = parse_expr(p);
        if (!init)
            return NULL;
    }

    if (!parser_expect(p, TOK_SEMI, "';'"))
        return NULL;

    return ast_var_decl(p->arena, type, name_tok->payload.str, init, start->loc);
}

static ASTNode *parse_expr_stmt(ParserCtx *p)
{
    Token *start = parser_peek(p);
    ASTNode *expr = parse_expr(p);
    if (!expr)
        return NULL;
    if (!parser_expect(p, TOK_SEMI, "';'"))
        return NULL;
    return ast_expr_stmt(p->arena, expr, start->loc);
}

static ASTNode *parse_if_stmt(ParserCtx *p)
{
    Token *start = parser_peek(p);
    ASSERT(start->kind == TOK_KW_IF);
    parser_advance(p); /* consume 'if' */

    if (!parser_expect(p, TOK_LPAREN, "'('"))
        return NULL;

    ASTNode *cond = parse_expr(p);
    if (!cond)
        return NULL;

    if (!parser_expect(p, TOK_RPAREN, "')'"))
        return NULL;

    ASTNode *then_branch = parse_stmt(p);
    if (!then_branch)
        return NULL;

    ASTNode *else_branch = NULL;
    if (parser_peek(p)->kind == TOK_KW_ELSE)
    {
        parser_advance(p); /* consume 'else' */
        else_branch = parse_stmt(p);
        if (!else_branch)
            return NULL;
    }

    return ast_if_stmt(p->arena, cond, then_branch, else_branch, start->loc);
}

static ASTNode *parse_stmt(ParserCtx *p)
{
    Token *t = parser_peek(p);
    switch (t->kind)
    {
        case TOK_KW_INT:
            return parse_var_decl(p);
        case TOK_KW_RETURN:
            return parse_return_stmt(p);
        case TOK_KW_IF:
            return parse_if_stmt(p);
        case TOK_LBRACE:
            return parse_compound_stmt(p);
        default:
            return parse_expr_stmt(p);
    }
}

/* Forward declarations for expression levels */
static ASTNode *parse_primary(ParserCtx *p);
static ASTNode *parse_unary(ParserCtx *p);
static ASTNode *parse_mul(ParserCtx *p);
static ASTNode *parse_add(ParserCtx *p);
static ASTNode *parse_assign(ParserCtx *p);

static ASTNode *parse_expr(ParserCtx *p)
{
    return parse_assign(p);
}

static ASTNode *parse_primary(ParserCtx *p)
{
    Token *t = parser_peek(p);
    if (t->kind == TOK_INT_LIT)
    {
        parser_advance(p);
        return ast_int_literal(p->arena, t->payload.int_val, t->loc);
    }
    if (t->kind == TOK_IDENT)
    {
        parser_advance(p);
        const char *name = t->payload.str;
        if (parser_peek(p)->kind == TOK_LPAREN)
        {
            parser_advance(p); /* consume '(' */
            Vec *args = vec_new(p->arena);
            if (parser_peek(p)->kind != TOK_RPAREN)
            {
                while (true)
                {
                    ASTNode *arg = parse_expr(p);
                    if (!arg)
                        return NULL;
                    vec_push(args, arg);
                    if (parser_peek(p)->kind == TOK_COMMA)
                    {
                        parser_advance(p);
                        continue;
                    }
                    break;
                }
            }
            if (!parser_expect(p, TOK_RPAREN, "')'"))
                return NULL;
            return ast_call_expr(p->arena, name, args, t->loc);
        }
        return ast_ident(p->arena, name, t->loc);
    }
    if (t->kind == TOK_LPAREN)
    {
        parser_advance(p);
        ASTNode *inner = parse_expr(p);
        if (!inner)
            return NULL;
        if (!parser_expect(p, TOK_RPAREN, "')'"))
            return NULL;
        return inner;
    }
    parser_error(p, "expected expression");
    return NULL;
}

static ASTNode *parse_unary(ParserCtx *p)
{
    Token *t = parser_peek(p);
    if (t->kind == TOK_MINUS)
    {
        parser_advance(p);
        ASTNode *operand = parse_unary(p);
        if (!operand)
            return NULL;
        return ast_unary_expr(p->arena, UN_NEG, operand, t->loc);
    }
    return parse_primary(p);
}

static ASTNode *parse_mul(ParserCtx *p)
{
    ASTNode *left = parse_unary(p);
    if (!left)
        return NULL;

    while (true)
    {
        Token *t = parser_peek(p);
        BinOpKind op;
        switch (t->kind)
        {
            case TOK_STAR:
                op = BIN_MUL;
                break;
            case TOK_SLASH:
                op = BIN_DIV;
                break;
            case TOK_PERCENT:
                op = BIN_REM;
                break;
            default:
                return left;
        }
        parser_advance(p);
        ASTNode *right = parse_unary(p);
        if (!right)
            return NULL;
        left = ast_binary_expr(p->arena, op, left, right, t->loc);
    }
}

static ASTNode *parse_add(ParserCtx *p)
{
    ASTNode *left = parse_mul(p);
    if (!left)
        return NULL;

    while (true)
    {
        Token *t = parser_peek(p);
        BinOpKind op;
        switch (t->kind)
        {
            case TOK_PLUS:
                op = BIN_ADD;
                break;
            case TOK_MINUS:
                op = BIN_SUB;
                break;
            default:
                return left;
        }
        parser_advance(p);
        ASTNode *right = parse_mul(p);
        if (!right)
            return NULL;
        left = ast_binary_expr(p->arena, op, left, right, t->loc);
    }
}

static ASTNode *parse_assign(ParserCtx *p)
{
    ASTNode *left = parse_add(p);
    if (!left)
        return NULL;

    if (parser_peek(p)->kind == TOK_ASSIGN)
    {
        Token *t = parser_peek(p);
        parser_advance(p);
        ASTNode *right = parse_assign(p); /* right-associative */
        if (!right)
            return NULL;
        left = ast_binary_expr(p->arena, BIN_ASSIGN, left, right, t->loc);
    }
    return left;
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

    Vec *decls = vec_new(arena);
    while (parser_peek(&p)->kind != TOK_EOF)
    {
        ASTNode *node = parse_func_def(&p);
        if (!node)
            return NULL;
        vec_push(decls, node);
    }

    return ast_program(arena, decls, tokens[0].loc);
}
