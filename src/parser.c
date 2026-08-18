#include "parser.h"
#include "util/hashmap.h"
#include <stdarg.h>
#include <stdio.h>

typedef struct ParserCtx ParserCtx;
struct ParserCtx
{
    Token *tokens;
    u64 count;
    u64 pos;
    Arena *arena;
    StrMap *enum_consts; /* enumerator name -> i64* value, resolved at parse time */
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

static Type *parse_type_specifier(ParserCtx *p);
static Type *parse_array_suffix(ParserCtx *p, Type *type);
static ASTNode *parse_expr(ParserCtx *p);
static ASTNode *parse_stmt(ParserCtx *p);
static ASTNode *parse_primary(ParserCtx *p);
static bool parser_check_not_enumerator(ParserCtx *p, const char *name);
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
static ASTNode *parse_postfix(ParserCtx *p);

static Type *parse_type_specifier(ParserCtx *p)
{
    Token *t = parser_peek(p);
    Type *ty = NULL;
    switch (t->kind)
    {
        case TOK_KW_INT:
            parser_advance(p);
            ty = type_int();
            break;
        case TOK_KW_VOID:
            parser_advance(p);
            ty = type_void();
            break;
        case TOK_KW_CHAR:
            parser_advance(p);
            ty = type_char();
            break;
        case TOK_KW_SHORT:
            parser_advance(p);
            ty = type_short();
            break;
        case TOK_KW_LONG:
            parser_advance(p);
            ty = type_long();
            break;
        case TOK_KW_UNSIGNED:
        {
            parser_advance(p);
            Token *next = parser_peek(p);
            if (next->kind == TOK_KW_INT)
            {
                parser_advance(p);
                ty = type_uint();
            }
            else if (next->kind == TOK_KW_CHAR)
            {
                parser_advance(p);
                ty = type_uchar();
            }
            else if (next->kind == TOK_KW_SHORT)
            {
                parser_advance(p);
                ty = type_ushort();
            }
            else if (next->kind == TOK_KW_LONG)
            {
                parser_advance(p);
                ty = type_ulong();
            }
            else
            {
                ty = type_uint();
            }
            break;
        }
        case TOK_KW_STRUCT:
        case TOK_KW_UNION:
        {
            bool is_union = t->kind == TOK_KW_UNION;
            parser_advance(p);
            Token *tag_tok = parser_peek(p);
            if (tag_tok->kind != TOK_IDENT)
            {
                parser_error(p, "expected tag name after '%s'", is_union ? "union" : "struct");
                return NULL;
            }
            parser_advance(p);
            TypeKind kind = is_union ? TYPE_UNION : TYPE_STRUCT;
            Type *existing = type_record_lookup(tag_tok->payload.str);
            if (existing && existing->kind != kind)
            {
                parser_error(p, "tag '%s' redeclared with a different kind", tag_tok->payload.str);
                return NULL;
            }
            ty = type_record(kind, tag_tok->payload.str);
            break;
        }
        case TOK_KW_ENUM:
        {
            parser_advance(p);
            Token *tag_tok = parser_peek(p);
            if (tag_tok->kind != TOK_IDENT)
            {
                parser_error(p, "expected tag name after 'enum'");
                return NULL;
            }
            parser_advance(p);
            Type *existing = type_record_lookup(tag_tok->payload.str);
            if (existing && existing->kind != TYPE_ENUM)
            {
                parser_error(p, "tag '%s' redeclared with a different kind", tag_tok->payload.str);
                return NULL;
            }
            ty = type_enum(tag_tok->payload.str);
            break;
        }
        default:
            parser_error(p, "expected type specifier");
            return NULL;
    }

    /* Postfix type operators: * only ([] is part of declarator) */
    while (parser_peek(p)->kind == TOK_STAR)
    {
        parser_advance(p);
        ty = type_ptr(ty);
    }

    return ty;
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

    if (!parser_check_not_enumerator(p, name->payload.str))
    {
        return NULL;
    }

    /* Array parameters decay to pointer (C11 §6.7.6.3p7). */
    type = parse_array_suffix(p, type);
    if (!type)
    {
        return NULL;
    }
    type = type_decay(type);

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

static Type *parse_array_suffix(ParserCtx *p, Type *type)
{
    while (parser_peek(p)->kind == TOK_LBRACKET)
    {
        parser_advance(p);
        u64 len = 0;
        if (parser_peek(p)->kind != TOK_RBRACKET)
        {
            ASTNode *size_expr = parse_expr(p);
            if (!size_expr || size_expr->kind != AST_INT_LITERAL)
            {
                parser_error(p, "array size must be an integer constant");
                return NULL;
            }
            len = (u64) ast_as(ASTIntLiteral, size_expr)->value;
        }
        parser_expect(p, TOK_RBRACKET, "]");
        type = type_array(type, len);
    }
    return type;
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

    if (!parser_check_not_enumerator(p, name->payload.str))
    {
        return NULL;
    }

    type = parse_array_suffix(p, type);
    if (!type)
    {
        return NULL;
    }

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
            parser_peek(p)->kind == TOK_KW_UNSIGNED || parser_peek(p)->kind == TOK_KW_STRUCT ||
            parser_peek(p)->kind == TOK_KW_UNION)
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
        case TOK_KW_VOID:
        case TOK_KW_STRUCT:
        case TOK_KW_UNION:
        case TOK_KW_ENUM:
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
            i64 *const_val = strmap_get(p->enum_consts, t->payload.str);
            if (const_val)
            {
                parser_advance(p);
                return ast_int_literal(*const_val, false, SUFFIX_NONE, false, t->loc, p->arena);
            }
            return parse_identifier_expr(p, t);
        }
        case TOK_STRING_LIT:
        {
            parser_advance(p);
            return ast_string_literal(t->payload.str, t->str_len, t->loc, p->arena);
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
    static const struct
    {
        TokenKind tok;
        UnaryOpKind uop;
    } unary_ops[] = {
        {TOK_MINUS, UN_NEG},  {TOK_NOT, UN_LOG_NOT}, {TOK_TILDE, UN_BIT_NOT},
        {TOK_STAR, UN_DEREF}, {TOK_BW_AND, UN_ADDR},
    };
    for (size_t i = 0; i < sizeof(unary_ops) / sizeof(unary_ops[0]); i++)
    {
        if (t->kind == unary_ops[i].tok)
        {
            parser_advance(p);
            ASTNode *operand = parse_unary(p);
            if (!operand)
            {
                return NULL;
            }
            return ast_unary_expr(unary_ops[i].uop, operand, t->loc, p->arena);
        }
    }
    if (t->kind == TOK_KW_SIZEOF)
    {
        parser_advance(p);
        bool is_type = false;
        if (parser_peek(p)->kind == TOK_LPAREN)
        {
            Token *la = &p->tokens[p->pos + 1];
            if (la->kind == TOK_KW_INT || la->kind == TOK_KW_CHAR || la->kind == TOK_KW_VOID ||
                la->kind == TOK_KW_SHORT || la->kind == TOK_KW_LONG ||
                la->kind == TOK_KW_UNSIGNED || la->kind == TOK_KW_STRUCT ||
                la->kind == TOK_KW_UNION || la->kind == TOK_KW_ENUM)
            {
                is_type = true;
            }
        }
        if (is_type)
        {
            parser_advance(p); /* consume '(' */
            Type *ty = parse_type_specifier(p);
            if (!ty)
            {
                return NULL;
            }
            ty = parse_array_suffix(p, ty);
            if (!ty)
            {
                return NULL;
            }
            parser_expect(p, TOK_RPAREN, ")");
            return ast_sizeof_type(ty, 0, t->loc, p->arena);
        }
        else
        {
            ASTNode *operand = parse_unary(p);
            if (!operand)
            {
                return NULL;
            }
            return ast_sizeof_expr(operand, 0, t->loc, p->arena);
        }
    }
    return parse_postfix(p);
}

static ASTNode *parse_postfix(ParserCtx *p)
{
    ASTNode *node = parse_primary(p);
    if (!node)
    {
        return NULL;
    }
    while (true)
    {
        Token *t = parser_peek(p);
        if (t->kind == TOK_LBRACKET)
        {
            parser_advance(p);
            ASTNode *index = parse_expr(p);
            if (!index)
            {
                return NULL;
            }
            if (!parser_expect(p, TOK_RBRACKET, "]"))
            {
                return NULL;
            }
            node = ast_subscript_expr(node, index, t->loc, p->arena);
        }
        else if (t->kind == TOK_DOT || t->kind == TOK_ARROW)
        {
            bool is_arrow = t->kind == TOK_ARROW;
            parser_advance(p);
            Token *member = parser_peek(p);
            if (member->kind != TOK_IDENT)
            {
                parser_error(p, "expected member name after '%s'", is_arrow ? "->" : ".");
                return NULL;
            }
            parser_advance(p);
            node = ast_member_access(node, member->payload.str, is_arrow, t->loc, p->arena);
        }
        else
        {
            break;
        }
    }
    return node;
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

static ASTNode *parse_record_decl(ParserCtx *p, bool is_union)
{
    Token *start = parser_peek(p);
    parser_advance(p);

    Token *tag_tok = parser_peek(p);
    if (tag_tok->kind != TOK_IDENT)
    {
        parser_error(p, "expected tag name");
        return NULL;
    }
    parser_advance(p);
    const char *tag = tag_tok->payload.str;

    TypeKind kind = is_union ? TYPE_UNION : TYPE_STRUCT;
    Type *existing = type_record_lookup(tag);
    if (existing && existing->kind != kind)
    {
        parser_error(p, "tag '%s' redeclared with a different kind", tag);
        return NULL;
    }
    Type *rec = type_record(kind, tag); /* register incomplete before members (self-ref) */

    /* Forward declaration: "struct tag;" */
    if (parser_peek(p)->kind == TOK_SEMI)
    {
        parser_advance(p);
        return ast_struct_decl(tag, is_union, vec_new(p->arena), start->loc, p->arena);
    }

    if (rec->record.complete)
    {
        parser_error(p, "redefinition of '%s'", tag);
        return NULL;
    }

    if (!parser_expect(p, TOK_LBRACE, "'{'"))
    {
        return NULL;
    }

    Vec *field_decls = vec_new(p->arena);   /* Vec<ASTVarDecl*>, kept for the AST dump */
    Vec *record_fields = vec_new(p->arena); /* Vec<RecordField*>, completes the type */
    while (parser_peek(p)->kind != TOK_RBRACE)
    {
        Token *fstart = parser_peek(p);
        Type *ftype = parse_type_specifier(p);
        if (!ftype)
        {
            return NULL;
        }

        Token *fname = parser_peek(p);
        if (fname->kind != TOK_IDENT)
        {
            parser_error(p, "expected field name");
            return NULL;
        }
        parser_advance(p);

        ftype = parse_array_suffix(p, ftype);
        if (!ftype)
        {
            return NULL;
        }

        if (!parser_expect(p, TOK_SEMI, "';'"))
        {
            return NULL;
        }
        ASTNode *field_decl = ast_var_decl(ftype, fname->payload.str, NULL, fstart->loc, p->arena);
        vec_push(field_decls, field_decl);

        RecordField *rf = arena_alloc(p->arena, sizeof(RecordField), _Alignof(RecordField));
        rf->name = fname->payload.str;
        rf->type = ftype;
        rf->offset = 0;
        vec_push(record_fields, rf);
    }
    if (!parser_expect(p, TOK_RBRACE, "'}'"))
    {
        return NULL;
    }
    if (!parser_expect(p, TOK_SEMI, "';'"))
    {
        return NULL;
    }

    /* Complete the type now so array/pointer types formed later (and the
       type's size/alignment) see the finished layout. */
    type_record_complete(rec, record_fields);

    return ast_struct_decl(tag, is_union, field_decls, start->loc, p->arena);
}

static bool parser_check_not_enumerator(ParserCtx *p, const char *name)
{
    if (strmap_get(p->enum_consts, name))
    {
        parser_error(p, "redeclaration of enumerator '%s'", name);
        return false;
    }
    return true;
}

/* Fold an integer constant expression (C11 §6.6). Returns false with an error
   already reported if the node is not foldable or not constant. */
static bool fold_constant_expr(ParserCtx *p, ASTNode *node, i64 *out)
{
    if (!node)
    {
        return false;
    }
    switch (node->kind)
    {
        case AST_INT_LITERAL:
            *out = ast_as(ASTIntLiteral, node)->value;
            return true;
        case AST_UNARY_EXPR:
        {
            ASTUnaryExpr *u = ast_as(ASTUnaryExpr, node);
            i64 v;
            if (!fold_constant_expr(p, u->operand, &v))
            {
                return false;
            }
            switch (u->op)
            {
                case UN_NEG:
                    *out = -v;
                    return true;
                case UN_BIT_NOT:
                    *out = ~v;
                    return true;
                case UN_LOG_NOT:
                    *out = !v;
                    return true;
                default:
                    return false;
            }
        }
        case AST_BINARY_EXPR:
        {
            ASTBinaryExpr *b = ast_as(ASTBinaryExpr, node);
            i64 l, r;
            if (!fold_constant_expr(p, b->left, &l) || !fold_constant_expr(p, b->right, &r))
            {
                return false;
            }
            switch (b->op)
            {
                case BIN_ADD:
                    *out = l + r;
                    return true;
                case BIN_SUB:
                    *out = l - r;
                    return true;
                case BIN_MUL:
                    *out = l * r;
                    return true;
                case BIN_DIV:
                case BIN_REM:
                    if (r == 0)
                    {
                        parser_error(p, "division by zero in enumerator value");
                        return false;
                    }
                    *out = b->op == BIN_DIV ? l / r : l % r;
                    return true;
                case BIN_SHL:
                case BIN_SHR:
                    if (r < 0 || r > 63)
                    {
                        parser_error(p, "shift count out of range in enumerator value");
                        return false;
                    }
                    *out = b->op == BIN_SHL ? l << r : l >> r;
                    return true;
                case BIN_AND:
                    *out = l & r;
                    return true;
                case BIN_OR:
                    *out = l | r;
                    return true;
                case BIN_XOR:
                    *out = l ^ r;
                    return true;
                case BIN_EQ:
                    *out = l == r;
                    return true;
                case BIN_NE:
                    *out = l != r;
                    return true;
                case BIN_LT:
                    *out = l < r;
                    return true;
                case BIN_GT:
                    *out = l > r;
                    return true;
                case BIN_LE:
                    *out = l <= r;
                    return true;
                case BIN_GE:
                    *out = l >= r;
                    return true;
                default:
                    return false;
            }
        }
        default:
            return false;
    }
}

static ASTNode *parse_enum_decl(ParserCtx *p)
{
    Token *start = parser_peek(p);
    parser_advance(p); /* consume 'enum' */

    const char *tag = NULL;
    if (parser_peek(p)->kind == TOK_IDENT)
    {
        Token *tag_tok = parser_peek(p);
        parser_advance(p);
        tag = tag_tok->payload.str;

        Type *existing = type_record_lookup(tag);
        if (existing && existing->kind != TYPE_ENUM)
        {
            parser_error(p, "tag '%s' redeclared with a different kind", tag);
            return NULL;
        }
        if (existing && existing->enumm.complete)
        {
            parser_error(p, "redefinition of '%s'", tag);
            return NULL;
        }
    }

    if (parser_peek(p)->kind == TOK_SEMI)
    {
        parser_error(p, "expected '{' after enum tag; enum types cannot be incomplete");
        return NULL;
    }
    if (!parser_expect(p, TOK_LBRACE, "'{'"))
    {
        return NULL;
    }

    Vec *constants = vec_new(p->arena);
    i64 next_value = 0;
    while (parser_peek(p)->kind != TOK_RBRACE)
    {
        Token *name_tok = parser_peek(p);
        if (name_tok->kind != TOK_IDENT)
        {
            parser_error(p, "expected enumerator name");
            return NULL;
        }
        parser_advance(p);
        const char *name = name_tok->payload.str;

        if (strmap_get(p->enum_consts, name))
        {
            parser_error(p, "redefinition of enumerator '%s'", name);
            return NULL;
        }

        i64 value = next_value; /* auto-increment (C11 §6.7.2.2p3) */
        if (parser_peek(p)->kind == TOK_ASSIGN)
        {
            parser_advance(p); /* consume '=' */
            ASTNode *init = parse_assign(p);
            if (!init)
            {
                return NULL;
            }
            if (!fold_constant_expr(p, init, &value))
            {
                parser_error(p, "enumerator value is not an integer constant expression");
                return NULL;
            }
        }

        if (value < INT32_MIN || value > INT32_MAX)
        {
            parser_error(p, "enumerator value out of range (must fit in int)");
            return NULL;
        }

        i64 *slot = arena_alloc(p->arena, sizeof(i64), _Alignof(i64));
        *slot = value;
        strmap_set(p->enum_consts, name, slot);

        EnumConstant *c = arena_alloc(p->arena, sizeof(EnumConstant), _Alignof(EnumConstant));
        c->name = name;
        c->value = value;
        vec_push(constants, c);

        next_value = value + 1;

        TokenKind sep = parser_peek(p)->kind;
        if (sep == TOK_COMMA)
        {
            parser_advance(p);
        }
        else if (sep != TOK_RBRACE)
        {
            parser_error(p, "expected ',' or '}' in enum declaration");
            return NULL;
        }
    }
    if (!parser_expect(p, TOK_RBRACE, "'}'"))
    {
        return NULL;
    }
    if (!parser_expect(p, TOK_SEMI, "';'"))
    {
        return NULL;
    }

    if (tag)
    {
        Type *et = type_enum(tag); /* completes the type registered by `enum tag` */
        et->enumm.complete = true;
    }

    return ast_enum_decl(tag, constants, start->loc, p->arena);
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

    if (!parser_check_not_enumerator(p, name->payload.str))
    {
        return NULL;
    }

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
    ParserCtx p = {tokens, count, 0, arena, strmap_new(arena)};

    Vec *decls = vec_new(arena);
    while (parser_peek(&p)->kind != TOK_EOF)
    {
        TokenKind k = parser_peek(&p)->kind;
        ASTNode *node;
        if (k == TOK_KW_STRUCT || k == TOK_KW_UNION)
        {
            /* A record declaration is `struct Tag { ... }` or `struct Tag;`.
               Anything else starting with `struct` (e.g. `struct Tag f(...)`)
               is a function definition whose return type is that record. */
            bool is_record_decl = false;
            if (p.pos + 1 < p.count && p.tokens[p.pos + 1].kind == TOK_IDENT)
            {
                TokenKind after_tag = (p.pos + 2 < p.count) ? p.tokens[p.pos + 2].kind : TOK_EOF;
                is_record_decl = (after_tag == TOK_LBRACE || after_tag == TOK_SEMI);
            }
            if (is_record_decl)
            {
                node = parse_record_decl(&p, /* is_union */ k == TOK_KW_UNION);
            }
            else
            {
                node = parse_func_def(&p);
            }
        }
        else if (k == TOK_KW_ENUM)
        {
            /* `enum Tag { ... }` (or anonymous `enum { ... }`) is an enum
               declaration; anything else (e.g. `enum E f(...)`) is a function
               definition whose return type is that enum. */
            bool is_enum_decl = false;
            if (p.pos + 1 < p.count && p.tokens[p.pos + 1].kind == TOK_LBRACE)
            {
                is_enum_decl = true; /* anonymous enum */
            }
            else if (p.pos + 1 < p.count && p.tokens[p.pos + 1].kind == TOK_IDENT)
            {
                TokenKind after_tag = (p.pos + 2 < p.count) ? p.tokens[p.pos + 2].kind : TOK_EOF;
                is_enum_decl = (after_tag == TOK_LBRACE || after_tag == TOK_SEMI);
            }
            if (is_enum_decl)
            {
                node = parse_enum_decl(&p);
            }
            else
            {
                node = parse_func_def(&p);
            }
        }
        else
        {
            node = parse_func_def(&p);
        }
        if (!node)
        {
            return NULL;
        }
        vec_push(decls, node);
    }

    return ast_program(decls, tokens[0].loc, arena);
}
