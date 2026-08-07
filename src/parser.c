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
    {
        return &p->tokens[p->pos];
    }

    return &p->tokens[p->count - 1]; /* EOF */
}

static Token *parser_advance(ParserCtx *p)
{
    Token *t = parser_peek(p);
    if (p->pos < p->count - 1)
    {
        p->pos++;
    }
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
static ASTNode *parse_primary(ParserCtx *p);
static ASTNode *parse_unary(ParserCtx *p);
static ASTNode *parse_mul(ParserCtx *p);
static ASTNode *parse_add(ParserCtx *p);
static ASTNode *parse_shift(ParserCtx *p);
static ASTNode *parse_relational(ParserCtx *p);
static ASTNode *parse_equality(ParserCtx *p);
static ASTNode *parse_bit_and(ParserCtx *p);
static ASTNode *parse_bit_xor(ParserCtx *p);
static ASTNode *parse_bit_or(ParserCtx *p);
static ASTNode *parse_log_and(ParserCtx *p);
static ASTNode *parse_log_or(ParserCtx *p);
static ASTNode *parse_ternary(ParserCtx *p);
static ASTNode *parse_assign(ParserCtx *p);

static Type *parse_type_specifier(ParserCtx *p)
{
    Token *t = parser_peek(p);
    switch (t->kind)
    {
        case TOK_KW_INT:
            parser_advance(p);
            return type_int();
        case TOK_KW_VOID:
            parser_advance(p);
            return type_void();
        case TOK_KW_CHAR:
            parser_advance(p);
            return type_char();
        case TOK_KW_SHORT:
            parser_advance(p);
            return type_short();
        case TOK_KW_LONG:
            parser_advance(p);
            return type_long();
        case TOK_KW_UNSIGNED:
        {
            parser_advance(p);
            Token *next = parser_peek(p);
            if (next->kind == TOK_KW_INT)
            {
                parser_advance(p);
                return type_uint();
            }
            if (next->kind == TOK_KW_CHAR)
            {
                parser_advance(p);
                return type_uchar();
            }
            if (next->kind == TOK_KW_SHORT)
            {
                parser_advance(p);
                return type_ushort();
            }
            if (next->kind == TOK_KW_LONG)
            {
                parser_advance(p);
                return type_ulong();
            }
            return type_uint();
        }
        default:
            parser_error(p, "expected type specifier");
            return NULL;
    }
}

static ASTNode *parse_param(ParserCtx *p)
{
    Token *start = parser_peek(p);
    Type *type = parse_type_specifier(p);
    if (!type)
    {
        return NULL;
    }

    Token *name = parser_peek(p);
    if (name->kind != TOK_IDENT)
    {
        parser_error(p, "expected parameter name");
        return NULL;
    }
    parser_advance(p);

    return ast_var_decl(type, name->payload.str, NULL, start->loc, p->arena);
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

    if (t->kind == TOK_RPAREN)
    {
        return params;
    }

    ASTNode *first = parse_param(p);
    if (!first)
    {
        return NULL;
    }
    vec_push(params, first);

    while (parser_peek(p)->kind == TOK_COMMA)
    {
        parser_advance(p); /* consume ',' */
        ASTNode *next = parse_param(p);
        if (!next)
        {
            return NULL;
        }
        vec_push(params, next);
    }

    return params;
}

static ASTNode *parse_compound_stmt(ParserCtx *p)
{
    Token *start = parser_peek(p);
    if (!parser_expect(p, TOK_LBRACE, "'{'"))
    {
        return NULL;
    }

    Vec *stmts = vec_new(p->arena);
    while (parser_peek(p)->kind != TOK_RBRACE)
    {
        ASTNode *stmt = parse_stmt(p);
        if (!stmt)
        {
            return NULL;
        }
        vec_push(stmts, stmt);
    }
    if (!parser_expect(p, TOK_RBRACE, "'}'"))
    {
        return NULL;
    }

    return ast_compound_stmt(stmts, start->loc, p->arena);
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
        {
            return NULL;
        }
    }
    if (!parser_expect(p, TOK_SEMI, "';'"))
    {
        return NULL;
    }

    return ast_return_stmt(expr, start->loc, p->arena);
}

static ASTNode *parse_var_decl(ParserCtx *p)
{
    Token *start = parser_peek(p);
    (void) start; /* type-checking done by parse_type_specifier */
    Type *type = parse_type_specifier(p);
    if (!type)
    {
        return NULL;
    }

    Token *name = parser_peek(p);
    if (name->kind != TOK_IDENT)
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
        {
            return NULL;
        }
    }

    if (!parser_expect(p, TOK_SEMI, "';'"))
    {
        return NULL;
    }

    return ast_var_decl(type, name->payload.str, init, start->loc, p->arena);
}

static ASTNode *parse_expr_stmt(ParserCtx *p)
{
    Token *start = parser_peek(p);
    ASTNode *expr = parse_expr(p);
    if (!expr)
    {
        return NULL;
    }
    if (!parser_expect(p, TOK_SEMI, "';'"))
    {
        return NULL;
    }
    return ast_expr_stmt(expr, start->loc, p->arena);
}

static ASTNode *parse_if_stmt(ParserCtx *p)
{
    Token *start = parser_peek(p);
    ASSERT(start->kind == TOK_KW_IF);
    parser_advance(p); /* consume 'if' */

    if (!parser_expect(p, TOK_LPAREN, "'('"))
    {
        return NULL;
    }

    ASTNode *cond = parse_expr(p);
    if (!cond)
    {
        return NULL;
    }

    if (!parser_expect(p, TOK_RPAREN, "')'"))
    {
        return NULL;
    }

    ASTNode *then_branch = parse_stmt(p);
    if (!then_branch)
    {
        return NULL;
    }

    ASTNode *else_branch = NULL;
    if (parser_peek(p)->kind == TOK_KW_ELSE)
    {
        parser_advance(p); /* consume 'else' */
        else_branch = parse_stmt(p);
        if (!else_branch)
        {
            return NULL;
        }
    }

    return ast_if_stmt(cond, then_branch, else_branch, start->loc, p->arena);
}

static ASTNode *parse_while_stmt(ParserCtx *p)
{
    Token *start = parser_peek(p);
    ASSERT(start->kind == TOK_KW_WHILE);
    parser_advance(p); /* consume 'while' */

    if (!parser_expect(p, TOK_LPAREN, "'('"))
    {
        return NULL;
    }

    ASTNode *cond = parse_expr(p);
    if (!cond)
    {
        return NULL;
    }

    if (!parser_expect(p, TOK_RPAREN, "')'"))
    {
        return NULL;
    }

    ASTNode *body = parse_stmt(p);
    if (!body)
    {
        return NULL;
    }

    return ast_while_stmt(cond, body, start->loc, p->arena);
}

static ASTNode *parse_do_while_stmt(ParserCtx *p)
{
    Token *start = parser_peek(p);
    ASSERT(start->kind == TOK_KW_DO);
    parser_advance(p); /* consume 'do' */

    ASTNode *body = parse_stmt(p);
    if (!body)
    {
        return NULL;
    }

    if (!parser_expect(p, TOK_KW_WHILE, "'while'"))
    {
        return NULL;
    }
    if (!parser_expect(p, TOK_LPAREN, "'('"))
    {
        return NULL;
    }

    ASTNode *cond = parse_expr(p);
    if (!cond)
    {
        return NULL;
    }

    if (!parser_expect(p, TOK_RPAREN, "')'"))
    {
        return NULL;
    }
    if (!parser_expect(p, TOK_SEMI, "';'"))
    {
        return NULL;
    }

    return ast_do_while_stmt(cond, body, start->loc, p->arena);
}

static ASTNode *parse_for_stmt(ParserCtx *p)
{
    Token *start = parser_peek(p);
    ASSERT(start->kind == TOK_KW_FOR);
    parser_advance(p); /* consume 'for' */

    if (!parser_expect(p, TOK_LPAREN, "'('"))
    {
        return NULL;
    }

    ASTNode *init = NULL;
    if (parser_peek(p)->kind != TOK_SEMI)
    {
        if (parser_peek(p)->kind == TOK_KW_INT || parser_peek(p)->kind == TOK_KW_CHAR ||
            parser_peek(p)->kind == TOK_KW_SHORT || parser_peek(p)->kind == TOK_KW_LONG ||
            parser_peek(p)->kind == TOK_KW_UNSIGNED)
        {
            init = parse_var_decl(p);
        }
        else
        {
            init = parse_expr_stmt(p);
        }
        if (!init)
        {
            return NULL;
        }
    }
    else
    {
        /* empty init clause */
        parser_advance(p); /* consume ';' */
    }

    ASTNode *cond = NULL;
    if (parser_peek(p)->kind != TOK_SEMI)
    {
        cond = parse_expr(p);
        if (!cond)
        {
            return NULL;
        }
    }
    if (!parser_expect(p, TOK_SEMI, "';'"))
    {
        return NULL;
    }

    ASTNode *post = NULL;
    if (parser_peek(p)->kind != TOK_RPAREN)
    {
        post = parse_expr(p);
        if (!post)
        {
            return NULL;
        }
    }
    if (!parser_expect(p, TOK_RPAREN, "')'"))
    {
        return NULL;
    }

    ASTNode *body = parse_stmt(p);
    if (!body)
    {
        return NULL;
    }

    return ast_for_stmt(init, cond, post, body, start->loc, p->arena);
}

static ASTNode *parse_break_stmt(ParserCtx *p)
{
    Token *start = parser_peek(p);
    ASSERT(start->kind == TOK_KW_BREAK);
    parser_advance(p); /* consume 'break' */
    if (!parser_expect(p, TOK_SEMI, "';'"))
    {
        return NULL;
    }
    return ast_break_stmt(start->loc, p->arena);
}

static ASTNode *parse_continue_stmt(ParserCtx *p)
{
    Token *start = parser_peek(p);
    ASSERT(start->kind == TOK_KW_CONTINUE);
    parser_advance(p); /* consume 'continue' */
    if (!parser_expect(p, TOK_SEMI, "';'"))
    {
        return NULL;
    }
    return ast_continue_stmt(start->loc, p->arena);
}

static ASTNode *parse_goto_stmt(ParserCtx *p)
{
    Token *start = parser_peek(p);
    ASSERT(start->kind == TOK_KW_GOTO);
    parser_advance(p); /* consume 'goto' */

    Token *label = parser_peek(p);
    if (label->kind != TOK_IDENT)
    {
        parser_error(p, "expected label name");
        return NULL;
    }
    parser_advance(p);

    if (!parser_expect(p, TOK_SEMI, "';'"))
    {
        return NULL;
    }

    return ast_goto_stmt(label->payload.str, start->loc, p->arena);
}

static ASTNode *parse_label_stmt(ParserCtx *p)
{
    Token *start = parser_peek(p);
    ASSERT(start->kind == TOK_IDENT);
    const char *label = start->payload.str;
    parser_advance(p); /* consume label */
    parser_advance(p); /* consume ':' */

    ASTNode *stmt = parse_stmt(p);
    if (!stmt)
    {
        return NULL;
    }

    return ast_label_stmt(label, stmt, start->loc, p->arena);
}

static ASTNode *parse_stmt(ParserCtx *p)
{
    Token *t = parser_peek(p);
    switch (t->kind)
    {
        case TOK_KW_INT:
        case TOK_KW_CHAR:
        case TOK_KW_SHORT:
        case TOK_KW_LONG:
        case TOK_KW_UNSIGNED:
            return parse_var_decl(p);
        case TOK_KW_RETURN:
            return parse_return_stmt(p);
        case TOK_KW_IF:
            return parse_if_stmt(p);
        case TOK_KW_WHILE:
            return parse_while_stmt(p);
        case TOK_KW_FOR:
            return parse_for_stmt(p);
        case TOK_KW_DO:
            return parse_do_while_stmt(p);
        case TOK_KW_BREAK:
            return parse_break_stmt(p);
        case TOK_KW_CONTINUE:
            return parse_continue_stmt(p);
        case TOK_KW_GOTO:
            return parse_goto_stmt(p);
        case TOK_LBRACE:
            return parse_compound_stmt(p);
        case TOK_IDENT:
            if (p->pos + 1 < p->count && p->tokens[p->pos + 1].kind == TOK_COLON)
            {
                return parse_label_stmt(p);
            }
            return parse_expr_stmt(p);
        default:
            return parse_expr_stmt(p);
    }
}

static ASTNode *parse_expr(ParserCtx *p)
{
    return parse_assign(p);
}

static ASTNode *parse_identifier_expr(ParserCtx *p, Token *t)
{
    parser_advance(p);
    const char *name = t->payload.str;
    if (parser_peek(p)->kind == TOK_LPAREN)
    {
        parser_advance(p);
        Vec *args = vec_new(p->arena);
        if (parser_peek(p)->kind != TOK_RPAREN)
        {
            while (true)
            {
                ASTNode *arg = parse_expr(p);
                if (!arg)
                {
                    return NULL;
                }

                vec_push(args, arg);

                if (parser_peek(p)->kind != TOK_COMMA)
                {
                    break;
                }

                parser_advance(p);
            }
        }
        if (!parser_expect(p, TOK_RPAREN, "')'"))
        {
            return NULL;
        }
        return ast_call_expr(name, args, t->loc, p->arena);
    }
    return ast_ident(name, t->loc, p->arena);
}

static ASTNode *parse_primary(ParserCtx *p)
{
    Token *t = parser_peek(p);
    switch (t->kind)
    {
        case TOK_INT_LIT:
        {
            parser_advance(p);
            return ast_int_literal(t->payload.int_val, t->int_suffix.is_unsigned,
                                   t->int_suffix.length, t->int_suffix.is_hex, t->loc, p->arena);
        }
        case TOK_IDENT:
        {
            return parse_identifier_expr(p, t);
        }
        case TOK_LPAREN:
        {
            parser_advance(p);
            ASTNode *inner = parse_expr(p);
            if (!inner)
            {
                return NULL;
            }
            if (!parser_expect(p, TOK_RPAREN, "')'"))
            {
                return NULL;
            }
            return inner;
        }
        default:
        {
            parser_error(p, "expected expression");
            return NULL;
        }
    }
}

static ASTNode *parse_unary(ParserCtx *p)
{
    Token *t = parser_peek(p);
    if (t->kind == TOK_MINUS)
    {
        parser_advance(p);
        ASTNode *operand = parse_unary(p);
        if (!operand)
        {
            return NULL;
        }
        return ast_unary_expr(UN_NEG, operand, t->loc, p->arena);
    }
    if (t->kind == TOK_NOT)
    {
        parser_advance(p);
        ASTNode *operand = parse_unary(p);
        if (!operand)
        {
            return NULL;
        }
        return ast_unary_expr(UN_LOG_NOT, operand, t->loc, p->arena);
    }
    if (t->kind == TOK_TILDE)
    {
        parser_advance(p);
        ASTNode *operand = parse_unary(p);
        if (!operand)
        {
            return NULL;
        }
        return ast_unary_expr(UN_BIT_NOT, operand, t->loc, p->arena);
    }
    return parse_primary(p);
}

static ASTNode *parse_mul(ParserCtx *p)
{
    ASTNode *left = parse_unary(p);
    if (!left)
    {
        return NULL;
    }

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
        {
            return NULL;
        }
        left = ast_binary_expr(op, left, right, t->loc, p->arena);
    }
}

static ASTNode *parse_add(ParserCtx *p)
{
    ASTNode *left = parse_mul(p);
    if (!left)
    {
        return NULL;
    }

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
        {
            return NULL;
        }
        left = ast_binary_expr(op, left, right, t->loc, p->arena);
    }
}

static ASTNode *parse_shift(ParserCtx *p)
{
    ASTNode *left = parse_add(p);
    if (!left)
    {
        return NULL;
    }

    while (true)
    {
        Token *t = parser_peek(p);
        BinOpKind op;
        switch (t->kind)
        {
            case TOK_SHL:
                op = BIN_SHL;
                break;
            case TOK_SHR:
                op = BIN_SHR;
                break;
            default:
                return left;
        }
        parser_advance(p);
        ASTNode *right = parse_add(p);
        if (!right)
        {
            return NULL;
        }
        left = ast_binary_expr(op, left, right, t->loc, p->arena);
    }
}

static ASTNode *parse_relational(ParserCtx *p)
{
    ASTNode *left = parse_shift(p);
    if (!left)
    {
        return NULL;
    }

    while (true)
    {
        Token *t = parser_peek(p);
        BinOpKind op;
        switch (t->kind)
        {
            case TOK_LT:
                op = BIN_LT;
                break;
            case TOK_GT:
                op = BIN_GT;
                break;
            case TOK_LE:
                op = BIN_LE;
                break;
            case TOK_GE:
                op = BIN_GE;
                break;
            default:
                return left;
        }
        parser_advance(p);
        ASTNode *right = parse_shift(p);
        if (!right)
        {
            return NULL;
        }
        left = ast_binary_expr(op, left, right, t->loc, p->arena);
    }
}

static ASTNode *parse_equality(ParserCtx *p)
{
    ASTNode *left = parse_relational(p);
    if (!left)
    {
        return NULL;
    }

    while (true)
    {
        Token *t = parser_peek(p);
        BinOpKind op;
        switch (t->kind)
        {
            case TOK_EQ:
                op = BIN_EQ;
                break;
            case TOK_NE:
                op = BIN_NE;
                break;
            default:
                return left;
        }
        parser_advance(p);
        ASTNode *right = parse_relational(p);
        if (!right)
        {
            return NULL;
        }
        left = ast_binary_expr(op, left, right, t->loc, p->arena);
    }
}

static ASTNode *parse_bit_and(ParserCtx *p)
{
    ASTNode *left = parse_equality(p);
    if (!left)
    {
        return NULL;
    }

    while (parser_peek(p)->kind == TOK_BW_AND)
    {
        Token *t = parser_peek(p);
        parser_advance(p);
        ASTNode *right = parse_equality(p);
        if (!right)
        {
            return NULL;
        }
        left = ast_binary_expr(BIN_AND, left, right, t->loc, p->arena);
    }
    return left;
}

static ASTNode *parse_bit_xor(ParserCtx *p)
{
    ASTNode *left = parse_bit_and(p);
    if (!left)
    {
        return NULL;
    }

    while (parser_peek(p)->kind == TOK_BW_XOR)
    {
        Token *t = parser_peek(p);
        parser_advance(p);
        ASTNode *right = parse_bit_and(p);
        if (!right)
        {
            return NULL;
        }
        left = ast_binary_expr(BIN_XOR, left, right, t->loc, p->arena);
    }
    return left;
}

static ASTNode *parse_bit_or(ParserCtx *p)
{
    ASTNode *left = parse_bit_xor(p);
    if (!left)
    {
        return NULL;
    }

    while (parser_peek(p)->kind == TOK_BW_OR)
    {
        Token *t = parser_peek(p);
        parser_advance(p);
        ASTNode *right = parse_bit_xor(p);
        if (!right)
        {
            return NULL;
        }
        left = ast_binary_expr(BIN_OR, left, right, t->loc, p->arena);
    }
    return left;
}

static ASTNode *parse_log_and(ParserCtx *p)
{
    ASTNode *left = parse_bit_or(p);
    if (!left)
    {
        return NULL;
    }

    while (parser_peek(p)->kind == TOK_LOG_AND)
    {
        Token *t = parser_peek(p);
        parser_advance(p);
        ASTNode *right = parse_bit_or(p);
        if (!right)
        {
            return NULL;
        }
        left = ast_binary_expr(BIN_LOG_AND, left, right, t->loc, p->arena);
    }
    return left;
}

static ASTNode *parse_log_or(ParserCtx *p)
{
    ASTNode *left = parse_log_and(p);
    if (!left)
    {
        return NULL;
    }

    while (parser_peek(p)->kind == TOK_LOG_OR)
    {
        Token *t = parser_peek(p);
        parser_advance(p);
        ASTNode *right = parse_log_and(p);
        if (!right)
        {
            return NULL;
        }
        left = ast_binary_expr(BIN_LOG_OR, left, right, t->loc, p->arena);
    }
    return left;
}

static ASTNode *parse_ternary(ParserCtx *p)
{
    ASTNode *cond = parse_log_or(p);
    if (!cond)
    {
        return NULL;
    }

    if (parser_peek(p)->kind != TOK_QUESTION)
    {
        return cond;
    }

    Token *t = parser_peek(p);
    parser_advance(p); /* consume '?' */
    ASTNode *then_expr = parse_expr(p);
    if (!then_expr)
    {
        return NULL;
    }
    if (!parser_expect(p, TOK_COLON, "':'"))
    {
        return NULL;
    }
    ASTNode *else_expr = parse_ternary(p);
    if (!else_expr)
    {
        return NULL;
    }
    return ast_ternary_expr(cond, then_expr, else_expr, t->loc, p->arena);
}

static ASTNode *parse_assign(ParserCtx *p)
{
    ASTNode *left = parse_ternary(p);
    if (!left)
    {
        return NULL;
    }

    if (parser_peek(p)->kind == TOK_ASSIGN)
    {
        Token *t = parser_peek(p);
        parser_advance(p);
        ASTNode *right = parse_assign(p);
        if (!right)
        {
            return NULL;
        }
        left = ast_binary_expr(BIN_ASSIGN, left, right, t->loc, p->arena);
    }
    return left;
}

static ASTNode *parse_func_def(ParserCtx *p)
{
    Token *start = parser_peek(p);
    Type *ret_type = parse_type_specifier(p);
    if (!ret_type)
    {
        return NULL;
    }

    Token *name = parser_peek(p);
    if (name->kind != TOK_IDENT)
    {
        parser_error(p, "expected function name");
        return NULL;
    }
    parser_advance(p);

    if (!parser_expect(p, TOK_LPAREN, "'('"))
    {
        return NULL;
    }

    Vec *params = parse_param_list(p);
    if (!params)
    {
        return NULL;
    }

    if (!parser_expect(p, TOK_RPAREN, "')'"))
    {
        return NULL;
    }

    ASTNode *body = parse_compound_stmt(p);
    if (!body)
    {
        return NULL;
    }

    return ast_func_def(ret_type, name->payload.str, params, body, start->loc, p->arena);
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
        {
            return NULL;
        }
        vec_push(decls, node);
    }

    return ast_program(decls, tokens[0].loc, arena);
}
