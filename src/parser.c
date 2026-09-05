#include "parser.h"
#include "util/assert.h"
#include "util/hashmap.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct Parser Parser;
struct Parser
{
    Token *tokens;
    u64 count;
    u64 pos;
    Arena *arena;
    StrMap *enum_consts;
    Vec *name_scopes;
};

typedef enum
{
    BIND_TYPEDEF,
    BIND_VAR,
    BIND_FUNC,
} BindingKind;

typedef struct Binding Binding;
struct Binding
{
    BindingKind kind;
    Type *type; /* BIND_TYPEDEF only */
};

typedef struct Declarator
{
    Type *type;
    const char *name;
} Declarator;

typedef struct Specs
{
    Type *type;
    StorageClass storage;
    u32 alignas;
    ASTNode *tag_def;
} Specs;

static Token *peek_token(Parser *p)
{
    if (p->pos < p->count)
    {
        return &p->tokens[p->pos];
    }
    else
    {
        return &p->tokens[p->count - 1];
    }
}

static Token *next_token(Parser *p)
{
    Token *t = peek_token(p);
    if (p->pos < p->count - 1)
    {
        p->pos++;
    }
    return t;
}

static void parse_error(Parser *p, const char *fmt, ...)
{
    Token *t = peek_token(p);
    fprintf(stderr, "%s:%u:%u: [parse] error: ", t->loc.file, t->loc.line, t->loc.col);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

static bool expect_token(Parser *p, TokenKind kind, const char *what)
{
    Token *t = peek_token(p);
    if (t->kind != kind)
    {
        parse_error(p, "expected %s, got %s", what, token_kind_name(t->kind));
        return false;
    }
    next_token(p);
    return true;
}

static StrMap *current_scope(Parser *p)
{
    return (StrMap *) vec_last(p->name_scopes);
}

static void push_scope(Parser *p)
{
    vec_push(p->name_scopes, strmap_new(p->arena));
}

static void pop_scope(Parser *p)
{
    (void) vec_pop(p->name_scopes);
}

static Binding *name_lookup(Parser *p, const char *name)
{
    size_t n = vec_size(p->name_scopes);
    for (size_t i = n; i > 0; i--)
    {
        Binding *b = strmap_get((StrMap *) vec_get(p->name_scopes, i - 1), name);
        if (b)
        {
            return b;
        }
    }
    return NULL;
}

static bool declare_name(Parser *p, const char *name, BindingKind kind, Type *type)
{
    Binding *existing = strmap_get(current_scope(p), name);
    if (existing)
    {
        if (existing->kind == BIND_TYPEDEF && kind == BIND_TYPEDEF)
        {
            if (existing->type != type)
            {
                parse_error(p, "typedef '%s' redefined with a different type", name);
                return false;
            }
            return true;
        }
        if (existing->kind == BIND_TYPEDEF || kind == BIND_TYPEDEF)
        {
            parse_error(p, "'%s' redeclared as a different kind of symbol", name);
            return false;
        }
        return true;
    }
    Binding *b = arena_alloc(p->arena, sizeof(Binding), sizeof(void *));
    b->kind = kind;
    b->type = type;
    strmap_set(current_scope(p), name, b);
    return true;
}

static bool check_not_enumerator(Parser *p, const char *name)
{
    if (strmap_get(p->enum_consts, name))
    {
        parse_error(p, "redeclaration of enumerator '%s'", name);
        return false;
    }
    return true;
}

static bool is_type_start(TokenKind k)
{
    return k == TOK_KW_INT || k == TOK_KW_BOOL || k == TOK_KW_CHAR || k == TOK_KW_SHORT ||
           k == TOK_KW_LONG || k == TOK_KW_UNSIGNED || k == TOK_KW_SIGNED || k == TOK_KW_VOID ||
           k == TOK_KW_STRUCT || k == TOK_KW_UNION || k == TOK_KW_ENUM;
}

static bool is_typename_start_at(Parser *p, size_t pos)
{
    if (pos >= p->count)
    {
        return false;
    }
    Token *t = &p->tokens[pos];
    if (is_type_start(t->kind))
    {
        return true;
    }
    if (t->kind == TOK_IDENT)
    {
        Binding *b = name_lookup(p, t->payload.str);
        return b && b->kind == BIND_TYPEDEF;
    }
    return false;
}

static size_t skip_consts(Parser *p, size_t pos)
{
    while (pos < p->count && p->tokens[pos].kind == TOK_KW_CONST)
    {
        pos++;
    }
    return pos;
}

static bool is_typename_start(Parser *p)
{
    return is_typename_start_at(p, p->pos);
}

static bool paren_is_typename(Parser *p)
{
    return peek_token(p)->kind == TOK_LPAREN && is_typename_start_at(p, skip_consts(p, p->pos + 1));
}

static Type *parse_type_specifier(Parser *p);
static Type *parse_abstract_declarator(Parser *p, Type *base);
static Type *parse_array_suffix(Parser *p, Type *type);
static Vec *parse_param_list(Parser *p, bool *out_variadic);
static Specs parse_decl_specifiers(Parser *p);
static ASTNode *parse_expression(Parser *p);
static ASTNode *parse_assign(Parser *p);
static ASTNode *parse_initializer(Parser *p);
static ASTNode *parse_postfix(Parser *p);
static ASTNode *parse_postfix_ops(Parser *p, ASTNode *node);
static ASTNode *parse_init_list(Parser *p);
static Type *parse_paren_type_name(Parser *p);
static ASTNode *parse_compound_stmt(Parser *p);
static ASTNode *parse_static_assert(Parser *p);
static ASTNode *parse_var_decl(Parser *p, Specs s);
static ASTNode *parse_stmt(Parser *p);
static bool folded_const(Parser *p, ASTNode *node, i64 *out);
static bool resolve_constant_init(Parser *p, ASTVarDecl *vd, ASTNode *expr);
static bool parse_declarator_core(Parser *p, Type *base, Declarator *out, bool name_optional);
static bool parse_declarator(Parser *p, Type *base, Declarator *out);

static ASTNode *parse_member_decl(Parser *p, Type *base, u32 alignas, Token *start)
{
    if (peek_token(p)->kind == TOK_SEMI)
    {
        parse_error(p, "member declaration must declare a member");
        return NULL;
    }

    Vec *decls = vec_new(p->arena);
    while (true)
    {
        Declarator d;
        if (!parse_declarator(p, base, &d))
        {
            return NULL;
        }
        if (!check_not_enumerator(p, d.name))
        {
            return NULL;
        }
        ASTNode *decl = ast_var_decl(d.type, d.name, NULL, SC_NONE, start->loc, p->arena);
        ast_as(ASTVarDecl, decl)->alignas = alignas;
        vec_push(decls, decl);
        if (peek_token(p)->kind != TOK_COMMA)
        {
            break;
        }
        next_token(p);
    }
    if (!expect_token(p, TOK_SEMI, "';'"))
    {
        return NULL;
    }
    if (vec_size(decls) == 1)
    {
        return (ASTNode *) vec_get(decls, 0);
    }
    return ast_decl_list(decls, start->loc, p->arena);
}

static void collect_member_fields(Arena *arena, Vec *record_fields, ASTNode *member)
{
    Vec *list = member->kind == AST_DECL_LIST ? ast_as(ASTDeclList, member)->decls : NULL;
    size_t n = list ? vec_size(list) : 1;
    for (size_t i = 0; i < n; i++)
    {
        ASTNode *node = list ? (ASTNode *) vec_get(list, i) : member;
        ASTVarDecl *vd = ast_as(ASTVarDecl, node);
        RecordField *rf = arena_alloc(arena, sizeof(RecordField), _Alignof(RecordField));
        rf->name = vd->name;
        rf->type = vd->type;
        rf->offset = 0;
        vec_push(record_fields, rf);
    }
}

static Vec *parse_record_body(Parser *p, Type *rec)
{
    if (!expect_token(p, TOK_LBRACE, "'{'"))
    {
        return NULL;
    }

    Vec *field_decls = vec_new(p->arena);
    Vec *record_fields = vec_new(p->arena);
    while (peek_token(p)->kind != TOK_RBRACE)
    {
        Token *mstart = peek_token(p);
        Specs mspecs = parse_decl_specifiers(p);
        if (!mspecs.type)
        {
            return NULL;
        }

        ASTNode *member = parse_member_decl(p, mspecs.type, mspecs.alignas, mstart);
        if (!member)
        {
            return NULL;
        }
        vec_push(field_decls, member);
        collect_member_fields(p->arena, record_fields, member);
    }
    if (!expect_token(p, TOK_RBRACE, "'}'"))
    {
        return NULL;
    }
    type_record_complete(rec, record_fields);
    return field_decls;
}

static bool parse_enumerator_body(Parser *p, Vec *constants, i64 *next_value)
{
    if (!expect_token(p, TOK_LBRACE, "'{'"))
    {
        return false;
    }

    while (peek_token(p)->kind != TOK_RBRACE)
    {
        Token *name_tok = peek_token(p);
        if (name_tok->kind != TOK_IDENT)
        {
            parse_error(p, "expected enumerator name");
            return false;
        }
        next_token(p);
        const char *name = name_tok->payload.str;

        if (strmap_get(p->enum_consts, name))
        {
            parse_error(p, "redefinition of enumerator '%s'", name);
            return false;
        }

        i64 value = *next_value;
        if (peek_token(p)->kind == TOK_ASSIGN)
        {
            next_token(p);
            ASTNode *init = parse_assign(p);
            if (!init)
            {
                return false;
            }
            if (!folded_const(p, init, &value))
            {
                parse_error(p, "enumerator value is not an integer constant expression");
                return false;
            }
        }

        if (value < INT32_MIN || value > INT32_MAX)
        {
            parse_error(p, "enumerator value out of range (must fit in int)");
            return false;
        }

        i64 *slot = arena_alloc(p->arena, sizeof(i64), _Alignof(i64));
        *slot = value;
        strmap_set(p->enum_consts, name, slot);

        EnumConstant *c = arena_alloc(p->arena, sizeof(EnumConstant), _Alignof(EnumConstant));
        c->name = name;
        c->value = value;
        vec_push(constants, c);

        *next_value = value + 1;

        TokenKind sep = peek_token(p)->kind;
        if (sep == TOK_COMMA)
        {
            next_token(p);
        }
        else if (sep != TOK_RBRACE)
        {
            parse_error(p, "expected ',' or '}' in enum declaration");
            return false;
        }
    }
    return expect_token(p, TOK_RBRACE, "'}'");
}

static bool parse_tag_prefix(Parser *p, TypeKind kind, const char **tag, Type **out)
{
    Token *t = peek_token(p);
    if (t->kind != TOK_IDENT)
    {
        return true;
    }
    next_token(p);

    Type *existing = type_record_lookup(t->payload.str);
    if (existing && existing->kind != kind)
    {
        parse_error(p, "tag '%s' redeclared with a different kind", t->payload.str);
        return false;
    }

    *tag = t->payload.str;
    *out = kind == TYPE_ENUM ? type_enum(t->payload.str) : type_record(kind, t->payload.str);
    return true;
}

static Type *parse_record_specifier(Parser *p, bool is_union, ASTNode **tag_def)
{
    Token *kw = peek_token(p);
    next_token(p);
    TypeKind kind = is_union ? TYPE_UNION : TYPE_STRUCT;

    const char *tag = NULL;
    Type *ty = NULL;
    if (!parse_tag_prefix(p, kind, &tag, &ty))
    {
        return NULL;
    }

    if (peek_token(p)->kind == TOK_LBRACE)
    {
        if (ty && ty->record.complete)
        {
            parse_error(p, "redefinition of '%s'", tag);
            return NULL;
        }
        if (!ty)
        {
            ty = type_record_anon(kind);
        }
        Vec *fields = parse_record_body(p, ty);
        if (!fields)
        {
            return NULL;
        }
        if (tag)
        {
            *tag_def = ast_struct_decl(tag, is_union, fields, kw->loc, p->arena);
        }
        return ty;
    }

    if (tag)
    {
        *tag_def = ast_struct_decl(tag, is_union, vec_new(p->arena), kw->loc, p->arena);
        return ty;
    }
    parse_error(p, "expected tag name or '{' after '%s'", is_union ? "union" : "struct");
    return NULL;
}

static Type *parse_enum_specifier(Parser *p, ASTNode **tag_def)
{
    Token *kw = peek_token(p);
    next_token(p);

    const char *tag = NULL;
    Type *ty = NULL;
    if (!parse_tag_prefix(p, TYPE_ENUM, &tag, &ty))
    {
        return NULL;
    }

    if (peek_token(p)->kind == TOK_LBRACE)
    {
        if (ty && ty->enumm.complete)
        {
            parse_error(p, "redefinition of '%s'", tag);
            return NULL;
        }
        Vec *constants = vec_new(p->arena);
        i64 next_value = 0;
        if (!parse_enumerator_body(p, constants, &next_value))
        {
            return NULL;
        }
        if (!ty)
        {
            ty = type_enum_anon();
        }
        else
        {
            ty->enumm.complete = true;
        }
        *tag_def = ast_enum_decl(tag, constants, kw->loc, p->arena);
        return ty;
    }

    if (tag)
    {
        return ty;
    }
    parse_error(p, "expected '{' after enum tag; enum types cannot be incomplete");
    return NULL;
}

static Type *parse_integer_specifiers(Parser *p)
{
    int n_signed = 0;
    int n_unsigned = 0;
    int n_char = 0;
    int n_short = 0;
    int n_int = 0;
    int n_long = 0;

    Token *t = peek_token(p);
    while (t->kind == TOK_KW_SIGNED || t->kind == TOK_KW_UNSIGNED || t->kind == TOK_KW_CHAR ||
           t->kind == TOK_KW_SHORT || t->kind == TOK_KW_INT || t->kind == TOK_KW_LONG)
    {
        switch (t->kind)
        {
            case TOK_KW_SIGNED:
                n_signed++;
                break;
            case TOK_KW_UNSIGNED:
                n_unsigned++;
                break;
            case TOK_KW_CHAR:
                n_char++;
                break;
            case TOK_KW_SHORT:
                n_short++;
                break;
            case TOK_KW_INT:
                n_int++;
                break;
            case TOK_KW_LONG:
                n_long++;
                break;
            default:
                break;
        }
        next_token(p);
        t = peek_token(p);
    }

    if (n_signed && n_unsigned)
    {
        parse_error(p, "cannot combine 'signed' and 'unsigned'");
        return NULL;
    }
    if (n_signed > 1 || n_unsigned > 1 || n_int > 1)
    {
        parse_error(p, "duplicate type specifier");
        return NULL;
    }
    if (n_char && (n_short || n_int || n_long))
    {
        parse_error(p, "cannot combine 'char' with short/int/long");
        return NULL;
    }
    if (n_short && n_long)
    {
        parse_error(p, "cannot combine 'short' and 'long'");
        return NULL;
    }
    if (n_long > 2)
    {
        parse_error(p, "too many 'long' type specifiers");
        return NULL;
    }

    if (n_char)
    {
        return n_unsigned ? type_uchar() : type_char();
    }
    if (n_short)
    {
        return n_unsigned ? type_ushort() : type_short();
    }
    if (n_long)
    {
        if (n_unsigned)
        {
            return n_long >= 2 ? type_ullong() : type_ulong();
        }
        return n_long >= 2 ? type_llong() : type_long();
    }
    return n_unsigned ? type_uint() : type_int();
}

static u32 parse_alignas_specifier(Parser *p)
{
    next_token(p);
    if (!expect_token(p, TOK_LPAREN, "'('"))
    {
        return 0;
    }

    i64 align = 0;
    bool ok;
    if (is_typename_start_at(p, skip_consts(p, p->pos)))
    {
        Type *ty = parse_abstract_declarator(p, parse_type_specifier(p));
        if (!ty)
        {
            return 0;
        }
        align = (i64) type_alignof(ty);
        ok = true;
    }
    else
    {
        ASTNode *expr = parse_assign(p);
        if (!expr)
        {
            return 0;
        }
        ok = folded_const(p, expr, &align);
    }
    if (!ok)
    {
        parse_error(p, "alignment is not a constant expression");
        return 0;
    }
    if (!expect_token(p, TOK_RPAREN, "')'"))
    {
        return 0;
    }

    if (align <= 0 || (align & (align - 1)) != 0)
    {
        parse_error(p, "invalid alignment (must be a nonzero power of two)");
        return 0;
    }
    return (u32) align;
}

static bool parse_qualifiers(Parser *p, bool *has_const, u32 *alignas)
{
    bool saw_const = false;
    for (;;)
    {
        Token *t = peek_token(p);
        if (t->kind == TOK_KW_CONST)
        {
            next_token(p);
            saw_const = true;
        }
        else if (t->kind == TOK_KW_ALIGNAS)
        {
            u32 a = parse_alignas_specifier(p);
            if (a == 0)
            {
                return false;
            }
            if (a > *alignas)
            {
                *alignas = a;
            }
        }
        else
        {
            break;
        }
    }
    *has_const = saw_const;
    return true;
}

static Specs parse_decl_specifiers(Parser *p)
{
    Specs s = {0};
    bool lead_const = false;
    if (!parse_qualifiers(p, &lead_const, &s.alignas))
    {
        return s;
    }

    Token *t = peek_token(p);
    switch (t->kind)
    {
        case TOK_KW_CHAR:
        case TOK_KW_SHORT:
        case TOK_KW_LONG:
        case TOK_KW_SIGNED:
        case TOK_KW_UNSIGNED:
        case TOK_KW_INT:
            s.type = parse_integer_specifiers(p);
            break;
        case TOK_KW_BOOL:
            next_token(p);
            s.type = type_cbool();
            break;
        case TOK_KW_VOID:
            next_token(p);
            s.type = type_void();
            break;
        case TOK_KW_STRUCT:
        case TOK_KW_UNION:
            s.type = parse_record_specifier(p, t->kind == TOK_KW_UNION, &s.tag_def);
            break;
        case TOK_KW_ENUM:
            s.type = parse_enum_specifier(p, &s.tag_def);
            break;
        case TOK_IDENT:
        {
            Binding *b = name_lookup(p, t->payload.str);
            if (b && b->kind == BIND_TYPEDEF)
            {
                next_token(p);
                s.type = b->type;
                break;
            }
            parse_error(p, "expected type specifier");
            return s;
        }
        default:
            parse_error(p, "expected type specifier");
            return s;
    }
    if (!s.type)
    {
        return s;
    }

    if (lead_const)
    {
        s.type = type_const(s.type);
    }
    bool trail_const = false;
    if (!parse_qualifiers(p, &trail_const, &s.alignas))
    {
        return s;
    }
    if (trail_const)
    {
        s.type = type_const(s.type);
    }
    return s;
}

static Type *parse_type_specifier(Parser *p)
{
    return parse_decl_specifiers(p).type;
}

static Type *ptr_layers(Type *t, u32 n)
{
    for (u32 i = 0; i < n; i++)
    {
        t = type_ptr(t);
    }
    return t;
}

static u32 strip_ptrs(Type *t, Type **innermost)
{
    u32 n = 0;
    while (t->kind == TYPE_PTR)
    {
        n++;
        t = t->ptr.pointee;
    }
    *innermost = t;
    return n;
}

static Type *parse_group_suffixes(Parser *p, Type *t, u32 *nptr)
{
    for (;;)
    {
        TokenKind k = peek_token(p)->kind;
        if (k == TOK_LBRACKET)
        {
            t = parse_array_suffix(p, ptr_layers(t, *nptr));
            if (!t)
            {
                return NULL;
            }
            *nptr = 0;
        }
        else if (k == TOK_LPAREN)
        {
            next_token(p);
            bool variadic = false;
            Vec *params = parse_param_list(p, &variadic);
            if (!params)
            {
                return NULL;
            }
            if (!expect_token(p, TOK_RPAREN, "')'"))
            {
                return NULL;
            }
            Vec *param_types = vec_new(p->arena);
            for (size_t i = 0; i < vec_size(params); i++)
            {
                ASTVarDecl *pd = ast_as(ASTVarDecl, (ASTNode *) vec_get(params, i));
                vec_push(param_types, type_unqual(pd->type));
            }
            t = type_func(ptr_layers(t, *nptr), param_types, variadic);
            *nptr = 0;
        }
        else
        {
            return t;
        }
    }
}

static bool parse_declarator_group(Parser *p, Type *base, u32 nptr, Declarator *out,
                                   bool name_optional)
{
    Declarator inner;
    if (!parse_declarator_core(p, base, &inner, name_optional))
    {
        return false;
    }
    if (!expect_token(p, TOK_RPAREN, "')'"))
    {
        return false;
    }

    Type *core;
    u32 inner_ptrs = strip_ptrs(inner.type, &core);

    Type *suffix = parse_group_suffixes(p, core, &nptr);
    if (!suffix)
    {
        return false;
    }
    out->type = ptr_layers(ptr_layers(suffix, nptr), inner_ptrs);
    out->name = inner.name;
    return true;
}

static Type *parse_abstract_declarator(Parser *p, Type *base)
{
    if (peek_token(p)->kind == TOK_LPAREN)
    {
        next_token(p);
        Declarator d;
        if (!parse_declarator_group(p, base, 0, &d, true))
        {
            return NULL;
        }
        return d.type;
    }
    while (peek_token(p)->kind == TOK_STAR)
    {
        next_token(p);
        base = type_ptr(base);
        while (peek_token(p)->kind == TOK_KW_CONST)
        {
            next_token(p);
            base = type_const(base);
        }
    }
    return base;
}

static bool parse_declarator_core(Parser *p, Type *base, Declarator *out, bool name_optional)
{
    out->type = base;
    out->name = NULL;

    u32 nptr = 0;
    u32 nconst = 0;
    while (peek_token(p)->kind == TOK_STAR)
    {
        next_token(p);
        nptr++;
        while (peek_token(p)->kind == TOK_KW_CONST)
        {
            next_token(p);
            nconst++;
        }
    }

    if (peek_token(p)->kind == TOK_LPAREN)
    {
        next_token(p);
        return parse_declarator_group(p, base, nptr, out, true);
    }

    if (peek_token(p)->kind == TOK_IDENT)
    {
        out->name = peek_token(p)->payload.str;
        next_token(p);
    }
    else if (!name_optional)
    {
        parse_error(p, "expected declarator name");
        return false;
    }

    if (peek_token(p)->kind == TOK_LBRACKET)
    {
        out->type = parse_array_suffix(p, ptr_layers(out->type, nptr));
        if (!out->type)
        {
            return false;
        }
        nptr = 0;
    }
    out->type = ptr_layers(out->type, nptr);
    for (u32 i = 0; i < nconst; i++)
    {
        out->type = type_const(out->type);
    }
    return true;
}

static bool parse_declarator(Parser *p, Type *base, Declarator *out)
{
    return parse_declarator_core(p, base, out, false);
}

static ASTNode *parse_param(Parser *p)
{
    Token *start = peek_token(p);
    Specs s = parse_decl_specifiers(p);
    if (!s.type)
    {
        return NULL;
    }

    Declarator d;
    if (!parse_declarator_core(p, s.type, &d, true))
    {
        return NULL;
    }

    if (d.name)
    {
        if (!check_not_enumerator(p, d.name))
        {
            return NULL;
        }
        if (!declare_name(p, d.name, BIND_VAR, NULL))
        {
            return NULL;
        }
    }

    ASTNode *decl = ast_var_decl(type_decay(d.type), d.name, NULL, SC_NONE, start->loc, p->arena);
    ast_as(ASTVarDecl, decl)->alignas = s.alignas;
    return decl;
}

static Vec *parse_param_list(Parser *p, bool *out_variadic)
{
    if (out_variadic)
    {
        *out_variadic = false;
    }
    Vec *params = vec_new(p->arena);
    Token *t = peek_token(p);

    if (t->kind == TOK_KW_VOID && p->pos + 1 < p->count && p->tokens[p->pos + 1].kind == TOK_RPAREN)
    {
        next_token(p);
        return params;
    }
    if (t->kind == TOK_RPAREN)
    {
        return params;
    }
    if (t->kind == TOK_ELLIPSIS)
    {
        parse_error(p, "'...' must follow at least one parameter");
        return NULL;
    }

    ASTNode *first = parse_param(p);
    if (!first)
    {
        return NULL;
    }
    vec_push(params, first);

    while (peek_token(p)->kind == TOK_COMMA)
    {
        next_token(p);
        if (peek_token(p)->kind == TOK_ELLIPSIS)
        {
            if (!(p->pos + 1 < p->count && p->tokens[p->pos + 1].kind == TOK_RPAREN))
            {
                parse_error(p, "expected ')' after '...'");
                return NULL;
            }
            next_token(p);
            if (out_variadic)
            {
                *out_variadic = true;
            }
            return params;
        }
        ASTNode *next = parse_param(p);
        if (!next)
        {
            return NULL;
        }
        vec_push(params, next);
    }

    return params;
}

static Type *parse_array_suffix(Parser *p, Type *type)
{
    enum
    {
        MAX_ARRAY_DIM = 32,
    };
    u64 dims[MAX_ARRAY_DIM];
    size_t ndim = 0;
    while (peek_token(p)->kind == TOK_LBRACKET)
    {
        next_token(p);
        u64 len = 0;
        if (peek_token(p)->kind != TOK_RBRACKET)
        {
            ASTNode *size_expr = parse_assign(p);
            if (!size_expr || size_expr->kind != AST_INT_LITERAL)
            {
                parse_error(p, "array size must be an integer constant");
                return NULL;
            }
            len = (u64) ast_as(ASTIntLiteral, size_expr)->value;
        }
        expect_token(p, TOK_RBRACKET, "]");
        if (ndim == MAX_ARRAY_DIM)
        {
            parse_error(p, "too many array dimensions");
            return NULL;
        }
        dims[ndim++] = len;
    }
    for (size_t i = ndim; i > 0; i--)
    {
        type = type_array(type, dims[i - 1]);
    }
    return type;
}

typedef struct StoragePrefix
{
    StorageClass storage;
    u32 lead_consts; /* consts consumed ahead of the storage class */
} StoragePrefix;

static StoragePrefix parse_storage_prefix(Parser *p)
{
    StoragePrefix pre = {SC_NONE, 0};
    Token *t = peek_token(p);
    if (t->kind != TOK_KW_CONST && t->kind != TOK_KW_STATIC && t->kind != TOK_KW_EXTERN)
    {
        return pre;
    }

    u32 nconst = 0;
    while (p->pos + nconst < p->count && p->tokens[p->pos + nconst].kind == TOK_KW_CONST)
    {
        nconst++;
    }
    TokenKind nxt = (p->pos + nconst < p->count) ? p->tokens[p->pos + nconst].kind : TOK_EOF;
    if (nxt != TOK_KW_STATIC && nxt != TOK_KW_EXTERN)
    {
        return pre;
    }
    for (u32 i = 0; i < nconst; i++)
    {
        next_token(p);
    }
    next_token(p);
    pre.storage = nxt == TOK_KW_STATIC ? SC_STATIC : SC_EXTERN;
    pre.lead_consts = nconst;
    return pre;
}

static Specs parse_specs_storage(Parser *p, bool storage_ok)
{
    u32 lead = 0;
    StorageClass storage = SC_NONE;
    if (storage_ok)
    {
        StoragePrefix pre = parse_storage_prefix(p);
        storage = pre.storage;
        lead = pre.lead_consts;
    }

    Specs s = parse_decl_specifiers(p);
    for (u32 i = 0; i < lead; i++)
    {
        s.type = type_const(s.type);
    }
    s.storage = storage;
    return s;
}

static ASTNode *parse_typedef_decl(Parser *p)
{
    Token *start = peek_token(p);
    next_token(p);

    Specs s = parse_decl_specifiers(p);
    if (!s.type)
    {
        return NULL;
    }
    if (s.alignas)
    {
        parse_error(p, "_Alignas is not permitted in a typedef");
        return NULL;
    }

    Declarator d;
    if (!parse_declarator(p, s.type, &d))
    {
        return NULL;
    }

    if (!check_not_enumerator(p, d.name))
    {
        return NULL;
    }
    if (!declare_name(p, d.name, BIND_TYPEDEF, d.type))
    {
        return NULL;
    }
    if (!expect_token(p, TOK_SEMI, "';'"))
    {
        return NULL;
    }
    return ast_typedef_decl(d.type, d.name, start->loc, p->arena);
}

static bool push_declarator(Parser *p, Specs s, Declarator d, Loc start, Vec *decls,
                            bool file_scope)
{
    if (!check_not_enumerator(p, d.name))
    {
        return false;
    }
    if (!declare_name(p, d.name, BIND_VAR, NULL))
    {
        return false;
    }

    ASTNode *decl = ast_var_decl(d.type, d.name, NULL, s.storage, start, p->arena);
    ASTVarDecl *vd = ast_as(ASTVarDecl, decl);
    vd->alignas = s.alignas;
    if (peek_token(p)->kind == TOK_ASSIGN)
    {
        next_token(p);
        ASTNode *expr = parse_initializer(p);
        if (!expr)
        {
            return false;
        }
        if (file_scope || s.storage == SC_STATIC)
        {
            const char *what = file_scope ? "file-scope" : "static";
            if (!resolve_constant_init(p, vd, expr))
            {
                parse_error(p, "initializer for %s variable must be a constant expression", what);
                return false;
            }
        }
        else
        {
            vd->init = expr;
        }
    }
    vec_push(decls, decl);
    return true;
}

static ASTNode *parse_var_decl(Parser *p, Specs s)
{
    Token *start = peek_token(p);
    if (peek_token(p)->kind == TOK_SEMI)
    {
        next_token(p);
        if (s.tag_def)
        {
            return s.tag_def;
        }
        parse_error(p, "declaration declares nothing");
        return NULL;
    }

    Vec *decls = vec_new(p->arena);
    Declarator d;
    while (true)
    {
        if (!parse_declarator(p, s.type, &d))
        {
            return NULL;
        }
        if (!push_declarator(p, s, d, start->loc, decls, false))
        {
            return NULL;
        }
        if (peek_token(p)->kind != TOK_COMMA)
        {
            break;
        }
        next_token(p);
    }
    if (!expect_token(p, TOK_SEMI, "';'"))
    {
        return NULL;
    }
    if (vec_size(decls) == 1)
    {
        return (ASTNode *) vec_get(decls, 0);
    }
    return ast_decl_list(decls, start->loc, p->arena);
}

static ASTNode *parse_file_vars(Parser *p, Specs s, Declarator d0, Loc start)
{
    Vec *decls = vec_new(p->arena);
    Declarator d = d0;
    while (true)
    {
        if (!push_declarator(p, s, d, start, decls, true))
        {
            return NULL;
        }
        if (peek_token(p)->kind != TOK_COMMA)
        {
            break;
        }
        next_token(p);
        if (!parse_declarator(p, s.type, &d))
        {
            return NULL;
        }
        if (peek_token(p)->kind == TOK_LPAREN)
        {
            parse_error(p, "a function definition must be the only declarator in its "
                           "declaration");
            return NULL;
        }
    }
    if (!expect_token(p, TOK_SEMI, "';'"))
    {
        return NULL;
    }
    if (vec_size(decls) == 1)
    {
        return (ASTNode *) vec_get(decls, 0);
    }
    return ast_decl_list(decls, start, p->arena);
}

static ASTNode *parse_function(Parser *p, Specs s, Declarator d, Loc start)
{
    if (s.alignas)
    {
        parse_error(p, "_Alignas is not permitted on a function");
        return NULL;
    }
    StorageClass storage = s.storage == SC_STATIC ? SC_STATIC : SC_NONE;
    if (!check_not_enumerator(p, d.name))
    {
        return NULL;
    }
    if (!declare_name(p, d.name, BIND_FUNC, NULL))
    {
        return NULL;
    }

    next_token(p); /* '(' */
    push_scope(p);
    bool variadic = false;
    Vec *params = parse_param_list(p, &variadic);
    if (!params)
    {
        return NULL;
    }
    if (!expect_token(p, TOK_RPAREN, "')'"))
    {
        return NULL;
    }

    if (peek_token(p)->kind == TOK_SEMI)
    {
        next_token(p);
        pop_scope(p);
        return ast_func_decl(d.type, d.name, params, storage, variadic, start, p->arena);
    }

    for (size_t i = 0; i < vec_size(params); i++)
    {
        ASTVarDecl *param = ast_as(ASTVarDecl, (ASTNode *) vec_get(params, i));
        if (!param->name)
        {
            parse_error(p, "parameter %zu in definition of '%s' must have a name", i + 1, d.name);
            return NULL;
        }
    }

    ASTNode *body = parse_compound_stmt(p);
    if (!body)
    {
        return NULL;
    }
    pop_scope(p);
    return ast_func_def(d.type, d.name, params, body, storage, variadic, start, p->arena);
}

static ASTNode *parse_toplevel_decl(Parser *p)
{
    Token *start = peek_token(p);
    if (start->kind == TOK_KW_TYPEDEF)
    {
        return parse_typedef_decl(p);
    }
    if (start->kind == TOK_KW_STATIC_ASSERT)
    {
        return parse_static_assert(p);
    }

    Specs s = parse_specs_storage(p, true);
    if (!s.type)
    {
        return NULL;
    }

    if (peek_token(p)->kind == TOK_SEMI)
    {
        next_token(p);
        if (s.tag_def)
        {
            return s.tag_def;
        }
        parse_error(p, "declaration declares nothing");
        return NULL;
    }

    Declarator d;
    if (!parse_declarator(p, s.type, &d))
    {
        return NULL;
    }
    if (peek_token(p)->kind == TOK_LPAREN)
    {
        return parse_function(p, s, d, start->loc);
    }
    return parse_file_vars(p, s, d, start->loc);
}

static ASTNode *parse_compound_stmt(Parser *p)
{
    Token *start = peek_token(p);
    if (!expect_token(p, TOK_LBRACE, "'{'"))
    {
        return NULL;
    }
    push_scope(p);

    Vec *stmts = vec_new(p->arena);
    while (peek_token(p)->kind != TOK_RBRACE)
    {
        ASTNode *stmt = parse_stmt(p);
        if (!stmt)
        {
            return NULL;
        }
        vec_push(stmts, stmt);
    }
    if (!expect_token(p, TOK_RBRACE, "'}'"))
    {
        return NULL;
    }
    pop_scope(p);
    return ast_compound_stmt(stmts, start->loc, p->arena);
}

static ASTNode *parse_return_stmt(Parser *p)
{
    Token *start = peek_token(p);
    next_token(p);

    ASTNode *expr = NULL;
    if (peek_token(p)->kind != TOK_SEMI)
    {
        expr = parse_expression(p);
        if (!expr)
        {
            return NULL;
        }
    }
    if (!expect_token(p, TOK_SEMI, "';'"))
    {
        return NULL;
    }
    return ast_return_stmt(expr, start->loc, p->arena);
}

static ASTNode *parse_expr_stmt(Parser *p)
{
    Token *start = peek_token(p);
    ASTNode *expr = parse_expression(p);
    if (!expr)
    {
        return NULL;
    }
    if (!expect_token(p, TOK_SEMI, "';'"))
    {
        return NULL;
    }
    return ast_expr_stmt(expr, start->loc, p->arena);
}

static ASTNode *parse_condition(Parser *p)
{
    if (!expect_token(p, TOK_LPAREN, "'('"))
    {
        return NULL;
    }
    ASTNode *cond = parse_expression(p);
    if (!cond)
    {
        return NULL;
    }
    if (!expect_token(p, TOK_RPAREN, "')'"))
    {
        return NULL;
    }
    return cond;
}

static ASTNode *parse_if_stmt(Parser *p)
{
    Token *start = peek_token(p);
    next_token(p);
    ASTNode *cond = parse_condition(p);
    if (!cond)
    {
        return NULL;
    }
    ASTNode *then = parse_stmt(p);
    if (!then)
    {
        return NULL;
    }
    ASTNode *els = NULL;
    if (peek_token(p)->kind == TOK_KW_ELSE)
    {
        next_token(p);
        els = parse_stmt(p);
        if (!els)
        {
            return NULL;
        }
    }
    return ast_if_stmt(cond, then, els, start->loc, p->arena);
}

static ASTNode *parse_while_stmt(Parser *p)
{
    Token *start = peek_token(p);
    next_token(p);
    ASTNode *cond = parse_condition(p);
    if (!cond)
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

static ASTNode *parse_do_stmt(Parser *p)
{
    Token *start = peek_token(p);
    next_token(p);
    ASTNode *body = parse_stmt(p);
    if (!body)
    {
        return NULL;
    }
    if (!expect_token(p, TOK_KW_WHILE, "'while'"))
    {
        return NULL;
    }
    ASTNode *cond = parse_condition(p);
    if (!cond)
    {
        return NULL;
    }
    if (!expect_token(p, TOK_SEMI, "';'"))
    {
        return NULL;
    }
    return ast_do_while_stmt(cond, body, start->loc, p->arena);
}

static ASTNode *parse_for_stmt(Parser *p)
{
    Token *start = peek_token(p);
    next_token(p);
    if (!expect_token(p, TOK_LPAREN, "'('"))
    {
        return NULL;
    }

    ASTNode *init = NULL;
    if (peek_token(p)->kind != TOK_SEMI)
    {
        if (is_typename_start(p) || peek_token(p)->kind == TOK_KW_CONST)
        {
            Specs s = parse_specs_storage(p, false);
            if (!s.type)
            {
                return NULL;
            }
            init = parse_var_decl(p, s);
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
        next_token(p);
    }

    ASTNode *cond = NULL;
    if (peek_token(p)->kind != TOK_SEMI)
    {
        cond = parse_expression(p);
        if (!cond)
        {
            return NULL;
        }
    }
    if (!expect_token(p, TOK_SEMI, "';'"))
    {
        return NULL;
    }

    ASTNode *post = NULL;
    if (peek_token(p)->kind != TOK_RPAREN)
    {
        post = parse_expression(p);
        if (!post)
        {
            return NULL;
        }
    }
    if (!expect_token(p, TOK_RPAREN, "')'"))
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

static ASTNode *parse_break_stmt(Parser *p)
{
    Token *start = peek_token(p);
    next_token(p);
    if (!expect_token(p, TOK_SEMI, "';'"))
    {
        return NULL;
    }
    return ast_break_stmt(start->loc, p->arena);
}

static ASTNode *parse_continue_stmt(Parser *p)
{
    Token *start = peek_token(p);
    next_token(p);
    if (!expect_token(p, TOK_SEMI, "';'"))
    {
        return NULL;
    }
    return ast_continue_stmt(start->loc, p->arena);
}

static ASTNode *parse_goto_stmt(Parser *p)
{
    Token *start = peek_token(p);
    next_token(p);
    Token *label = peek_token(p);
    if (label->kind != TOK_IDENT)
    {
        parse_error(p, "expected label name");
        return NULL;
    }
    next_token(p);
    if (!expect_token(p, TOK_SEMI, "';'"))
    {
        return NULL;
    }
    return ast_goto_stmt(label->payload.str, start->loc, p->arena);
}

static ASTNode *parse_label_stmt(Parser *p)
{
    Token *start = peek_token(p);
    const char *label = start->payload.str;
    next_token(p); /* label */
    next_token(p); /* ':' */
    ASTNode *stmt = parse_stmt(p);
    if (!stmt)
    {
        return NULL;
    }
    return ast_label_stmt(label, stmt, start->loc, p->arena);
}

static ASTNode *parse_switch_stmt(Parser *p)
{
    Token *start = peek_token(p);
    next_token(p);
    ASTNode *cond = parse_condition(p);
    if (!cond)
    {
        return NULL;
    }
    ASTNode *body = parse_stmt(p);
    if (!body)
    {
        return NULL;
    }
    return ast_switch_stmt(cond, body, start->loc, p->arena);
}

static ASTNode *parse_case_stmt(Parser *p)
{
    Token *start = peek_token(p);
    next_token(p);

    ASTNode *expr = parse_assign(p);
    if (!expr)
    {
        return NULL;
    }
    i64 value = 0;
    bool value_known = folded_const(p, expr, &value);
    if (!expect_token(p, TOK_COLON, "':'"))
    {
        return NULL;
    }
    ASTNode *stmt = parse_stmt(p);
    if (!stmt)
    {
        return NULL;
    }

    Vec *stmts = vec_new(p->arena);
    vec_push(stmts, stmt);
    return ast_case_stmt(expr, value, value_known, stmts, start->loc, p->arena);
}

static ASTNode *parse_default_stmt(Parser *p)
{
    Token *start = peek_token(p);
    next_token(p);
    if (!expect_token(p, TOK_COLON, "':'"))
    {
        return NULL;
    }
    ASTNode *stmt = parse_stmt(p);
    if (!stmt)
    {
        return NULL;
    }
    Vec *stmts = vec_new(p->arena);
    vec_push(stmts, stmt);
    return ast_default_stmt(stmts, start->loc, p->arena);
}

static ASTNode *parse_static_assert(Parser *p)
{
    Token *start = peek_token(p);
    next_token(p);
    if (!expect_token(p, TOK_LPAREN, "'('"))
    {
        return NULL;
    }
    ASTNode *expr = parse_assign(p);
    if (!expr)
    {
        return NULL;
    }
    if (!expect_token(p, TOK_COMMA, "','"))
    {
        return NULL;
    }
    Token *msg = peek_token(p);
    if (msg->kind != TOK_STRING_LIT)
    {
        parse_error(p, "expected string literal in _Static_assert");
        return NULL;
    }
    next_token(p);
    if (!expect_token(p, TOK_RPAREN, "')'"))
    {
        return NULL;
    }
    if (!expect_token(p, TOK_SEMI, "';'"))
    {
        return NULL;
    }
    return ast_static_assert(expr, msg->payload.str, start->loc, p->arena);
}

typedef ASTNode *(*StmtFn)(Parser *);

static const struct
{
    TokenKind tok;
    StmtFn fn;
} STMT_KEYWORDS[] = {
    {TOK_KW_RETURN, parse_return_stmt},
    {TOK_KW_IF, parse_if_stmt},
    {TOK_KW_WHILE, parse_while_stmt},
    {TOK_KW_FOR, parse_for_stmt},
    {TOK_KW_DO, parse_do_stmt},
    {TOK_KW_BREAK, parse_break_stmt},
    {TOK_KW_CONTINUE, parse_continue_stmt},
    {TOK_KW_GOTO, parse_goto_stmt},
    {TOK_KW_SWITCH, parse_switch_stmt},
    {TOK_KW_CASE, parse_case_stmt},
    {TOK_KW_DEFAULT, parse_default_stmt},
    {TOK_KW_STATIC_ASSERT, parse_static_assert},
};

static ASTNode *parse_stmt(Parser *p)
{
    Token *t = peek_token(p);
    if (is_type_start(t->kind) || t->kind == TOK_KW_ALIGNAS || t->kind == TOK_KW_CONST ||
        t->kind == TOK_KW_STATIC || t->kind == TOK_KW_EXTERN)
    {
        Specs s = parse_specs_storage(p, true);
        if (!s.type)
        {
            return NULL;
        }
        return parse_var_decl(p, s);
    }
    if (t->kind == TOK_LBRACE)
    {
        return parse_compound_stmt(p);
    }
    if (t->kind == TOK_KW_TYPEDEF)
    {
        return parse_typedef_decl(p);
    }
    if (t->kind == TOK_IDENT)
    {
        if (p->pos + 1 < p->count && p->tokens[p->pos + 1].kind == TOK_COLON)
        {
            return parse_label_stmt(p);
        }
        if (is_typename_start(p))
        {
            Specs s = parse_specs_storage(p, true);
            if (!s.type)
            {
                return NULL;
            }
            return parse_var_decl(p, s);
        }
    }

    for (size_t i = 0; i < sizeof(STMT_KEYWORDS) / sizeof(STMT_KEYWORDS[0]); i++)
    {
        if (STMT_KEYWORDS[i].tok == t->kind)
        {
            return STMT_KEYWORDS[i].fn(p);
        }
    }
    return parse_expr_stmt(p);
}

static ASTNode *parse_expression(Parser *p)
{
    ASTNode *left = parse_assign(p);
    if (!left)
    {
        return NULL;
    }
    while (peek_token(p)->kind == TOK_COMMA)
    {
        Token *t = peek_token(p);
        next_token(p);
        ASTNode *right = parse_assign(p);
        if (!right)
        {
            return NULL;
        }
        left = ast_binary_expr(BIN_COMMA, left, right, t->loc, p->arena);
    }
    return left;
}

static Vec *parse_arg_list(Parser *p)
{
    Vec *args = vec_new(p->arena);
    if (peek_token(p)->kind != TOK_RPAREN)
    {
        while (true)
        {
            ASTNode *arg = parse_assign(p);
            if (!arg)
            {
                return NULL;
            }
            vec_push(args, arg);
            if (peek_token(p)->kind != TOK_COMMA)
            {
                break;
            }
            next_token(p);
        }
    }
    if (!expect_token(p, TOK_RPAREN, "')'"))
    {
        return NULL;
    }
    return args;
}

static ASTNode *parse_identifier(Parser *p, Token *t)
{
    next_token(p);
    if (peek_token(p)->kind == TOK_LPAREN)
    {
        next_token(p);
        Vec *args = parse_arg_list(p);
        if (!args)
        {
            return NULL;
        }
        return ast_call_expr(t->payload.str, args, t->loc, p->arena);
    }
    return ast_ident(t->payload.str, t->loc, p->arena);
}

static ASTNode *parse_builtin_va_arg(Parser *p, Token *t)
{
    next_token(p);
    if (!expect_token(p, TOK_LPAREN, "'('"))
    {
        return NULL;
    }
    ASTNode *ap = parse_assign(p);
    if (!ap)
    {
        return NULL;
    }
    if (!expect_token(p, TOK_COMMA, "','"))
    {
        return NULL;
    }
    if (!is_typename_start_at(p, p->pos))
    {
        parse_error(p, "expected a type name after ',' in '__builtin_va_arg'");
        return NULL;
    }
    Type *ty = parse_abstract_declarator(p, parse_type_specifier(p));
    if (!ty)
    {
        return NULL;
    }
    if (!expect_token(p, TOK_RPAREN, "')'"))
    {
        return NULL;
    }
    return ast_va_arg_expr(ap, ty, t->loc, p->arena);
}

static ASTNode *parse_primary(Parser *p)
{
    Token *t = peek_token(p);
    switch (t->kind)
    {
        case TOK_INT_LIT:
            next_token(p);
            return ast_int_literal(t->payload.int_val, t->int_suffix.is_unsigned,
                                   t->int_suffix.length, t->int_suffix.is_hex, t->loc, p->arena);
        case TOK_CHAR_LIT:
            next_token(p);
            return ast_int_literal(t->payload.int_val, false, SUFFIX_NONE, false, t->loc, p->arena);
        case TOK_IDENT:
            if (strcmp(t->payload.str, "__builtin_va_arg") == 0 && p->pos + 1 < p->count &&
                p->tokens[p->pos + 1].kind == TOK_LPAREN)
            {
                return parse_builtin_va_arg(p, t);
            }
            {
                i64 *const_val = strmap_get(p->enum_consts, t->payload.str);
                if (const_val)
                {
                    next_token(p);
                    return ast_int_literal(*const_val, false, SUFFIX_NONE, false, t->loc, p->arena);
                }
            }
            return parse_identifier(p, t);
        case TOK_STRING_LIT:
            next_token(p);
            return ast_string_literal(t->payload.str, t->str_len, t->loc, p->arena);
        case TOK_LPAREN:
        {
            next_token(p);
            ASTNode *inner = parse_expression(p);
            if (!inner)
            {
                return NULL;
            }
            if (!expect_token(p, TOK_RPAREN, "')'"))
            {
                return NULL;
            }
            return inner;
        }
        default:
            parse_error(p, "expected expression");
            return NULL;
    }
}

static ASTNode *parse_compound_literal(Parser *p, Type *target, Loc start)
{
    ASTNode *init = parse_init_list(p);
    if (!init)
    {
        return NULL;
    }
    return parse_postfix_ops(p, ast_compound_literal(target, init, start, p->arena));
}

static Type *parse_paren_type_name(Parser *p)
{
    Type *ty = parse_type_specifier(p);
    if (!ty)
    {
        return NULL;
    }
    ty = parse_abstract_declarator(p, ty);
    if (!ty)
    {
        return NULL;
    }
    ty = parse_array_suffix(p, ty);
    if (!ty)
    {
        return NULL;
    }
    if (!expect_token(p, TOK_RPAREN, "')'"))
    {
        return NULL;
    }
    return ty;
}

static ASTNode *parse_unary(Parser *p)
{
    Token *t = peek_token(p);
    if (paren_is_typename(p))
    {
        next_token(p); /* '(' */
        Type *target = parse_paren_type_name(p);
        if (!target)
        {
            return NULL;
        }
        if (peek_token(p)->kind == TOK_LBRACE)
        {
            return parse_compound_literal(p, target, t->loc);
        }
        if (type_is_array(target))
        {
            parse_error(p, "expected '{' after compound literal type name");
            return NULL;
        }
        ASTNode *operand = parse_unary(p);
        if (!operand)
        {
            return NULL;
        }
        return ast_cast_expr(target, operand, t->loc, p->arena);
    }

    if (t->kind == TOK_KW_SIZEOF)
    {
        next_token(p);
        if (paren_is_typename(p))
        {
            next_token(p); /* '(' */
            Type *ty = parse_paren_type_name(p);
            if (!ty)
            {
                return NULL;
            }
            return ast_sizeof_type(ty, 0, t->loc, p->arena);
        }
        ASTNode *operand = parse_unary(p);
        if (!operand)
        {
            return NULL;
        }
        return ast_sizeof_expr(operand, 0, t->loc, p->arena);
    }
    if (t->kind == TOK_KW_ALIGNOF)
    {
        next_token(p);
        if (paren_is_typename(p))
        {
            next_token(p); /* '(' */
            Type *ty = parse_paren_type_name(p);
            if (!ty)
            {
                return NULL;
            }
            return ast_alignof_type(ty, 0, t->loc, p->arena);
        }
        ASTNode *operand = parse_unary(p);
        if (!operand)
        {
            return NULL;
        }
        return ast_alignof_expr(operand, 0, t->loc, p->arena);
    }

    if (t->kind == TOK_PLUS_PLUS || t->kind == TOK_MINUS_MINUS)
    {
        next_token(p);
        ASTNode *operand = parse_unary(p);
        if (!operand)
        {
            return NULL;
        }
        return ast_incdec_expr(operand, t->kind == TOK_PLUS_PLUS, false, t->loc, p->arena);
    }

    static const struct
    {
        TokenKind tok;
        UnaryOpKind op;
    } UNARY_OPS[] = {
        {TOK_MINUS, UN_NEG},  {TOK_NOT, UN_LOG_NOT}, {TOK_TILDE, UN_BIT_NOT},
        {TOK_STAR, UN_DEREF}, {TOK_BW_AND, UN_ADDR},
    };
    for (size_t i = 0; i < sizeof(UNARY_OPS) / sizeof(UNARY_OPS[0]); i++)
    {
        if (UNARY_OPS[i].tok == t->kind)
        {
            next_token(p);
            ASTNode *operand = parse_unary(p);
            if (!operand)
            {
                return NULL;
            }
            return ast_unary_expr(UNARY_OPS[i].op, operand, t->loc, p->arena);
        }
    }

    return parse_postfix(p);
}

static ASTNode *parse_postfix_ops(Parser *p, ASTNode *node)
{
    for (;;)
    {
        Token *t = peek_token(p);
        switch (t->kind)
        {
            case TOK_LBRACKET:
            {
                next_token(p);
                ASTNode *index = parse_expression(p);
                if (!index)
                {
                    return NULL;
                }
                if (!expect_token(p, TOK_RBRACKET, "]"))
                {
                    return NULL;
                }
                node = ast_subscript_expr(node, index, t->loc, p->arena);
                continue;
            }
            case TOK_DOT:
            case TOK_ARROW:
            {
                bool is_arrow = t->kind == TOK_ARROW;
                next_token(p);
                Token *member = peek_token(p);
                if (member->kind != TOK_IDENT)
                {
                    parse_error(p, "expected member name after '%s'", is_arrow ? "->" : ".");
                    return NULL;
                }
                next_token(p);
                node = ast_member_access(node, member->payload.str, is_arrow, t->loc, p->arena);
                continue;
            }
            case TOK_PLUS_PLUS:
            case TOK_MINUS_MINUS:
                next_token(p);
                node = ast_incdec_expr(node, t->kind == TOK_PLUS_PLUS, true, t->loc, p->arena);
                continue;
            case TOK_LPAREN:
                next_token(p);
                {
                    Vec *args = parse_arg_list(p);
                    if (!args)
                    {
                        return NULL;
                    }
                    node = ast_indirect_call(node, args, t->loc, p->arena);
                    continue;
                }
            default:
                return node;
        }
    }
}

static ASTNode *parse_postfix(Parser *p)
{
    ASTNode *node = parse_primary(p);
    if (!node)
    {
        return NULL;
    }
    return parse_postfix_ops(p, node);
}

/* Left-associative binary operators, multiplicative down to logical-or, with
   precedence (higher binds tighter). */
enum
{
    PREC_LOG_OR = 1,
    PREC_LOG_AND,
    PREC_BIT_OR,
    PREC_BIT_XOR,
    PREC_BIT_AND,
    PREC_EQUALITY,
    PREC_RELATIONAL,
    PREC_SHIFT,
    PREC_ADDITIVE,
    PREC_MULTIPLICATIVE,
};

typedef struct BinOpToken
{
    TokenKind tok;
    BinOpKind op;
    u8 prec;
} BinOpToken;

static const BinOpToken BIN_OPS[] = {
    {TOK_LOG_OR, BIN_LOG_OR, PREC_LOG_OR},
    {TOK_LOG_AND, BIN_LOG_AND, PREC_LOG_AND},
    {TOK_BW_OR, BIN_OR, PREC_BIT_OR},
    {TOK_BW_XOR, BIN_XOR, PREC_BIT_XOR},
    {TOK_BW_AND, BIN_AND, PREC_BIT_AND},
    {TOK_EQ, BIN_EQ, PREC_EQUALITY},
    {TOK_NE, BIN_NE, PREC_EQUALITY},
    {TOK_LT, BIN_LT, PREC_RELATIONAL},
    {TOK_GT, BIN_GT, PREC_RELATIONAL},
    {TOK_LE, BIN_LE, PREC_RELATIONAL},
    {TOK_GE, BIN_GE, PREC_RELATIONAL},
    {TOK_SHL, BIN_SHL, PREC_SHIFT},
    {TOK_SHR, BIN_SHR, PREC_SHIFT},
    {TOK_PLUS, BIN_ADD, PREC_ADDITIVE},
    {TOK_MINUS, BIN_SUB, PREC_ADDITIVE},
    {TOK_STAR, BIN_MUL, PREC_MULTIPLICATIVE},
    {TOK_SLASH, BIN_DIV, PREC_MULTIPLICATIVE},
    {TOK_PERCENT, BIN_REM, PREC_MULTIPLICATIVE},
};

static const BinOpToken *binary_op(TokenKind kind)
{
    for (size_t i = 0; i < (sizeof(BIN_OPS) / sizeof(BIN_OPS[0])); i++)
    {
        if (BIN_OPS[i].tok == kind)
        {
            return &BIN_OPS[i];
        }
    }
    return NULL;
}

/* Precedence climbing over the left-associative binary table. */
static ASTNode *parse_binary(Parser *p, u8 min_prec)
{
    ASTNode *left = parse_unary(p);
    if (!left)
    {
        return NULL;
    }
    for (;;)
    {
        Token *t = peek_token(p);
        const BinOpToken *op = binary_op(t->kind);
        if (!op || op->prec < min_prec)
        {
            return left;
        }
        next_token(p);
        ASTNode *right = parse_binary(p, op->prec + 1);
        if (!right)
        {
            return NULL;
        }
        left = ast_binary_expr(op->op, left, right, t->loc, p->arena);
    }
}

static ASTNode *parse_ternary(Parser *p)
{
    ASTNode *cond = parse_binary(p, PREC_LOG_OR);
    if (!cond)
    {
        return NULL;
    }
    if (peek_token(p)->kind != TOK_QUESTION)
    {
        return cond;
    }

    Token *t = peek_token(p);
    next_token(p);
    ASTNode *then = parse_expression(p);
    if (!then)
    {
        return NULL;
    }
    if (!expect_token(p, TOK_COLON, "':'"))
    {
        return NULL;
    }
    ASTNode *els = parse_ternary(p);
    if (!els)
    {
        return NULL;
    }
    return ast_ternary_expr(cond, then, els, t->loc, p->arena);
}

static BinOpKind assignment_op(TokenKind kind)
{
    switch (kind)
    {
        case TOK_ASSIGN:
            return BIN_ASSIGN;
        case TOK_PLUS_ASSIGN:
            return BIN_ADD_ASSIGN;
        case TOK_MINUS_ASSIGN:
            return BIN_SUB_ASSIGN;
        case TOK_STAR_ASSIGN:
            return BIN_MUL_ASSIGN;
        case TOK_SLASH_ASSIGN:
            return BIN_DIV_ASSIGN;
        case TOK_PERCENT_ASSIGN:
            return BIN_REM_ASSIGN;
        case TOK_SHL_ASSIGN:
            return BIN_SHL_ASSIGN;
        case TOK_SHR_ASSIGN:
            return BIN_SHR_ASSIGN;
        case TOK_BW_AND_ASSIGN:
            return BIN_AND_ASSIGN;
        case TOK_BW_OR_ASSIGN:
            return BIN_OR_ASSIGN;
        case TOK_BW_XOR_ASSIGN:
            return BIN_XOR_ASSIGN;
        default:
            return (BinOpKind) -1;
    }
}

static ASTNode *parse_assign(Parser *p)
{
    ASTNode *left = parse_ternary(p);
    if (!left)
    {
        return NULL;
    }

    BinOpKind op = assignment_op(peek_token(p)->kind);
    if (op != (BinOpKind) -1)
    {
        Token *t = peek_token(p);
        next_token(p);
        ASTNode *right = parse_assign(p);
        if (!right)
        {
            return NULL;
        }
        left = ast_binary_expr(op, left, right, t->loc, p->arena);
    }
    return left;
}

static Designator *parse_designators(Parser *p)
{
    Designator *head = NULL;
    Designator **tail = &head;
    while (peek_token(p)->kind == TOK_DOT || peek_token(p)->kind == TOK_LBRACKET)
    {
        Designator *d = arena_alloc(p->arena, sizeof(Designator), _Alignof(Designator));
        d->next = NULL;
        if (peek_token(p)->kind == TOK_DOT)
        {
            next_token(p);
            Token *name = peek_token(p);
            if (name->kind != TOK_IDENT)
            {
                parse_error(p, "expected field name after '.' designator");
                return NULL;
            }
            next_token(p);
            d->kind = ND_FIELD;
            d->field = name->payload.str;
            d->index = 0;
        }
        else
        {
            next_token(p);
            ASTNode *idx = parse_assign(p);
            if (!idx || idx->kind != AST_INT_LITERAL)
            {
                parse_error(p, "array designator index must be an integer constant");
                return NULL;
            }
            if (!expect_token(p, TOK_RBRACKET, "']'"))
            {
                return NULL;
            }
            d->kind = ND_INDEX;
            d->index = ast_as(ASTIntLiteral, idx)->value;
            d->field = NULL;
        }
        *tail = d;
        tail = &d->next;
    }
    return head;
}

static ASTNode *parse_init_list(Parser *p)
{
    Token *start = peek_token(p);
    next_token(p);

    Vec *elems = vec_new(p->arena);
    while (peek_token(p)->kind != TOK_RBRACE)
    {
        Token *elem_start = peek_token(p);
        InitElem *e = arena_alloc(p->arena, sizeof(InitElem), _Alignof(InitElem));
        e->loc = elem_start->loc;
        e->design = parse_designators(p);
        if (!e->design)
        {
            e->value = parse_initializer(p);
        }
        else
        {
            if (!expect_token(p, TOK_ASSIGN, "'='"))
            {
                return NULL;
            }
            e->value = parse_initializer(p);
        }
        if (!e->value)
        {
            return NULL;
        }
        vec_push(elems, e);

        TokenKind sep = peek_token(p)->kind;
        if (sep == TOK_COMMA)
        {
            next_token(p);
            if (peek_token(p)->kind == TOK_RBRACE)
            {
                break;
            }
        }
        else if (sep != TOK_RBRACE)
        {
            parse_error(p, "expected ',' or '}' in initializer list");
            return NULL;
        }
    }
    if (!expect_token(p, TOK_RBRACE, "'}'"))
    {
        return NULL;
    }
    return ast_init_list(elems, start->loc, p->arena);
}

static ASTNode *parse_initializer(Parser *p)
{
    if (peek_token(p)->kind == TOK_LBRACE)
    {
        return parse_init_list(p);
    }
    return parse_assign(p);
}

static bool folded_const(Parser *p, ASTNode *node, i64 *out)
{
    (void) p;
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
            if (!folded_const(p, u->operand, &v))
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
            if (!folded_const(p, b->left, &l) || !folded_const(p, b->right, &r))
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
                        return false;
                    }
                    *out = b->op == BIN_DIV ? l / r : l % r;
                    return true;
                case BIN_SHL:
                case BIN_SHR:
                    if (r < 0 || r >= (i64) (sizeof(i64) * 8))
                    {
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
                case BIN_LOG_AND:
                    *out = l && r;
                    return true;
                case BIN_LOG_OR:
                    *out = l || r;
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
        case AST_TERNARY_EXPR:
        {
            ASTTernaryExpr *te = ast_as(ASTTernaryExpr, node);
            i64 cond;
            if (!folded_const(p, te->cond, &cond))
            {
                return false;
            }
            return folded_const(p, cond ? te->then_expr : te->else_expr, out);
        }
        case AST_SIZEOF_TYPE:
            *out = (i64) type_sizeof(ast_as(ASTSizeofType, node)->type);
            return true;
        case AST_ALIGNOF_TYPE:
            *out = (i64) type_alignof(ast_as(ASTAlignofType, node)->type);
            return true;
        case AST_CAST_EXPR:
        {
            ASTCastExpr *ce = ast_as(ASTCastExpr, node);
            i64 v;
            if (!folded_const(p, ce->operand, &v) || !type_is_integer(ce->target_type))
            {
                return false;
            }
            *out = type_reduce_int(ce->target_type, v);
            return true;
        }
        default:
            return false;
    }
}

static bool resolve_constant_init(Parser *p, ASTVarDecl *vd, ASTNode *expr)
{
    if (expr->kind == AST_INIT_LIST)
    {
        vd->init = expr;
        return true;
    }
    if (expr->kind == AST_IDENT)
    {
        Binding *b = name_lookup(p, ast_as(ASTIdent, expr)->name);
        if (b && b->kind == BIND_FUNC)
        {
            vd->init = expr;
            return true;
        }
    }
    if (expr->kind == AST_UNARY_EXPR)
    {
        ASTUnaryExpr *u = ast_as(ASTUnaryExpr, expr);
        if (u->op == UN_ADDR &&
            (u->operand->kind == AST_IDENT || u->operand->kind == AST_COMPOUND_LITERAL))
        {
            vd->init = expr;
            return true;
        }
    }
    i64 value;
    if (folded_const(p, expr, &value))
    {
        vd->const_init = value;
        vd->has_const_init = true;
        return true;
    }
    if (expr->kind == AST_STRING_LITERAL)
    {
        vd->init = expr;
        return true;
    }
    return false;
}

ASTNode *parse(Token *tokens, u64 count, Arena *arena)
{
    ASSERT(count > 0);
    Parser p = {tokens, count, 0, arena, strmap_new(arena), vec_new(arena)};
    push_scope(&p);

    if (!declare_name(&p, "__builtin_va_list", BIND_TYPEDEF, type_va_list()))
    {
        return NULL;
    }

    Vec *decls = vec_new(arena);
    while (peek_token(&p)->kind != TOK_EOF)
    {
        ASTNode *node = parse_toplevel_decl(&p);
        if (!node)
        {
            return NULL;
        }
        vec_push(decls, node);
    }

    return ast_program(decls, tokens[0].loc, arena);
}
